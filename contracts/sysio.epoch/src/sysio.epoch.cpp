#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.token/sysio.token.hpp>
// Canonical sysio.system emissions types + compute_epoch_emission. The
// [[sysio::contract("sysio.system")]] attribute on emission_config / t5_state
// pins them to sysio.system's ABI; no readonly mirror needed here.
#include <sysio.system/emissions.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>

namespace sysio {

using opp::types::OperatorType;
using opp::types::AttestationType;
using opp::types::OperatorStatus;
using opp::types::EmissionsBlockReason;

// ---------------------------------------------------------------------------
// Emissions readiness gate. Runs in advance() before any state mutation:
// reads sysio.system::emitcfg + t5state and sysio.token::accounts to compute
// whether emissions can pay this epoch. If not, advance emits an
// EmissionsBlocked attestation per outpost (deduped by the local blocklog
// table) and returns without state change. The wall clock for the current
// epoch effectively extends until conditions allow a successful gate pass.
// ---------------------------------------------------------------------------

namespace {

constexpr name SYSTEM_ACCOUNT     = "sysio"_n;
constexpr name TOKEN_ACCOUNT      = "sysio.token"_n;

// System-owned rows are billed to the sysio RAM pool rather than to this contract account (the
// privileged-contract model sysio.token uses): the contract account stays finite at its code+abi
// size while table growth draws from sysio's pool. Permitted because the contract is privileged.
constexpr name ram_payer          = "sysio"_n;
constexpr symbol WIRE_SYMBOL{"WIRE", 9};

/// True when a chains row represents an active outpost (i.e. not the depot
/// self-row and is active). Pulled out so every fanout loop in `advance`
/// uses the identical predicate.
inline bool is_active_outpost(const sysio::chains::chain_row& row) {
   return row.active && !row.is_depot;
}

struct emissions_gate_result {
   bool                  ready              = false;
   bool                  is_pay_epoch       = false; // true on the period-boundary epoch where payepoch fires
   int64_t               emission_amount    = 0;  // per-epoch emission for THIS epoch (always populated when ready)
   int64_t               period_emission    = 0;  // pending + emission_amount; populated only on pay-epoch
   int64_t               treasury_remaining = 0;  // for blocklog row
   int64_t               sysio_balance      = 0;  // for blocklog row
   EmissionsBlockReason  reason             = opp::types::EMISSIONS_BLOCK_REASON_UNSPECIFIED;
};

emissions_gate_result check_emissions_ready(uint32_t epoch_duration_sec, uint32_t target_epoch) {
   emissions_gate_result r;

   sysiosystem::emissions::emitcfg_t emit_cfg_tbl(SYSTEM_ACCOUNT);
   if (!emit_cfg_tbl.exists()) {
      r.reason = opp::types::EMISSIONS_BLOCK_REASON_CONFIG_MISSING;
      return r;
   }
   const auto cfg = emit_cfg_tbl.get();

   sysiosystem::emissions::t5state_t t5s_tbl(SYSTEM_ACCOUNT);
   if (!t5s_tbl.exists()) {
      r.reason = opp::types::EMISSIONS_BLOCK_REASON_STATE_UNINITIALIZED;
      return r;
   }
   const auto t5s = t5s_tbl.get();
   r.treasury_remaining = cfg.t5_distributable - cfg.t5_floor - t5s.total_distributed;

   // epoch_duration_sec is the canonical value from sysio.epoch::epochcfg,
   // passed by advance() so this gate and sysio.system see identical inputs
   // and the gate doesn't repeat advance()'s read of the same singleton.

   // Compute would-be per-epoch emission. First-epoch case: use initial; cap at remaining.
   if (t5s.epoch_count == 0) {
      r.emission_amount = sysiosystem::emissions::scale_annual_to_epoch(
         cfg.annual_initial_emission, epoch_duration_sec);
      if (r.emission_amount > r.treasury_remaining) r.emission_amount = r.treasury_remaining;
   } else {
      r.emission_amount = sysiosystem::emissions::compute_epoch_emission(
         cfg, epoch_duration_sec, t5s.last_epoch_emission, t5s.total_distributed);
   }

   // TREASURY_EXHAUSTED gates every epoch (pay or non-pay). A zero per-epoch
   // emission means the treasury is at floor; advancing the epoch silently
   // would let the chain roll forward into a depleted treasury.
   if (r.emission_amount <= 0) {
      r.reason = opp::types::EMISSIONS_BLOCK_REASON_TREASURY_EXHAUSTED;
      return r;
   }

   // Decide whether this advance fires payepoch. Pay fires when the target
   // epoch is `pay_cadence_epochs - 1` past `period_start_epoch`. Genesis
   // case: t5s.period_start_epoch = 0, so the first period covers epochs
   // 1..(pay_cadence - 1) (one short, since epoch 0 is genesis). Subsequent
   // periods are exactly pay_cadence_epochs long.
   r.is_pay_epoch = (target_epoch >= t5s.period_start_epoch + cfg.pay_cadence_epochs - 1);

   // Pay-epoch only: check the period total (pending + this epoch's share)
   // against sysio's balance. Non-pay epochs do not transfer, so no balance
   // check is needed; pending accumulates in t5state via accrueepoch.
   if (r.is_pay_epoch) {
      r.period_emission = t5s.pending_emission_amount + r.emission_amount;

      sysio::token::token::accounts acct_tbl(TOKEN_ACCOUNT, SYSTEM_ACCOUNT.value);
      sysio::token::token::acct_key key{WIRE_SYMBOL.code().raw()};
      if (acct_tbl.contains(key)) {
         r.sysio_balance = acct_tbl.get(key).balance.amount;
      }

      // emission_amount / period_emission is retained on the
      // BALANCE_INSUFFICIENT path so the blocklog row + cross-chain
      // EmissionsBlocked attestation report the real shortfall (period
      // total vs. available balance), not zero.
      if (r.sysio_balance < r.period_emission) {
         r.reason = opp::types::EMISSIONS_BLOCK_REASON_BALANCE_INSUFFICIENT;
         return r;
      }
   }

   r.ready = true;
   return r;
}

// Broadcast an EmissionsBlocked attestation to every active outpost.
// Called from record_gate_block on the first block for a given epoch_index
// or when the reason changes since the previous attempt. The
// first_blocked_at_secs argument is the original blocking time -- on a
// reason-change rebroadcast the caller passes the existing row's
// first_blocked_at so the timestamp tracks the original block, not "now".
void emit_emissions_block_attestation(name self,
                                      uint32_t epoch_index,
                                      const emissions_gate_result& gate,
                                      uint32_t first_blocked_at_secs) {
   opp::attestations::EmissionsBlocked msg;
   msg.epoch_index        = epoch_index;
   msg.reason             = gate.reason;
   msg.attempted_emission = gate.emission_amount;
   msg.treasury_remaining = gate.treasury_remaining;
   msg.sysio_balance      = gate.sysio_balance;
   msg.first_blocked_at   = first_blocked_at_secs;

   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(msg);

   sysio::chains::chains_t chains_tbl(epoch::CHAINS_ACCOUNT);
   for (auto it = chains_tbl.begin(); it != chains_tbl.end(); ++it) {
      if (!is_active_outpost(*it)) continue;
      action(
         permission_level{self, "owner"_n},
         epoch::MSGCH_ACCOUNT,
         "queueout"_n,
         std::make_tuple(
            it->code.value,
            opp::types::ATTESTATION_TYPE_EMISSIONS_BLOCKED,
            encoded
         )
      ).send();
   }
}

// Inserts or updates the local blocklog row for a gate-block on epoch_index.
// On INSERT (first block for this epoch_index) OR when the reason has changed
// since the previous attempt, also broadcasts an EmissionsBlocked attestation
// per outpost. On UPDATE with same reason, only bumps last_retry_at and
// retry_count -- no outbound emission (dedup).
void record_gate_block(name self, uint32_t epoch_index, const emissions_gate_result& gate) {
   epoch::blocklog_t log_tbl(self);
   epoch::blocklog_key pk{epoch_index};

   const uint32_t now_secs = static_cast<uint32_t>(current_time_point().sec_since_epoch());

   if (!log_tbl.contains(pk)) {
      log_tbl.emplace(ram_payer, pk, epoch::blocklog_entry{
         .epoch_index        = epoch_index,
         .reason             = gate.reason,
         .attempted_emission = gate.emission_amount,
         .treasury_remaining = gate.treasury_remaining,
         .sysio_balance      = gate.sysio_balance,
         .first_blocked_at   = now_secs,
         .last_retry_at      = now_secs,
         .retry_count        = 1,
      });
      emit_emissions_block_attestation(self, epoch_index, gate, now_secs);
      return;
   }

   const auto existing = log_tbl.get(pk);
   const bool reason_changed = existing.reason != gate.reason;

   log_tbl.modify(ram_payer, pk, [&](auto& row) {
      row.reason             = gate.reason;
      row.attempted_emission = gate.emission_amount;
      row.treasury_remaining = gate.treasury_remaining;
      row.sysio_balance      = gate.sysio_balance;
      row.last_retry_at      = now_secs;
      row.retry_count       += 1;
      // first_blocked_at intentionally unchanged: tracks the original block.
   });

   if (reason_changed) {
      emit_emissions_block_attestation(self, epoch_index, gate, existing.first_blocked_at);
   }
}

// Removes the blocklog row for epoch_index if present. Called on the gate
// success path so the row reflects only currently-blocked epochs.
void clear_gate_block(name self, uint32_t epoch_index) {
   epoch::blocklog_t log_tbl(self);
   epoch::blocklog_key pk{epoch_index};
   if (log_tbl.contains(pk)) {
      log_tbl.erase(pk);
   }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void epoch::setconfig(uint32_t epoch_duration_sec,
                      uint32_t operators_per_epoch,
                      uint32_t batch_operator_minimum_active,
                      uint32_t batch_op_groups,
                      uint32_t epoch_retention_envelope_log_count) {
   require_auth(get_self());

   check(epoch_duration_sec >= MIN_EPOCH_DURATION_SEC,
         "epoch_duration_sec must be >= MIN_EPOCH_DURATION_SEC");
   check(epoch_duration_sec <= MAX_EPOCH_DURATION_SEC,
         "epoch_duration_sec exceeds 30-day ceiling");
   check(operators_per_epoch > 0, "operators_per_epoch must be positive");
   check(batch_op_groups > 0, "batch_op_groups must be positive");
   check(batch_operator_minimum_active == operators_per_epoch * batch_op_groups,
         "batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups");
   check(epoch_retention_envelope_log_count > 0,
         "epoch_retention_envelope_log_count must be positive");

   epochcfg_t cfg_tbl(get_self());
   epoch_config cfg = cfg_tbl.get_or_default(epoch_config{});
   cfg.epoch_duration_sec = epoch_duration_sec;
   cfg.operators_per_epoch = operators_per_epoch;
   cfg.batch_operator_minimum_active = batch_operator_minimum_active;
   cfg.batch_op_groups = batch_op_groups;
   cfg.epoch_retention_envelope_log_count = epoch_retention_envelope_log_count;
   cfg_tbl.set(cfg, ram_payer);
}

// ---------------------------------------------------------------------------
//  advance
// ---------------------------------------------------------------------------
void epoch::advance() {
   epochcfg_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});

