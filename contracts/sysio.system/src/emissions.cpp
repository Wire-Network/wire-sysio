#include <sysio.system/sysio.system.hpp>
#include <sysio.system/emissions.hpp>

namespace sysiosystem {

   using namespace emissions;

   // - - - - LOCAL EMISSIONS CONSTANTS - - - -

   static constexpr sysio::symbol WIRE_SYMBOL = sysio::symbol("WIRE", 8);
   // Minimum amount any claim action will allow.
   static const sysio::asset MIN_CLAIMABLE = sysio::asset(1000000000, WIRE_SYMBOL);

   // Node Owner total_claimable amounts (in WIRE subunits)
   static const sysio::asset T1_ALLOCATION(750000000000000, WIRE_SYMBOL);
   static const sysio::asset T2_ALLOCATION(100000000000000, WIRE_SYMBOL);
   static const sysio::asset T3_ALLOCATION(10000000000000,   WIRE_SYMBOL);

   // Durations
   static constexpr uint32_t SECONDS_PER_MONTH = 30u * 24u * 60u * 60u;
   static constexpr uint32_t T1_DURATION       = 12u * SECONDS_PER_MONTH;  // 12 months
   static constexpr uint32_t T2_DURATION       = 24u * SECONDS_PER_MONTH;  // 24 months
   static constexpr uint32_t T3_DURATION       = 36u * SECONDS_PER_MONTH;  // 36 months;

   // - - - - T5 TREASURY EMISSIONS CONSTANTS - - - -

   // Total distributable and floor (8-decimal subunits)
   static constexpr int64_t T5_DISTRIBUTABLE    = 37'500'000'000'000'000LL; // 375M WIRE
   static constexpr int64_t T5_FLOOR            = 12'500'000'000'000'000LL; // 125M floor

   // Epoch duration
   static constexpr uint32_t EPOCH_DURATION_SECS = 24u * 60u * 60u; // 24 hours

   // Decay: new_emission = prev * DECAY_NUM / DECAY_DEN
   static constexpr int64_t DECAY_NUMERATOR     = 9990;
   static constexpr int64_t DECAY_DENOMINATOR   = 10000;

   // Initial epoch emission (E_0) - calibrated so sum ≈ 375M over 1095 epochs
   static constexpr int64_t EPOCH_INITIAL_EMISSION = 56'315'000'000'000LL; // ~563,150 WIRE

   // Clamps
   static constexpr int64_t EPOCH_MAX_EMISSION  = 300'000'000'000'000LL;  // 3M WIRE
   static constexpr int64_t EPOCH_MIN_EMISSION  = 10'000'000'000'000LL;   // 100K WIRE

   // Category splits (basis points, must sum to 10000)
   static constexpr uint16_t COMPUTE_BPS    = 4000; // 40%
   static constexpr uint16_t CAPITAL_BPS    = 3000; // 30%
   static constexpr uint16_t CAPEX_BPS      = 2000; // 20%
   static constexpr uint16_t GOVERNANCE_BPS = 1000; // 10%

   // Compute sub-split
   static constexpr uint16_t PRODUCER_BPS   = 7000; // 70% of compute
   static constexpr uint16_t BATCH_OP_BPS   = 3000; // 30% of compute

   // Producer queue
   static constexpr uint32_t ACTIVE_PRODUCER_COUNT = 21;
   static constexpr uint32_t STANDBY_START_RANK    = 22;
   static constexpr uint32_t STANDBY_END_RANK      = 28;  // 7 standby
   static constexpr uint32_t BLOCKS_PER_ROUND      = ACTIVE_PRODUCER_COUNT * 12; // 252

   // Holding accounts (stubs)
   static constexpr sysio::name CAPITAL_ACCOUNT    = "sysio.cap"_n;
   static constexpr sysio::name GOVERNANCE_ACCOUNT = "sysio.gov"_n;
   static constexpr sysio::name BATCH_OP_ACCOUNT   = "sysio.batch"_n;
   static constexpr sysio::name CAPEX_ACCOUNT      = "sysio.ops"_n;

