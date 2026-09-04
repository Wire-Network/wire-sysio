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
constexpr uint32_t MAX_STANDBY_END_RANK   = 100; // safety cap: bounds the standby credit count in payepoch
constexpr int64_t  MS_PER_SECOND          = 1000;

// Basis-point denominator for all category / sub-split ratios.
constexpr int64_t  BPS_DENOMINATOR        = 10000;

constexpr sysio::name CAPITAL_ACCOUNT            = "sysio.dclaim"_n;
constexpr sysio::name GOVERNANCE_ACCOUNT         = "sysio.gov"_n;
// Capex ("capital expenditure") bucket lives on sysio.ops -- operational spend.
constexpr sysio::name CAPEX_OPERATIONS_ACCOUNT   = "sysio.ops"_n;
constexpr sysio::name TOKEN_CONTRACT             = "sysio.token"_n;
constexpr sysio::name ROA_CONTRACT               = "sysio.roa"_n;
// sysio.reserv holds the swap-fee rewards bucket that payepoch folds into the
// per-epoch batch-operator distribution.
constexpr sysio::name RESERV_CONTRACT            = "sysio.reserv"_n;

namespace memo {
   constexpr std::string_view capital          = "T5 capital";
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

// Local layout-compatible view of sysio.reserv's rewards_bucket singleton. A
// [[sysio::table]]-attributed struct cannot be shared into sysio.system's
// translation unit -- doing so corrupts this contract's read-only-action return
// codegen (getpeerkeys) -- so the layout is mirrored here. The kv row is keyed
// by table name ("rewardbkt") + the reserv account scope, so this reads the
// exact bytes reserv wrote. MUST stay in lockstep with sysio.reserv.hpp's
// rewards_bucket (balance, lifetime_accrued); the cross-contract read is
// exercised end-to-end by t5_emissions_tests/payepoch_folds_swap_fee_rewards,
// which fails if the two layouts ever diverge.
struct reserv_rewards_bucket {
   uint64_t balance          = 0;
   uint64_t lifetime_accrued = 0;
   SYSLIB_SERIALIZE(reserv_rewards_bucket, (balance)(lifetime_accrued))
};
using reserv_rewardbkt_t = sysio::kv::global<"rewardbkt"_n, reserv_rewards_bucket>;

// Read the live swap-fee rewards balance held in sysio.reserv's custody.
// Returns 0 when never accrued. Clamps to asset::max_amount because the value is
// later carried as a sysio::asset (drained + transferred as WIRE); the bucket is
// backed by real WIRE so the clamp is only a defensive guard against accounting
// drift constructing an out-of-range asset.
int64_t get_reserv_rewards_balance() {
   reserv_rewardbkt_t bkt(RESERV_CONTRACT);
   const uint64_t bal = bkt.get_or_default(reserv_rewards_bucket{}).balance;
   constexpr uint64_t max_amt = static_cast<uint64_t>(sysio::asset::max_amount);
   return bal > max_amt ? sysio::asset::max_amount : static_cast<int64_t>(bal);
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
// THREE call sites remain, and every one targets a PROTOCOL-CONTROLLED account. What makes each
// safe differs, and the difference is the thing to preserve:
//
//   * `fundclaim`             -> `sysio.dclaim`  — a DEPLOYED contract, not a bare account. Safe
//                                                  because it is protocol-controlled and its code
//                                                  has no failing `sysio.token::transfer` notify
//                                                  path; that invariant must hold as dclaim evolves.
//   * `payepoch` (capex)      -> `sysio.ops`     — no code deployed
//   * `payepoch` (governance) -> `sysio.gov`     — no code deployed
//
// The two `payepoch` pushes are on the never-throw `advance` path, so those two accounts are the
// ones that could abort epoch advancement if code were ever deployed on them — see the standing
// constraint at that call site. Adding a fourth AUTOMATIC destination outside protocol control
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

   // The expiry stamp is RECORDED, not acted on: no sweep is wired against `payclaims` (see the
   // note on the table). Refreshing it on every credit means an account still being paid never
   // ages, so the stamp already carries the "last had activity" signal a future retention pass
   // (WIRE-339) needs, rather than that history starting the day a sweep lands.
   const uint32_t now_sec = current_time_point().sec_since_epoch();

   payclaims_t claims(self);
   sysio::opp::claimable::credit(claims, self, payclaim_key{to.value},
                                 pay_claim{.account_name = to}, amt,
                                 now_sec + PAY_CLAIM_WINDOW_SEC);

   // Reserve the credited WIRE so fundclaim and the epoch gate cannot re-commit it. Saturates on
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

// Canonical epoch duration lives on sysio.epoch::epochcfg. Both payepoch
// (the producer pay period's slot count) and viewepoch (seconds_until_next) read it
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
   sysio::check(cfg.standby_bps <= BPS_DENOMINATOR,
                 "standby_bps must be <= 10000");

   // Audit-log retention
   sysio::check(cfg.epoch_log_retention_count > 0,
                 "epoch_log_retention_count must be positive");

   // Pay cadence (number of epochs accumulated per payepoch firing). Zero
   // would divide-by-zero in the period share-by-rounds math. This upper bound
   // caps retained history; the joint check after epochcfg is loaded separately
   // caps expensive per-recipient payout work.
   sysio::check(cfg.pay_cadence_epochs > 0,
                 "pay_cadence_epochs must be positive");
   sysio::check(cfg.pay_cadence_epochs <= emissions::MAX_PAY_CADENCE_EPOCHS,
                 "pay_cadence_epochs exceeds batch roster history safety cap");

   // Single read of sysio.epoch::epochcfg shared by the round-to-zero guards
   // (which need epoch_secs to scale annual values) and the post-init guard
   // (which compares per-epoch floor against remaining distributable).
   sysio::epoch::epochcfg_t epoch_cfg_tbl(epoch_refs::account);
   const bool epoch_configured = epoch_cfg_tbl.exists();
   const auto epoch_cfg = epoch_configured
      ? epoch_cfg_tbl.get()
      : sysio::epoch::epoch_config{};
   const uint32_t epoch_secs = epoch_cfg.epoch_duration_sec;

   // Single read of t5_state shared by the period-accrual bound (which needs
   // the already-accrued pending amount) and the post-init brick guards below.
   t5state_t t5s(get_self());
   const bool t5_initialized = t5s.exists();
   const t5_state t5now      = t5_initialized ? t5s.get() : t5_state{};

   // If sysio.epoch is configured, sanity-check that each nonzero annual
   // value scales to a non-zero per-epoch share at the canonical
   // epoch_duration_sec. Without this guard, a tiny annual value can round
   // down to 0 in scale_annual_to_epoch, the gate sees emission_amount = 0,
   // and emissions silently disable. Skipped pre-bootstrap (sysio.epoch not
   // yet configured); the same check fires on the next setemitcfg call.
   if (epoch_configured) {
      sysio::check(
         emissions::batch_payout_work_fits(cfg.pay_cadence_epochs,
                                            epoch_cfg.operators_per_epoch),
         "pay_cadence_epochs x operators_per_epoch exceeds the batch payout credit safety cap (100)");

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
      // gate demands a balance no treasury can hold and epoch advancement
      // blocks permanently. sysio.epoch::setconfig enforces the same bound when
      // epoch_duration_sec changes.
      sysio::check(
         emissions::period_accrual_fits_asset_range(cfg, epoch_secs, t5now.pending_emission_amount),
         "per-epoch emission ceiling x pay_cadence_epochs exceeds the asset range at current epoch_duration_sec");
   }

   // If t5_state already exists, prevent config changes that would brick future
   // emissions. Post-init, remaining distributable must still cover what's been
   // paid, and the per-epoch floor (derived from annual_min_emission and the
   // canonical epoch_duration_sec) can't exceed what's left to distribute.
   // initt5 requires sysio.epoch to be configured, so t5_initialized implies
   // epoch_configured -- safe to use epoch_secs directly.
   if (t5_initialized) {
      sysio::check(cfg.t5_distributable >= cfg.t5_floor + t5now.total_distributed,
                    "t5_distributable must cover floor + already-distributed");
      const int64_t remaining = cfg.t5_distributable - cfg.t5_floor - t5now.total_distributed;
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

   // Node-owner vesting draws on the SAME `sysio` WIRE balance that backs unclaimed epoch pay, so
   // it must respect the same reserve `fundclaim` and the epoch readiness gate hold. `payclaims`
   // rows are already owed: paying them out is a promise this contract has made and cannot revoke,
   // whereas this withdrawal is a claim on the free remainder.
   //
   // Without the reserve the two overdraw each other. Credit 60 of epoch pay against a 100
   // balance, let a vested owner withdraw 50, and only 50 backs the 60 owed -- the later
   // `claimpay` cannot pay out, and `payepoch` stays balance-blocked from then on. That exposure
   // is new: before payouts became claimable, epoch pay had already left the treasury by the time
   // this ran, so there was nothing outstanding for it to spend into.
   //
   // Rejecting rather than capping keeps `claimed` exactly in step with what was transferred; the
   // claim stays fully available and succeeds once claims are pulled or emissions refill the
   // treasury. `claimnodedis` is user-initiated, so the throw reaches the caller who asked.
   payclaimtot_t nd_tot_tbl(get_self());
   const int64_t nd_outstanding =
      static_cast<int64_t>(nd_tot_tbl.get_or_default(pay_claim_total{}).outstanding);
   const int64_t nd_spendable = get_wire_balance(get_self()) - nd_outstanding;
   sysio::check(info.claimable.amount <= nd_spendable,
                "treasury balance is reserved against unclaimed epoch pay; try again later");

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
// decremented in lockstep, releasing the reserve that fundclaim and the epoch gate hold against it.
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
   // underflowing into a huge value that would freeze fundclaim and the epoch gate permanently.
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

   // Saturating accumulate (shared helper in emissions.hpp). accrueepoch runs
   // as an inline action from sysio.epoch::advance, so this must never throw --
   // a throw would abort epoch advancement chain-wide. The readiness gate
   // precomputed the identical saturated total as period_emission and payepoch
   // asserts equality against it, so the three sites cannot diverge. The
   // period-accrual bound enforced by setemitcfg / sysio.epoch::setconfig keeps
   // any accepted configuration from actually reaching the clamp; saturating
   // here is defense-in-depth behind those boundary checks.
   state.pending_emission_amount =
      saturating_accrue(state.pending_emission_amount, per_epoch_emission);

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

// rcrdbatch - retain the exact roster that accrued this epoch. The schedule
// mutates before advance queues its inline actions, so a current position is
// not a stable identity for an earlier epoch.
void system_contract::rcrdbatch(uint32_t epoch_index, std::vector<sysio::name> members) {
   require_auth(epoch_refs::account);

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");
   const auto state = t5s.get();
   sysio::check(epoch_index == state.last_epoch_index,
                "rcrdbatch must run after accrueepoch for the same epoch_index");

   // The scheduler supplies canonical order today. Sorting here keeps the
   // table identity stable even if a future scheduler changes that detail.
   std::sort(members.begin(), members.end());

   batchepochs_t history(get_self());
   const batch_epoch_key key{epoch_index};
   sysio::check(!history.contains(key), "batch roster already recorded for epoch");

   // Never let an old stored cadence make this mandatory inline action throw.
   // Normal advances pay at most every MAX_PAY_CADENCE_EPOCHS, but the exact
   // oldest key probe also heals a pre-bound configuration without
   // deserializing every historical roster on each advance.
   if (epoch_index > emissions::MAX_PAY_CADENCE_EPOCHS) {
      const batch_epoch_key oldest_retained{
         static_cast<uint32_t>(epoch_index - emissions::MAX_PAY_CADENCE_EPOCHS)};
      if (history.contains(oldest_retained)) {
         history.erase(oldest_retained);
      }
   }

   history.emplace(get_self(), key, batch_epoch{
      .sysio_epoch_index = epoch_index,
      .members           = members,
   });
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
// Swap-fee rewards: when immutable roster history is complete and contains at
// least one non-empty roster, the batch-operator share of collected swap fees
// (sysio.reserv's rewards_bucket) is swept here via an inline drainrewards and
// allocated EXCLUSIVELY to the batch-operator distribution, on top of their
// emission share and weighted by the same historical-roster active-epoch count.
// Incomplete or all-empty history leaves the bucket in sysio.reserv for a later
// payable period.
// Producers are NOT paid out of swap fees, so producer_bps / batch_op_bps govern
// the emission split only -- see the fold-in comment at the drain. Allocated is
// not paid: only ELIGIBLE shares go out, and any skipped amount from a completed
// sweep stays in this treasury. The audit row records retained batch emission,
// retained swept fees, and whether roster history was complete. Fees are funded
// by the sweep (not the treasury) and so are excluded from total_distributed.
//
// Single-trx semantics guarantee gate conditions hold through this call;
// payepoch trusts the gate-computed period_emission and does not recompute.
// Strict sysio::check throws inside payepoch flag true bugs (arithmetic
// invariants, BPS sums).
//
// Slashed / terminated batch-op group members are skipped via opreg filter;
// their slice remains in the treasury.
void system_contract::payepoch(uint32_t epoch_index,
                               std::vector<std::vector<sysio::name>>,
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

   // ----- The period's ACTUAL length, in accrued epochs -----
   // BOTH distributions below normalize by this, and NEITHER may use
   // cfg.pay_cadence_epochs for it. accrueepoch increments one batch_group_epochs
   // slot per epoch unconditionally, while setemitcfg may change
   // pay_cadence_epochs at any time (taking effect on the next advance), so the
   // configured cadence and the epochs this period actually spans can disagree --
   // lowering 3->1 after one accrual leaves the counters summing to 2 against a
   // configured 1. Deriving the divisor from the counters themselves keeps both
   // normalizations correct whatever the config did mid-period.
   //
   // Sum in int64: each counter is a uint32 epoch tally and the vector is sized
   // from a scheduler-bounded group list, so the total cannot approach the int64 range. Zero is
   // impossible in practice (payepoch asserts accrueepoch ran for this same
   // epoch_index, and accrueepoch always increments a slot) but is guarded at
   // each use, because a zero divisor would abort the whole advance chain.
   int64_t accrued_epochs = 0;
   for (const uint32_t group_epoch_count : state.batch_group_epochs) {
      accrued_epochs += group_epoch_count;
   }

   // Preserve roster identity separately from the legacy positional counters.
   // advance() slides its schedule before queueing this action, so a counter at
   // position g cannot identify the roster that was active in a prior epoch.
   struct recorded_batch_group {
      std::vector<sysio::name> members;
      uint32_t                 active_epochs = 0;
   };

   batchepochs_t batch_history(get_self());
   std::vector<recorded_batch_group> recorded_batch_groups;
   bool batch_history_complete = accrued_epochs > 0;
   bool has_nonempty_batch_roster = false;
   int64_t recorded_epochs = 0;
   uint32_t batch_payout_credits = 0;
   uint64_t expected_epoch_index = state.period_start_epoch;

   for (auto it = batch_history.begin(); it != batch_history.end(); ++it) {
      // A stale/corrupted table must not make the mandatory payepoch inline
      // action abort. Bound deserialization to the configured safety window,
      // retain the batch slice, and clear the table below so the next period
      // starts from a fresh immutable roster history.
      if (recorded_epochs == emissions::MAX_PAY_CADENCE_EPOCHS) {
         batch_history_complete = false;
         break;
      }
      ++recorded_epochs;

      // A clean activation may initialize T5 after sysio.epoch has already
      // advanced. In that first period, the earliest recorded roster defines
      // the start rather than an obsolete literal epoch-one assumption.
      if (expected_epoch_index == 0) {
         expected_epoch_index = it->sysio_epoch_index;
      }
      if (static_cast<uint64_t>(it->sysio_epoch_index) != expected_epoch_index) {
         batch_history_complete = false;
      }
      ++expected_epoch_index;

      auto group_it = std::find_if(
         recorded_batch_groups.begin(), recorded_batch_groups.end(),
         [&](const auto& group) { return group.members == it->members; });
      if (group_it == recorded_batch_groups.end()) {
         const uint64_t credits_with_group =
            static_cast<uint64_t>(batch_payout_credits) + it->members.size();
         if (credits_with_group > emissions::MAX_BATCH_PAYOUT_CREDITS) {
            batch_history_complete = false;
         } else {
            batch_payout_credits = static_cast<uint32_t>(credits_with_group);
            has_nonempty_batch_roster =
               has_nonempty_batch_roster || !it->members.empty();
            recorded_batch_groups.push_back(recorded_batch_group{
               .members       = it->members,
               .active_epochs = 1,
            });
         }
      } else {
         group_it->active_epochs += 1;
      }
   }

   batch_history_complete =
      batch_history_complete
      && recorded_epochs == accrued_epochs
      && expected_epoch_index == static_cast<uint64_t>(epoch_index) + 1;

   // ----- Swap-fee rewards fold-in -----
   // The BATCH-OPERATOR half of collected swap fees accrues in sysio.reserv's
   // rewards_bucket. The other half accrues per-underwriter in sysio.reserv and
   // is drawn by that account's own `claimuwfee` — it never passes through this
   // treasury. (When reserv's `fee_emissions_share_bps` dial is non-zero, that
   // configured share of the batch-op half is transferred straight to this
   // account at collection time and never enters the bucket; the dial defaults
   // to zero, leaving the whole half here.) Fold the whole bucket into THIS
   // period's batch-operator distribution so batch ops receive it alongside
   // emissions, weighted identically by active-epoch count.
   //
   // Producers are NOT paid out of swap fees: the fee compensates the parties
   // that carry an individual swap — the underwriter who locks collateral for it
   // and the batch operators who relay it — while producers earn emissions for
   // securing the chain. So `producer_bps` / `batch_op_bps` govern the emission
   // `compute_amount` split only, and the entire drained fee pool goes to the
   // batch-op distribution below.
   //
   // The fee WIRE lives in sysio.reserv's custody. Sweep it only when immutable
   // roster history is complete and at least one roster can receive a share;
   // otherwise leave the bucket in reserv so a later payable period can
   // distribute it. When swept, drainrewards is queued FIRST
   // (ahead of every payout transfer): inline actions execute depth-first, so the
   // drain -- and the reserv->sysio transfer it queues -- run to completion before
   // any sibling payout queued after it, landing the WIRE in this account's balance
   // first. MUST remain ahead of the first send_wire_transfer below.
   //
   // Fees are funded by that transfer, NOT the T5 treasury, so fee payouts are
   // tracked in `fee_paid` and excluded from total_distributed (which governs
   // the emission curve). After a sweep, any amount skipped for an empty roster
   // alongside a non-empty one, non-ACTIVE members, or integer-division
   // remainders stays in this treasury. Incomplete or all-empty history leaves
   // the entire bucket in reserv.
   int64_t fee_batch_pool = 0;
   if (batch_history_complete && has_nonempty_batch_roster) {
      fee_batch_pool = get_reserv_rewards_balance();
      if (fee_batch_pool > 0) {
         sysio::action(
            {get_self(), "active"_n},
            RESERV_CONTRACT,
            "drainrewards"_n,
            std::make_tuple(fee_batch_pool)
         ).send();
      }
   }

   // "paid" here means DISTRIBUTED -- credited to `payclaims` for producers / standbys /
   // batch operators, transferred for the category buckets. Both leave the treasury's
   // spendable position, which is what these counters feed.
   int64_t actual_paid = 0; // emission actually distributed (counts toward total_distributed)
   int64_t batch_emission_paid = 0;
   int64_t fee_paid    = 0; // swap-fee rewards actually distributed (does NOT count toward treasury)

   // =======================================================================
   // Producer + standby pay.
   //
   // Producers are paid PER BLOCK. The active slice of the producer pool is spread over the block
   // slots the period held, and every schedulable producer is credited that rate for each block
   // it made. A missed block is never counted, so its pay stays in the treasury: it does not flow
   // to the producers that did show up, because the rate does not depend on who did. The divisor
   // is the period's nominal slot count, raised to the blocks actually produced when a period runs
   // long (an epoch can extend while a batch operator delivers), so the slice is never exceeded.
   //
   // Standbys (positions 22..cfg.standby_end_rank) draw a retainer from the standby slice
   // (cfg.standby_bps of the pool). Each POSITION holds a fixed share, decaying linearly from
   // position 22, over the constant sum of every position's weight -- a vacant position's share
   // stays in the treasury. Block pay is not gated on position, so a producer that slid from 21
   // to 22 mid-period is still paid for the blocks it made before the schedule caught up.
   //
   // Counters accumulate across non-pay epochs (no reset by accrueepoch) and are zeroed at the
   // end of this action for every producer PAID by it. A producer that is not schedulable when the
   // walk reaches it -- keyless, or one whose standing ended since its last rescore -- is neither
   // paid nor reset: its block count waits for the first payepoch where it is schedulable again
   // (a re-keyed producer's return; a terminated operator's, should it settle and re-register;
   // never, for a slashed one, whose row is never pruned and which `regoperator` refuses). A
   // producer BELOW the walk (demoted, parked, unbonded, slashed, terminated -- each rescored at
   // the event) is not visited at all, with the same effect. Every block a producer makes is paid
   // exactly once, at the first payepoch where it is payable.
   // =======================================================================
   {
      auto prod_by_rank = _producers.get_index<"prodrank"_n>();

      const int64_t standby_pool = split_bps(producer_pool, cfg.standby_bps);
      const int64_t active_pool  = producer_pool - standby_pool;

      // Nominal block slots in the period: the configured epoch duration (canonical on
      // sysio.epoch) times the epochs the period ACTUALLY accrued -- never
      // cfg.pay_cadence_epochs, which a mid-period change makes disagree with the accrual --
      // at one slot per block interval. uint64: a 30-day epoch times a large cadence overflows
      // uint32.
      const uint64_t nominal_slots =
         static_cast<uint64_t>(get_epoch_duration_sec())
         * static_cast<uint64_t>(accrued_epochs > 0 ? accrued_epochs : 1)
         * static_cast<uint64_t>(MS_PER_SECOND)
         / static_cast<uint64_t>(sysio::block_timestamp::block_interval_ms);

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
         bool blocks;   // a paid row starts its count over; an unpayable row keeps it
      };
      std::vector<pay_entry>   entries;
      std::vector<reset_entry> to_reset; // snapshot before modify: avoids
                                          // iterating while mutating secondary idx
      uint64_t produced_blocks = 0;

      // Single pass over the rank-ordered producers: builds both the pay list (entries) and the
      // counter-reset list (to_reset). `position` is POSITION in this index among SCHEDULABLE
      // producers, counted while walking -- not a stored ordinal. The demoted tier sorts last and
      // is never schedulable, so it bounds the walk over what is a permissionless, unbounded
      // table; every row above it was a live, bonded producer operator at its last rescore, and
      // every event that ends that standing rescores the row (see producer_rank::compute).
      //
      // The divisor counts exactly the blocks this payepoch pays for. A count that waits on an
      // unpayable row is neither paid nor counted now; when its producer is payable again the
      // carried blocks are paid at THAT period's rate and counted in THAT period's divisor.
      uint32_t position = 0;
      for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
         if (producer_rank::tier_of(it->rank_score) == producer_tier::demoted) break;

         // is_schedulable requires an active row, ACTIVE opreg status, and an active finalizer
         // key: a producer missing any of them can never be scheduled, so it draws neither block
         // pay nor a standby retainer -- and keeps its block count for when it can. The snapshot
         // counter is per period regardless.
         if (!producer_rank::is_schedulable(*it, _finalizers)) {
            if (it->snapshot_attestations > 0) to_reset.push_back({it->owner, false});
            continue;
         }

         produced_blocks += it->unpaid_blocks;
         if (it->unpaid_blocks > 0 || it->snapshot_attestations > 0) {
            to_reset.push_back({it->owner, true});
         }

         ++position;
         const bool standby = position >= STANDBY_START_RANK && position <= cfg.standby_end_rank;
         const uint64_t standby_weight = standby ? cfg.standby_end_rank + 1 - position : 0;
         if (it->unpaid_blocks > 0 || standby_weight > 0) {
            entries.push_back({it->owner, it->unpaid_blocks, standby_weight});
         }
      }

      const uint64_t slot_divisor = std::max<uint64_t>(std::max(nominal_slots, produced_blocks), 1);

      // Producers are paid the emission share only — swap fees go to the
      // underwriter + batch operators (see the fold-in comment above).
      int64_t distributed_to_producers = 0;
      for (const auto& entry : entries) {
         int64_t pay = static_cast<int64_t>(
            static_cast<__int128>(active_pool) * entry.blocks / slot_divisor);
         if (entry.standby_weight > 0) {
            pay += static_cast<int64_t>(
               static_cast<__int128>(standby_pool) * entry.standby_weight / standby_weight_sum);
         }
         if (pay > 0) {
            credit_pay(get_self(), entry.owner, pay, memo::producer_reward);
            distributed_to_producers += pay;
         }
      }

      actual_paid += distributed_to_producers;

      // Reset the period's counters after distribution (iteration-safe: uses PK snapshot).
      for (const auto& entry : to_reset) {
         auto key = producer_key_t{entry.owner.value};
         _producers.modify(same_payer, key, [&](auto& p) {
            if (entry.blocks) p.unpaid_blocks = 0;
            p.snapshot_attestations = 0;
         });
         // Zeroing snapshot_attestations moved the snapshot factor; keep the sort key in step.
         rescore_producer(entry.owner);
      }
   }

   // =======================================================================
   // Batch-op pay. Each historical roster receives a slice weighted by its
   // actual active epochs over the period. The legacy counters still supply the
   // actual period length, rather than cfg.pay_cadence_epochs: configuration can
   // change between accruals. Complete immutable history is required for a
   // batch payout. History is bounded by MAX_PAY_CADENCE_EPOCHS and recipient
   // credits by MAX_BATCH_PAYOUT_CREDITS. Incomplete or over-budget history
   // takes the non-halting retention path below, so it cannot abort advance.
   // =======================================================================
   auto pay_batch_group = [&](const std::vector<sysio::name>& group,
                              uint32_t active_epochs) {
      if (group.empty() || active_epochs == 0) return;

      // Period-weighted slices for this group, divided evenly among members.
      // Emission and fee are weighted identically by active-epoch count.
      const int64_t members = static_cast<int64_t>(group.size());
      const int64_t group_pool = static_cast<int64_t>(
         static_cast<__int128>(batch_pool) * active_epochs / accrued_epochs);
      const int64_t fee_group_pool = static_cast<int64_t>(
         static_cast<__int128>(fee_batch_pool) * active_epochs / accrued_epochs);
      const int64_t per_member     = group_pool / members;
      const int64_t fee_per_member = fee_group_pool / members;

      for (const auto& m : group) {
         if (!is_op_active(m, OperatorType::OPERATOR_TYPE_BATCH)) continue;
         // One credit carries both the emission and the fee share.
         credit_pay(get_self(), m, per_member + fee_per_member, memo::batch_op_reward);
         actual_paid += per_member;
         batch_emission_paid += per_member;
         fee_paid    += fee_per_member;
      }
   };

   if (batch_history_complete) {
      for (const auto& group : recorded_batch_groups) {
         pay_batch_group(group.members, group.active_epochs);
      }
   } else if (accrued_epochs > 0) {
      // Do not guess a roster during a mixed-version upgrade or an incomplete
      // first period: retain its batch emission in the treasury, leave swap fees
      // in sysio.reserv, and let the next period establish complete history.
      sysio::print("batch roster history incomplete; retaining batch emission and deferring swap fees\n");
   }

   const int64_t batch_emission_retained = batch_pool - batch_emission_paid;
   const int64_t batch_fee_retained = fee_batch_pool - fee_paid;

   // A pay period is the history lifetime. Clear only after an actual accrued
   // period: on the defensive zero-accrual path the history is preserved rather
   // than silently discarding roster identity without a batch payout. Keep the
   // cleanup bounded so an overlong legacy/corrupt table cannot exhaust the
   // mandatory epoch-advance transaction. Since normal operation adds at most
   // MAX_PAY_CADENCE_EPOCHS rows per period and this removes twice that many,
   // stale history drains monotonically while rewards remain on the audited
   // incomplete-history recovery path.
   if (accrued_epochs > 0) {
      uint32_t cleaned = 0;
      for (auto it = batch_history.begin();
           it != batch_history.end()
              && cleaned < emissions::MAX_BATCH_HISTORY_CLEANUP_ROWS;
           ++cleaned) {
         it = batch_history.erase(it);
      }
   }

   // =======================================================================
   // Category buckets: fixed accounts, no opreg filter. Capital is NOT paid
   // here -- it drains lazily via fundclaim per incoming OPP claim, so
   // dclaim has WIRE the moment the claim is credited rather than waiting
   // for the next pay-epoch.
   // =======================================================================
   // PUSHED, not credited -- the deliberate exception to this contract's pull rule, and the same
   // exception `fundclaim` already makes for `sysio.dclaim`.
   //
   // The pull model exists because a payout to an account the protocol does not control lets that
   // account's transfer-notify handler abort `advance`. These two are protocol-owned holding
   // accounts: they carry no code, so there is no handler to run, and only governance can ever put
   // code there. The threat the credit path defends against does not exist for them.
   //
   // Crediting them instead would strand the money. A claim needs `require_auth(account_name)`,
   // and a `sysio.*` account can neither sign nor be acted for here: `sysio.roa` forces
   // `net_weight`/`cpu_weight` to zero for every account whose prefix is `sysio`
   // (`is_sysio_account`), so they cannot pay for a transaction, and unlike `sysio.dclaim` they
   // have no contract of their own to emit the claim inline. Their pay would accrue in
   // `payclaims` forever while `payclaimtot` reserved the backing WIRE against `fundclaim` and the
   // epoch readiness gate -- a permanent, growing reservation of WIRE nobody can move.
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
   std::fill(state.batch_group_epochs.begin(), state.batch_group_epochs.end(), 0);
   state.period_start_epoch = epoch_index + 1;

   t5s.set(state, get_self());

   // Audit log: records the AUTHORIZED period emission + the four category
   // amounts for the period that just paid, plus the swap-fee rewards folded
   // into the batch-operator distribution (fee_distributed, sourced from swap
   // fees rather than the treasury). (Producer / batch-op sub-distribution is
   // implicit. Those shares are CREDITED to `payclaims` rather than transferred, so a
   // recipient no longer appears as its own transfer trace -- per-recipient attribution
   // comes from the `payclaims` row deltas, and the eventual `claimpay`.) One row per
   // pay-epoch; non-pay-epochs have no audit-log row.
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
      .batch_history_complete  = batch_history_complete,
      .batch_emission_retained = batch_emission_retained,
      .batch_fee_retained      = batch_fee_retained,
   });

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
//   * `sysio WIRE balance - pending_emission_amount
//                         - outstanding payclaims`            -- balance
// Both accounting and balance caps reserve `pending_emission_amount` for
// the next payepoch. `pending_emission_amount` is curve emission already
// accrued via accrueepoch but not yet paid; payepoch will distribute
// it across compute/capital/capex/governance. Drawing against those funds
// here would either trip the emissions readiness gate at the next epoch
// boundary (BALANCE_INSUFFICIENT) or leave payepoch's credits unbacked.
//
// The balance cap ALSO reserves outstanding `payclaims`. Those balances have
// already been credited to producers / standbys / batch operators but not yet
// pulled, so the WIRE backing them is still sitting in this account's
// token balance while being fully owed. Spending it here would leave a later
// `claimpay` unable to transfer, stranding earned pay -- the claimable-payout
// equivalent of the "overdrawn balance" abort this cap has always guarded.
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

   payclaimtot_t claim_tot_tbl(get_self());
   const int64_t claims_reserve =
      static_cast<int64_t>(claim_tot_tbl.get_or_default(pay_claim_total{}).outstanding);

   const auto cfg = get_emit_cfg(get_self());
   const int64_t lifetime_headroom    = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
   const int64_t pending_reserve      = state.pending_emission_amount;
   const int64_t sysio_balance        = get_wire_balance(get_self());
   const int64_t accounting_available = lifetime_headroom - pending_reserve;
   const int64_t balance_available    = sysio_balance - pending_reserve - claims_reserve;

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