   check(!state.is_paused, "epoch advancement is paused");

   // Post-genesis: only sysio.msgch (via evalcons consensus) may advance.
   // Genesis (epoch 0→1): permissionless for bootstrap.
   if (state.current_epoch_index > 0) {
      check(has_auth(MSGCH_ACCOUNT) || has_auth(get_self()),
            "only sysio.msgch may advance the epoch after genesis");
   }

   // Wall-clock minimum: don't advance before next_epoch_start
   auto now = current_time_point();
   if (now < state.next_epoch_start) return;

   // Emissions readiness gate: epochs are never partially-advanced. If the
   // sysio.system treasury cannot pay this epoch, record the block locally,
   // emit an EmissionsBlocked attestation per outpost (deduped), and return
   // without mutating state. The wall clock for the current epoch effectively
   // extends until the gate eventually passes on a subsequent chkcons retry.
   const uint32_t target_epoch = state.current_epoch_index + 1;
   const auto gate = check_emissions_ready(cfg.epoch_duration_sec, target_epoch);
   if (!gate.ready) {
      record_gate_block(get_self(), target_epoch, gate);
      return;
   }
   // Gate passed: drop any prior block_log row for this epoch (if a previous
   // attempt blocked and we're now succeeding) and proceed.
   clear_gate_block(get_self(), target_epoch);

