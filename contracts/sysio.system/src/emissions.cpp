#include <sysio.system/sysio.system.hpp>
#include <sysio.system/emissions.hpp>

#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

// Canonical contract headers used for cross-contract reads. The
// [[sysio::contract("sysio.<name>")]] attribute on each table struct pins
// the table to its owning contract's ABI; including these from sysio.system
// does not pollute sysio.system's ABI.
#include <sysio.token/sysio.token.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.epoch/sysio.epoch.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace sysiosystem {

using namespace emissions;

// ---------------------------------------------------------------------------
// Well-known OPP accounts.
// ---------------------------------------------------------------------------

namespace opreg_refs {
   constexpr sysio::name account = "sysio.opreg"_n;
}

namespace epoch_refs {
   constexpr sysio::name account = "sysio.epoch"_n;
}

namespace {

// ---------------------------------------------------------------------------
// Compile-time constants (not user-configurable)
// ---------------------------------------------------------------------------

constexpr sysio::symbol WIRE_SYMBOL{"WIRE", 9};

constexpr uint32_t ACTIVE_PRODUCER_COUNT  = 21;
constexpr uint32_t STANDBY_START_RANK     = 22;
constexpr uint32_t MAX_STANDBY_END_RANK   = 100; // safety cap: bounds inline-action count in payepoch
constexpr uint32_t TOTAL_BLOCKS_PER_ROUND = ACTIVE_PRODUCER_COUNT * blocks_per_round; // 252
constexpr uint32_t ACTIVE_PRODUCER_WEIGHT = 15; // > any standby weight (1..cfg.standby_end_rank-21)

// Basis-point denominator for all category / sub-split ratios.
constexpr int64_t  BPS_DENOMINATOR        = 10000;

constexpr sysio::name CAPITAL_ACCOUNT            = "sysio.dclaim"_n;
constexpr sysio::name GOVERNANCE_ACCOUNT         = "sysio.gov"_n;
// Capex ("capital expenditure") bucket lives on sysio.ops -- operational spend.
constexpr sysio::name CAPEX_OPERATIONS_ACCOUNT   = "sysio.ops"_n;
constexpr sysio::name TOKEN_CONTRACT             = "sysio.token"_n;
constexpr sysio::name ROA_CONTRACT               = "sysio.roa"_n;
// sysio.reserv holds the swap-fee rewards bucket that payepoch folds into the
// per-epoch compute distribution.
constexpr sysio::name RESERV_CONTRACT            = "sysio.reserv"_n;

namespace memo {
   constexpr std::string_view capital          = "T5 capital";
   constexpr std::string_view capex            = "T5 capex";
   constexpr std::string_view governance       = "T5 governance";
   constexpr std::string_view batch_op_reward  = "T5 batch operator reward";
   constexpr std::string_view producer_reward  = "T5 producer reward";
   constexpr std::string_view node_owner_dist  = "Node Owner distribution";
}

using sysio::asset;
using sysio::current_time_point;
using sysio::name;
using sysio::time_point_sec;
using sysio::opp::types::OperatorStatus;

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
   sysio::token::token::acct_key key{WIRE_SYMBOL.code().raw()};
   if (!acct_tbl.contains(key)) return 0;
   return acct_tbl.get(key).balance.amount;
}

// Local mirror of sysio.reserv's rewards_bucket singleton. The kv row is keyed
// by table name ("rewardbkt") + the reserv account scope, so a mirror with the
// same table name and field layout reads the exact bytes reserv wrote. Mirrored
// rather than #include'd so sysio.system does not pull sysio.reserv's AMM /
// attestation headers into its translation unit. Layout MUST stay in lockstep
// with sysio.reserv.hpp's rewards_bucket (balance, lifetime_accrued).
struct reserv_rewards_bucket {
   uint64_t balance          = 0;
   uint64_t lifetime_accrued = 0;
   SYSLIB_SERIALIZE(reserv_rewards_bucket, (balance)(lifetime_accrued))
};
using reserv_rewardbkt_t = sysio::kv::global<"rewardbkt"_n, reserv_rewards_bucket>;