   namespace {
      using sysio::time_point;
      using sysio::time_point_sec;
      using sysio::asset;

      emissions::node_claim_result compute_node_claim(
         const emissions::emission_state&           emission,
         const emissions::node_owner_distribution&  row
      ) {
         emissions::node_claim_result info{};

         const uint32_t start_secs = emission.node_rewards_start.sec_since_epoch();
         sysio::check(start_secs > 0, "node rewards have not started");

         const uint32_t duration = row.total_duration;

         const int64_t total_amount    = row.total_allocation.amount;
         const int64_t already_claimed = row.claimed.amount;

         // Current time
         const time_point     now_tp   = sysio::current_time_point();
         const time_point_sec now      = time_point_sec{ now_tp };
         const uint32_t       now_secs = now.sec_since_epoch();

         uint32_t elapsed = 0;
         if (now_secs > start_secs) {
            elapsed = now_secs - start_secs;
         }

         if (elapsed > duration) {
            elapsed = duration;
         }

         int64_t total_vested_amount = 0;
         if (elapsed == 0) {
            // nothing vested yet
            total_vested_amount = 0;
         } else if (elapsed == duration) {
            // fully vested: everything is available (modulo already_claimed)
            total_vested_amount = total_amount;
         } else {
            // partially vested: do the linear fraction with 128-bit intermediate
            __int128 numerator = static_cast<__int128>(total_amount) *
                                 static_cast<__int128>(elapsed);

            total_vested_amount =
               static_cast<int64_t>(numerator / duration);
         }

         int64_t claimable_amount = total_vested_amount - already_claimed;
         if (claimable_amount < 0) {
            claimable_amount = 0;
         }

         info.total_allocation = row.total_allocation;
         info.claimed          = row.claimed;
         info.claimable        = asset{claimable_amount, row.total_allocation.symbol};
         info.can_claim        = (claimable_amount >= MIN_CLAIMABLE.amount || elapsed == duration);

         return info;
      }

   } // anonymous namespace

   // - - - - CONTRACT ACTIONS - - - -

   void system_contract::setinittime(const sysio::time_point_sec &no_reward_init_time) {
      require_auth(get_self());

      // TODO: Do we want to add any validation to init_time?

      emissionstate_t emissionstate(get_self(), get_self().value);
      check(!emissionstate.exists(), "emission table already exists");

      emissionstate.set(emission_state{
         .node_rewards_start = no_reward_init_time
      }, get_self());
   }

   void system_contract::addnodeowner(const sysio::name &account_name, const uint8_t &tier) {
      // Called inline from sysio.roa
      require_auth("sysio.roa"_n);

      // Tier sanity check
      sysio::check(tier >= 1 && tier <=3, "invalid tier");

      // Ensure this account isn't already in the table.
      nodedist_t nodedist(get_self(), get_self().value);
      auto itr = nodedist.find(account_name.value);
      check(itr == nodedist.end(), "account already exists");

      sysio::asset total_allocation;
      uint32_t duration_seconds = 0;

      switch (tier) {
         case 1:
            total_allocation = T1_ALLOCATION;
            duration_seconds = T1_DURATION;
            break;
         case 2:
            total_allocation = T2_ALLOCATION;
            duration_seconds = T2_DURATION;
            break;
         case 3:
            total_allocation = T3_ALLOCATION;
            duration_seconds = T3_DURATION;
            break;
         default:
            sysio::check(false, "invalid tier");
            break;
      }

      nodedist.emplace(get_self(), [&](auto& row) {
         row.account_name = account_name;
         row.total_allocation = total_allocation;
         row.claimed = sysio::asset(0, total_allocation.symbol);
         row.total_duration = duration_seconds;
      });
   }

