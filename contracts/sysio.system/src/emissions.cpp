#include <sysio.system/sysio.system.hpp>
#include <sysio.system/emissions.hpp>
#include <sysio.system/opreg_status.hpp>
#include <sysio.system/producer_score.hpp>

#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <sysio.opp.common/claimable.hpp>
#include <sysio.opp.common/wire_asset.hpp>

// Canonical contract headers used for cross-contract reads. The
// [[sysio::contract("sysio.<name>")]] attribute on each table struct pins
// the table to its owning contract's ABI; including these from sysio.system
// does not pollute sysio.system's ABI.
#include <sysio.token/sysio.token.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.epoch/sysio.epoch.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sysiosystem {

using namespace emissions;

// ---------------------------------------------------------------------------
// Well-known OPP accounts.
// ---------------------------------------------------------------------------

namespace epoch_refs {
   constexpr sysio::name account = "sysio.epoch"_n;
}

namespace {

// ---------------------------------------------------------------------------
// Compile-time constants (not user-configurable)
// ---------------------------------------------------------------------------

constexpr uint32_t STANDBY_START_RANK     = 22;
constexpr uint32_t MAX_STANDBY_END_RANK   = 100; // safety cap: bounds how many STANDBY retainers one
                                                 // payepoch can credit. It does NOT bound the pay walk --
                                                 // no-forfeiture means a producer far below the standby
                                                 // band still has carried blocks to collect, so the walk
                                                 // runs past this and is bounded by max_rank_walk_rows.
constexpr int64_t  MS_PER_SECOND          = 1000;

// Basis-point denominator for all category / sub-split ratios.
constexpr int64_t  BPS_DENOMINATOR        = 10000;

constexpr sysio::name GOVERNANCE_ACCOUNT         = "sysio.gov"_n;
// Capex ("capital expenditure") bucket lives on sysio.ops -- operational spend.
constexpr sysio::name CAPEX_OPERATIONS_ACCOUNT   = "sysio.ops"_n;
constexpr sysio::name TOKEN_CONTRACT             = "sysio.token"_n;
constexpr sysio::name ROA_CONTRACT               = "sysio.roa"_n;

namespace memo {
   constexpr std::string_view capex            = "T5 capex";
   constexpr std::string_view governance       = "T5 governance";
   constexpr std::string_view batch_op_reward  = "T5 batch operator reward";
   constexpr std::string_view producer_reward  = "T5 producer reward";
   constexpr std::string_view node_owner_dist  = "Node Owner distribution";
   constexpr std::string_view epoch_pay_claim  = "T5 epoch pay claim";
}

// Error surfaced by claimpay when the caller has no credited pay to draw.
constexpr const char* NOTHING_TO_CLAIM_MSG = "no epoch pay to claim";

using sysio::asset;
using sysio::current_time_point;
using sysio::name;
using sysio::time_point_sec;
using sysio::opp::types::OperatorType;

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

int64_t split_bps(int64_t total, uint16_t bps) {
   __int128 product = static_cast<__int128>(total) * static_cast<__int128>(bps);
   return static_cast<int64_t>(product / BPS_DENOMINATOR);
}

node_claim_result compute_node_claim(const emission_state& emission,
                                     const node_owner_distribution& row,
                                     int64_t min_claimable) {
   node_claim_result info{};

   const uint32_t start_secs = emission.node_rewards_start.sec_since_epoch();
   sysio::check(start_secs > 0, "node rewards have not started");

   const uint32_t duration       = row.total_duration;
   const int64_t  total_amount   = row.total_allocation.amount;
   const int64_t  already_claimed = row.claimed.amount;

   const time_point_sec now      = time_point_sec{current_time_point()};
   const uint32_t       now_secs = now.sec_since_epoch();

   uint32_t elapsed = (now_secs > start_secs) ? (now_secs - start_secs) : 0;
   if (elapsed > duration) elapsed = duration;

   int64_t total_vested_amount = 0;
   if (elapsed == 0) {
      total_vested_amount = 0;
   } else if (elapsed == duration) {
      total_vested_amount = total_amount;
   } else {
      __int128 numerator = static_cast<__int128>(total_amount) *
                           static_cast<__int128>(elapsed);
      total_vested_amount = static_cast<int64_t>(numerator / duration);
   }

   int64_t claimable_amount = total_vested_amount - already_claimed;
   if (claimable_amount < 0) claimable_amount = 0;

   info.total_allocation = row.total_allocation;
   info.claimed          = row.claimed;
   info.claimable        = asset{claimable_amount, row.total_allocation.symbol};
   info.can_claim        = (claimable_amount >= min_claimable || elapsed == duration);

   return info;
}

// ---------------------------------------------------------------------------
// Cross-contract helpers (read-only)
// ---------------------------------------------------------------------------

// Read sysio's WIRE token balance via the sysio.token kv::scoped_table.
// Returns 0 if no balance entry exists. Uses a local mirror of sysio.token's
// accounts table because the upstream types are private.
int64_t get_wire_balance(name account) {
   sysio::token::token::accounts acct_tbl(TOKEN_CONTRACT, account.value);
   sysio::token::token::acct_key key{sysio::opp::wire::asset_symbol.code().raw()};
   if (!acct_tbl.contains(key)) return 0;
   return acct_tbl.get(key).balance.amount;
}

// ---------------------------------------------------------------------------
// Payout helpers
// ---------------------------------------------------------------------------

// Direct push transfer. RESERVED FOR SYSTEM-ACCOUNT DESTINATIONS.
//
// `sysio.token::transfer` notifies `to`, and the chain runs notified receivers with no exception
// isolation, so the destination decides whether the enclosing transaction commits. Pushing to an
// account the protocol does not control therefore hands it an abort switch over every parent
// inline action.
//
// Two automatic call sites target protocol-controlled holding accounts with no deployed code:
//
//   * `payepoch` (capex)      -> `sysio.ops`     — no code deployed
//   * `payepoch` (governance) -> `sysio.gov`     — no code deployed
//
// The two `payepoch` pushes are on the never-throw `advance` path, so those two accounts are the
// ones that could abort epoch advancement if code were ever deployed on them — see the standing
// constraint at that call site. Adding another AUTOMATIC destination outside protocol control
// reopens the vulnerability this file exists to close.
//
// The scope is automatic payouts on never-throw paths. A claimant-authorized action MAY transfer
// straight to an uncontrolled account — `claimpay` and `claimnodedis` do, correctly: they carry the
// claimant's own authority, so a hostile notify handler blocks nothing but that caller's own payout.
// It is the payepoch path that credits via `credit_pay` instead.
void send_wire_transfer(name self, name to, int64_t amount, std::string_view memo_str) {
   if (amount <= 0) return;
   sysio::action(
      {self, "active"_n},
      TOKEN_CONTRACT,
      "transfer"_n,
      std::make_tuple(self, to, asset{amount, sysio::opp::wire::asset_symbol}, std::string{memo_str})
   ).send();
}

// Credit `amount` to `to`'s claimable pay row and reserve it against the treasury balance.
//
// Never throws (the credit saturates rather than aborting), so this is safe on the payepoch path,
// which runs inline from `sysio.epoch::advance` and must not be abortable by any recipient.
//
// Observability note: the per-recipient inline transfer this replaces used to carry `memo_str` and
// showed up as its own action trace HERE, on the payepoch path. That trace is gone -- the credit is
// a kv write -- so attribution at credit time is the `payclaims` table delta (visible to
// state-history consumers) plus the aggregate `epochlog` row. A per-recipient transfer trace does
// still appear later, when the claimant calls `claimpay`; what no longer exists anywhere is a
// CATEGORY-tagged one.
//
// `memo_str` is DISCARDED, and nothing downstream recovers it: `claimpay` drains the whole row in
// one transfer under its own constant memo (`memo::epoch_pay_claim`), so a claimant owed both a
// producer and a batch-op share gets ONE transfer with no per-bucket memo. The parameter survives
// only so the call sites read as which bucket they are paying (`memo::producer_reward` /
// `memo::batch_op_reward`) — it does not reach the chain, and no monitoring may key on it.
void credit_pay(name self, name to, int64_t amount, std::string_view /*memo_str*/) {
   if (amount <= 0) return;
   const uint64_t amt = static_cast<uint64_t>(amount);

   payclaims_t claims(self);
   sysio::opp::claimable::credit(claims, self, payclaim_key{to.value},
                                 pay_claim{.account_name = to}, amt);

   // Reserve the credited WIRE so treasury withdrawals and the epoch gate cannot re-commit it. Saturates on
   // the same cap as the row itself, keeping the counter consistent with the sum of the rows.
   payclaimtot_t tot_tbl(self);
   auto tot = tot_tbl.get_or_default(pay_claim_total{});
   tot.outstanding = sysio::opp::claimable::add_capped(tot.outstanding, amt);
   tot_tbl.set(tot, self);
}

// ---------------------------------------------------------------------------
// emission_config loader
// ---------------------------------------------------------------------------

emission_config get_emit_cfg(name self) {
   emitcfg_t cfgtbl(self);
   sysio::check(cfgtbl.exists(), "emission config not set; call setemitcfg first");
   return cfgtbl.get();
}

// The economic clock's configuration lives on sysio.epoch::economycfg. initt5 (start time),
// payepoch (the producer pay period's slot count) and viewepoch (seconds_until_next) read it here
// cross-contract so the values cannot drift from what tickeconomy uses.
sysio::epoch::economic_config get_economic_config() {
   sysio::epoch::economycfg_t cfg_tbl(epoch_refs::account);
   sysio::check(cfg_tbl.exists(), "sysio.epoch economic clock not configured");
   return cfg_tbl.get();
}

// The economic epoch duration for callers that may not throw -- see accrueepoch.
uint32_t economic_epoch_duration_sec_or_zero() {
   sysio::epoch::economycfg_t cfg_tbl(epoch_refs::account);
   return cfg_tbl.exists() ? cfg_tbl.get().epoch_duration_sec : 0;
}

// Block slots one economic epoch of `epoch_duration_sec` holds: the producer pay divisor's unit.
uint64_t nominal_slots_of(uint32_t epoch_duration_sec) {
   return static_cast<uint64_t>(epoch_duration_sec) * static_cast<uint64_t>(MS_PER_SECOND)
      / static_cast<uint64_t>(sysio::block_timestamp::block_interval_ms);
}

// ---------------------------------------------------------------------------
// Batch-duty settlement
// ---------------------------------------------------------------------------

// Pay `row`'s units of the settling period at that period's rate -- its pool over its units -- when
// its operator is still an ACTIVE batch operator, and erase the row. An inactive operator's share
// stays in the treasury. Returns the amount credited. Never throws.
int64_t settle_duty_row(name self, duty_state& duty, batchduty_t& rows, const batch_duty& row) {
   int64_t pay = 0;
   if (duty.settling_units > 0 && is_op_active(row.account_name, OperatorType::OPERATOR_TYPE_BATCH)) {
      pay = static_cast<int64_t>(
         static_cast<__int128>(duty.settling_pool) * row.units / duty.settling_units);
   }
   credit_pay(self, row.account_name, pay, memo::batch_op_reward);
   duty.settling_paid += pay;
   rows.erase(batch_duty_key{row.account_name.value});
   return pay;
}

// Close the settlement: release what it did not credit back to the T5 budget and finish its
// epochlog row. Never throws.
void finish_settlement(name self, duty_state& duty, t5_state& state) {
   state.unsettled_batch_emission = 0;

   epochlog_t log(self);
   const epochlog_key key{duty.settling_epoch_index};
   if (log.contains(key)) {
      log.modify(self, key, [&](auto& row) {
         row.batch_emission_retained = duty.settling_pool - duty.settling_paid;
         row.batch_settled           = true;
      });
   }

   duty.settling_period      = 0;
   duty.settling_pool        = 0;
   duty.settling_units       = 0;
   duty.settling_paid        = 0;
   duty.settling_epoch_index = 0;
   duty.settle_cursor        = 0;
}

// One bounded settlement step: walk the settling period's `batchduty` rows (the `byperiod` index) from
// the cursor, paying at most MAX_DUTY_SETTLEMENT_ROWS of them; passing the period's last row finishes
// the settlement. Credited amounts move from unsettled to distributed. Returns the rows paid. A no-op
// when no settlement is open. Never throws.
uint32_t step_settlement(name self, duty_state& duty, t5_state& state) {
   if (!duty.settling()) return 0;

   // Snapshot the step's rows and the cursor first, then settle: settling erases rows, and the walk
   // must not hold an iterator across those erasures.
   batchduty_t rows(self);
   auto by_period = rows.get_index<"byperiod"_n>();
   std::vector<batch_duty> due;
   due.reserve(MAX_DUTY_SETTLEMENT_ROWS);
   auto it = by_period.lower_bound(duty_period_key(duty.settling_period, duty.settle_cursor));
   for (; it != by_period.end() && it->period == duty.settling_period && due.size() < MAX_DUTY_SETTLEMENT_ROWS;
        ++it) {
      due.push_back(*it);
   }
   const bool walked_to_end = it == by_period.end() || it->period != duty.settling_period;
   if (!walked_to_end) duty.settle_cursor = it->account_name.value;

   for (const auto& row : due) {
      const int64_t paid = settle_duty_row(self, duty, rows, row);
      state.unsettled_batch_emission -= paid;
      state.total_distributed        += paid;
   }

   if (walked_to_end) finish_settlement(self, duty, state);
   return static_cast<uint32_t>(due.size());
}

// Erase up to `budget` `batchduty` rows of periods that closed without a settlement (duty but no batch
// pool), so no later settlement walks past them. Periods settle one at a time and in order, so every row
// older than the settling period, or than the open period when none is settling, is one. Never throws.
void prune_obsolete_duty(name self, const duty_state& duty, uint32_t budget) {
   const uint32_t live_from = duty.settling() ? duty.settling_period : duty.open_period;
   batchduty_t rows(self);
   auto by_period = rows.get_index<"byperiod"_n>();
   std::vector<uint64_t> obsolete;
   for (auto it = by_period.lower_bound(duty_period_key(0, 0));
        it != by_period.end() && it->period < live_from && obsolete.size() < budget; ++it) {
      obsolete.push_back(it->account_name.value);
   }
   for (const uint64_t account : obsolete) rows.erase(batch_duty_key{account});
}

} // anonymous namespace