   // Before incrementing: evaluate per-op delivery state for the EXPIRING
   // epoch. The active group of the expiring epoch (`current_batch_op_group`
   // BEFORE the increment) is the set of ops responsible for delivering
   // every active outpost's inbound envelope for `current_epoch_index`.
   //
   // For each (outpost × member of the expiring group):
   //   - scan `msgch::envelopes` (`byoutepoch` index) for any row matching
   //     (chain_code, current_epoch_index, batch_op_name == member)
   //   - inline `opreg::recorddel(member, current_epoch_index, did_deliver)`
   //   - inline `opreg::termcheck(member)` — the threshold + window come
   //     from `op_config`, so tests dial the thresholds via setconfig
   //
   // The outpost set is sourced via a cross-contract read of
   // `sysio.chains::chains` (no local mirror) filtered to
   // `active==true && !is_depot`. Each surviving row's `code` is the
   // outpost's chain code; its underlying `uint64` value is what
   // `sysio.msgch::envelopes.chain_code` carries on the wire.
   //
   // Skipped on the genesis epoch (`current_epoch_index == 0`) — no group
   // existed yet, and the membership vector is empty.
   if (state.current_epoch_index > 0 &&
       !state.batch_op_groups.empty() &&
       state.current_batch_op_group < state.batch_op_groups.size()) {
      const auto& expiring_group =
         state.batch_op_groups[state.current_batch_op_group];

      msgch::envelopes_t envs(MSGCH_ACCOUNT);
      auto oe_idx = envs.get_index<"byoutepoch"_n>();

      msgch::outpost_consensus_t opcons(MSGCH_ACCOUNT);

      // Operators that delivered a NON-canonical envelope for ANY outpost this epoch (deduped).
      // Slashed once, after the per-outpost loop: an operator that is non-canonical on multiple
      // outposts must be slashed a single time (opreg::slash throws on a second slash of the same
      // operator, which would abort advance and stall the chain).
      std::vector<name> to_slash;

      sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
      for (auto op_it = chains_tbl.begin(); op_it != chains_tbl.end(); ++op_it) {
         if (!is_active_outpost(*op_it)) continue;

         const uint64_t chain_code = op_it->code.value;
         const uint64_t composite =
            (chain_code << 32) | state.current_epoch_index;

         // Walk the (outpost, epoch) bucket and collect each distinct delivering batch op together
         // with the checksum it delivered. Vector linear-scan is fine — group size is small
         // (single-digit ops/group in every practical config).
         std::vector<name>        delivered;
         std::vector<checksum256> delivered_checksums;
         for (auto e = oe_idx.lower_bound(composite);
              e != oe_idx.end() && e->by_outpost_epoch() == composite; ++e) {
            bool already = false;
            for (const auto& d : delivered) {
               if (d == e->batch_op_name) { already = true; break; }
            }
            if (!already) {
               delivered.push_back(e->batch_op_name);
               delivered_checksums.push_back(e->checksum);
            }
         }

         // Canonical envelope checksum for this (outpost, epoch), recorded by msgch consensus or by
         // dispute resolution. Present only once a winner exists for the expiring epoch; absent it,
         // nothing is slashed for this outpost (e.g. an outpost that never reached a winner).
         checksum256 winner{};
         bool        have_winner = false;
         {
            auto opc_pk = msgch::outpost_consensus_key{chain_code};
            if (opcons.contains(opc_pk)) {
               auto opc = opcons.get(opc_pk);
               if (opc.epoch_index == state.current_epoch_index) {
                  winner      = opc.winning_checksum;
                  have_winner = true;
               }
            }
         }

         for (const auto& member : expiring_group) {
            bool        did_deliver = false;
            checksum256 member_checksum{};
            for (size_t i = 0; i < delivered.size(); ++i) {
               if (delivered[i] == member) {
                  did_deliver     = true;
                  member_checksum = delivered_checksums[i];
                  break;
               }
            }
            action(
               permission_level{get_self(), "owner"_n},
               OPREG_ACCOUNT,
               "recorddel"_n,
               std::make_tuple(member, state.current_epoch_index, did_deliver)
            ).send();
            action(
               permission_level{get_self(), "owner"_n},
               OPREG_ACCOUNT,
               "termcheck"_n,
               std::make_tuple(member)
            ).send();

            // Single slash path (dispute-vote design, per-operator outcome table): a delivered
            // NON-canonical checksum is a fault -> slash. Silence (no delivery) is never slashed; it
            // stays on the recorddel/termcheck miss ladder above. Collect here; flush once below.
            if (did_deliver && have_winner && member_checksum != winner) {
               bool queued = false;
               for (const auto& s : to_slash) {
                  if (s == member) { queued = true; break; }
               }
               if (!queued) to_slash.push_back(member);
            }
         }
      }

      // Flush the non-canonical slashes. Routed through sysio.chalg (the slashing chokepoint that
      // holds opreg::slash authority): the slashable bond is moved to the matching LP and the
      // operator is marked SLASHED.
      for (const auto& member : to_slash) {
         action(
            permission_level{get_self(), "owner"_n},
            CHALG_ACCOUNT,
            "slashop"_n,
            std::make_tuple(member,
                            std::string("non-canonical OPP envelope delivery, epoch ")
                               + std::to_string(state.current_epoch_index))
         ).send();
      }

      // NOTE: we intentionally do NOT erase the per-batch-op envelope
      // metadata rows here. `evalcons` already cleared their heavy
      // `raw_data` (1-2 KB → 0 bytes) at consensus reach, so the residual
      // weight is just the tuple `(id, chain_code, epoch_index,
      // batch_op_name, checksum, ...)` — small and bounded by group
      // membership × outposts × retained-epochs. A dedicated bounded-
      // retention sweep belongs in a separate periodic ix; trying to
      // erase here races with the permissionless `chkcons` →
      // inline-`advance` pattern that fires from every batchop every
      // cron tick and trips kv-index-remove on already-evicted buckets.
   }