   void system_contract::claimnodedis(const sysio::name &account_name) {
      // Can only claim for self
      require_auth(account_name);

      // Load emission state (global start time)
      emissionstate_t emission_s(get_self(), get_self().value);
      sysio::check(emission_s.exists(), "emission state not initialized");
      const auto emission = emission_s.get();

      // Lookup node owner row
      nodedist_t nodedist(get_self(), get_self().value);
      auto itr = nodedist.find(account_name.value);
      sysio::check(itr != nodedist.end(), "account is not a node owner");
      const auto& row = *itr;

      sysio::check(row.claimed != row.total_allocation, "all node owner rewards already claimed");

      // Get claim info.
      auto info = compute_node_claim(emission, row);
      sysio::check(info.can_claim, "claim amount below minimum threshold");

      // Update internal accounting
      nodedist.modify(itr, get_self(), [&](auto& mrow) {
         mrow.claimed += info.claimable;
      });

      sysio::action(
           {get_self(), "active"_n},                                                          // auth used for this call
           "sysio.token"_n,                                                                          // contract account
           "transfer"_n,                                                                             // action name
           std::make_tuple(
              get_self(),
              account_name,
              info.claimable,
              std::string("Node Owner distribution")
            )                                                                                         // action data
      ).send();
   }

   emissions::node_claim_result system_contract::viewnodedist(const sysio::name &account_name) {
      // Load emission state
      emissionstate_t emission_s(get_self(), get_self().value);
      sysio::check(emission_s.exists(), "emission state not initialized");
      const auto emission = emission_s.get();

      // Lookup node owner distribution row
      nodedist_t nodedist(get_self(), get_self().value);
      auto itr = nodedist.find(account_name.value);
      sysio::check(itr != nodedist.end(), "account is not a node owner");
      const auto& row = *itr;

      return compute_node_claim(emission, row);
   }

   // ===================================================================
   // T5 Treasury Emissions
   // ===================================================================

   namespace {

      int64_t compute_epoch_emission(int64_t prev_emission, int64_t total_distributed) {
         int64_t remaining = T5_DISTRIBUTABLE - total_distributed;
         if (remaining <= 0)
            return 0;

         // Apply decay: new = prev * DECAY_NUMERATOR / DECAY_DENOMINATOR
         __int128 product = static_cast<__int128>(prev_emission) *
                            static_cast<__int128>(DECAY_NUMERATOR);
         int64_t emission = static_cast<int64_t>(product / DECAY_DENOMINATOR);

         // Clamp
         if (emission > EPOCH_MAX_EMISSION) emission = EPOCH_MAX_EMISSION;
         if (emission < EPOCH_MIN_EMISSION) emission = EPOCH_MIN_EMISSION;

         // Floor enforcement: don't exceed distributable ceiling
         if (emission > remaining) emission = remaining;

         return emission;
      }

      int64_t split_bps(int64_t total, uint16_t bps) {
         __int128 product = static_cast<__int128>(total) * static_cast<__int128>(bps);
         return static_cast<int64_t>(product / 10000);
      }

      void send_wire_transfer(sysio::name self, sysio::name to, int64_t amount, const std::string& memo) {
         if (amount <= 0) return;
         sysio::action(
            {self, "active"_n},
            "sysio.token"_n,
            "transfer"_n,
            std::make_tuple(self, to, sysio::asset(amount, WIRE_SYMBOL), memo)
         ).send();
      }

   } // anonymous namespace

   // - - - - T5 ACTIONS - - - -

   void system_contract::initt5(const sysio::time_point_sec& start_time) {
      require_auth(get_self());

      emissions::t5state_t t5s(get_self(), get_self().value);
      sysio::check(!t5s.exists(), "t5 state already initialized");

      t5s.set(emissions::t5_state{
         .start_time          = start_time,
         .epoch_count         = 0,
         .last_epoch_time     = start_time,
         .last_epoch_emission = EPOCH_INITIAL_EMISSION,
         .total_distributed   = 0
      }, get_self());
   }