// ===========================================================================
// setemitcfg -- set or update emission configuration
// ===========================================================================

void system_contract::setemitcfg(const emissions::emission_config& cfg) {
   require_auth(get_self());

   // Node-owner params
   sysio::check(cfg.t1_allocation >= 0, "t1_allocation must be non-negative");
   sysio::check(cfg.t2_allocation >= 0, "t2_allocation must be non-negative");
   sysio::check(cfg.t3_allocation >= 0, "t3_allocation must be non-negative");
   sysio::check(cfg.t1_duration > 0,    "t1_duration must be positive");
   sysio::check(cfg.t2_duration > 0,    "t2_duration must be positive");
   sysio::check(cfg.t3_duration > 0,    "t3_duration must be positive");
   sysio::check(cfg.min_claimable >= 0, "min_claimable must be non-negative");

   // T5 params
   sysio::check(cfg.t5_distributable >= 0,       "t5_distributable must be non-negative");
   sysio::check(cfg.t5_floor >= 0,               "t5_floor must be non-negative");
   sysio::check(cfg.t5_floor <= cfg.t5_distributable,
                 "t5_floor must be <= t5_distributable");
   sysio::check(cfg.target_annual_decay_bps > 0 && cfg.target_annual_decay_bps <= 10000,
                 "target_annual_decay_bps must be in (0, 10000]");
   sysio::check(cfg.annual_initial_emission >= 0, "annual_initial_emission must be non-negative");
   sysio::check(cfg.annual_max_emission >= 0,     "annual_max_emission must be non-negative");
   sysio::check(cfg.annual_min_emission >= 0,     "annual_min_emission must be non-negative");
   sysio::check(cfg.annual_min_emission <= cfg.annual_max_emission,
                 "annual_min_emission must be <= annual_max_emission");

   // BPS splits. compute + capex + governance bound what payepoch transfers
   // each period; any remainder stays in the treasury. Sum > 10000
   // would over-commit period_emission.
   const uint32_t paid_at_payepoch_bps =
      static_cast<uint32_t>(cfg.compute_bps)
      + static_cast<uint32_t>(cfg.capex_bps)
      + static_cast<uint32_t>(cfg.governance_bps);
   sysio::check(paid_at_payepoch_bps <= BPS_DENOMINATOR,
                 "compute + capex + governance BPS must be <= 10000");
   sysio::check(cfg.producer_bps + cfg.batch_op_bps == BPS_DENOMINATOR,
                 "compute sub-split BPS must sum to 10000");

   // Producer config
   sysio::check(cfg.standby_end_rank >= STANDBY_START_RANK,
                 "standby_end_rank must be >= standby_start_rank (22)");
   sysio::check(cfg.standby_end_rank <= MAX_STANDBY_END_RANK,
                 "standby_end_rank exceeds safety cap");
   sysio::check(cfg.standby_bps <= BPS_DENOMINATOR,
                 "standby_bps must be <= 10000");

   // Audit-log retention
   sysio::check(cfg.epoch_log_retention_count > 0,
                 "epoch_log_retention_count must be positive");

   // Pay cadence (number of economic epochs accumulated per payepoch firing).
   sysio::check(cfg.pay_cadence_epochs > 0,
                 "pay_cadence_epochs must be positive");
   sysio::check(cfg.pay_cadence_epochs <= emissions::MAX_PAY_CADENCE_EPOCHS,
                 "pay_cadence_epochs exceeds the pay-period cap");

   // Single read of sysio.epoch::economycfg shared by the round-to-zero guards
   // (which need epoch_secs to scale annual values) and the post-init guard
   // (which compares per-epoch floor against remaining distributable).
   sysio::epoch::economycfg_t economy_cfg_tbl(epoch_refs::account);
   const bool epoch_configured = economy_cfg_tbl.exists();
   const uint32_t epoch_secs = epoch_configured ? economy_cfg_tbl.get().epoch_duration_sec : 0;

   // Single read of t5_state shared by the period-accrual bound (which needs
   // the already-accrued pending amount) and the post-init brick guards below.
   t5state_t t5s(get_self());
   const bool t5_initialized = t5s.exists();
   const t5_state t5now      = t5_initialized ? t5s.get() : t5_state{};

   // If the economic clock is configured, sanity-check that each nonzero annual
   // value scales to a non-zero per-epoch share at the canonical economic
   // epoch_duration_sec. Without this guard, a tiny annual value can round
   // down to 0 in scale_annual_to_epoch, the gate sees emission_amount = 0,
   // and emissions silently disable. Skipped pre-bootstrap (economic clock not
   // yet configured); the same check fires on the next setemitcfg call.
   if (epoch_configured) {
      if (cfg.annual_initial_emission > 0) {
         sysio::check(emissions::scale_annual_to_epoch(cfg.annual_initial_emission, epoch_secs) > 0,
                       "annual_initial_emission per-epoch share rounds to 0 at current epoch_duration_sec");
      }
      if (cfg.annual_max_emission > 0) {
         sysio::check(emissions::scale_annual_to_epoch(cfg.annual_max_emission, epoch_secs) > 0,
                       "annual_max_emission per-epoch share rounds to 0 at current epoch_duration_sec");
      }
      if (cfg.annual_min_emission > 0) {
         sysio::check(emissions::scale_annual_to_epoch(cfg.annual_min_emission, epoch_secs) > 0,
                       "annual_min_emission per-epoch share rounds to 0 at current epoch_duration_sec");
      }

      // Bound the pay-period accumulation: anything already pending plus a
      // full cadence of ceiling-rate epochs must fit in the asset range.
      // Without this, pending_emission_amount can saturate at asset::max_amount
      // mid-period (see saturating_accrue), after which the pay-epoch readiness
      // gate demands a balance no treasury can hold and every payout blocks
      // permanently. sysio.epoch::seteconomy enforces the same bound when the
      // economic epoch_duration_sec changes.
      sysio::check(
         emissions::period_accrual_fits_asset_range(cfg, epoch_secs, t5now.pending_emission_amount),
         "per-epoch emission ceiling x pay_cadence_epochs exceeds the asset range at current epoch_duration_sec");
   }

   // If t5_state already exists, prevent config changes that would brick future emissions. Post-init, the budget must
   // still cover what has been paid out AND what the open pay period has accrued (it is owed at the boundary), and the
   // per-epoch floor (derived from annual_min_emission and the canonical economic epoch_duration_sec) can't exceed
   // what is left uncommitted. initt5 requires the economic clock to be configured, so t5_initialized implies
   // epoch_configured and epoch_secs is safe to use directly.
   if (t5_initialized) {
      const int64_t remaining = emissions::net_headroom(cfg, t5now);
      sysio::check(remaining >= 0,
                    "t5_distributable must cover floor + already-distributed + pending emission");
      const int64_t per_epoch_min =
         emissions::scale_annual_to_epoch(cfg.annual_min_emission, epoch_secs);
      sysio::check(per_epoch_min <= remaining,
                    "annual_min_emission per-epoch share exceeds remaining distributable");
   }

   emitcfg_t cfgtbl(get_self());
   cfgtbl.set(cfg, get_self());
}

