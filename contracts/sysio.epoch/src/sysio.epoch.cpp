#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.opp.common/opp_keys.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.token/sysio.token.hpp>
// Canonical sysio.system emissions types + compute_epoch_emission. The
// [[sysio::contract("sysio.system")]] attribute on emission_config / t5_state
// pins them to sysio.system's ABI; no readonly mirror needed here.
#include <sysio.system/emissions.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <algorithm>

namespace sysio {

using opp::types::OperatorType;
using opp::types::AttestationType;
using opp::types::OperatorStatus;
using opp::types::EmissionsBlockReason;

// ---------------------------------------------------------------------------
// Emissions readiness gate. Runs in tickeconomy() before any state mutation:
// reads sysio.system::emitcfg + t5state and sysio.token::accounts to decide
// what this economic epoch's emissions may do. A blocked gate holds emissions
// ONLY -- the economic clock still ticks (withdrawals mature) and consensus
// never waits on the treasury:
//   - no config / no T5 state / exhausted treasury: nothing accrues;
//   - a pay epoch whose period total exceeds the spendable balance: the epoch
//     still accrues, and the payout waits for the first economic epoch whose
//     balance covers it.
// Each blocked economic epoch refreshes the one open blocklog row (+ a greppable
// console diagnostic). The gate state is not broadcast cross-chain: no outpost
// acts on it. Missing-config reasons hard-fail the production bootstrap entry
// (sysio.msgch::bootstrap), so on a properly-bootstrapped chain only the
// economic reasons are reachable.
// ---------------------------------------------------------------------------

namespace {

constexpr name SYSTEM_ACCOUNT     = "sysio"_n;
constexpr name TOKEN_ACCOUNT      = "sysio.token"_n;
constexpr name FINISH_ADVANCE     = "finishadv"_n;

/// Action identifiers owned by sysio.system and invoked by the economic and consensus clocks.
namespace system_actions {
constexpr name ACCRUE_EPOCH  = "accrueepoch"_n;
constexpr name PAY_EPOCH     = "payepoch"_n;
constexpr name SETTLE_DUTY   = "settleduty"_n;
constexpr name RECORD_DUTY   = "rcrdduty"_n;
} // namespace system_actions

/// Action identifiers owned by sysio.msgch and invoked by epoch close.
namespace msgch_actions {
constexpr name QUEUE_OUT      = "queueout"_n;
constexpr name BUILD_ENVELOPE = "buildenv"_n;
} // namespace msgch_actions

/// Action identifiers owned by sysio.opreg and invoked by the consensus and economic clocks.
namespace opreg_actions {
constexpr name RECORD_DELIVERY   = "recorddel"_n;
constexpr name TERMINATION_CHECK = "termcheck"_n;
constexpr name FLUSH_WITHDRAWALS = "flushwtdw"_n;
} // namespace opreg_actions

/// Messages of the `check`s `advance` raises.
constexpr std::string_view advance_auth_msg = "only sysio.msgch or sysio.epoch may advance a chain's epoch";
constexpr std::string_view genesis_not_active_outpost_msg =
   "a chain's genesis advance requires the chain to be an active outpost";
constexpr std::string_view genesis_without_schedule_msg =
   "a chain's genesis advance requires its schedule window: run schbatchgps for the chain first";
constexpr std::string_view governance_advance_undecided_msg =
   "governance cannot advance an active outpost whose current epoch has not reached consensus";

// System-owned rows are billed to the sysio RAM pool rather than to this contract account (the
// privileged-contract model sysio.token uses): the contract account stays finite at its code+abi
// size while table growth draws from sysio's pool. Permitted because the contract is privileged.
constexpr name ram_payer          = "sysio"_n;
constexpr symbol WIRE_SYMBOL{"WIRE", 9};

/// What the readiness gate decided for one economic epoch.
struct emissions_gate_result {
   bool                  accrue             = false; ///< accrueepoch runs for this economic epoch
   bool                  pay                = false; ///< payepoch runs for this economic epoch
   bool                  is_pay_epoch       = false; ///< the open pay period has reached its cadence
   /// A pay epoch ticked while a later boundary has already passed: the payout waits for the tick that catches up.
   /// Not a block: the blocklog is left as it is.
   bool                  deferred           = false;
   /// This epoch's share, sized on the net headroom. May be 0 when accruing: the open pay period has emission
   /// pending, and either it has committed the rest of the budget or the curve is zero.
   int64_t               emission_amount    = 0;
   int64_t               period_emission    = 0;  // pending + emission_amount; populated only on a pay epoch
   /// Net headroom (budget not yet paid out, accrued or settling), for the blocklog row.
   int64_t               treasury_remaining = 0;
   int64_t               sysio_balance      = 0;  // spendable balance, for the blocklog row
   EmissionsBlockReason  reason             = opp::types::EMISSIONS_BLOCK_REASON_UNSPECIFIED;

   /// True when the gate held anything back this economic epoch.
   bool blocked() const { return reason != opp::types::EMISSIONS_BLOCK_REASON_UNSPECIFIED; }