   void system_contract::processepoch() {
      emissions::t5state_t t5s(get_self(), get_self().value);
      sysio::check(t5s.exists(), "t5 state not initialized");

      auto state = t5s.get();

      const auto now = sysio::time_point_sec(sysio::current_time_point());
      const uint32_t elapsed = now.sec_since_epoch() - state.last_epoch_time.sec_since_epoch();
      sysio::check(elapsed >= EPOCH_DURATION_SECS, "epoch duration has not elapsed");

      // Compute emission
      int64_t emission;
      if (state.epoch_count == 0) {
         // First epoch uses E_0 directly
         emission = EPOCH_INITIAL_EMISSION;
         // Floor enforcement
         int64_t remaining = T5_DISTRIBUTABLE - state.total_distributed;
         if (emission > remaining) emission = remaining;
      } else {
         emission = compute_epoch_emission(state.last_epoch_emission, state.total_distributed);
      }

      if (emission <= 0) {
         sysio::check(false, "treasury exhausted");
      }

      // Category splits
      int64_t compute_amount    = split_bps(emission, COMPUTE_BPS);
      int64_t capital_amount    = split_bps(emission, CAPITAL_BPS);
      int64_t capex_amount      = split_bps(emission, CAPEX_BPS);
      // Governance gets the remainder to avoid dust loss
      int64_t governance_amount = emission - compute_amount - capital_amount - capex_amount;

      // Compute sub-split: producers + batch operators
      int64_t producer_pool = split_bps(compute_amount, PRODUCER_BPS);
      int64_t batch_pool    = compute_amount - producer_pool; // remainder goes to batch

      // Distribute producer pool with performance-based pay
      int64_t undistributed_producer = 0;
      {
         auto prod_by_rank = _producers.get_index<"prodrank"_n>();

         // Expected rounds: epoch_elapsed * 2 blocks/sec / BLOCKS_PER_ROUND
         uint32_t epoch_elapsed = now.sec_since_epoch() - state.last_epoch_time.sec_since_epoch();
         uint32_t expected_rounds = (epoch_elapsed * 2) / BLOCKS_PER_ROUND;
         if (expected_rounds == 0) expected_rounds = 1;

         struct prod_entry {
            sysio::name owner;
            uint32_t    weight;
            uint32_t    elig_rounds;
            bool        is_standby;
         };
         std::vector<prod_entry> eligible;
         uint32_t total_weight = 0;

         for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
            if (it->rank > STANDBY_END_RANK) break;
            if (!it->is_active) continue;

            uint32_t w = 0;
            bool standby = false;
            uint32_t rounds = 0;

            if (it->rank >= 1 && it->rank <= ACTIVE_PRODUCER_COUNT) {
               // Finalize any in-progress round
               rounds = it->eligible_rounds;
               if (it->current_round_blocks >= 6) rounds++;
               if (rounds == 0) continue; // no eligible rounds → skip
               w = 15;
            } else if (it->rank >= STANDBY_START_RANK && it->rank <= STANDBY_END_RANK) {
               w = 29 - it->rank;  // 22→7, ... 28→1
               standby = true;
            }

            if (w > 0) {
               eligible.push_back({it->owner, w, rounds, standby});
               total_weight += w;
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
                  send_wire_transfer(get_self(), pe.owner, pay, "T5 producer reward");
                  distributed_to_producers += pay;
               }
            }
         }

         undistributed_producer = producer_pool - distributed_to_producers;