// ===========================================================================
// Node-owner distribution
// ===========================================================================

void system_contract::setinittime(const sysio::time_point_sec& no_reward_init_time) {
   require_auth(get_self());
   get_emit_cfg(get_self()); // ensure config exists

   sysio::check(no_reward_init_time.sec_since_epoch() > 0,
                "node_rewards_start must be non-zero");

   emissionstate_t emstate(get_self());
   sysio::check(!emstate.exists(), "emission state already initialized");

   emstate.set(emission_state{
      .node_rewards_start = no_reward_init_time
   }, get_self());
}

void system_contract::addnodeowner(const sysio::name& account_name, uint8_t tier) {
   require_auth(ROA_CONTRACT);

   sysio::check(tier >= 1 && tier <= 3, "invalid tier");

   const auto cfg = get_emit_cfg(get_self());

   nodedist_t nodedist(get_self());
   auto pk = nodedist_key{account_name.value};
   sysio::check(!nodedist.contains(pk), "account already registered");

   int64_t  total_allocation_amount = 0;
   uint32_t duration_seconds        = 0;

   switch (tier) {
      case 1:
         total_allocation_amount = cfg.t1_allocation;
         duration_seconds        = cfg.t1_duration;
         break;
      case 2:
         total_allocation_amount = cfg.t2_allocation;
         duration_seconds        = cfg.t2_duration;
         break;
      case 3:
         total_allocation_amount = cfg.t3_allocation;
         duration_seconds        = cfg.t3_duration;
         break;
   }

   // Per-tier count cap: tier sizing comes from the network's economic
   // constants (TN_MAX_NODE_OWNERS in emissions.hpp), shared with
   // sysio.roa::activateroa. The running count per tier is held in
   // nodecount (created lazily here so addnodeowner has no init-order
   // dependency on setinittime/initt5).
   nodecountstate_t cstate(get_self());
   auto counts = cstate.get_or_default(node_count_state{});
   switch (tier) {
      case 1:
         sysio::check(counts.t1_count < emissions::T1_MAX_NODE_OWNERS, "t1 node owner cap reached");
         ++counts.t1_count;
         break;
      case 2:
         sysio::check(counts.t2_count < emissions::T2_MAX_NODE_OWNERS, "t2 node owner cap reached");
         ++counts.t2_count;
         break;
      case 3:
         sysio::check(counts.t3_count < emissions::T3_MAX_NODE_OWNERS, "t3 node owner cap reached");
         ++counts.t3_count;
         break;
   }
   cstate.set(counts, get_self());

   nodedist.emplace(get_self(), pk, node_owner_distribution{
      .account_name     = account_name,
      .total_allocation = asset{total_allocation_amount, sysio::opp::wire::asset_symbol},
      .claimed          = asset{0, sysio::opp::wire::asset_symbol},
      .total_duration   = duration_seconds,
   });
}