   const bool had_expiring_group = state.current_epoch_index > 0;

   state.current_epoch_index++;
   state.current_epoch_start =
      (state.next_epoch_start.sec_since_epoch() == 0) ? now : state.next_epoch_start;
   state.next_epoch_start = state.current_epoch_start +
      microseconds(static_cast<int64_t>(cfg.epoch_duration_sec) * 1'000'000);

   // ── Slide the schedule window ───────────────────────────────────────────
   // Skip on the genesis advance (0 → 1): schbatchgps just placed
   // [G1, G2, G3] for epochs 1, 2, 3 and G1 is now the current (front)
   // group — popping here would lose it. From the SECOND advance onward
   // (1 → 2, 2 → 3, ...), the front group has just expired so we pop
   // it and compute a new tail.
   //
   // Eligibility for the new tail: ACTIVE batch ops, sorted non-bootstrapped
   // first (preference rule), MINUS anyone already resident in the N-1
   // surviving groups. The window itself encodes "scheduled in the last
   // N-1 epochs" — no separate history table.
   //
   // After: window = [current, current+1, ..., current+N-1], front is
   // always the active group → current_batch_op_group stays at 0.
   if (had_expiring_group && !state.batch_op_groups.empty()) {
      state.batch_op_groups.erase(state.batch_op_groups.begin());

      // Collect already-resident accounts so the new tail excludes them.
      std::vector<name> resident;
      resident.reserve(cfg.batch_op_groups * cfg.operators_per_epoch);
      for (const auto& g : state.batch_op_groups) {
         for (const auto& a : g) resident.push_back(a);
      }
      auto is_resident = [&](name a) {
         for (const auto& r : resident) if (r == a) return true;
         return false;
      };

      // Pull ACTIVE batch ops, non-bootstrapped first, exclude resident.
      opreg::operators_t opreg_ops(OPREG_ACCOUNT);
      auto status_idx = opreg_ops.get_index<"bystatus"_n>();
      std::vector<std::pair<name, bool>> pool;
      for (auto it = status_idx.lower_bound(
              static_cast<uint64_t>(OperatorStatus::OPERATOR_STATUS_ACTIVE));
           it != status_idx.end() &&
           it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE; ++it) {
         if (it->type == OperatorType::OPERATOR_TYPE_BATCH && !is_resident(it->account)) {
            pool.push_back({it->account, it->is_bootstrapped});
         }
      }
      std::sort(pool.begin(), pool.end(),
         [](const auto& a, const auto& b) {
            if (a.second != b.second) return !a.second; // non-bootstrapped first
            return a.first < b.first;
         });

      std::vector<name> new_tail;
      new_tail.reserve(cfg.operators_per_epoch);
      for (size_t i = 0; i < pool.size() && new_tail.size() < cfg.operators_per_epoch; ++i) {
         new_tail.push_back(pool[i].first);
      }
      // An empty or short new tail is a degraded but non-fatal state: batch
      // operators were terminated faster than replacements could activate.
      // Aborting advance() here would halt OPP epoch advancement chain-wide,
      // leaving manual operator-roster repair as the only recovery -- so the
      // window keeps its N groups (this one possibly empty) and the shortfall
      // is reported instead. The empty group is also visible cross-chain in
      // the BatchOperatorGroups attestation built below.
      if (new_tail.empty()) {
         sysio::print("sysio.epoch::advance: no eligible batch operators for "
                      "the new tail group at epoch ",
                      state.current_epoch_index + cfg.batch_op_groups - 1,
                      "; pushing an empty group -- operator roster needs "
                      "attention\n");
      }

      state.batch_op_groups.push_back(std::move(new_tail));
   }