// Read the live swap-fee rewards balance held in sysio.reserv's custody.
// Returns 0 when never accrued. Clamps to int64 max defensively (reserv caps
// the bucket at uint64 max via saturating add).
int64_t get_reserv_rewards_balance() {
   reserv_rewardbkt_t bkt(RESERV_CONTRACT);
   const uint64_t bal = bkt.get_or_default(reserv_rewards_bucket{}).balance;
   constexpr uint64_t int64_max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
   return bal > int64_max ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(bal);
}

// Returns true iff the account is registered in sysio.opreg with AVAILABLE
// status (OPERATOR_STATUS_ACTIVE). Slashed/terminated/unknown all return false.
// Non-registered accounts return false.
bool is_op_active(name account) {
   sysio::opreg::operators_t ops(opreg_refs::account);
   auto key = sysio::opreg::operator_key{account.value};
   if (!ops.contains(key)) return false;
   return ops.get(key).status == OperatorStatus::OPERATOR_STATUS_ACTIVE;
}

// ---------------------------------------------------------------------------
// Transfer helper
// ---------------------------------------------------------------------------

void send_wire_transfer(name self, name to, int64_t amount, std::string_view memo_str) {
   if (amount <= 0) return;
   sysio::action(
      {self, "active"_n},
      TOKEN_CONTRACT,
      "transfer"_n,
      std::make_tuple(self, to, asset{amount, WIRE_SYMBOL}, std::string{memo_str})
   ).send();
}

// ---------------------------------------------------------------------------
// emission_config loader
// ---------------------------------------------------------------------------

emission_config get_emit_cfg(name self) {
   emitcfg_t cfgtbl(self);
   sysio::check(cfgtbl.exists(), "emission config not set; call setemitcfg first");
   return cfgtbl.get();
}