void system_contract::claimnodedis(const sysio::name& account_name) {
   require_auth(account_name);

   const auto cfg = get_emit_cfg(get_self());

   emissionstate_t emstate(get_self());
   sysio::check(emstate.exists(), "emission state not initialized");
   const auto emission = emstate.get();

   nodedist_t nodedist(get_self());
   auto pk = nodedist_key{account_name.value};
   sysio::check(nodedist.contains(pk), "account is not a node owner");

   const auto row = nodedist.get(pk);
   sysio::check(row.claimed != row.total_allocation, "all node owner rewards already claimed");

   const auto info = compute_node_claim(emission, row, cfg.min_claimable);
   sysio::check(info.can_claim, "claim amount below minimum threshold");

   // Node-owner vesting draws on the SAME `sysio` WIRE balance that backs owed epoch pay -- `payclaims`
   // credited and not yet pulled, and batch emission still settling -- so it spends only what the
   // epoch readiness gate also treats as spendable. Those amounts are promises this contract has
   // made; this withdrawal is a claim on the free remainder. Without the reserve the two overdraw
   // each other: a later `claimpay` or settlement step could not pay out, and `payepoch` would stay
   // balance-blocked.
   //
   // Rejecting rather than capping keeps `claimed` exactly in step with what was transferred; the
   // claim stays fully available and succeeds once claims are pulled or emissions refill the
   // treasury. `claimnodedis` is user-initiated, so the throw reaches the caller who asked.
   payclaimtot_t nd_tot_tbl(get_self());
   t5state_t     nd_t5_tbl(get_self());
   const int64_t nd_spendable = emissions::spendable_wire(get_wire_balance(get_self()),
                                                          nd_tot_tbl.get_or_default(pay_claim_total{}),
                                                          nd_t5_tbl.get_or_default(t5_state{}));
   sysio::check(info.claimable.amount <= nd_spendable,
                "treasury balance is reserved against owed epoch pay; try again later");

   nodedist.modify(same_payer, pk, [&](auto& mrow) {
      mrow.claimed += info.claimable;
      sysio::check(mrow.claimed <= mrow.total_allocation, "claim would exceed total allocation");
   });

   sysio::action(
      {get_self(), "active"_n},
      TOKEN_CONTRACT,
      "transfer"_n,
      std::make_tuple(get_self(), account_name, info.claimable, std::string{memo::node_owner_dist})
   ).send();
}