   state.current_batch_op_group = 0;

   // Note: last_elected_epoch tracking is epoch-internal state.
   // No operator table writes needed — group membership is in epoch_state.batch_op_groups.

   state_tbl.set(state, ram_payer);

   // Drain matured rows from `sysio.opreg::wtdwqueue`. Operators that queued
   // a withdrawal at least WITHDRAW_WAIT_EPOCHS ago are now eligible — opreg
   // subtracts from the balance and emits OPERATOR_ACTION(WITHDRAW_REMIT) to
   // the matching outpost (or transfers WIRE tokens directly for WIRE-direct
   // withdraws). Slashed-during-the-wait rows are dropped silently inside
   // opreg's flushwtdw. See CLAUDE-WIRE-OPERATOR-COLLATERAL-IMPL-PLAN.md §3.3.
   action(
      permission_level{get_self(), "owner"_n},
      OPREG_ACCOUNT,
      "flushwtdw"_n,
      std::make_tuple(state.current_epoch_index)
   ).send();

   // Queue OPERATORS attestation (full roster with authex chain addresses) for each outpost.
   // IMPORTANT: Must come before BATCH_OPERATOR_GROUPS so that the ETH outpost's
   // _handleOperators populates operatorEthAddress before _handleBatchOperatorGroups
   // looks up those addresses.
   {
      opp::attestations::Operators ops_attest;
      opreg::operators_t opreg_ops(OPREG_ACCOUNT);
      authex::links_t authex_links(AUTHEX_ACCOUNT);
      auto links_by_name = authex_links.get_index<"byname"_n>();

      for (auto it = opreg_ops.begin(); it != opreg_ops.end(); ++it) {
         opp::attestations::OperatorEntry entry;
         entry.account.name = it->account.to_string();
         entry.type = it->type;
         entry.status = it->status;

         // Collect all authex-linked chain addresses for this operator.
         // Store raw public key bytes from the variant (33 bytes for EM/secp256k1,
         // 32 bytes for ED/Ed25519).
         auto link_it = links_by_name.lower_bound(it->account.value);
         while (link_it != links_by_name.end() && link_it->username == it->account) {
            opp::types::ChainAddress chain_addr;
            chain_addr.kind = static_cast<opp::types::ChainKind>(link_it->chain_kind);

            std::visit([&](const auto& key_data) {
               using T = std::decay_t<decltype(key_data)>;
               if constexpr (std::is_same_v<T, webauthn_public_key>) {
                  // EM (secp256k1 compressed) — 33 bytes in key.key
                  chain_addr.address.assign(key_data.key.begin(), key_data.key.end());
               } else if constexpr (std::is_same_v<T, ed_public_key>) {
                  // ED (Ed25519) — 32 bytes
                  chain_addr.address.assign(
                     reinterpret_cast<const char*>(key_data.data()),
                     reinterpret_cast<const char*>(key_data.data() + key_data.size()));
               } else if constexpr (std::is_same_v<T, ecc_public_key>) {
                  // K1/R1 (secp256k1/P-256 compressed) — 33 bytes
                  chain_addr.address.assign(key_data.begin(), key_data.end());
               }
               // Skip BLS keys — not used for chain address linking
            }, link_it->pub_key);

            entry.addresses.push_back(std::move(chain_addr));
            ++link_it;
         }

         ops_attest.operators.push_back(std::move(entry));
      }

      std::vector<char> encoded;
      auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
      (void)out(ops_attest);

      sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
      for (auto it = chains_tbl.begin(); it != chains_tbl.end(); ++it) {
         if (!is_active_outpost(*it)) continue;
         action(
            permission_level{get_self(), "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->code.value,
               opp::types::ATTESTATION_TYPE_OPERATORS,
               encoded
            )
         ).send();
      }
   }

