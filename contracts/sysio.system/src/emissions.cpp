#include <sysio.system/sysio.system.hpp>
#include <sysio.system/emissions.hpp>

#include <sysio/crypto.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

#include <vector>

namespace sysiosystem {

using namespace emissions;

// ===========================================================================
// Read-only mirror of sysio.token accounts table (kv::scoped_table).
// The upstream types are private inside sysio::token; we only need read access
// to check sysio's own WIRE balance before emitting.
// ===========================================================================

namespace token_readonly {

struct acct_key {
   uint64_t sym_code;
   SYSLIB_SERIALIZE(acct_key, (sym_code))
};

struct account {
   sysio::asset balance;
   SYSLIB_SERIALIZE(account, (balance))
};

using accounts_t = sysio::kv::scoped_table<"accounts"_n, acct_key, account>;

} // namespace token_readonly

// ===========================================================================
// Read-only cross-contract mirrors (implementation detail).
// ---------------------------------------------------------------------------
// These structs mirror the authoritative table definitions that live on
// sysio.opreg and sysio.epoch. Used for read-only cross-contract access --
// sysio.system never writes to these tables. Field order and serialization
// MUST stay in lock-step with the upstream definitions.
//
// Mirror approach follows the authex_readonly pattern used inside sysio.opreg
// and sysio.epoch for reading sysio.authex::links.
//
// Kept here (not in emissions.hpp) so that sysio.roa can include emissions.hpp
// without pulling protobuf / opp dependencies.
// ===========================================================================

namespace opp_readonly {

// Mirrors sysio::opreg::stake_entry
struct stake_entry {
   sysio::opp::types::ChainAddress chain_addr;
   sysio::opp::types::TokenAmount  amount;
   uint64_t                        timestamp_ms = 0;

   SYSLIB_SERIALIZE(stake_entry, (chain_addr)(amount)(timestamp_ms))
};

// Mirrors sysio::opreg::operator_key
struct operator_key {
   uint64_t account;
   SYSLIB_SERIALIZE(operator_key, (account))
};

// Mirrors sysio::opreg::operator_entry. Field order / types MUST track
// sysio.opreg/include/sysio.opreg/sysio.opreg.hpp. No [[sysio::table]]
// attribute -- this is a read-only mirror, not a table owned by sysio.system.
struct operator_entry {
   sysio::name                        account;
   sysio::opp::types::OperatorType    type;
   sysio::opp::types::OperatorStatus  status;
   bool                               is_bootstrapped = false;
   std::vector<stake_entry>           stakes;
   uint64_t                           registered_at   = 0;
   uint64_t                           available_at    = 0;
   uint64_t                           slashed_at      = 0;
   uint64_t                           terminated_at   = 0;

   uint64_t by_type()   const { return static_cast<uint64_t>(type);   }
   uint64_t by_status() const { return static_cast<uint64_t>(status); }

   SYSLIB_SERIALIZE(operator_entry,
      (account)(type)(status)(is_bootstrapped)(stakes)
      (registered_at)(available_at)(slashed_at)(terminated_at))
};

using operators_t = sysio::kv::table<"operators"_n, operator_key, operator_entry,
   sysio::kv::index<"bytype"_n,
      sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_type>>,
   sysio::kv::index<"bystatus"_n,
      sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_status>>
>;

// Mirrors sysio::epoch::epoch_state. Field order / types MUST track
// sysio.epoch/include/sysio.epoch/sysio.epoch.hpp.
struct epoch_state {
   uint32_t                              current_epoch_index    = 0;
   sysio::time_point                     current_epoch_start{};
   sysio::time_point                     next_epoch_start{};
   uint8_t                               current_batch_op_group = 0;
   std::vector<std::vector<sysio::name>> batch_op_groups;
   sysio::checksum256                    last_consensus_hash;
   bool                                  is_paused              = false;