// claimpay - pull the caller's credited epoch pay.
//
// This is the ONLY place an epoch pay CREDIT becomes a token transfer. payepoch credits the
// producer / standby / batch-operator shares and transfers nothing to them, because it runs inline
// from sysio.epoch::advance and a recipient's transfer-notify handler would otherwise be able to
// abort advance and stall epoch advancement chain-wide. (payepoch does still push the T5 category
// buckets to `sysio.ops` / `sysio.gov` -- protocol-owned accounts with no code; see the note at
// that call site.) Here the transfer runs under the claimant's own authority, so a hostile
// handler blocks nothing but this caller's own claim.
//
// The row is erased before the transfer is queued (inside pay_out), so a notify handler that
// re-enters claimpay finds no row and cannot double spend. The outstanding-total counter is
// decremented in lockstep, releasing the reserve held against treasury withdrawals and the epoch gate.
void system_contract::claimpay(const sysio::name& account_name) {
   require_auth(account_name);

   payclaims_t claims(get_self());
   const uint64_t paid = sysio::opp::claimable::pay_out(
      claims, payclaim_key{account_name.value}, get_self(), TOKEN_CONTRACT,
      account_name, sysio::opp::wire::asset_symbol, std::string{memo::epoch_pay_claim}, NOTHING_TO_CLAIM_MSG);

   payclaimtot_t tot_tbl(get_self());
   auto tot = tot_tbl.get_or_default(pay_claim_total{});
   // Defensive floor: the counter and the row sum are maintained together, so `paid` can only
   // exceed `outstanding` if they have already diverged. Clamping keeps the reserve from
   // underflowing into a huge value that would freeze treasury withdrawals and the epoch gate permanently.
   tot.outstanding = (paid >= tot.outstanding) ? 0 : tot.outstanding - paid;
   tot_tbl.set(tot, get_self());
}

emissions::node_claim_result system_contract::viewnodedist(const sysio::name& account_name) {
   const auto cfg = get_emit_cfg(get_self());

   emissionstate_t emstate(get_self());
   sysio::check(emstate.exists(), "emission state not initialized");
   const auto emission = emstate.get();

   nodedist_t nodedist(get_self());
   auto pk = nodedist_key{account_name.value};
   sysio::check(nodedist.contains(pk), "account is not a node owner");

   return compute_node_claim(emission, nodedist.get(pk), cfg.min_claimable);
}

// ===========================================================================
// T5 treasury emissions
// ===========================================================================

void system_contract::initt5() {
   require_auth(get_self());

   const auto cfg     = get_emit_cfg(get_self());
   const auto economy = get_economic_config();

   t5state_t t5s(get_self());
   sysio::check(!t5s.exists(), "t5 state already initialized");

   // last_epoch_emission seeds the decay chain. Stored as the per-epoch share
   // of annual_initial_emission scaled by the canonical economic epoch_duration_sec,
   // so subsequent compute_epoch_emission calls operate on a value already in
   // per-epoch units.
   const int64_t initial_per_epoch =
      emissions::scale_annual_to_epoch(cfg.annual_initial_emission, economy.epoch_duration_sec);

   t5s.set(t5_state{
      .start_time          = economy.start_time,
      .epoch_count         = 0,
      .last_epoch_index    = 0,
      .last_epoch_time     = economy.start_time,
      .last_epoch_emission = initial_per_epoch,
      .total_distributed   = 0,
   }, get_self());
}

// accrueepoch - record economic epoch `epoch_index`'s emission share onto t5state.
// Called inline by sysio.epoch::tickeconomy whenever the readiness gate lets the
// epoch accrue (pay or non-pay). On a pay epoch accrueepoch runs first (FIFO) and
// payepoch follows, reading the state this action just wrote.
//
// Updates:
//   - pending_emission_amount += per_epoch_emission   (drained on the pay epoch)
//   - pending_nominal_slots   += the epoch's slots    (drained on the pay epoch)
//   - last_epoch_emission = per_epoch_emission        (decay continuity; kept on a zero share)
//   - last_epoch_index = epoch_index                  (replay guard)
//   - last_epoch_time = now
void system_contract::accrueepoch(uint32_t epoch_index, int64_t per_epoch_emission) {
   require_auth(epoch_refs::account);

   // Defense in depth. Zero is a valid share: the gate passes one to an open pay period with emission pending once the
   // rest of the T5 budget is committed or the curve is zero, and the epoch still counts toward that period.
   sysio::check(per_epoch_emission >= 0, "accrueepoch per_epoch_emission must be non-negative");

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");
   auto state = t5s.get();

   sysio::check(epoch_index > state.last_epoch_index, "accrueepoch epoch already accrued");

   // Saturating accumulate (shared helper in emissions.hpp). The readiness gate
   // precomputed the identical saturated total as period_emission and payepoch
   // asserts equality against it, so the three sites cannot diverge. The
   // period-accrual bound enforced by setemitcfg / sysio.epoch::seteconomy keeps
   // any accepted configuration from actually reaching the clamp; saturating
   // here is defense-in-depth behind those boundary checks.
   state.pending_emission_amount =
      saturating_accrue(state.pending_emission_amount, per_epoch_emission);

   // The divisor accrues with the pool, at the duration in force for THIS epoch. Computing it at
   // payout from the current duration would apply today's value to epochs that ran under a
   // different one, mis-sizing the divisor for any period spanning a duration change.
   // Read WITHOUT asserting the economic config exists: this action is inline from
   // sysio.epoch::tickeconomy and must not throw. A 0 duration accrues no slots and payout falls
   // through to the produced-block divisor.
   state.pending_nominal_slots += nominal_slots_of(economic_epoch_duration_sec_or_zero());

   const auto now = time_point_sec{current_time_point()};
   state.last_epoch_index    = epoch_index;
   state.last_epoch_time     = now;
   // A zero share keeps the last positive share as the decay base; storing 0 would restart the curve at the per-epoch
   // minimum, or at nothing when that minimum is 0. Near the end of the budget the last positive share may itself be
   // clamped to the remainder.
   if (per_epoch_emission > 0) state.last_epoch_emission = per_epoch_emission;

   t5s.set(state, get_self());
}

// rcrdduty - credit one duty unit to each member of the group that served
// consensus epoch `epoch_index`. Runs inline from sysio.epoch::advance, so it
// never throws: a member holding units of the settling period is settled at
// that period's rate before its new unit opens a row in the open period.
void system_contract::rcrdduty(uint32_t /*epoch_index*/, std::vector<sysio::name> members) {
   require_auth(epoch_refs::account);

   dutystate_t duty_tbl(get_self());
   auto duty = duty_tbl.get_or_default(duty_state{});
   t5state_t t5s(get_self());
   // A settlement only exists once payepoch has run, which requires t5state.
   std::optional<t5_state> state = t5s.exists() ? std::optional<t5_state>{t5s.get()} : std::nullopt;

   batchduty_t rows(get_self());
   for (const auto& member : members) {
      auto row = rows.try_get(batch_duty_key{member.value});
      if (row && row->period != duty.open_period) {
         // Units from a closed period wait only for the settling one; pay them out first.
         if (duty.settling() && row->period == duty.settling_period && state) {
            const int64_t paid = settle_duty_row(get_self(), duty, rows, *row);
            state->unsettled_batch_emission -= paid;
            state->total_distributed        += paid;
         } else {
            rows.erase(batch_duty_key{member.value});
         }
         row.reset();
      }

      batch_duty updated = row.value_or(batch_duty{.account_name = member, .period = duty.open_period});
      updated.units += 1;
      rows.set(get_self(), batch_duty_key{member.value}, updated);
      duty.open_units += 1;
   }

   duty_tbl.set(duty, get_self());
   if (state) t5s.set(*state, get_self());
}