   /// What the gate tried to authorize, for the blocklog row: the period total on a pay epoch, otherwise this
   /// epoch's share.
   int64_t attempted_emission() const { return is_pay_epoch ? period_emission : emission_amount; }
};

/// Decide what economic epoch `target_epoch`'s emissions may do. `spend` is whether the clock had caught up to the grid
/// before this tick, which decides whether its payout spends; `defer_payout` is `spend` with a later economic boundary
/// already passed. Never throws.
///
/// `closed_epoch_duration_sec` is the length of the economic epoch the tick closes, the one whose share it accrues
/// (`epoch::closing_epoch_duration_sec`); tickeconomy passes the same value on to sysio.system::accrueepoch.
emissions_gate_result check_emissions_ready(uint32_t closed_epoch_duration_sec, uint32_t target_epoch,
                                            bool defer_payout, bool spend) {
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
   r.treasury_remaining = sysiosystem::emissions::net_headroom(cfg, t5s);

   // Capped at the net headroom, so the open pay period can never commit more than the budget.
   r.emission_amount = sysiosystem::emissions::compute_epoch_emission(cfg, closed_epoch_duration_sec, t5s);

   // TREASURY_EXHAUSTED is decided on the GROSS headroom: a spent budget accrues nothing. An open period whose share
   // drops to zero, because it has committed the rest of the budget or the curve is zero, still accrues (a zero
   // share) and pays what it accrued at its boundary. A zero share with nothing pending accrues nothing: there is
   // nothing to pay, and payepoch asserts on an empty period.
   if (sysiosystem::emissions::gross_headroom(cfg, t5s) <= 0 ||
       (r.emission_amount <= 0 && t5s.pending_emission_amount <= 0)) {
      r.reason = opp::types::EMISSIONS_BLOCK_REASON_TREASURY_EXHAUSTED;
      return r;
   }
   r.accrue = true;

   // Pay fires once the target epoch is `pay_cadence_epochs - 1` past `period_start_epoch`. Genesis case:
   // t5s.period_start_epoch = 0, so the first period covers economic epochs 1..(pay_cadence - 1) (one short,
   // since 0 is the clock's pre-start index). A payout the balance could not cover stays open past its cadence
   // and fires at the first economic epoch that can pay it.
   r.is_pay_epoch = (target_epoch >= t5s.period_start_epoch + cfg.pay_cadence_epochs - 1);
   if (!r.is_pay_epoch) return r;

   // While the crank catches up, the payout waits for the tick that reaches the grid: one payout then pays the pools of
   // every overdue period against all the blocks and duty counted since the last payout, at one rate.
   if (defer_payout) {
      r.deferred = true;
      return r;
   }

   // Same saturating accumulation as sysio.system::accrueepoch (shared helper in emissions.hpp) so the gate's
   // period total exactly matches what accrueepoch will store and payepoch will assert against.
   r.period_emission = sysiosystem::emissions::saturating_accrue(t5s.pending_emission_amount, r.emission_amount);

   // A payout that does not spend (sysio.system::payepoch) is never held for balance.
   if (!spend) {
      r.pay = true;
      return r;
   }

   sysio::token::token::accounts acct_tbl(TOKEN_ACCOUNT, SYSTEM_ACCOUNT.value);
   sysio::token::token::acct_key key{WIRE_SYMBOL.code().raw()};
   if (acct_tbl.contains(key)) {
      r.sysio_balance = acct_tbl.get(key).balance.amount;
   }

   // Reserve what is already owed but still sits in sysio's balance: pay credited by payepoch and not yet pulled,
   // and batch emission of a paid period not yet credited to its operators. Counting either as available would let
   // the gate authorize a period the treasury cannot cover, and a later claimpay would fail on overdraw.
   // `sysio_balance` is reported to the blocklog as this SPENDABLE amount, the one a shortfall is measured against.
   sysiosystem::emissions::payclaimtot_t claim_tot_tbl(SYSTEM_ACCOUNT);
   r.sysio_balance = sysiosystem::emissions::spendable_wire(
      r.sysio_balance, claim_tot_tbl.get_or_default(sysiosystem::emissions::pay_claim_total{}), t5s);

   if (r.sysio_balance < r.period_emission) {
      r.reason = opp::types::EMISSIONS_BLOCK_REASON_BALANCE_INSUFFICIENT;
      return r;
   }

   r.pay = true;
   return r;
}

// Records a gate block on economic epoch `epoch_index` in the blocklog: the first blocked epoch of an
// episode inserts the row (keyed by that epoch); every further blocked epoch refreshes it. The
// table holds at most the one open episode, so it cannot grow however long emissions stay blocked.
void record_gate_block(name self, uint32_t epoch_index, const emissions_gate_result& gate) {
   epoch::blocklog_t log_tbl(self);
   const uint32_t now_secs = static_cast<uint32_t>(current_time_point().sec_since_epoch());

   auto open = log_tbl.begin();
   if (open == log_tbl.end()) {
      log_tbl.emplace(ram_payer, epoch::blocklog_key{epoch_index}, epoch::blocklog_entry{
         .epoch_index        = epoch_index,
         .last_epoch_index   = epoch_index,
         .reason             = gate.reason,
         .attempted_emission = gate.attempted_emission(),
         .treasury_remaining = gate.treasury_remaining,
         .sysio_balance      = gate.sysio_balance,
         .first_blocked_at   = now_secs,
         .last_blocked_at    = now_secs,
         .blocked_epochs     = 1,
      });
      return;
   }

   log_tbl.modify(ram_payer, epoch::blocklog_key{open->epoch_index}, [&](auto& row) {
      row.last_epoch_index   = epoch_index;
      row.reason             = gate.reason;
      row.attempted_emission = gate.attempted_emission();
      row.treasury_remaining = gate.treasury_remaining;
      row.sysio_balance      = gate.sysio_balance;
      row.last_blocked_at    = now_secs;
      row.blocked_epochs    += 1;
      // epoch_index / first_blocked_at intentionally unchanged: they date the episode.
   });
}

// Closes the open block episode, if any. Called when an economic epoch's emissions pass the gate.
void clear_gate_block(name self) {
   epoch::blocklog_t log_tbl(self);
   auto open = log_tbl.begin();
   if (open != log_tbl.end()) {
      log_tbl.erase(open);
   }
}

/// One batch operator a chain's schedule may seat.
struct pool_member {
   name account;
   bool is_bootstrapped = false;
};

/// The batch operators outpost `chain_code`'s schedule may seat, leaving out `excluded`: every ACTIVE
/// batch operator in `sysio.opreg` whose membership of the chain is SERVING, non-bootstrapped first,
/// then by account. The ONE pool `schbatchgps` and `finishadv` draw seats from. The walk visits only
/// the ACTIVE operators (`bystatus`): a member without bonded collateral adds nothing to it.
std::vector<pool_member> schedule_pool(sysio::slug_name chain_code, const std::vector<name>& excluded) {
   opreg::operators_t ops(epoch::OPREG_ACCOUNT);
   auto               by_status = ops.get_index<"bystatus"_n>();
   std::vector<pool_member> pool;
   for (auto it = by_status.lower_bound(magic_enum::enum_integer(OperatorStatus::OPERATOR_STATUS_ACTIVE));
        it != by_status.end() && it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE; ++it) {
      if (it->type != OperatorType::OPERATOR_TYPE_BATCH) continue;
      if (std::find(excluded.begin(), excluded.end(), it->account) != excluded.end()) continue;
      const std::optional<opreg::served_chain> membership = opreg::membership_of(it->account, chain_code);
      if (!membership || membership->status != opreg::chain_service_status::SERVING) continue;
      pool.push_back({.account = it->account, .is_bootstrapped = it->is_bootstrapped});
   }
   std::sort(pool.begin(), pool.end(), [](const pool_member& a, const pool_member& b) {
      if (a.is_bootstrapped != b.is_bootstrapped) return !a.is_bootstrapped;
      return a.account < b.account;
   });
   return pool;
}

/// Recompute `state.due_at` from its pending traffic: the later of `current_epoch_start` plus the chain's minimum
/// duration `cfg.epoch_duration_sec` and `traffic_since` plus its `cfg.traffic_wait_sec`. Zero while the chain has no
/// traffic.
void recompute_due(epoch::epoch_state& state, const epoch::epoch_config& cfg) {
   state.due_at = state.traffic_since == time_point{}
                     ? time_point{}
                     : std::max(state.current_epoch_start + seconds(static_cast<int64_t>(cfg.epoch_duration_sec)),
                                state.traffic_since + seconds(static_cast<int64_t>(cfg.traffic_wait_sec)));
}

/// Record traffic that began at `since` on a chain past its genesis: `traffic_since` keeps the earlier of its value
/// and `since`, and `due_at` follows. A `since` that is zero, which would read as no traffic, or later than `now`
/// counts as `now`. Returns whether the state changed.
bool record_traffic(epoch::epoch_state& state, const epoch::epoch_config& cfg, time_point since, time_point now) {
   const time_point noted = since == time_point{} || since > now ? now : since;
   if (state.traffic_since != time_point{} && state.traffic_since <= noted) return false;
   state.traffic_since = noted;
   recompute_due(state, cfg);
   return true;
}

/// Record traffic that began at `since` on outpost `chain_code`, through `record_traffic`. No effect before the
/// chain's genesis advance, which ships epoch 1 with no traffic. Never throws: `buildenv` reaches it inside an
/// advance.
void note_chain_traffic(name self, sysio::slug_name chain_code, time_point since) {
   const epoch::chain_key            key{chain_code.value};
   epoch::epochstate_t               state_tbl(self);
   std::optional<epoch::epoch_state> state = state_tbl.try_get(key);
   if (!state || state->current_epoch_index == 0) return;
   const std::optional<epoch::epoch_config> cfg = epoch::epochcfg_t(self).try_get(key);
   if (!cfg) return;
   if (record_traffic(*state, *cfg, since, current_time_point())) state_tbl.set(ram_payer, key, *state);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void epoch::setconfig(sysio::slug_name chain_code,
                      uint32_t epoch_duration_sec,
                      uint32_t traffic_wait_sec,
                      uint32_t late_window_sec,
                      uint32_t operators_per_epoch,
                      uint32_t batch_operator_minimum_active,
                      uint32_t batch_op_groups,
                      uint32_t epoch_retention_envelope_log_count) {
   require_auth(get_self());

   check(epoch_duration_sec >= MIN_EPOCH_DURATION_SEC,
         "epoch_duration_sec must be >= MIN_EPOCH_DURATION_SEC");
   check(epoch_duration_sec <= MAX_EPOCH_DURATION_SEC,
         "epoch_duration_sec exceeds 30-day ceiling");
   const uint32_t traffic_wait =
      traffic_wait_sec == 0 ? default_share_of_minimum(epoch_duration_sec) : traffic_wait_sec;
   check(traffic_wait <= epoch_duration_sec, "traffic_wait_sec must be <= epoch_duration_sec");
   const uint32_t late_window =
      late_window_sec == 0 ? default_share_of_minimum(epoch_duration_sec) : late_window_sec;
   check(late_window <= epoch_duration_sec, "late_window_sec must be <= epoch_duration_sec");
   check(operators_per_epoch > 0, "operators_per_epoch must be positive");
   check(batch_op_groups > 0, "batch_op_groups must be positive");
   check(batch_op_groups <= MAX_BATCH_OP_GROUPS,
         "batch_op_groups exceeds the outpost group ceiling (8)");
   // Widen the product to uint64 so a large operators_per_epoch cannot wrap the
   // uint32 multiply into a small value that spuriously matches a small
   // batch_operator_minimum_active; an honest product beyond uint32 simply fails
   // the equality against the uint32 field, which is the intended rejection.
   check(static_cast<uint64_t>(operators_per_epoch) * batch_op_groups == batch_operator_minimum_active,
         "batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups");
   // Magnitude bound, checked after the relationship above so an inconsistent
   // triple reports the equality error. The equality alone cannot reject an
   // internally consistent but absurd size (UINT32_MAX operators in one group),
   // which would later abort advance() on its schedule-window vector reserves and
   // halt epoch advancement chain-wide. With both factors bounded, the window
   // cannot exceed MAX_SCHEDULED_BATCH_OPERATORS.
   check(operators_per_epoch <= MAX_OPERATORS_PER_EPOCH,
         "operators_per_epoch exceeds the outpost group-size ceiling (7)");
   check(epoch_retention_envelope_log_count > 0,
         "epoch_retention_envelope_log_count must be positive");

   // The materialized rotation schedule (epoch_state.batch_op_groups) is
   // sized from batch_op_groups once, at schbatchgps; advance() thereafter
   // preserves its length (pop-front / push-back) and never re-reads the
   // config to resize it. advance()'s scheduling horizon
   // (current_epoch_index + batch_op_groups - 1) assumes cfg.batch_op_groups
   // equals the live rotation length, so once a schedule exists the group
   // count is fixed. Reject a change rather than let the config drift from
   // the live rotation (which would mis-date newly scheduled groups).
   // Reshaping the rotation is a fresh schbatchgps, a bootstrap-time
   // operation.
   std::optional<epoch_state> state = epoch_state_of(chain_code.value);
   if (state && !state->batch_op_groups.empty()) {
      check(batch_op_groups == state->batch_op_groups.size(),
            "batch_op_groups cannot change once the rotation schedule is materialized");
   }

   const epoch_config cfg{
      .chain_code                         = chain_code.value,
      .epoch_duration_sec                 = epoch_duration_sec,
      .traffic_wait_sec                   = traffic_wait,
      .late_window_sec                    = late_window,
      .operators_per_epoch                = operators_per_epoch,
      .batch_operator_minimum_active      = batch_operator_minimum_active,
      .batch_op_groups                    = batch_op_groups,
      .epoch_retention_envelope_log_count = epoch_retention_envelope_log_count,
   };
   epochcfg_t cfg_tbl(get_self());
   cfg_tbl.set(ram_payer, chain_key{chain_code.value}, cfg);

   // Pending traffic is due again under the new minimum and wait.
   if (state && state->traffic_since != time_point{}) {
      recompute_due(*state, cfg);
      epochstate_t(get_self()).set(ram_payer, chain_key{chain_code.value}, *state);
   }
}

// ---------------------------------------------------------------------------
//  advance
// ---------------------------------------------------------------------------
void epoch::advance(sysio::slug_name chain_code) {
   const chain_key key{chain_code.value};
   epochcfg_t cfg_tbl(get_self());
   const std::optional<epoch_config> cfg = cfg_tbl.try_get(key);
   check(cfg.has_value(), EPOCH_CONFIG_NOT_INITIALIZED);

   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.try_get(key).value_or(epoch_state{.chain_code = key.chain_code});

   check(!state.is_paused, "epoch advancement is paused");

   // sysio.msgch advances a chain: its genesis from `bootstrap` / `bootchain`, every later epoch from
   // `chkcons`. sysio.epoch's own authority is the governance path.
   check(has_auth(MSGCH_ACCOUNT) || has_auth(get_self()), advance_auth_msg);

   // A chain starts only as an active outpost with its schedule window seated: the genesis advance
   // opens epoch 1 with the window's front group on duty.
   const bool active_outpost = sysio::chains::outpost_kind_of(CHAINS_ACCOUNT, chain_code).has_value();
   if (state.current_epoch_index == 0) {
      check(active_outpost, genesis_not_active_outpost_msg);
      check(!state.batch_op_groups.empty(), genesis_without_schedule_msg);
   }

   // Governance never closes an active outpost's undecided epoch: the outpost's next envelope chains to that epoch's
   // winner, which the depot never accepted, so the chain's inbound stream would break. sysio.msgch advances only a
   // decided epoch. An undecided epoch is decided or disputed instead (README, Recovery).
   if (state.current_epoch_index > 0 && active_outpost && !has_auth(MSGCH_ACCOUNT)) {
      check(msgch::consensus_for_epoch(MSGCH_ACCOUNT, chain_code.value, state.current_epoch_index).has_value(),
            governance_advance_undecided_msg);
   }

   // After its genesis a chain advances only once it is due, under either authority: an idle chain never moves, and
   // governance recovers one with `resendroster`, which records traffic.
   const time_point now = current_time_point();
   if (state.current_epoch_index > 0 && !advance_due(state, now)) return;

   // Close the books of the epoch before the closing one, `current_epoch_index - 1`, so the late
   // deliveries `sysio.msgch::deliver` accepted for it during the closing epoch count. Each member of
   // `previous_group`, the group that served it, is recorded delivered when `msgch::envelopes` holds its
   // row for (chain, current_epoch_index - 1), whatever the row's checksum: a delivery that differs from
   // the epoch's winner was slashed when `sysio.msgch` accepted that winner.
   //
   // `finishadv` keeps `previous_group` only for an epoch the chain closed as an active outpost
   // (`sysio.chains::chains`, active && !is_depot), so its books are recorded here even when the chain is
   // no longer active. `previous_group` is empty on the genesis advance and on the advance from epoch 1,
   // so neither records anything.
   if (state.current_epoch_index > 1 && !state.previous_group.empty()) {
      const uint32_t previous_epoch = state.current_epoch_index - 1;

      // Walk the (outpost, epoch) bucket and collect each delivering batch op. `deliver` admits one row
      // per operator, each from a member of the group that served the epoch, so the walk stays within it.
      msgch::envelopes_t envs(MSGCH_ACCOUNT);
      auto oe_idx = envs.get_index<"byoutepoch"_n>();
      const uint128_t composite = opp::outpost_epoch_key(key.chain_code, previous_epoch);
      std::vector<name> delivered;
      for (auto e = oe_idx.lower_bound(composite);
           e != oe_idx.end() && e->by_outpost_epoch() == composite; ++e) {
         delivered.push_back(e->batch_op_name);
      }

      // Preserve this chain's delivery history and its eviction accounting. `recorddel` records every
      // member, a slashed or evicted one included, while `termcheck` skips SLASHED and TERMINATED
      // operators and EVICTED members. All mutations complete before finishadv reads the registry for
      // outbound attestations.
      for (const auto& member : state.previous_group) {
         const bool did_deliver = std::find(delivered.begin(), delivered.end(), member) != delivered.end();
         action(
            permission_level{get_self(), "owner"_n},
            OPREG_ACCOUNT,
            opreg_actions::RECORD_DELIVERY,
            std::make_tuple(member, chain_code, previous_epoch, did_deliver)
         ).send();
         action(
            permission_level{get_self(), "owner"_n},
            OPREG_ACCOUNT,
            opreg_actions::TERMINATION_CHECK,
            std::make_tuple(member, chain_code)
         ).send();
      }

      // Each member earns the chain's minimum duration as configured now, when the epoch's books close, in
      // seat-seconds on this chain, however long the epoch ran: duty pays epochs served, so an idle or stretched
      // epoch earns nothing more. sysio.system splits
      // each economic pay period's one batch pool by seat-seconds over every chain; whether a member is
      // still ACTIVE is decided when it is paid, so a slashed member draws nothing, and a member the
      // termchecks above evicted from this chain is credited nothing here. Skipped until initt5 has run:
      // nothing can be paid before then, and consensus must not depend on sysio.system.
      if (sysiosystem::emissions::records_batch_duty(SYSTEM_ACCOUNT)) {
         action(
            permission_level{get_self(), "owner"_n},
            SYSTEM_ACCOUNT,
            system_actions::RECORD_DUTY,
            std::make_tuple(chain_code, previous_epoch, state.previous_group, cfg->epoch_duration_sec)
         ).send();
      }

      // The envelope rows read above are left in place: `sysio.msgch::deliver`
      // erases them once they fall out of its retention window.
   }

   // The first chain's genesis advance is the network genesis.
   if (state.current_epoch_index == 0) {
      netgenesis_t genesis_tbl(get_self());
      if (!genesis_tbl.exists()) genesis_tbl.set(network_genesis{.genesis_at = now}, ram_payer);
   }

   // The next epoch opens now, with no traffic: `buildenv` marks any outbound rows this advance's envelope leaves
   // behind.
   state.current_epoch_index++;
   state.current_epoch_start = now;
   state.traffic_since       = time_point{};
   state.due_at              = time_point{};
   state_tbl.set(ram_payer, key, state);

   action(
      permission_level{get_self(), "owner"_n},
      get_self(),
      FINISH_ADVANCE,
      std::make_tuple(chain_code, state.current_epoch_index)
   ).send();
}

// ---------------------------------------------------------------------------
//  notetraffic
// ---------------------------------------------------------------------------
void epoch::notetraffic(sysio::slug_name chain_code, time_point since) {
   require_auth(MSGCH_ACCOUNT);
   note_chain_traffic(get_self(), chain_code, since);
}

void epoch::onqueueout(uint64_t chain_code, AttestationType attest_type, [[maybe_unused]] std::vector<char> data) {
   // Only a general-lane row is traffic: the schedule lane rides every advance's own envelope.
   if (msgch::lane_of(attest_type) != msgch::queue_lane::general) return;
   note_chain_traffic(get_self(), sysio::slug_name{chain_code}, current_time_point());
}

void epoch::finishadv(sysio::slug_name chain_code, uint32_t epoch_index) {
   require_auth(get_self());
   check(get_sender() == get_self(), "finishadv must be sent inline by sysio.epoch");
   const chain_key key{chain_code.value};
   const epoch_config cfg = epochcfg_t(get_self()).get(key, EPOCH_CONFIG_NOT_INITIALIZED.data());
   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get(key, "epoch state not initialized");
   check(state.current_epoch_index == epoch_index, "finishadv epoch mismatch");
   const bool had_expiring_group = epoch_index > 1;
   // Outbound attestations go to this chain only, and only while it is an active outpost.
   const bool active_outpost = sysio::chains::outpost_kind_of(CHAINS_ACCOUNT, chain_code).has_value();

   // The front group just served epoch `epoch_index - 1`. Keep it, before the slide below erases it, for the
   // next `advance`, which closes that epoch's books. Only an active outpost owes deliveries, so an epoch
   // closed while the chain is not one keeps no books. The genesis advance leaves it empty.
   state.previous_group = had_expiring_group && active_outpost && !state.batch_op_groups.empty()
                             ? state.batch_op_groups.front()
                             : std::vector<name>{};

   // ── Slide the schedule window ───────────────────────────────────────────
   // Skip on the genesis advance (0 → 1): schbatchgps just placed
   // [G1, G2, G3] for epochs 1, 2, 3 and G1 is now the current (front)
   // group — popping here would lose it. From the SECOND advance onward
   // (1 → 2, 2 → 3, ...), the front group has just expired so we pop
   // it and compute a new tail.
   //
   // Eligibility for the new tail: ACTIVE batch ops SERVING this chain, sorted
   // non-bootstrapped first (preference rule), MINUS anyone already resident in
   // this chain's N-1 surviving groups. The window itself encodes "scheduled in
   // the last N-1 epochs" — no separate history table.
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

      // The tail is drawn ONLY from operators not already resident in the
      // surviving window groups. That residency exclusion is what makes "at
      // most every Nth epoch" hold, and -- less obviously -- it is what keeps
      // the window's groups DISJOINT, which the Ethereum outpost depends on by
      // construction: `OPPInbound._resolveChunkPosition` scans the groups in
      // order and returns the FIRST one containing the sender, using the
      // sender's index WITHIN that group as its chunk-staging header slot. An
      // operator seated in two groups therefore stages against a group it is
      // not serving in. Do not "fill" a short tail by re-seating a resident.
      const std::vector<pool_member> pool = schedule_pool(chain_code, resident);

      std::vector<name> new_tail;
      new_tail.reserve(cfg.operators_per_epoch);
      for (size_t i = 0; i < pool.size() && new_tail.size() < cfg.operators_per_epoch; ++i) {
         new_tail.push_back(pool[i].account);
      }

      // A tail SHORTER than `operators_per_epoch` means the chain's serving ACTIVE batch-operator
      // roster has fallen below `batch_operator_minimum_active` (the config
      // equality at ::setconfig pins that minimum to
      // `operators_per_epoch * batch_op_groups`, i.e. exactly this window). The
      // depot cannot repair that here: with a pool smaller than the window, N
      // groups that are both FULL and DISJOINT do not exist, and both escapes
      // are unsound -- re-seating a resident breaks the Ethereum disjointness
      // above, while a short group lowers the quorum denominator it defines and
      // makes EVEN group sizes reachable, where Ethereum's `(groupSize + 1) / 2`
      // is an exact half and two competing digests can both tip.
      //
      // So the schedule is left as-is and the DECISION is pushed to the emit
      // site: an empty active group is never published (see the withhold
      // below). Short-but-non-empty is pre-existing behaviour and is not made
      // safe here -- it is reported so the roster can be repaired off-chain.
      if (new_tail.size() < cfg.operators_per_epoch) {
         sysio::print("sysio.epoch::finishadv: only ", new_tail.size(), " of ",
                      cfg.operators_per_epoch,
                      " eligible batch operators for the new tail group of chain ", chain_code.to_string(),
                      " at epoch ", state.current_epoch_index + cfg.batch_op_groups - 1,
                      "; the ACTIVE roster is below batch_operator_minimum_active "
                      "-- operator roster needs attention\n");
      }

      state.batch_op_groups.push_back(std::move(new_tail));
   }

   // Pinned to the FRONT of the sliding window, unconditionally. The window
   // slides (erase-front + push-back), it does not rotate, so the group on duty
   // is always at index 0 -- and the next-group lookahead further down derives
   // `active_group_index` as `cursor + 1`, which is only the NEXT epoch's group
   // because this is 0. A change that gives the cursor any other value must
   // revisit that derivation; it is stated here, at the write, because that is
   // the only place the invariant can actually be violated.
   state.current_batch_op_group = 0;

   // Note: last_elected_epoch tracking is epoch-internal state.
   // No operator table writes needed — group membership is in epoch_state.batch_op_groups.

   // A chain that is not an active outpost builds no envelope below, so its general-lane rows stay queued: they are
   // still its traffic, since the oldest became ready, which the advance cleared and no later `queueout` records.
   if (!active_outpost) {
      if (const std::optional<time_point> oldest_ready = msgch::oldest_ready_general(MSGCH_ACCOUNT, key.chain_code)) {
         record_traffic(state, cfg, *oldest_ready, current_time_point());
      }
   }

   state_tbl.set(ram_payer, key, state);

   // Queue the OPERATORS roster for this outpost: every operator seated in its window, with its current
   // status and authex chain addresses. Outposts read the roster only to map the window's members to
   // the keys they deliver with (Solana also refuses a window that seats an ineligible member), so the
   // window is all they need, and the roster stays within MAX_SCHEDULED_BATCH_OPERATORS however many
   // operators register. A member whose opreg row was pruned is left out.
   // An outpost keeps the last roster it received, so the roster is queued to it only when it
   // differs from the one last queued to it (`rostersent`). Members are listed by account, so a window
   // that only rotates its groups encodes the same roster.
   // IMPORTANT: Must come before BATCH_OPERATOR_GROUPS: Solana applies attestations in order and
   // resolves the groups against the roster it holds. (Ethereum seats the groups after the envelope's
   // last attestation.)
   if (active_outpost) {
      opp::attestations::Operators ops_attest;
      opreg::operators_t opreg_ops(OPREG_ACCOUNT);
      authex::links_t authex_links(AUTHEX_ACCOUNT);
      auto links_by_name = authex_links.get_index<"byname"_n>();

      std::vector<name> members;
      for (const auto& group : state.batch_op_groups) {
         members.insert(members.end(), group.begin(), group.end());
      }
      std::sort(members.begin(), members.end());

      for (const name member : members) {
         const auto op = opreg_ops.try_get(opreg::operator_key{member.value});
         if (!op) continue;

         opp::attestations::OperatorEntry entry;
         entry.account.name = member.to_string();
         entry.type = op->type;
         entry.status = op->status;

         // Collect all authex-linked chain addresses for this operator.
         // Store raw public key bytes from the variant (33 bytes for EM/secp256k1,
         // 32 bytes for ED/Ed25519).
         auto link_it = links_by_name.lower_bound(member.value);
         while (link_it != links_by_name.end() && link_it->username == member) {
            opp::types::ChainAddress chain_addr;
            chain_addr.kind = link_it->chain_kind;

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

      // An empty roster tells an outpost nothing (Solana skips it as an anomaly), so an empty window
      // queues none; the BatchOperatorGroups withhold below reports that state.
      if (!ops_attest.operators.empty()) {
         std::vector<char> encoded;
         auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
         (void)out(ops_attest);
         const checksum256 roster_hash = sha256(encoded.data(), encoded.size());

         rostersent_t sent_tbl(get_self());
         const std::optional<roster_sent> sent = sent_tbl.try_get(key);
         if (!sent || sent->roster_hash != roster_hash) {
            sent_tbl.set(ram_payer, key, roster_sent{.chain_code = key.chain_code, .roster_hash = roster_hash});
            action(
               permission_level{get_self(), "owner"_n},
               MSGCH_ACCOUNT,
               msgch_actions::QUEUE_OUT,
               std::make_tuple(
                  key.chain_code,
                  opp::types::ATTESTATION_TYPE_OPERATORS,
                  encoded
               )
            ).send();
         }
      }
   }

   // Queue the BATCH_OPERATOR_GROUPS attestation for this outpost.
   //
   // Ships ALL groups, and an active index that points ONE EPOCH AHEAD --
   // at the group that will be on duty for `current_epoch_index + 1`, not the
   // one on duty now (SOL-378 / WNS-141).
   //
   // The lookahead is what makes outpost-side admission possible at all. An
   // outpost that scopes `epoch_in` admission to its seated active group must
   // already hold epoch N's duty group BEFORE epoch N's envelope arrives --
   // because that envelope's own deliverer is a member of epoch N's group, and
   // authorising it is what lets the envelope land. Shipping epoch N's roster
   // inside epoch N's envelope is circular: the roster that would authorise the
   // delivery is inside the envelope being refused, so the bridge stalls
   // permanently at the first rotation.
   //
   // Emitting the NEXT epoch's group here breaks that circle without loosening
   // anything on the outpost: envelope N-1 seats epoch N's group, so when
   // envelope N arrives the outpost already knows who is allowed to deliver it.
   //
   // Only the ATTESTATION looks ahead. The depot's own schedule state is
   // untouched -- `current_batch_op_group` still names the group on duty NOW,
   // and `finishadv` still slides the window so the front is the current epoch.
   // Nothing that reads `epoch_state` changes meaning.
   //
   // `epoch_index` stays the epoch this envelope IS for (this chain's index);
   // it identifies the envelope, not the roster, and no outpost reads it.
   if (active_outpost) {
      opp::attestations::BatchOperatorGroups attest;
      // The window SLIDES; it does not rotate. `finishadv` erases the front and
      // pushes a new tail, and every write to the cursor pins it to 0 (here,
      // and `schbatchgps`) -- so the group on duty NEXT is simply the one
      // after the cursor.
      //
      // Deliberately NOT `(cursor + 1) % group_count`. A modulo encodes ring
      // semantics this window does not have: on wrap it yields 0, which names
      // the group whose duty just STARTED. That ships a stale roster with
      // nothing to catch it -- no compile error, no failing test, and the
      // outpost cannot distinguish a stale index from a fresh one.
      //
      // The bound check is also what keeps an EMPTY schedule off a division.
      // `group_count == 0` is reachable here: the slide above is guarded by
      // `!empty()`, but nothing requires a seated schedule before this block,
      // and `% 0` is an `i32.rem_u` trap that would abort `finishadv` and halt
      // epoch advancement chain-wide.
      //
      // Falling back to the cursor covers the single-group case: the same
      // operators serve every epoch, so current IS next.
      //
      // The cursor is pinned to 0 by every write to it, and the fallback is
      // only correct BECAUSE of that -- with a non-zero cursor it would return
      // the group whose duty just started, which is the stale-roster outcome
      // the modulo was rejected for. The invariant is asserted at the WRITE
      // site (`state.current_batch_op_group = 0` earlier in this same call),
      // not here: a check at this point is unreachable-by-construction and so
      // proves nothing -- it can only ever observe the value assigned a few
      // hundred lines above it.
      const uint32_t group_count = static_cast<uint32_t>(state.batch_op_groups.size());
      const uint32_t next_index  = state.current_batch_op_group + 1;
      const uint32_t next_group_index =
         next_index < group_count ? next_index : state.current_batch_op_group;

      // NEVER publish an empty "next". The index names the group the outpost
      // will admit `epoch_in` against and size its quorum from, so an empty
      // one is not a degraded roster -- it is an invalid attestation, and
      // seating it wedges the outpost permanently (the handler that could
      // replace the window runs only past the gate the empty group breaks).
      //
      // This is the ONE sound guarantee available here. The slide cannot buy
      // non-emptiness by backfilling: with an ACTIVE pool smaller than the
      // window, N groups that are both FULL and DISJOINT do not exist, and
      // both escapes are unsound (see the slide's comment -- re-seating a
      // resident breaks Ethereum's chunk-position disjointness; a short group
      // lowers the quorum denominator it defines). So the schedule is left
      // alone and the decision lands here. Withholding the attestation leaves
      // the outpost on its previous window -- the same end state its own
      // guards reach, without shipping an invalid payload.
      //
      // Cost, accepted deliberately: the withheld attestation also carries
      // `epoch_duration_sec` and the whole-window resync that
      // batch-operator-schedule-window.md wants on every envelope, so both are
      // skipped for this epoch too. Shipping the payload with the index pinned
      // to the CURRENT group instead would keep them, but it names a group the
      // outpost must not treat as next, and the Solana handler refuses a window
      // carrying an empty group regardless -- so it buys nothing here.
      //
      // Withheld by SKIPPING THE QUEUEOUT ONLY -- never by returning from
      // `finishadv`, which still has the epoch's remaining attestations and
      // actions to issue after this block.
      const bool have_next_group =
         next_group_index < group_count && !state.batch_op_groups[next_group_index].empty();
      if (!have_next_group) {
         sysio::print("sysio.epoch::finishadv: no non-empty next group to publish for chain ", chain_code.to_string(),
                      " at epoch ", state.current_epoch_index,
                      " (groups=", group_count, ", next_index=", next_group_index,
                      "); withholding BatchOperatorGroups -- outposts retain their "
                      "previous window\n");
      }
      attest.active_group_index = zpp::bits::vuint32_t{next_group_index};
      attest.epoch_index = zpp::bits::vuint32_t{state.current_epoch_index};
      // Propagate this chain's minimum epoch duration. The outposts keep it for information only: their consensus
      // tips on the first strict majority, at any time.
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

      // `have_next_group` gates the QUEUEOUT, not `finishadv` -- see above.
      if (have_next_group) {
         action(
            permission_level{get_self(), "owner"_n},
            MSGCH_ACCOUNT,
            msgch_actions::QUEUE_OUT,
            std::make_tuple(
               key.chain_code,
               opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS,
               encoded
            )
         ).send();
      }
   }

   // Build this outpost's outbound envelope, stamped with its own epoch index.
   if (active_outpost) {
      action(
         permission_level{get_self(), "owner"_n},
         MSGCH_ACCOUNT,
         msgch_actions::BUILD_ENVELOPE,
         std::make_tuple(key.chain_code)
      ).send();
   }

   // No scheduled cleanup of `sysio.msgch::envelopes` is needed here:
   // `deliver` prunes rows older than the previous epoch. The durable audit
   // trail lives in the `envelope_log` table on the same contract, where each
   // outpost keeps `2 * cfg.epoch_retention_envelope_log_count` rows.
}

// ---------------------------------------------------------------------------
//  schbatchgps — initial fill of one chain's N-group sliding window
//
//  Called ONCE per chain at bootstrap. Reads the ACTIVE batch operators SERVING the
//  chain from sysio.opreg, sorts non-bootstrapped first (progressive-takeover preference per
//  .claude/rules/batch-operator-schedule-preference.md), and partitions
//  them into N groups (`cfg.batch_op_groups`). The resulting window is
//  [epoch_1_group, epoch_2_group, ..., epoch_N_group].
//
//  After this, every per-epoch `finishadv` pops the front group and pushes
//  a new tail group, where the tail's members are drawn from the same
//  pool MINUS anyone still resident in the N-1 surviving groups. The
//  window itself encodes "scheduled in the last N-1 epochs"; no separate
//  history table is needed.
// ---------------------------------------------------------------------------
void epoch::schbatchgps(sysio::slug_name chain_code) {
   require_auth(get_self());

   const chain_key    key{chain_code.value};
   const epoch_config cfg = epochcfg_t(get_self()).get(key, EPOCH_CONFIG_NOT_INITIALIZED.data());

   // The chain's ACTIVE serving batch operators, non-bootstrapped first, then bootstrapped.
   std::vector<pool_member> available_batch = schedule_pool(chain_code, {});
   check(available_batch.size() >= cfg.batch_operator_minimum_active,
         "not enough available batch operators for group assignment");
   available_batch.resize(cfg.batch_operator_minimum_active);

   // Even/odd interleave to spread non-bootstrapped + bootstrapped across groups.
   std::vector<name> shuffled;
   shuffled.reserve(available_batch.size());
   for (size_t i = 0; i < available_batch.size(); i += 2)
      shuffled.push_back(available_batch[i].account);
   for (size_t i = 1; i < available_batch.size(); i += 2)
      shuffled.push_back(available_batch[i].account);

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
   epoch_state state = state_tbl.try_get(key).value_or(epoch_state{.chain_code = key.chain_code});
   state.batch_op_groups = new_groups;
   state.current_batch_op_group = 0; // front-of-window is always current
   state_tbl.set(ram_payer, key, state);
}

// ---------------------------------------------------------------------------
//  seteconomy
// ---------------------------------------------------------------------------
void epoch::seteconomy(uint32_t epoch_duration_sec, sysio::time_point_sec start_time) {
   require_auth(get_self());

   check(epoch_duration_sec >= MIN_ECONOMIC_EPOCH_DURATION_SEC,
         "economic epoch_duration_sec must be >= MIN_ECONOMIC_EPOCH_DURATION_SEC");
   check(epoch_duration_sec <= MAX_ECONOMIC_EPOCH_DURATION_SEC,
         "economic epoch_duration_sec exceeds 30-day ceiling");
   check(start_time.sec_since_epoch() > 0, "economic start_time must be set");

   // The grid is anchored at start_time: once an economic epoch has opened, moving the anchor would
   // re-date every epoch already accrued.
   //
   // While a boundary has passed without its tick, withdrawals are already dated by the boundaries still to be
   // ticked, so the grid may not change until the crank is current. Once it is, a new duration spaces only the
   // boundaries after the next one: no boundary that has passed moves, nor, once the clock has ticked, the next one.
   // A grid whose start is still ahead may be re-anchored, even into the past unless a collateral withdrawal is
   // queued.
   economycfg_t         cfg_tbl(get_self());
   const economic_state state = economystate_t(get_self()).get_or_default(economic_state{});
   bool start_moves = true;
   if (cfg_tbl.exists()) {
      const economic_config current = cfg_tbl.get();
      if (state.current_epoch_index > 0) {
         check(start_time == current.start_time,
               "economic start_time is fixed once the economic clock has started");
      }
      start_moves = start_time != current.start_time;
      const bool changes = epoch_duration_sec != current.epoch_duration_sec || start_moves;
      check(!changes || economic_epoch_index_at(current_time_point()) == state.current_epoch_index,
            "economic clock is behind: tick every passed boundary before changing it");
   }

   // A queued withdrawal waits for boundaries after its request (sysio.opreg's economic wait). Every boundary of a
   // start that is not ahead lies at or before now, so while one is queued the start may only be set ahead.
   if (start_moves && time_point{start_time} <= current_time_point()) {
      opreg::wtdwqueue_t withdrawals(OPREG_ACCOUNT);
      check(withdrawals.begin() == withdrawals.end(),
            "economic start_time must be ahead while a collateral withdrawal is queued");
   }

   const economic_config updated{
      .epoch_duration_sec = epoch_duration_sec,
      .start_time         = start_time,
   };

   // The emissions pay-period accumulation bound (see emissions.hpp) depends on the length of the epochs a tick
   // accrues: scale_annual_to_epoch is linear in it, so a longer epoch raises the per-epoch emission ceiling that
   // sysio.system::setemitcfg validated against the pay cadence. The next tick closes the open epoch at the length it
   // opened with, and later ticks close epochs of the new duration, so the bound takes the longer of the two.
   // Re-validated against the stored emission config so no ordering of the two setters can accept a period whose
   // accumulation would saturate the pending accumulator.
   {
      sysiosystem::emissions::emitcfg_t emit_cfg_tbl(SYSTEM_ACCOUNT);
      if (emit_cfg_tbl.exists()) {
         sysiosystem::emissions::t5state_t t5s_tbl(SYSTEM_ACCOUNT);
         const int64_t pending = t5s_tbl.exists() ? t5s_tbl.get().pending_emission_amount : 0;
         check(sysiosystem::emissions::period_accrual_fits_asset_range(
                  emit_cfg_tbl.get(), longest_accruing_epoch_sec(updated, state), pending),
               "per-epoch emission ceiling x pay_cadence_epochs exceeds the asset range at this epoch_duration_sec");
      }
   }

   cfg_tbl.set(updated, ram_payer);
}

// ---------------------------------------------------------------------------
//  tickeconomy
// ---------------------------------------------------------------------------
void epoch::tickeconomy(uint32_t epoch_index) {
   economycfg_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "economic clock not configured");
   const auto cfg = cfg_tbl.get();

   // The economy starts with the network: no economic epoch opens before the network genesis,
   // however far in the past start_time lies. From then on the clock never waits on consensus.
   check(network_genesis_done(), "the economic clock starts at the network genesis");

   economystate_t state_tbl(get_self());
   economic_state state = state_tbl.get_or_default(economic_state{});
   check(epoch_index == state.current_epoch_index + 1, "economic epoch_index is not the next economic epoch");

   // Fixed grid: epoch 1 opens at start_time and each later boundary is the previous one plus the
   // duration in force, so a late crank changes when an epoch is processed, never what it accrues.
   const time_point boundary = state.current_epoch_index == 0
      ? time_point{cfg.start_time}
      : state.next_epoch_start;
   check(current_time_point() >= boundary, "economic epoch boundary has not passed");

   // The epoch this tick closes, from the previous boundary to this one, at the length it opened with: the tick
   // accrues its emission share and its block slots, so neither follows a duration set since.
   const uint32_t closed_epoch_duration_sec = closing_epoch_duration_sec(cfg, state);
   const time_point next_boundary = boundary + microseconds(static_cast<int64_t>(cfg.epoch_duration_sec) * 1'000'000);

   // A payout spends only if the clock had caught up to the grid before this tick, so the tick that reaches it spends
   // nothing either. Until then each pay epoch closes its own period; from then on payouts merge while the crank is
   // behind.
   const bool spend        = state.caught_up;
   const bool crank_behind = current_time_point() >= next_boundary;
   const auto gate = check_emissions_ready(closed_epoch_duration_sec, epoch_index, crank_behind && spend, spend);
   if (gate.blocked()) {
      sysio::print_f("epoch::tickeconomy: economic epoch %u emissions held -- see blocklog\n", epoch_index);
      record_gate_block(get_self(), epoch_index, gate);
   } else if (!gate.deferred) {
      clear_gate_block(get_self());
   }

   state.current_epoch_index = epoch_index;
   state.current_epoch_start = boundary;
   state.next_epoch_start    = next_boundary;
   if (!crank_behind) state.caught_up = true;
   state_tbl.set(state, ram_payer);

   // Withdrawals mature on this clock, each held until every chain it waits on has also passed its
   // consensus guard (sysio.opreg::flushwtdw reads the chains' epochs and their decisions). Matured rows
   // credit remitclaims; no attestation is sent.
   action(
      permission_level{get_self(), "owner"_n},
      OPREG_ACCOUNT,
      opreg_actions::FLUSH_WITHDRAWALS,
      std::make_tuple(epoch_index)
   ).send();

   // Accrual precedes the payout sibling, so payepoch reads the period total including this epoch.
   if (gate.accrue) {
      action(
         permission_level{get_self(), "owner"_n},
         SYSTEM_ACCOUNT,
         system_actions::ACCRUE_EPOCH,
         std::make_tuple(epoch_index, gate.emission_amount, closed_epoch_duration_sec)
      ).send();
   }
   if (gate.pay) {
      action(
         permission_level{get_self(), "owner"_n},
         SYSTEM_ACCOUNT,
         system_actions::PAY_EPOCH,
         std::make_tuple(epoch_index, gate.period_emission, spend)
      ).send();
   }

   // One bounded step of any batch-duty settlement still open, every economic epoch. Settlements
   // only exist once initt5 has run.
   if (sysiosystem::emissions::records_batch_duty(SYSTEM_ACCOUNT)) {
      action(
         permission_level{get_self(), "owner"_n},
         SYSTEM_ACCOUNT,
         system_actions::SETTLE_DUTY,
         std::make_tuple()
      ).send();
   }
}

// ---------------------------------------------------------------------------
//  pause / unpause
// ---------------------------------------------------------------------------
void epoch::pause(sysio::slug_name chain_code) {
   require_auth(CHALG_ACCOUNT);

   const chain_key key{chain_code.value};
   epochstate_t    state_tbl(get_self());
   epoch_state     state = state_tbl.try_get(key).value_or(epoch_state{.chain_code = key.chain_code});
   state.is_paused = true;
   state_tbl.set(ram_payer, key, state);
}

void epoch::unpause(sysio::slug_name chain_code) {
   require_auth(CHALG_ACCOUNT);

   const chain_key key{chain_code.value};
   epochstate_t    state_tbl(get_self());
   epoch_state     state = state_tbl.get(key, "epoch state not initialized");
   state.is_paused = false;
   state_tbl.set(ram_payer, key, state);
}

// ---------------------------------------------------------------------------
//  resendroster
// ---------------------------------------------------------------------------
void epoch::resendroster(sysio::slug_name code) {
   require_auth(get_self());

   rostersent_t sent_tbl(get_self());
   sent_tbl.erase(chain_key{code.value}, "no roster has been queued to this outpost");

   // The roster rides the chain's next envelope, so the chain is due no later than one minimum from now.
   note_chain_traffic(get_self(), code, current_time_point());
}

} // namespace sysio