// Canonical epoch duration lives on sysio.epoch::epochcfg. Both payepoch
// (producer expected_rounds) and viewepoch (seconds_until_next) read it
// here cross-contract so the value cannot drift from what advance() uses.
uint32_t get_epoch_duration_sec() {
   sysio::epoch::epochcfg_t cfg_tbl(epoch_refs::account);
   sysio::check(cfg_tbl.exists(), "sysio.epoch config not initialized");
   return cfg_tbl.get().epoch_duration_sec;
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
   // each period; the remainder (10000 - that sum) is the implicit capital
   // reserve, drained lazily by sysio.dclaim::onreward via fundclaim. Sum
   // exactly 10000 means no implicit reserve (capital draws come out of
   // future periods' headroom). Sum > 10000 would over-commit period_emission.
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

   // Audit-log retention
   sysio::check(cfg.epoch_log_retention_count > 0,
                 "epoch_log_retention_count must be positive");

   // Pay cadence (number of epochs accumulated per payepoch firing). Zero
   // would divide-by-zero in the period share-by-rounds math; no upper
   // bound is enforced (operator's call).
   sysio::check(cfg.pay_cadence_epochs > 0,
                 "pay_cadence_epochs must be positive");

   // Single read of sysio.epoch::epochcfg shared by the round-to-zero guards
   // (which need epoch_secs to scale annual values) and the post-init guard
   // (which compares per-epoch floor against remaining distributable).
   sysio::epoch::epochcfg_t epoch_cfg_tbl(epoch_refs::account);
   const bool epoch_configured = epoch_cfg_tbl.exists();
   const uint32_t epoch_secs   = epoch_configured ? epoch_cfg_tbl.get().epoch_duration_sec : 0;

   // If sysio.epoch is configured, sanity-check that each nonzero annual
   // value scales to a non-zero per-epoch share at the canonical
   // epoch_duration_sec. Without this guard, a tiny annual value can round
   // down to 0 in scale_annual_to_epoch, the gate sees emission_amount = 0,
   // and emissions silently disable. Skipped pre-bootstrap (sysio.epoch not
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
   }

   // If t5_state already exists, prevent config changes that would brick future
   // emissions. Post-init, remaining distributable must still cover what's been
   // paid, and the per-epoch floor (derived from annual_min_emission and the
   // canonical epoch_duration_sec) can't exceed what's left to distribute.
   // initt5 requires sysio.epoch to be configured, so t5s.exists() implies
   // epoch_configured -- safe to use epoch_secs directly.
   t5state_t t5s(get_self());
   if (t5s.exists()) {
      const auto state = t5s.get();
      sysio::check(cfg.t5_distributable >= cfg.t5_floor + state.total_distributed,
                    "t5_distributable must cover floor + already-distributed");
      const int64_t remaining = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
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
      .total_allocation = asset{total_allocation_amount, WIRE_SYMBOL},
      .claimed          = asset{0, WIRE_SYMBOL},
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

void system_contract::initt5(const sysio::time_point_sec& start_time) {
   require_auth(get_self());

   const auto cfg = get_emit_cfg(get_self());

   t5state_t t5s(get_self());
   sysio::check(!t5s.exists(), "t5 state already initialized");

   // last_epoch_emission seeds the decay chain. Stored as the per-epoch share
   // of annual_initial_emission scaled by the canonical epoch_duration_sec on
   // sysio.epoch, so subsequent compute_epoch_emission calls operate on a
   // value already in per-epoch units.
   const int64_t initial_per_epoch =
      emissions::scale_annual_to_epoch(cfg.annual_initial_emission, get_epoch_duration_sec());

   t5s.set(t5_state{
      .start_time          = start_time,
      .epoch_count         = 0,
      .last_epoch_index    = 0,
      .last_epoch_time     = start_time,
      .last_epoch_emission = initial_per_epoch,
      .total_distributed   = 0,
   }, get_self());
}

// accrueepoch - record this epoch's per-epoch emission share onto t5state.
// Called inline by sysio.epoch::advance on EVERY successful epoch advance
// (both pay and non-pay). On non-pay epochs this is the only emission-side
// inline action; on pay-epochs accrueepoch runs first (FIFO) and payepoch
// follows, reading the state this action just wrote.
//
// Updates:
//   - pending_emission_amount += per_epoch_emission   (drained on pay-epoch)
//   - batch_group_epochs[batch_group_index] += 1      (drained on pay-epoch)
//   - last_epoch_emission = per_epoch_emission        (decay continuity)
//   - last_epoch_index = epoch_index                  (replay guard)
//   - last_epoch_time = now
//
// Treasury / balance gating is the gate's responsibility upstream; the
// idempotency check (epoch_index > last_epoch_index) prevents replay.
void system_contract::accrueepoch(uint32_t epoch_index,
                                  uint8_t  batch_group_index,
                                  int64_t  per_epoch_emission) {
   require_auth(epoch_refs::account);

   // Defense in depth.
   sysio::check(per_epoch_emission > 0, "accrueepoch per_epoch_emission must be positive");

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");
   auto state = t5s.get();

   sysio::check(epoch_index > state.last_epoch_index, "accrueepoch epoch already accrued");

   state.pending_emission_amount += per_epoch_emission;

   // Lazy-grow batch_group_epochs to fit batch_group_index. Pre-pay-cadence
   // chains see length 0 and grow on first epoch under the new schema.
   if (batch_group_index >= state.batch_group_epochs.size()) {
      state.batch_group_epochs.resize(batch_group_index + 1, 0);
   }
   state.batch_group_epochs[batch_group_index] += 1;

   const auto now = time_point_sec{current_time_point()};
   state.last_epoch_index    = epoch_index;
   state.last_epoch_time     = now;
   state.last_epoch_emission = per_epoch_emission;

   t5s.set(state, get_self());
}

// payepoch - pay the compute, capex, and governance shares of accumulated
// emissions for the pay period ending at `epoch_index`. Called inline by
// sysio.epoch::advance on a pay-epoch (period boundary defined by
// emit_cfg.pay_cadence_epochs) after its readiness gate has verified that:
//   - emitcfg exists
//   - t5state exists
//   - per-epoch emission > 0 (treasury not at floor)
//   - sysio's WIRE balance >= period_emission (pending + this epoch's share)
//
// Capital is NOT paid here. The implicit capital reserve
// (period_emission - compute - capex - governance) stays in sysio's balance
// and is drained lazily by fundclaim as sysio.dclaim::onreward fires, so
// dclaim has funds the moment a claim is credited rather than at the next
// pay-epoch.
//
// Swap-fee rewards: the rewards half of collected swap fees (sysio.reserv's
// rewards_bucket) is swept here via an inline drainrewards and folded into the
// compute distribution -- producers + batch operators receive it alongside
// emissions, split by the same producer_bps / batch_op_bps. Fees are funded by
// the sweep (not the treasury) and so are excluded from total_distributed.
//
// Single-trx semantics guarantee gate conditions hold through this call;
// payepoch trusts the gate-computed period_emission and does not recompute.
// Strict sysio::check throws inside payepoch flag true bugs (arithmetic
// invariants, BPS sums).
//
// Slashed / terminated batch-op group members are skipped via opreg filter;
// their slice remains in the treasury.
void system_contract::payepoch(uint32_t epoch_index,
                               std::vector<std::vector<sysio::name>> batch_op_groups,
                               int64_t period_emission) {
   require_auth(epoch_refs::account);

   // Defense in depth. Single-trx semantics make these conditions gate-guaranteed in normal operation; firing
   // here means a bug or out-of-order call.
   sysio::check(period_emission > 0, "payepoch period_emission must be positive");

   const auto cfg = get_emit_cfg(get_self());

   // Gate guaranteed t5state exists; load directly. accrueepoch ran first
   // (FIFO inline order) and already merged this epoch into pending +
   // batch_group_epochs + last_epoch_*; trust that state.
   t5state_t t5s(get_self());
   auto state = t5s.get();

   sysio::check(epoch_index == state.last_epoch_index,
                "payepoch must run after accrueepoch for the same epoch_index");
   sysio::check(period_emission == state.pending_emission_amount,
                "payepoch period_emission must equal accrued pending_emission_amount");

   // ----- Category splits -----
   // payepoch transfers compute + capex + governance only. The implicit
   // capital reserve (period_emission - compute - capex - governance) stays
   // in sysio's balance and is drained lazily by fundclaim as
   // sysio.dclaim::onreward fires. Sum-to-10000 BPS means zero reserve;
   // sum < 10000 leaves the remainder available for capital coverage.
   const int64_t compute_amount    = split_bps(period_emission, cfg.compute_bps);
   const int64_t capex_amount      = split_bps(period_emission, cfg.capex_bps);
   const int64_t governance_amount = split_bps(period_emission, cfg.governance_bps);

   const int64_t producer_pool = split_bps(compute_amount, cfg.producer_bps);
   const int64_t batch_pool    = compute_amount - producer_pool;

   // ----- Swap-fee rewards fold-in -----
   // The rewards half of collected swap fees accrues in sysio.reserv's
   // rewards_bucket (the other half already went to this treasury at swap
   // time). Fold the whole bucket into THIS period's compute distribution so
   // producers + batch operators receive it alongside emissions, split by the
   // SAME producer_bps / batch_op_bps and weighted identically.
   //
   // The fee WIRE lives in sysio.reserv's custody, so it must be swept here
   // before the payouts below can spend it. drainrewards is queued FIRST (ahead
   // of every payout transfer): inline actions execute depth-first, so the drain
   // -- and the reserv->sysio transfer it queues -- run to completion before any
   // sibling payout queued after it, landing the WIRE in this account's balance
   // first. MUST remain ahead of the first send_wire_transfer below.
   //
   // Fees are funded by that transfer, NOT the T5 treasury, so fee payouts are
   // tracked in `fee_paid` and excluded from total_distributed (which governs
   // the emission curve). Any fee not distributed (producer round-scaling,
   // skipped slashed/terminated recipients, integer-division remainders) stays
   // in this treasury, exactly as undistributed emission does.
   const int64_t fee_total = get_reserv_rewards_balance();
   if (fee_total > 0) {
      sysio::action(
         {get_self(), "active"_n},
         RESERV_CONTRACT,
         "drainrewards"_n,
         std::make_tuple(fee_total)
      ).send();
   }
   const int64_t fee_producer_pool = split_bps(fee_total, cfg.producer_bps);
   const int64_t fee_batch_pool    = fee_total - fee_producer_pool;

   int64_t actual_paid = 0; // emission actually transferred (counts toward total_distributed)
   int64_t fee_paid    = 0; // swap-fee rewards actually transferred (does NOT count toward treasury)

   // =======================================================================
   // Producer + standby pay. Active producers (rank 1..21) are paid in
   // proportion to their eligible_rounds across the pay period; standbys
   // (rank 22..cfg.standby_end_rank) are paid by the existing rank-
   // decreasing weight without an eligible_rounds requirement. Producer
   // counters accumulate across non-pay epochs (no reset by accrueepoch)
   // and are zeroed at the end of this action. Recipients are filtered
   // by opreg status so slashed / terminated operators are skipped.
   // =======================================================================
   {
      auto prod_by_rank = _producers.get_index<"prodrank"_n>();

      // expected_rounds is derived from the configured epoch duration on
      // sysio.epoch (canonical source of truth) scaled by pay_cadence_epochs
      // because elig_rounds accumulates across all epochs in the period.
      const uint32_t epoch_duration_sec = get_epoch_duration_sec();
      uint32_t expected_rounds =
         (epoch_duration_sec * cfg.pay_cadence_epochs * 2) / TOTAL_BLOCKS_PER_ROUND;
      // Below ~126s of effective period duration (one full 21-producer round
      // at 0.5s/block), expected_rounds truncates to zero. Falling back to 1
      // keeps the pay formula well-defined -- producer pay collapses to
      // "elig_rounds clamped to 1, pay = full_share" at the floor. This
      // coarse-grained pay is the price of allowing sub-rotation period
      // durations; documented at MIN_EPOCH_DURATION_SEC.
      if (expected_rounds == 0) expected_rounds = 1;

      struct prod_entry {
         name     owner;
         uint32_t weight;
         uint32_t elig_rounds;
         bool     is_standby;
      };
      std::vector<prod_entry> eligible;
      std::vector<name>       to_reset; // snapshot before modify: avoids
                                         // iterating while mutating secondary idx
      uint32_t total_weight = 0;

      // Single pass over the rank-ordered producers: builds both the pay list
      // (eligible) and the counter-reset list (to_reset). The lists differ --
      // to_reset includes slashed / terminated producers with stale counters,
      // eligible does not.
      for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
         if (it->rank > cfg.standby_end_rank) break;

         // Reset list: every rank-ranged producer with stale counters gets
         // reset, regardless of is_active / opreg status. Slashed producers
         // still need their counters cleared for the next epoch.
         if (it->unpaid_blocks > 0 || it->eligible_rounds > 0 || it->current_round_blocks > 0) {
            to_reset.push_back(it->owner);
         }

         if (!it->is_active) continue;
         // opreg filter: skip slashed / terminated / unknown
         if (!is_op_active(it->owner)) continue;

         uint32_t w      = 0;
         bool     standby = false;
         uint32_t rounds  = 0;

         if (it->rank >= 1 && it->rank <= ACTIVE_PRODUCER_COUNT) {
            rounds = it->eligible_rounds;
            if (it->current_round_blocks >= min_blocks_per_round_for_pay) rounds++;
            if (rounds == 0) continue;
            w = ACTIVE_PRODUCER_WEIGHT;
         } else if (it->rank >= STANDBY_START_RANK && it->rank <= cfg.standby_end_rank) {
            w       = cfg.standby_end_rank + 1 - it->rank;
            standby = true;
         }

         if (w > 0) {
            eligible.push_back({it->owner, w, rounds, standby});
            total_weight += w;
         }
      }

      int64_t distributed_to_producers = 0; // emission portion
      int64_t fee_to_producers         = 0; // swap-fee portion
      if (total_weight > 0) {
         for (const auto& pe : eligible) {
            // Emission and fee shares use the same weight and the same
            // round-scaling, so a producer's fee tracks its emission reward.
            const int64_t emis_share = static_cast<int64_t>(
               static_cast<__int128>(producer_pool) * pe.weight / total_weight);
            const int64_t fee_share = static_cast<int64_t>(
               static_cast<__int128>(fee_producer_pool) * pe.weight / total_weight);
            int64_t emis_pay, fee_pay;
            if (pe.is_standby) {
               emis_pay = emis_share;
               fee_pay  = fee_share;
            } else {
               uint32_t r = (pe.elig_rounds > expected_rounds) ? expected_rounds : pe.elig_rounds;
               emis_pay = static_cast<int64_t>(
                  static_cast<__int128>(emis_share) * r / expected_rounds);
               fee_pay = static_cast<int64_t>(
                  static_cast<__int128>(fee_share) * r / expected_rounds);
            }
            const int64_t pay = emis_pay + fee_pay;
            if (pay > 0) {
               // One transfer carries both the emission and the fee share.
               send_wire_transfer(get_self(), pe.owner, pay, memo::producer_reward);
               distributed_to_producers += emis_pay;
               fee_to_producers         += fee_pay;
            }
         }
      }

      actual_paid += distributed_to_producers;
      fee_paid    += fee_to_producers;

      // Reset round-tracking after distribution (iteration-safe: uses PK snapshot).
      for (const auto& owner : to_reset) {
         auto key = producer_key_t{owner.value};
         _producers.modify(same_payer, key, [&](auto& p) {
            _gstate.total_unpaid_blocks -= p.unpaid_blocks;
            p.unpaid_blocks        = 0;
            p.eligible_rounds      = 0;
            p.current_round_blocks = 0;
            p.last_block_num       = no_prev_block;
         });
      }
   }

   // =======================================================================
   // Batch-op pay. With pay_cadence_epochs > 1 the active group can rotate
   // multiple times across a period, so each group's slice is weighted by
   // its active-epoch count (state.batch_group_epochs[g]) over the period.
   // sum(batch_group_epochs) == pay_cadence_epochs by construction. Groups
   // that were active in zero epochs are skipped (only possible when
   // pay_cadence < batch_op_groups.size()); their slice stays in treasury.
   // Members not registered as ACTIVE in sysio.opreg (slashed / terminated /
   // unknown) are skipped and their slice remains in the treasury.
   // =======================================================================
   if (cfg.pay_cadence_epochs > 0 && !batch_op_groups.empty()) {
      for (size_t g = 0; g < batch_op_groups.size(); ++g) {
         const auto& group = batch_op_groups[g];
         if (group.empty()) continue;
         const uint32_t group_epochs =
            (g < state.batch_group_epochs.size()) ? state.batch_group_epochs[g] : 0;
         if (group_epochs == 0) continue;

         // Period-weighted slices for this group, divided evenly among members.
         // Emission and fee are weighted identically (by the group's active-epoch
         // count over the period) so a member's fee tracks its emission reward.
         const int64_t members = static_cast<int64_t>(group.size());
         const int64_t group_pool = static_cast<int64_t>(
            static_cast<__int128>(batch_pool) * group_epochs / cfg.pay_cadence_epochs);
         const int64_t fee_group_pool = static_cast<int64_t>(
            static_cast<__int128>(fee_batch_pool) * group_epochs / cfg.pay_cadence_epochs);
         const int64_t per_member     = group_pool / members;
         const int64_t fee_per_member = fee_group_pool / members;

         for (const auto& m : group) {
            if (!is_op_active(m)) continue;
            // One transfer carries both the emission and the fee share.
            send_wire_transfer(get_self(), m, per_member + fee_per_member, memo::batch_op_reward);
            actual_paid += per_member;
            fee_paid    += fee_per_member;
         }
      }
   }

   // =======================================================================
   // Category buckets: fixed accounts, no opreg filter. Capital is NOT paid
   // here -- it drains lazily via fundclaim per incoming OPP claim, so
   // dclaim has WIRE the moment the claim is credited rather than waiting
   // for the next pay-epoch.
   // =======================================================================
   send_wire_transfer(get_self(), CAPEX_OPERATIONS_ACCOUNT, capex_amount,      memo::capex);
   send_wire_transfer(get_self(), GOVERNANCE_ACCOUNT, governance_amount, memo::governance);

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
   std::fill(state.batch_group_epochs.begin(), state.batch_group_epochs.end(), 0);
   state.period_start_epoch = epoch_index + 1;

   t5s.set(state, get_self());

   // Audit log: records the AUTHORIZED period emission + the four category
   // amounts for the period that just paid, plus the swap-fee rewards folded
   // into the compute distribution (fee_distributed, sourced from swap fees
   // rather than the treasury). (Producer / batch-op sub-distribution is
   // implicit -- recipients are in traces.) One row per pay-epoch;
   // non-pay-epochs have no audit-log row.
   epochlog_t epoch_table(get_self());
   epoch_table.emplace(get_self(), epochlog_key{epoch_index}, epoch_log{
      .sysio_epoch_index = epoch_index,
      .epoch_count       = state.epoch_count,
      .timestamp         = state.last_epoch_time,
      .total_emission    = period_emission,
      .compute_amount    = compute_amount,
      .capex_amount      = capex_amount,
      .governance_amount = governance_amount,
      .fee_distributed   = fee_paid,
   });

   // Head-first prune of the audit log past its retention cap. Rows are added
   // monotonically (one per successful payepoch) so live_count is computed in
   // O(1) from id arithmetic. Drop up to two oldest rows per call: only one
   // is needed in steady state, but a recent retention-cap shrink (governance
   // lowering epoch_log_retention_count from N to a smaller M) leaves the
   // table over cap by N - M; pruning two per call drains it twice as fast
   // without unbounded CPU per epoch.
   for (int i = 0; i < 2; ++i) {
      auto first_it = epoch_table.begin();
      if (first_it == epoch_table.end()) break;
      const uint64_t oldest_index = first_it.key().sysio_epoch_index;
      const uint64_t live_count =
         (static_cast<uint64_t>(epoch_index) + 1) - oldest_index;
      if (live_count <= cfg.epoch_log_retention_count) break;
      epoch_table.erase(first_it);
   }
}