         // Reset round-tracking fields for next epoch
         static constexpr uint32_t NO_PREV = std::numeric_limits<uint32_t>::max();
         for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
            if (it->rank > STANDBY_END_RANK) break;
            if (it->unpaid_blocks > 0 || it->eligible_rounds > 0 || it->current_round_blocks > 0) {
               _producers.modify(*it, sysio::same_payer, [&](auto& p) {
                  _gstate.total_unpaid_blocks -= p.unpaid_blocks;
                  p.unpaid_blocks = 0;
                  p.eligible_rounds = 0;
                  p.current_round_blocks = 0;
                  p.last_block_num = NO_PREV;
               });
            }
         }
      }

      // No producer dust redirect — undistributed stays in sysio

      // Transfer stubs
      send_wire_transfer(get_self(), BATCH_OP_ACCOUNT,   batch_pool,        "T5 batch operator");
      send_wire_transfer(get_self(), CAPITAL_ACCOUNT,    capital_amount,    "T5 capital");
      send_wire_transfer(get_self(), CAPEX_ACCOUNT,      capex_amount,      "T5 capex");
      send_wire_transfer(get_self(), GOVERNANCE_ACCOUNT, governance_amount, "T5 governance");

      // Update state — only count actually distributed
      state.epoch_count++;
      state.last_epoch_time     = now;
      state.last_epoch_emission = emission;
      state.total_distributed  += (emission - undistributed_producer);
      t5s.set(state, get_self());

      // Write epoch log
      emissions::epochlog_t epoch_table(get_self(), get_self().value);
      epoch_table.emplace(get_self(), [&](auto& row) {
         row.epoch_num        = state.epoch_count;
         row.timestamp        = now;
         row.total_emission   = emission;
         row.compute_amount   = compute_amount;
         row.capital_amount   = capital_amount;
         row.capex_amount     = capex_amount;
         row.governance_amount = governance_amount;
      });
   }

   emissions::epoch_info_result system_contract::viewepoch() {
      emissions::t5state_t t5s(get_self(), get_self().value);
      sysio::check(t5s.exists(), "t5 state not initialized");

      const auto state = t5s.get();
      const auto now   = sysio::time_point_sec(sysio::current_time_point());

      int64_t remaining = T5_DISTRIBUTABLE - state.total_distributed;
      if (remaining < 0) remaining = 0;

      // Estimate next emission
      int64_t next_est;
      if (state.epoch_count == 0) {
         next_est = EPOCH_INITIAL_EMISSION;
         if (next_est > remaining) next_est = remaining;
      } else {
         next_est = compute_epoch_emission(state.last_epoch_emission, state.total_distributed);
      }

      // Seconds until next epoch
      uint32_t secs_until = 0;
      uint32_t next_epoch_time = state.last_epoch_time.sec_since_epoch() + EPOCH_DURATION_SECS;
      if (now.sec_since_epoch() < next_epoch_time) {
         secs_until = next_epoch_time - now.sec_since_epoch();
      }

      return emissions::epoch_info_result{
         .epoch_count        = state.epoch_count,
         .last_epoch_time    = state.last_epoch_time,
         .last_epoch_emission = state.last_epoch_emission,
         .total_distributed  = state.total_distributed,
         .treasury_remaining = remaining,
         .next_emission_est  = next_est,
         .seconds_until_next = secs_until
      };
   }

   emissions::emission_config_result system_contract::viewemitcfg() {
      return emissions::emission_config_result{
         .t1_allocation          = T1_ALLOCATION,
         .t2_allocation          = T2_ALLOCATION,
         .t3_allocation          = T3_ALLOCATION,
         .t1_duration            = T1_DURATION,
         .t2_duration            = T2_DURATION,
         .t3_duration            = T3_DURATION,
         .min_claimable          = MIN_CLAIMABLE,
         .t5_distributable       = T5_DISTRIBUTABLE,
         .t5_floor               = T5_FLOOR,
         .epoch_duration_secs    = EPOCH_DURATION_SECS,
         .decay_numerator        = DECAY_NUMERATOR,
         .decay_denominator      = DECAY_DENOMINATOR,
         .epoch_initial_emission = EPOCH_INITIAL_EMISSION,
         .epoch_max_emission     = EPOCH_MAX_EMISSION,
         .epoch_min_emission     = EPOCH_MIN_EMISSION,
         .compute_bps            = COMPUTE_BPS,
         .capital_bps            = CAPITAL_BPS,
         .capex_bps              = CAPEX_BPS,
         .governance_bps         = GOVERNANCE_BPS,
         .producer_bps           = PRODUCER_BPS,
         .batch_op_bps           = BATCH_OP_BPS,
         .active_producer_count  = ACTIVE_PRODUCER_COUNT,
         .standby_start_rank     = STANDBY_START_RANK,
         .standby_end_rank       = STANDBY_END_RANK,
         .blocks_per_round       = BLOCKS_PER_ROUND
      };
   }

}