   SYSLIB_SERIALIZE(epoch_state,
      (current_epoch_index)(current_epoch_start)(next_epoch_start)
      (current_batch_op_group)(batch_op_groups)(last_consensus_hash)(is_paused))
};

using epochstate_t = sysio::kv::global<"epochstate"_n, epoch_state>;

} // namespace opp_readonly

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
constexpr uint32_t TOTAL_BLOCKS_PER_ROUND = ACTIVE_PRODUCER_COUNT * blocks_per_round; // 252
constexpr uint32_t BATCH_OP_GROUP_SIZE    = 7;  // fixed slice is always 1/7; slashed/absent members' share stays in treasury
constexpr uint32_t ACTIVE_PRODUCER_WEIGHT = 15; // > any standby weight (1..cfg.standby_end_rank-21)

constexpr sysio::name CAPITAL_ACCOUNT    = "sysio.cap"_n;
constexpr sysio::name GOVERNANCE_ACCOUNT = "sysio.gov"_n;
constexpr sysio::name CAPEX_ACCOUNT      = "sysio.ops"_n;
constexpr sysio::name TOKEN_CONTRACT     = "sysio.token"_n;
constexpr sysio::name ROA_CONTRACT       = "sysio.roa"_n;

namespace memo {
   constexpr auto capital          = "T5 capital";
   constexpr auto capex            = "T5 capex";
   constexpr auto governance       = "T5 governance";
   constexpr auto batch_op_reward  = "T5 batch operator reward";
   constexpr auto producer_reward  = "T5 producer reward";
   constexpr auto node_owner_dist  = "Node Owner distribution";
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
   return static_cast<int64_t>(product / 10000);
}

int64_t compute_epoch_emission(const emission_config& cfg, int64_t prev_emission, int64_t total_distributed) {
   int64_t remaining = cfg.t5_distributable - cfg.t5_floor - total_distributed;
   if (remaining <= 0)
      return 0;

   __int128 product = static_cast<__int128>(prev_emission) *
                      static_cast<__int128>(cfg.decay_numerator);
   int64_t emission = static_cast<int64_t>(product / cfg.decay_denominator);

   if (emission > cfg.epoch_max_emission) emission = cfg.epoch_max_emission;
   if (emission < cfg.epoch_min_emission) emission = cfg.epoch_min_emission;

   if (emission > remaining) emission = remaining;

   return emission;
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
   token_readonly::accounts_t acct_tbl(TOKEN_CONTRACT, account.value);
   token_readonly::acct_key key{WIRE_SYMBOL.code().raw()};
   if (!acct_tbl.contains(key)) return 0;
   return acct_tbl.get(key).balance.amount;
}

// Returns true iff the account is registered in sysio.opreg with AVAILABLE
// status (OPERATOR_STATUS_ACTIVE). Slashed/terminated/unknown all return false.
// Non-registered accounts return false.
bool is_op_active(name account) {
   opp_readonly::operators_t ops(opreg_refs::account);
   auto key = opp_readonly::operator_key{account.value};
   if (!ops.contains(key)) return false;
   return ops.get(key).status == OperatorStatus::OPERATOR_STATUS_ACTIVE;
}

// Returns the current epoch's active batch-op rotation group (names only).
// Empty vector if sysio.epoch state doesn't exist or the group index is out
// of range (both defensive — caller treats as "no batch ops this epoch").
std::vector<name> get_current_batch_group() {
   opp_readonly::epochstate_t est(epoch_refs::account);
   if (!est.exists()) return {};
   auto st = est.get();
   if (st.current_batch_op_group >= st.batch_op_groups.size()) return {};
   return st.batch_op_groups[st.current_batch_op_group];
}

// ---------------------------------------------------------------------------
// Transfer helper
// ---------------------------------------------------------------------------

void send_wire_transfer(name self, name to, int64_t amount, const char* memo_str) {
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
   sysio::check(cfg.epoch_duration_secs > 0,     "epoch_duration_secs must be positive");
   sysio::check(cfg.decay_denominator > 0,       "decay_denominator must be positive");
   sysio::check(cfg.decay_numerator >= 0,        "decay_numerator must be non-negative");
   sysio::check(cfg.epoch_initial_emission >= 0, "epoch_initial_emission must be non-negative");
   sysio::check(cfg.epoch_max_emission >= 0,     "epoch_max_emission must be non-negative");
   sysio::check(cfg.epoch_min_emission >= 0,     "epoch_min_emission must be non-negative");
   sysio::check(cfg.epoch_min_emission <= cfg.epoch_max_emission,
                 "epoch_min_emission must be <= epoch_max_emission");

   // BPS splits
   sysio::check(cfg.compute_bps + cfg.capital_bps + cfg.capex_bps + cfg.governance_bps == 10000,
                 "category BPS must sum to 10000");
   sysio::check(cfg.producer_bps + cfg.batch_op_bps == 10000,
                 "compute sub-split BPS must sum to 10000");

   // Producer config
   sysio::check(cfg.standby_end_rank >= STANDBY_START_RANK,
                 "standby_end_rank must be >= standby_start_rank (22)");

   emitcfg_t cfgtbl(get_self());
   cfgtbl.set(cfg, get_self());
}

// ===========================================================================
// Node-owner distribution
// ===========================================================================

void system_contract::setinittime(const sysio::time_point_sec& no_reward_init_time) {
   require_auth(get_self());
   get_emit_cfg(get_self()); // ensure config exists

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
      default:
         sysio::check(false, "invalid tier");
         break;
   }

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