   // Queue BATCH_OPERATOR_GROUPS attestation for each outpost.
   // Includes ALL groups and the active group index for the new epoch.
   {
      opp::attestations::BatchOperatorGroups attest;
      attest.active_group_index = zpp::bits::vuint32_t{state.current_batch_op_group};
      attest.epoch_index = zpp::bits::vuint32_t{state.current_epoch_index};
      // Propagate the depot's minimum epoch duration so the outpost can
      // evaluate the fallback (path-2) majority consensus after this many
      // seconds since the current epoch started — see
      // .claude/rules/opp-consensus.md.
      attest.epoch_duration_sec = zpp::bits::vuint32_t{cfg.epoch_duration_sec};
      for (auto& group : state.batch_op_groups) {
         opp::attestations::BatchOperatorGroup grp;
         for (auto& op_name : group) {
            opp::types::ChainAddress addr;
            addr.kind = opp::types::CHAIN_KIND_WIRE;
            auto name_str = op_name.to_string();
            addr.address.assign(name_str.begin(), name_str.end());
            grp.operators.push_back(std::move(addr));
         }
         attest.groups.push_back(std::move(grp));
      }

      std::vector<char> encoded;
      auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
      (void)out(attest);

      sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
      for (auto it = chains_tbl.begin(); it != chains_tbl.end(); ++it) {
         if (!is_active_outpost(*it)) continue;
         action(
            permission_level{get_self(), "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->code.value,
               opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS,
               encoded
            )
         ).send();
      }
   }

