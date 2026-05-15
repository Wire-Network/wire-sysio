#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::OperatorType;
using opp::types::AttestationType;
using opp::types::OperatorStatus;

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void epoch::setconfig(uint32_t epoch_duration_sec,
                      uint32_t operators_per_epoch,
                      uint32_t batch_operator_minimum_active,
                      uint32_t batch_op_groups,
                      uint32_t epoch_retention_envelope_log_count) {
   require_auth(get_self());

   check(epoch_duration_sec > 0, "epoch_duration_sec must be positive");
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
   cfg_tbl.set(cfg, get_self());
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

   // Before incrementing: evaluate per-op delivery state for the EXPIRING
   // epoch. The active group of the expiring epoch (`current_batch_op_group`
   // BEFORE the increment) is the set of ops responsible for delivering
   // every registered outpost's inbound envelope for `current_epoch_index`.
   //
   // For each (outpost × member of the expiring group):
   //   - scan `msgch::envelopes` (`byoutepoch` index) for any row matching
   //     (outpost_id, current_epoch_index, batch_op_name == member)
   //   - inline `opreg::recorddel(member, current_epoch_index, did_deliver)`
   //   - inline `opreg::termcheck(member)` — the threshold + window come
   //     from `op_config`, so tests dial the thresholds via setconfig
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

      outposts_t outposts_tbl(get_self());
      for (auto op_it = outposts_tbl.begin(); op_it != outposts_tbl.end(); ++op_it) {
         const uint64_t composite =
            (op_it->id << 32) | state.current_epoch_index;

         // Walk the (outpost, epoch) bucket and collect distinct delivering
         // batch ops. Vector linear-scan is fine — group size is small
         // (single-digit ops/group in every practical config).
         std::vector<name> delivered;
         for (auto e = oe_idx.lower_bound(composite);
              e != oe_idx.end() && e->by_outpost_epoch() == composite; ++e) {
            bool already = false;
            for (const auto& d : delivered) {
               if (d == e->batch_op_name) { already = true; break; }
            }
            if (!already) delivered.push_back(e->batch_op_name);
         }

         for (const auto& member : expiring_group) {
            bool did_deliver = false;
            for (const auto& d : delivered) {
               if (d == member) { did_deliver = true; break; }
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
         }
      }

      // NOTE: we intentionally do NOT erase the per-batch-op envelope
      // metadata rows here. `evalcons` already cleared their heavy
      // `raw_data` (1-2 KB → 0 bytes) at consensus reach, so the residual
      // weight is just the tuple `(id, outpost_id, epoch_index,
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
      check(!new_tail.empty(),
            "no eligible batch operators for the new tail group");

      state.batch_op_groups.push_back(std::move(new_tail));
   }

   state.current_batch_op_group = 0;

   // Note: last_elected_epoch tracking is epoch-internal state.
   // No operator table writes needed — group membership is in epoch_state.batch_op_groups.

   state_tbl.set(state, get_self());

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

      outposts_t outposts_tbl(get_self());
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->id,
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

      outposts_t outposts_tbl(get_self());
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->id,
               opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS,
               encoded
            )
         ).send();
      }
   }

   // Build outbound envelopes for each outpost
   {
      outposts_t outposts_tbl(get_self());
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "buildenv"_n,
            std::make_tuple(it->id)
         ).send();
      }
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
   state_tbl.set(state, get_self());
}

// ---------------------------------------------------------------------------
//  regoutpost
// ---------------------------------------------------------------------------
void epoch::regoutpost(opp::types::ChainKind chain_kind, uint32_t chain_id) {
   require_auth(get_self());

   outposts_t outposts(get_self());

   auto chain_idx = outposts.get_index<"bychain"_n>();
   uint64_t composite = (static_cast<uint64_t>(chain_kind) << 32) | chain_id;
   auto it = chain_idx.find(composite);
   check(it == chain_idx.end(), "outpost already registered");

   uint64_t next_id = outposts.available_primary_key();

   outposts.emplace(get_self(), outpost_key{next_id}, outpost_info{
      .id                  = next_id,
      .chain_kind          = chain_kind,
      .chain_id            = chain_id,
      .last_inbound_epoch  = 0,
      .last_outbound_epoch = 0,
   });
}

// ---------------------------------------------------------------------------
//  pause / unpause
// ---------------------------------------------------------------------------
void epoch::pause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});
   state.is_paused = true;
   state_tbl.set(state, get_self());
}

void epoch::unpause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self());
   check(state_tbl.exists(), "epoch state not initialized");
   auto state = state_tbl.get();
   state.is_paused = false;
   state_tbl.set(state, get_self());
}

} // namespace sysio