   t5s.set(t5_state{
      .start_time          = start_time,
      .epoch_count         = 0,
      .last_epoch_index    = 0,
      .last_epoch_time     = start_time,
      .last_epoch_emission = cfg.epoch_initial_emission,
      .total_distributed   = 0,
   }, get_self());
}

// Permissionless. Distributes exactly one epoch per call: advances
// t5state.last_epoch_index by 1. Callers catch up gaps by re-invoking.
//
// Decoupled from sysio.epoch::advance -- an emissions failure does not block
// protocol epoch advancement. Slashed / terminated recipients' shares stay in
// the treasury rather than being redistributed.
void system_contract::processepoch() {
   const auto cfg = get_emit_cfg(get_self());

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");
   auto state = t5s.get();

   // Read sysio.epoch state for the idempotency guard.
   opp_readonly::epochstate_t est(epoch_refs::account);
   sysio::check(est.exists(), "sysio.epoch state not initialized");
   const auto epoch_st = est.get();

   sysio::check(epoch_st.current_epoch_index > state.last_epoch_index,
                 "emissions already caught up to sysio.epoch");

   const uint32_t target_epoch = state.last_epoch_index + 1;

   // ----- Emission amount -----
   int64_t emission;
   if (state.epoch_count == 0) {
      emission = cfg.epoch_initial_emission;
      const int64_t remaining = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
      if (emission > remaining) emission = remaining;
   } else {
      emission = compute_epoch_emission(cfg, state.last_epoch_emission, state.total_distributed);
   }
   sysio::check(emission > 0, "treasury exhausted");

   // ----- Pool balance check: sysio must hold enough WIRE to cover the emission -----
   const int64_t balance = get_wire_balance(get_self());
   sysio::check(balance >= emission, "insufficient treasury WIRE balance");

   // ----- Category splits -----
   const int64_t compute_amount    = split_bps(emission, cfg.compute_bps);
   const int64_t capital_amount    = split_bps(emission, cfg.capital_bps);
   const int64_t capex_amount      = split_bps(emission, cfg.capex_bps);
   const int64_t governance_amount = emission - compute_amount - capital_amount - capex_amount;

   const int64_t producer_pool = split_bps(compute_amount, cfg.producer_bps);
   const int64_t batch_pool    = compute_amount - producer_pool;

   int64_t actual_paid = 0;

   // =======================================================================
   // Producer + standby pay. Active producers (rank 1..21) are paid in
   // proportion to their eligible_rounds this epoch; standbys (rank 22..
   // cfg.standby_end_rank) are paid by the existing rank-decreasing weight
   // without an eligible_rounds requirement. Recipients are filtered by
   // opreg status so slashed / terminated operators are skipped.
   // =======================================================================
   {
      auto prod_by_rank = _producers.get_index<"prodrank"_n>();

      // expected_rounds is derived from the configured epoch duration, not
      // wall clock: with one-epoch-per-call gap catch-up, wall clock may span
      // multiple configured epoch durations between calls.
      uint32_t expected_rounds = (cfg.epoch_duration_secs * 2) / TOTAL_BLOCKS_PER_ROUND;
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

      for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
         if (it->rank > cfg.standby_end_rank) break;
         if (!it->is_active) continue;

         // opreg filter: skip slashed/terminated/unknown
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

      // Capture reset-list separately: every rank-ranged producer that carries
      // stale counters gets reset, regardless of whether we paid them this epoch
      // (slashed producers still need their counters cleared for the next epoch).
      for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
         if (it->rank > cfg.standby_end_rank) break;
         if (it->unpaid_blocks > 0 || it->eligible_rounds > 0 || it->current_round_blocks > 0) {
            to_reset.push_back(it->owner);
         }
      }

      int64_t distributed_to_producers = 0;
      if (total_weight > 0) {
         for (const auto& pe : eligible) {
            int64_t full_share = static_cast<int64_t>(
               static_cast<__int128>(producer_pool) * pe.weight / total_weight);
            int64_t pay;
            if (pe.is_standby) {
               pay = full_share;
            } else {
               uint32_t r = (pe.elig_rounds > expected_rounds) ? expected_rounds : pe.elig_rounds;
               pay = static_cast<int64_t>(
                  static_cast<__int128>(full_share) * r / expected_rounds);
            }
            if (pay > 0) {
               send_wire_transfer(get_self(), pe.owner, pay, memo::producer_reward);
               distributed_to_producers += pay;
            }
         }
      }

      actual_paid += distributed_to_producers;

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
   // Batch-op pay. Divides batch_pool into a fixed 1/BATCH_OP_GROUP_SIZE slice
   // per seat in the current rotation group. Members not registered as ACTIVE
   // in sysio.opreg (slashed / terminated / unknown) are skipped and their
   // slice remains in the treasury rather than being redistributed.
   // =======================================================================
   {
      const auto group         = get_current_batch_group();
      const int64_t per_member = batch_pool / BATCH_OP_GROUP_SIZE;

      for (const auto& m : group) {
         if (!is_op_active(m)) continue; // slashed / terminated / unknown: share stays in treasury
         send_wire_transfer(get_self(), m, per_member, memo::batch_op_reward);
         actual_paid += per_member;
      }
   }

   // =======================================================================
   // Category buckets: fixed accounts, no opreg filter.
   // =======================================================================
   send_wire_transfer(get_self(), CAPITAL_ACCOUNT,    capital_amount,    memo::capital);
   send_wire_transfer(get_self(), CAPEX_ACCOUNT,      capex_amount,      memo::capex);
   send_wire_transfer(get_self(), GOVERNANCE_ACCOUNT, governance_amount, memo::governance);

   actual_paid += capital_amount + capex_amount + governance_amount;

   // =======================================================================
   // State update. Order matters: last_epoch_index must be bumped so that any
   // subsequent processepoch call (e.g. gap catch-up) sees the correct
   // idempotency guard.
   // =======================================================================
   const auto now = time_point_sec{current_time_point()};
   state.epoch_count++;
   state.last_epoch_index    = target_epoch;
   state.last_epoch_time     = now;
   state.last_epoch_emission = emission;
   state.total_distributed  += actual_paid; // track only amounts actually paid; skipped recipients' shares stay in treasury
   t5s.set(state, get_self());

   // Audit log: records the AUTHORIZED emission + the four category amounts.
   // (Producer / batch-op sub-distribution is implicit -- recipients are in traces.)
   epochlog_t epoch_table(get_self());
   epoch_table.emplace(get_self(), epochlog_key{state.epoch_count}, epoch_log{
      .epoch_num         = state.epoch_count,
      .timestamp         = now,
      .total_emission    = emission,
      .compute_amount    = compute_amount,
      .capital_amount    = capital_amount,
      .capex_amount      = capex_amount,
      .governance_amount = governance_amount,
   });
}

emissions::epoch_info_result system_contract::viewepoch() {
   const auto cfg = get_emit_cfg(get_self());

   t5state_t t5s(get_self());
   sysio::check(t5s.exists(), "t5 state not initialized");

   const auto state = t5s.get();
   const auto now   = time_point_sec{current_time_point()};

   int64_t remaining = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
   if (remaining < 0) remaining = 0;

   int64_t next_est;
   if (state.epoch_count == 0) {
      next_est = cfg.epoch_initial_emission;
      if (next_est > remaining) next_est = remaining;
   } else {
      next_est = compute_epoch_emission(cfg, state.last_epoch_emission, state.total_distributed);
   }

   uint32_t secs_until = 0;
   const uint64_t next_epoch_time =
      static_cast<uint64_t>(state.last_epoch_time.sec_since_epoch()) + cfg.epoch_duration_secs;
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