// settleduty - one bounded step of the open batch-duty settlement, then pruning of obsolete duty rows
// with the rest of the step's budget. Inline from sysio.epoch::tickeconomy every economic epoch.
void system_contract::settleduty() {
   require_auth(epoch_refs::account);

   dutystate_t duty_tbl(get_self());
   if (!duty_tbl.exists()) return;
   auto duty = duty_tbl.get();
   if (!duty.settling()) {
      prune_obsolete_duty(get_self(), duty, MAX_DUTY_SETTLEMENT_ROWS);
      return;
   }

   t5state_t t5s(get_self());
   if (!t5s.exists()) return;
   auto state = t5s.get();

   const uint32_t paid_rows = step_settlement(get_self(), duty, state);
   prune_obsolete_duty(get_self(), duty, MAX_DUTY_SETTLEMENT_ROWS - paid_rows);

   duty_tbl.set(duty, get_self());
   t5s.set(state, get_self());
}

// payepoch - pay the compute, capex, and governance shares of the emissions
// accrued over the pay period ending at economic epoch `epoch_index`. Called
// inline by sysio.epoch::tickeconomy on a pay epoch after its readiness gate
// has verified that:
//   - emitcfg exists
//   - t5state exists
//   - the gross headroom is positive and period_emission is too (this epoch's share may be 0 once earlier epochs
//     of the period have accrued, if they committed the rest of the budget or the curve has dropped to zero)
//   - sysio's spendable WIRE balance >= period_emission (pending + this epoch's share)
//
// The unallocated remainder (period_emission - compute - capex - governance)
// stays in sysio's balance. LIQ yield draws no treasury bonus.
//
// The batch-operator slice opens a duty settlement: it is split pro rata to the
// period's duty units (rcrdduty) and credited in bounded steps. Only ACTIVE
// batch operators are paid; any skipped amount stays in the treasury and is
// recorded on the epochlog row when the settlement completes.
//
// Single-trx semantics guarantee gate conditions hold through this call;
// payepoch trusts the gate-computed period_emission and does not recompute.
// Strict sysio::check throws inside payepoch flag true bugs (arithmetic
// invariants, BPS sums).
void system_contract::payepoch(uint32_t epoch_index, int64_t period_emission) {
   require_auth(epoch_refs::account);

   // Defense in depth. Single-trx semantics make these conditions gate-guaranteed in normal operation; firing
   // here means a bug or out-of-order call.
   sysio::check(period_emission > 0, "payepoch period_emission must be positive");

   const auto cfg = get_emit_cfg(get_self());

   // Gate guaranteed t5state exists; load directly. accrueepoch ran first
   // (FIFO inline order) and already merged this epoch into pending +
   // last_epoch_*; trust that state.
   t5state_t t5s(get_self());
   auto state = t5s.get();

   sysio::check(epoch_index == state.last_epoch_index,
                "payepoch must run after accrueepoch for the same epoch_index");
   sysio::check(period_emission == state.pending_emission_amount,
                "payepoch period_emission must equal accrued pending_emission_amount");

   // ----- The previous period's batch settlement must be done first -----
   // Each period's duty settles at that period's own rate, so two cannot settle at once. Step the open
   // one; if it is still not done, the payout waits with the period still pending, and the next economic
   // epoch's gate offers it again.
   dutystate_t duty_tbl(get_self());
   auto duty = duty_tbl.get_or_default(duty_state{});
   if (duty.settling()) {
      step_settlement(get_self(), duty, state);
      if (duty.settling()) {
         sysio::print("payepoch: batch duty settlement still open; payout waits\n");
         duty_tbl.set(duty, get_self());
         t5s.set(state, get_self());
         return;
      }
   }

   // ----- Category splits -----
   // payepoch transfers compute + capex + governance only. The implicit
   // remainder (period_emission - compute - capex - governance) stays
   // in sysio's balance. Sum-to-10000 BPS means no unallocated remainder.
   const int64_t compute_amount    = split_bps(period_emission, cfg.compute_bps);
   const int64_t capex_amount      = split_bps(period_emission, cfg.capex_bps);
   const int64_t governance_amount = split_bps(period_emission, cfg.governance_bps);

   const int64_t producer_pool = split_bps(compute_amount, cfg.producer_bps);
   const int64_t batch_pool    = compute_amount - producer_pool;

   // "paid" here means DISTRIBUTED -- credited to `payclaims` for producers / standbys /
   // batch operators, transferred for the category buckets. Both leave the treasury's
   // spendable position, which is what these counters feed.
   int64_t actual_paid = 0; // emission actually distributed (counts toward total_distributed)

   // =======================================================================
   // Producer + standby pay.
   //
   // Producers are paid PER COUNTED BLOCK. The active slice of the producer pool is spread over the
   // block slots the period held, and every schedulable producer is credited that rate for each
   // block `onblock` counted for it: every block it made, at most one round per rotation. A missed
   // block is never counted, so its pay stays in the treasury: it does not flow to the producers
   // that did show up, because the rate does not depend on who did. The divisor is the period's
   // nominal slot count, raised to the blocks counted when a period runs long (late economic cranks
   // count a later epoch's blocks into an earlier payout), so the slice is never exceeded.
   //
   // Standbys (positions 22..cfg.standby_end_rank) draw a retainer from the standby slice
   // (cfg.standby_bps of the pool). Each POSITION holds a fixed share, decaying linearly from
   // position 22, over the constant sum of every position's weight -- a vacant position's share
   // stays in the treasury. Block pay is not gated on position, so a producer that slid from 21
   // to 22 mid-period is still paid for the blocks counted before the schedule caught up.
   //
   // Counters accumulate across non-pay epochs (no reset by accrueepoch) and are zeroed at the
   // end of this action for every producer PAID by it. A producer that is not schedulable when the
   // walk reaches it -- keyless, or one whose standing ended since its last rescore -- is neither
   // paid nor reset: its block count waits for the first payepoch where it is schedulable again
   // (a re-keyed producer's return; a terminated operator's, should it settle and re-register;
   // never, for a slashed one, whose row is never pruned and which `regoperator` refuses). A
   // producer BELOW the walk (demoted, parked, unbonded, slashed, terminated -- each rescored at
   // the event) is not visited at all, with the same effect. Every counted block is paid exactly
   // once, at the first payepoch where it is payable.
   // =======================================================================
   {
      auto prod_by_rank = _producers.get_index<"prodrank"_n>();

      const int64_t standby_pool = split_bps(producer_pool, cfg.standby_bps);
      const int64_t active_pool  = producer_pool - standby_pool;

      // Nominal block slots in the period, accumulated by `accrueepoch` at each economic epoch's OWN
      // duration -- every epoch of the period including this one, exactly as
      // `pending_emission_amount` is (the equality check above pins that). The fallback is the
      // zero-accrual case (no economic config when the period accrued), not a compatibility path.
      const uint64_t nominal_slots = state.pending_nominal_slots > 0
         ? state.pending_nominal_slots
         : nominal_slots_of(get_economic_config().epoch_duration_sec);

      // Standby position weights run N at position 22 down to 1 at standby_end_rank; their sum
      // is the divisor, so a position's share is the same whether or not it is filled.
      const uint64_t standby_positions  = cfg.standby_end_rank + 1 - STANDBY_START_RANK;
      const uint64_t standby_weight_sum = standby_positions * (standby_positions + 1) / 2;

      struct pay_entry {
         name     owner;
         uint32_t blocks;
         uint64_t standby_weight;
      };
      struct reset_entry {
         name owner;
         bool snapshot; // this payout consumed the row's attestation credit
      };
      std::vector<pay_entry>   entries;
      std::vector<reset_entry> to_reset; // snapshot before modify: avoids
                                          // iterating while mutating secondary idx
      uint64_t produced_blocks = 0;

      // Single pass over the rank-ordered producers: builds both the pay list (entries) and the
      // counter-reset list (to_reset). `position` is POSITION in this index among SCHEDULABLE
      // producers, counted while walking -- not a stored ordinal. The demoted tier sorts last and
      // is never schedulable, so it bounds the walk over retained historical, parked and keyless
      // rows; every row above it was a live, bonded producer operator at its last rescore, and every
      // event that ends that standing rescores the row (see producer_rank::compute).
      //
      // The divisor counts exactly the blocks this payepoch pays for. A count that waits on an
      // unpayable row is neither paid nor counted now; when its producer is payable again the
      // carried blocks are paid at THAT period's rate and counted in THAT period's divisor.
      uint32_t position = 0;
      uint32_t examined = 0;
      for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
         if (producer_rank::tier_of(it->rank_score) == producer_tier::demoted) break;
         // Hard ceiling on rows examined. This walk runs INLINE in an epoch advance, where an
         // overrun stalls the chain, so its cost may not depend on the demoted tier actually
         // bounding it. A row past the ceiling is neither paid nor reset -- the same treatment an
         // unpayable row gets -- so its blocks carry rather than vanish.
         //
         // Its snapshot counter is not reset either, and nothing rescores it, so a SCHEDULABLE row
         // past this ceiling keeps a stored rank_score carrying last period's snapshot term until
         // some unrelated event rescores it -- `compute` would drop the credit as stale, but the
         // stored key is not recomputed. Accepted rather than swept: reaching it takes 500+ ranked
         // schedulable producers, and the rows short of that are unaffected. A demoted row's stale
         // term is inert (every walk breaks at that tier, so nothing reads the order within it),
         // and an inactive one is `unscored()` and carries no term at all.
         if (++examined > max_rank_walk_rows) break;

         // is_schedulable requires an active row, ACTIVE opreg status, and an active finalizer
         // key: a producer missing any of them can never be scheduled, so it draws neither block
         // pay nor a standby retainer -- and keeps its block count for when it can. The snapshot
         // counter is per period regardless.
         if (!producer_rank::is_schedulable(*it, _finalizers)) {
            if (it->snapshot_attestations > 0) to_reset.push_back({it->owner, true});
            continue;
         }

         produced_blocks += it->unpaid_blocks;
         if (it->unpaid_blocks > 0 || it->snapshot_attestations > 0) {
            to_reset.push_back({it->owner, it->snapshot_attestations > 0});
         }

         ++position;
         const bool standby = position >= STANDBY_START_RANK && position <= cfg.standby_end_rank;
         const uint64_t standby_weight = standby ? cfg.standby_end_rank + 1 - position : 0;
         if (it->unpaid_blocks > 0 || standby_weight > 0) {
            entries.push_back({it->owner, it->unpaid_blocks, standby_weight});
         }
      }

      // `nominal_slots` is the period's entitlement; `produced_blocks` raises it when a period ran
      // long, so no producer's rate exceeds its slice.
      //
      // Carried blocks are NOT subtracted back out. A row whose pay rounds to zero keeps its blocks
      // (that promise is what makes the model forfeiture-free) and they are counted again in the
      // period that settles them -- a hair of dilution, and only when the per-block rate is under
      // one subunit. Correcting it took a second pass that could overdraw the pool.
      const uint64_t slot_divisor = std::max<uint64_t>(std::max(nominal_slots, produced_blocks), 1);

      // A row's blocks are cleared ONLY when the block portion actually credited something. The
      // division is integer, so a small pool over a large divisor can round a real block count to
      // zero pay; clearing the count then would destroy work the producer did, which is the one
      // thing this model promises never to do. An uncredited count carries to the next payout
      // exactly as an unpayable row's does, and the rate it eventually settles at is the settling
      // period's -- so the blocks are worth something the moment the pool can represent them.
      int64_t distributed_to_producers = 0;
      std::vector<name> block_paid;
      block_paid.reserve(entries.size());
      for (const auto& entry : entries) {
         const int64_t block_pay = static_cast<int64_t>(
            static_cast<__int128>(active_pool) * entry.blocks / slot_divisor);
         int64_t pay = block_pay;
         if (entry.standby_weight > 0) {
            pay += static_cast<int64_t>(
               static_cast<__int128>(standby_pool) * entry.standby_weight / standby_weight_sum);
         }
         if (pay > 0) {
            credit_pay(get_self(), entry.owner, pay, memo::producer_reward);
            distributed_to_producers += pay;
         }
         // The BLOCK portion specifically -- a standby whose retainer paid but whose block pay
         // rounded to zero keeps its blocks too.
         if (block_pay > 0) block_paid.push_back(entry.owner);
      }

      actual_paid += distributed_to_producers;

      // Sorted so the join below is a binary search. This action runs INLINE in the epoch advance,
      // where an overrun stalls the chain, and both vectors are bounded by `max_rank_walk_rows` --
      // a linear scan per reset entry would otherwise be quadratic in a potentially large roster.
      std::sort(block_paid.begin(), block_paid.end());

      // Reset the period's counters after distribution (iteration-safe: uses PK snapshot).
      for (const auto& entry : to_reset) {
         const bool clear_blocks =
            std::binary_search(block_paid.begin(), block_paid.end(), entry.owner);
         if (!clear_blocks && !entry.snapshot) continue;
         auto key = producer_key_t{entry.owner.value};
         _producers.modify(same_payer, key, [&](auto& p) {
            if (clear_blocks)   p.unpaid_blocks          = 0;
            if (entry.snapshot) p.snapshot_attestations  = 0;
         });
         // Zeroing snapshot_attestations moved the snapshot factor; keep the sort key in step.
         if (entry.snapshot) rescore_producer(entry.owner);
      }
   }

   // =======================================================================
   // Batch-op pay. The period's duty units split the batch pool: open a
   // settlement for the period now closing and take its first step. A period
   // with no duty leaves the whole pool in the treasury.
   // =======================================================================
   const bool batch_has_duty = duty.open_units > 0 && batch_pool > 0;
   if (batch_has_duty) {
      duty.settling_period      = duty.open_period;
      duty.settling_pool        = batch_pool;
      duty.settling_units       = duty.open_units;
      duty.settling_paid        = 0;
      duty.settling_epoch_index = epoch_index;
      duty.settle_cursor        = 0;
      state.unsettled_batch_emission = batch_pool;
   }
   duty.open_period += 1;
   duty.open_units   = 0;

   // =======================================================================
   // Category buckets: fixed accounts, no opreg filter. The unallocated
   // remainder stays in the treasury; LIQ yield has no capital draw.
   // =======================================================================
   // PUSHED, not credited -- the deliberate exception to this contract's pull rule.
   //
   // The pull model exists because a payout to an account the protocol does not control lets that
   // account's transfer-notify handler abort `tickeconomy`. These two are protocol-owned holding
   // accounts: they carry no code, so there is no handler to run, and only governance can ever put
   // code there. The threat the credit path defends against does not exist for them.
   //
   // Crediting them instead would strand the money. A claim needs `require_auth(account_name)`,
   // and a `sysio.*` account can neither sign nor be acted for here: `sysio.roa` forces
   // `net_weight`/`cpu_weight` to zero for every account whose prefix is `sysio`
   // (`is_sysio_account`), so they cannot pay for a transaction. They also
   // have no contract of their own to emit the claim inline. Their pay would accrue in
   // `payclaims` forever while `payclaimtot` reserved the backing WIRE against treasury withdrawals and the
   // emissions readiness gate -- a permanent, growing reservation of WIRE nobody can move.
   //
   // This is a standing constraint on the two accounts, not a reason to revisit this call: any
   // contract ever deployed on `sysio.ops` or `sysio.gov` MUST NOT assert (or burn CPU) in an
   // `on_notify("sysio.token::transfer")` handler. Only governance can put code there, so that is
   // enforceable by review rather than by the protocol -- which is exactly why the same latitude
   // is not extended to any recipient outside protocol control.
   send_wire_transfer(get_self(), CAPEX_OPERATIONS_ACCOUNT, capex_amount,      memo::capex);
   send_wire_transfer(get_self(), GOVERNANCE_ACCOUNT,       governance_amount, memo::governance);

   actual_paid += capex_amount + governance_amount;

   // =======================================================================
   // State update. accrueepoch already wrote last_epoch_index / last_epoch_time
   // / last_epoch_emission (decay continuity); payepoch only updates the
   // pay-cadence accumulator + bookkeeping for amounts actually distributed.
   // =======================================================================
   state.epoch_count++;
   state.total_distributed += actual_paid; // track only amounts actually paid; skipped recipients' shares stay in treasury

   // Drain accumulator + advance period boundary.
   state.pending_emission_amount = 0;
   state.pending_nominal_slots   = 0;
   state.period_start_epoch = epoch_index + 1;

   // Audit log: records the AUTHORIZED period emission + the three category
   // amounts for the period that just paid. (Producer / batch-op sub-distribution is
   // implicit. Those shares are CREDITED to `payclaims` rather than transferred, so a
   // recipient no longer appears as its own transfer trace -- per-recipient attribution
   // comes from the `payclaims` row deltas, and the eventual `claimpay`.) One row per
   // pay epoch. The batch settlement fills in what it retained when it completes.
   epochlog_t epoch_table(get_self());
   epoch_table.emplace(get_self(), epochlog_key{epoch_index}, epoch_log{
      .economic_epoch_index    = epoch_index,
      .epoch_count             = state.epoch_count,
      .timestamp               = state.last_epoch_time,
      .total_emission          = period_emission,
      .compute_amount          = compute_amount,
      .capex_amount            = capex_amount,
      .governance_amount       = governance_amount,
      .batch_emission          = batch_pool,
      .batch_emission_retained = batch_has_duty ? 0 : batch_pool,
      .batch_settled           = !batch_has_duty,
   });

   // The first settlement step runs after the row exists, so a settlement that completes in this
   // step records its retained amount on it.
   step_settlement(get_self(), duty, state);

   duty_tbl.set(duty, get_self());
   t5s.set(state, get_self());

   // Head-first prune of the audit log past its retention cap. Rows are added
   // monotonically (one per successful payepoch), and epoch_count is the
   // contiguous payment-row sequence even when pay cadence is greater than
   // one. Drop up to two oldest rows per call: only one
   // is needed in steady state, but a recent retention-cap shrink (governance
   // lowering epoch_log_retention_count from N to a smaller M) leaves the
   // table over cap by N - M; pruning two per call drains it twice as fast
   // without unbounded CPU per epoch.
   for (int i = 0; i < 2; ++i) {
      auto first_it = epoch_table.begin();
      if (first_it == epoch_table.end()) break;
      const uint64_t live_count =
         state.epoch_count - first_it->epoch_count + 1;
      if (live_count <= cfg.epoch_log_retention_count) break;
      epoch_table.erase(first_it);
   }
}