// fundclaim - transfer up to `amount` WIRE from sysio's drainable pool to
// sysio.dclaim. Called inline by sysio.dclaim::onreward as each
// STAKING_REWARD attestation lands, so dclaim is funded against the credit
// it just took on before the staker can attempt to claim.
//
// Never throws. STAKING_REWARD dispatch from sysio.msgch must not be
// aborted on emissions-side conditions (the never-throw contract for OPP
// inbound handlers), so a pool-too-small case caps the transfer at what's
// available and accrues the unfunded delta to t5state.capital_shortfall_total
// for operator visibility.
//
// The transfer cap is the minimum of three caps that all must hold:
//   * `amount`                                                -- requested
//   * `lifetime headroom - pending_emission_amount`           -- accounting
//   * `sysio WIRE balance - pending_emission_amount`          -- balance
// Both accounting and balance caps reserve `pending_emission_amount` for
// the next payepoch. `pending_emission_amount` is curve emission already
// accrued via accrueepoch but not yet transferred; payepoch will distribute
// it across compute/capital/capex/governance. Drawing against those funds
// here would either trip the emissions readiness gate at the next epoch
// boundary (BALANCE_INSUFFICIENT) or cause the inline payepoch transfer
// to throw "overdrawn balance". The balance cap is the load-bearing one
// because `sysio.token::transfer` itself throws on overdraw, which would
// abort the inbound STAKING_REWARD OPP dispatch.
//
// Negative or zero requests are silent no-ops (defensive). The amount
// actually transferred counts toward total_distributed -- the curve sees
// less remaining headroom on its next per-epoch computation and emissions
// auto-throttle to match real claim load.
void system_contract::fundclaim(int64_t amount) {
   require_auth(CAPITAL_ACCOUNT);

   if (amount <= 0) return;

   t5state_t t5s(get_self());
   if (!t5s.exists()) return; // pre-init: silently absorb
   auto state = t5s.get();

   const auto cfg = get_emit_cfg(get_self());
   const int64_t lifetime_headroom    = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
   const int64_t pending_reserve      = state.pending_emission_amount;
   const int64_t sysio_balance        = get_wire_balance(get_self());
   const int64_t accounting_available = lifetime_headroom - pending_reserve;
   const int64_t balance_available    = sysio_balance     - pending_reserve;

   const int64_t cap = std::min({amount, accounting_available, balance_available});
   const int64_t to_transfer = (cap > 0) ? cap : 0;
   const int64_t shortfall   = amount - to_transfer;

   if (to_transfer > 0) {
      send_wire_transfer(get_self(), CAPITAL_ACCOUNT, to_transfer, memo::capital);
      state.total_distributed += to_transfer;
   }

   if (shortfall > 0) {
      state.capital_shortfall_total += shortfall;
   }

   if (to_transfer > 0 || shortfall > 0) {
      t5s.set(state, get_self());
   }
}