   // Build outbound envelopes for each outpost
   {
      sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
      for (auto it = chains_tbl.begin(); it != chains_tbl.end(); ++it) {
         if (!is_active_outpost(*it)) continue;
         action(
            permission_level{get_self(), "owner"_n},
            MSGCH_ACCOUNT,
            "buildenv"_n,
            std::make_tuple(it->code.value)
         ).send();
      }
   }

   // Sweep underwriter locks whose `expires_at_epoch` is now in the past.
   // The sweep walks `byexpire` ascending and stops at the first row that
   // hasn't aged out yet, so the per-advance cost is O(expiring locks),
   // not table size. An empty result is the steady-state case.
   action(
      permission_level{"sysio.epoch"_n, "owner"_n},
      "sysio.uwrit"_n,
      "chklocks"_n,
      std::make_tuple(state.current_epoch_index)
   ).send();

   // Emissions side. Two inline actions queued in FIFO order:
   //   1. accrueepoch: always queued. Records this epoch's per-epoch share
   //      onto t5state (pending_emission_amount + batch_group_epochs[group]
   //      + last_epoch_emission for decay continuity).
   //   2. payepoch: queued only on pay-epochs. Reads the now-updated t5state
   //      (which already includes this epoch's contribution from step 1),
   //      distributes period_emission, and resets the accumulator.
   // Both run after advance() returns; their FIFO ordering guarantees
   // payepoch sees the post-accrue state.
   action(
      permission_level{get_self(), "owner"_n},
      SYSTEM_ACCOUNT,
      "accrueepoch"_n,
      std::make_tuple(
         state.current_epoch_index,
         state.current_batch_op_group,
         gate.emission_amount
      )
   ).send();

   if (gate.is_pay_epoch) {
      action(
         permission_level{get_self(), "owner"_n},
         SYSTEM_ACCOUNT,
         "payepoch"_n,
         std::make_tuple(
            state.current_epoch_index,
            state.batch_op_groups,
            gate.period_emission
         )
      ).send();
   }