emissions::epoch_info_result system_contract::viewepoch() {
   const auto cfg = get_emit_cfg(get_self());

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");

   const auto state   = t5s.get();
   const auto economy = get_economic_config();

   // The next economic boundary: the clock's own next_epoch_start once it has ticked, else its start.
   sysio::epoch::economystate_t economy_state_tbl(epoch_refs::account);
   const auto economy_state = economy_state_tbl.get_or_default(sysio::epoch::economic_state{});
   const uint64_t next_epoch_time = economy_state.current_epoch_index > 0
      ? economy_state.next_epoch_start.sec_since_epoch()
      : economy.start_time.sec_since_epoch();

   const uint64_t now_sec = time_point_sec{current_time_point()}.sec_since_epoch();
   const uint32_t secs_until =
      now_sec < next_epoch_time ? static_cast<uint32_t>(next_epoch_time - now_sec) : 0;

   return emissions::epoch_info_result{
      .epoch_count         = state.epoch_count,
      .last_epoch_index    = state.last_epoch_index,
      .last_epoch_time     = state.last_epoch_time,
      .last_epoch_emission = state.last_epoch_emission,
      .total_distributed   = state.total_distributed,
      .treasury_remaining  = std::max<int64_t>(emissions::net_headroom(cfg, state), 0),
      .next_emission_est   = emissions::compute_epoch_emission(cfg, economy.epoch_duration_sec, state),
      .seconds_until_next  = secs_until,
   };
}

emissions::emission_config system_contract::viewemitcfg() {
   return get_emit_cfg(get_self());
}

} // namespace sysiosystem