emissions::epoch_info_result system_contract::viewepoch() {
   const auto cfg = get_emit_cfg(get_self());

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");

   const auto state = t5s.get();
   const auto now   = time_point_sec{current_time_point()};

   int64_t remaining = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
   if (remaining < 0) remaining = 0;

   const uint32_t epoch_duration_sec = get_epoch_duration_sec();

   int64_t next_est;
   if (state.epoch_count == 0) {
      next_est = emissions::scale_annual_to_epoch(cfg.annual_initial_emission, epoch_duration_sec);
      if (next_est > remaining) next_est = remaining;
   } else {
      next_est = emissions::compute_epoch_emission(
         cfg, epoch_duration_sec, state.last_epoch_emission, state.total_distributed);
   }

   uint32_t secs_until = 0;
   const uint64_t next_epoch_time =
      static_cast<uint64_t>(state.last_epoch_time.sec_since_epoch()) + epoch_duration_sec;
   if (now.sec_since_epoch() < next_epoch_time) {
      secs_until = static_cast<uint32_t>(next_epoch_time - now.sec_since_epoch());
   }

   return emissions::epoch_info_result{
      .epoch_count         = state.epoch_count,
      .last_epoch_index    = state.last_epoch_index,
      .last_epoch_time     = state.last_epoch_time,
      .last_epoch_emission = state.last_epoch_emission,
      .total_distributed   = state.total_distributed,
      .treasury_remaining  = remaining,
      .next_emission_est   = next_est,
      .seconds_until_next  = secs_until,
   };
}

emissions::emission_config system_contract::viewemitcfg() {
   return get_emit_cfg(get_self());
}

} // namespace sysiosystem