   // Working tables on `sysio.msgch` (`envelopes` / `messages` /
   // `attestations` / `outenvelopes`) are now drained inline by the
   // `evalcons` consensus-reach + `buildenv` write paths. The durable
   // audit trail lives in the `envelope_log` table on the same contract,
   // capped at `active_outposts * 2 * cfg.epoch_retention_envelope_log_count`
   // and pruned head-first on overflow. No scheduled cleanup needed.
}

// ---------------------------------------------------------------------------
//  schbatchgps — initial fill of the N-group sliding window
//
//  Called ONCE at bootstrap. Reads ACTIVE batch operators from sysio.opreg,
//  sorts non-bootstrapped first (progressive-takeover preference per
//  .claude/rules/batch-operator-schedule-preference.md), and partitions
//  them into N groups (`cfg.batch_op_groups`). The resulting window is
//  [epoch_1_group, epoch_2_group, ..., epoch_N_group].
//
//  After this, every per-epoch `advance` pops the front group and pushes
//  a new tail group, where the tail's members are drawn from the ACTIVE
//  pool MINUS anyone still resident in the N-1 surviving groups. The
//  window itself encodes "scheduled in the last N-1 epochs"; no separate
//  history table is needed.
// ---------------------------------------------------------------------------
void epoch::schbatchgps() {
   require_auth(get_self());

   epochcfg_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   // Collect ACTIVE batch operators (non-bootstrapped first, then bootstrapped).
   opreg::operators_t opreg_ops(OPREG_ACCOUNT);
   auto status_idx = opreg_ops.get_index<"bystatus"_n>();
   std::vector<std::pair<name, bool>> available_batch; // (account, is_bootstrapped)
   for (auto it = status_idx.lower_bound(
           static_cast<uint64_t>(OperatorStatus::OPERATOR_STATUS_ACTIVE));
        it != status_idx.end() &&
        it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE; ++it) {
      if (it->type == OperatorType::OPERATOR_TYPE_BATCH) {
         available_batch.push_back({it->account, it->is_bootstrapped});
      }
   }
   check(available_batch.size() >= cfg.batch_operator_minimum_active,
         "not enough available batch operators for group assignment");
   std::sort(available_batch.begin(), available_batch.end(),
      [](const auto& a, const auto& b) {
         if (a.second != b.second) return !a.second; // non-bootstrapped first
         return a.first < b.first;
      });
   available_batch.resize(cfg.batch_operator_minimum_active);

   // Even/odd interleave to spread non-bootstrapped + bootstrapped across groups.
   std::vector<name> shuffled;
   shuffled.reserve(available_batch.size());
   for (size_t i = 0; i < available_batch.size(); i += 2)
      shuffled.push_back(available_batch[i].first);
   for (size_t i = 1; i < available_batch.size(); i += 2)
      shuffled.push_back(available_batch[i].first);

   // Partition into N groups of `operators_per_epoch` members.
   std::vector<std::vector<name>> new_groups;
   new_groups.reserve(cfg.batch_op_groups);
   for (uint32_t g = 0; g < cfg.batch_op_groups; ++g) {
      std::vector<name> group;
      group.reserve(cfg.operators_per_epoch);
      uint32_t start = g * cfg.operators_per_epoch;
      for (uint32_t i = 0; i < cfg.operators_per_epoch && (start + i) < shuffled.size(); ++i) {
         group.push_back(shuffled[start + i]);
      }
      new_groups.push_back(std::move(group));
   }

   // Store the window; advance picks up from here.
   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});
   state.batch_op_groups = new_groups;
   state.current_batch_op_group = 0; // front-of-window is always current
   state_tbl.set(state, ram_payer);
}

// ---------------------------------------------------------------------------
//  pause / unpause
// ---------------------------------------------------------------------------
void epoch::pause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});
   state.is_paused = true;
   state_tbl.set(state, ram_payer);
}

void epoch::unpause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self());
   check(state_tbl.exists(), "epoch state not initialized");
   auto state = state_tbl.get();
   state.is_paused = false;
   state_tbl.set(state, ram_payer);
}

} // namespace sysio
