#include <sysio.system/sysio.system.hpp>
#include <sysio.system/emissions.hpp>

namespace sysiosystem {

   using namespace emissions;

   // Compile-time constants (not user-configurable)
   static constexpr sysio::symbol WIRE_SYMBOL = sysio::symbol("WIRE", 9);

   static constexpr uint32_t ACTIVE_PRODUCER_COUNT = 21;
   static constexpr uint32_t STANDBY_START_RANK    = 22;
   static constexpr uint32_t TOTAL_BLOCKS_PER_ROUND = ACTIVE_PRODUCER_COUNT * blocks_per_round; // 252

   static constexpr sysio::name CAPITAL_ACCOUNT    = "sysio.cap"_n;
   static constexpr sysio::name GOVERNANCE_ACCOUNT = "sysio.gov"_n;
   static constexpr sysio::name BATCH_OP_ACCOUNT   = "sysio.batch"_n;
   static constexpr sysio::name CAPEX_ACCOUNT      = "sysio.ops"_n;

   // Helper to load emission config singleton
   static emission_config get_emit_cfg(sysio::name self) {
      emitcfg_t cfgtbl(self, self.value);
      sysio::check(cfgtbl.exists(), "emission config not set; call setemitcfg first");
      return cfgtbl.get();
   }

   namespace {
      using sysio::time_point;
      using sysio::time_point_sec;
      using sysio::asset;

      emissions::node_claim_result compute_node_claim(
         const emissions::emission_state&          emission,
         const emissions::node_owner_distribution& row,
         int64_t                                   min_claimable
      ) {
         emissions::node_claim_result info{};

         const uint32_t start_secs = emission.node_rewards_start.sec_since_epoch();
         sysio::check(start_secs > 0, "node rewards have not started");

         const uint32_t duration = row.total_duration;

         const int64_t total_amount    = row.total_allocation.amount;
         const int64_t already_claimed = row.claimed.amount;

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
            total_vested_amount = 0;
         } else if (elapsed == duration) {
            total_vested_amount = total_amount;
         } else {
            __int128 numerator = static_cast<__int128>(total_amount) *
                                 static_cast<__int128>(elapsed);
            total_vested_amount = static_cast<int64_t>(numerator / duration);
         }

         int64_t claimable_amount = total_vested_amount - already_claimed;
         if (claimable_amount < 0) {
            claimable_amount = 0;
         }

         info.total_allocation = row.total_allocation;
         info.claimed          = row.claimed;
         info.claimable        = asset{claimable_amount, row.total_allocation.symbol};
         info.can_claim        = (claimable_amount >= min_claimable || elapsed == duration);

         return info;
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

   // ===================================================================
   // setemitcfg — set or update emission configuration
   // ===================================================================

   void system_contract::setemitcfg(const emissions::emission_config& cfg) {
      require_auth(get_self());

      // Validate node owner params
      sysio::check(cfg.t1_allocation >= 0, "t1_allocation must be non-negative");
      sysio::check(cfg.t2_allocation >= 0, "t2_allocation must be non-negative");
      sysio::check(cfg.t3_allocation >= 0, "t3_allocation must be non-negative");
      sysio::check(cfg.t1_duration > 0, "t1_duration must be positive");
      sysio::check(cfg.t2_duration > 0, "t2_duration must be positive");
      sysio::check(cfg.t3_duration > 0, "t3_duration must be positive");
      sysio::check(cfg.min_claimable >= 0, "min_claimable must be non-negative");

      // Validate T5 params
      sysio::check(cfg.t5_distributable >= 0, "t5_distributable must be non-negative");
      sysio::check(cfg.t5_floor >= 0, "t5_floor must be non-negative");
      sysio::check(cfg.epoch_duration_secs > 0, "epoch_duration_secs must be positive");
      sysio::check(cfg.decay_denominator > 0, "decay_denominator must be positive");
      sysio::check(cfg.decay_numerator >= 0, "decay_numerator must be non-negative");
      sysio::check(cfg.epoch_initial_emission >= 0, "epoch_initial_emission must be non-negative");
      sysio::check(cfg.epoch_max_emission >= 0, "epoch_max_emission must be non-negative");
      sysio::check(cfg.epoch_min_emission >= 0, "epoch_min_emission must be non-negative");
      sysio::check(cfg.epoch_min_emission <= cfg.epoch_max_emission,
                    "epoch_min_emission must be <= epoch_max_emission");

      // Validate BPS splits
      sysio::check(cfg.compute_bps + cfg.capital_bps + cfg.capex_bps + cfg.governance_bps == 10000,
                    "category BPS must sum to 10000");
      sysio::check(cfg.producer_bps + cfg.batch_op_bps == 10000,
                    "compute sub-split BPS must sum to 10000");

      // Validate producer config
      sysio::check(cfg.standby_end_rank >= STANDBY_START_RANK,
                    "standby_end_rank must be >= standby_start_rank (22)");

      emitcfg_t cfgtbl(get_self(), get_self().value);
      cfgtbl.set(cfg, get_self());
   }

   // ===================================================================
   // Node Owner Distribution
   // ===================================================================

   void system_contract::setinittime(const sysio::time_point_sec& no_reward_init_time) {
      require_auth(get_self());
      get_emit_cfg(get_self()); // ensure config exists

      emissionstate_t emissionstate(get_self(), get_self().value);
      sysio::check(!emissionstate.exists(), "emission table already exists");

      emissionstate.set(emission_state{
         .node_rewards_start = no_reward_init_time
      }, get_self());
   }

   void system_contract::addnodeowner(const sysio::name& account_name, uint8_t tier) {
      require_auth("sysio.roa"_n);

      sysio::check(tier >= 1 && tier <= 3, "invalid tier");

      const auto cfg = get_emit_cfg(get_self());

      nodedist_t nodedist(get_self(), get_self().value);
      auto itr = nodedist.find(account_name.value);
      sysio::check(itr == nodedist.end(), "account already exists");

      int64_t  total_allocation_amount = 0;
      uint32_t duration_seconds = 0;

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

      sysio::asset total_allocation(total_allocation_amount, WIRE_SYMBOL);

      nodedist.emplace(get_self(), [&](auto& row) {
         row.account_name    = account_name;
         row.total_allocation = total_allocation;
         row.claimed          = sysio::asset(0, WIRE_SYMBOL);
         row.total_duration   = duration_seconds;
      });
   }

   void system_contract::claimnodedis(const sysio::name& account_name) {
      require_auth(account_name);

      const auto cfg = get_emit_cfg(get_self());

      emissionstate_t emission_s(get_self(), get_self().value);
      sysio::check(emission_s.exists(), "emission state not initialized");
      const auto emission = emission_s.get();

      nodedist_t nodedist(get_self(), get_self().value);
      auto itr = nodedist.find(account_name.value);
      sysio::check(itr != nodedist.end(), "account is not a node owner");
      const auto& row = *itr;

      sysio::check(row.claimed != row.total_allocation, "all node owner rewards already claimed");

      auto info = compute_node_claim(emission, row, cfg.min_claimable);
      sysio::check(info.can_claim, "claim amount below minimum threshold");

      nodedist.modify(itr, get_self(), [&](auto& mrow) {
         mrow.claimed += info.claimable;
         sysio::check(mrow.claimed <= mrow.total_allocation, "claim would exceed total allocation");
      });

      sysio::action(
         {get_self(), "active"_n},
         "sysio.token"_n,
         "transfer"_n,
         std::make_tuple(
            get_self(),
            account_name,
            info.claimable,
            std::string("Node Owner distribution")
         )
      ).send();
   }

   emissions::node_claim_result system_contract::viewnodedist(const sysio::name& account_name) {
      const auto cfg = get_emit_cfg(get_self());

      emissionstate_t emission_s(get_self(), get_self().value);
      sysio::check(emission_s.exists(), "emission state not initialized");
      const auto emission = emission_s.get();

      nodedist_t nodedist(get_self(), get_self().value);
      auto itr = nodedist.find(account_name.value);
      sysio::check(itr != nodedist.end(), "account is not a node owner");

      return compute_node_claim(emission, *itr, cfg.min_claimable);
   }

   // ===================================================================
   // T5 Treasury Emissions
   // ===================================================================

   void system_contract::initt5(const sysio::time_point_sec& start_time) {
      require_auth(get_self());

      const auto cfg = get_emit_cfg(get_self());

      emissions::t5state_t t5s(get_self(), get_self().value);
      sysio::check(!t5s.exists(), "t5 state already initialized");

      t5s.set(emissions::t5_state{
         .start_time          = start_time,
         .epoch_count         = 0,
         .last_epoch_time     = start_time,
         .last_epoch_emission = cfg.epoch_initial_emission,
         .total_distributed   = 0
      }, get_self());
   }

   void system_contract::processepoch() {
      const auto cfg = get_emit_cfg(get_self());

      emissions::t5state_t t5s(get_self(), get_self().value);
      sysio::check(t5s.exists(), "t5 state not initialized");

      auto state = t5s.get();

      const auto now = sysio::time_point_sec(sysio::current_time_point());
      const uint32_t elapsed = now.sec_since_epoch() - state.last_epoch_time.sec_since_epoch();
      sysio::check(elapsed >= cfg.epoch_duration_secs, "epoch duration has not elapsed");

      // Compute emission
      int64_t emission;
      if (state.epoch_count == 0) {
         emission = cfg.epoch_initial_emission;
         int64_t remaining = cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
         if (emission > remaining) emission = remaining;
      } else {
         emission = compute_epoch_emission(cfg, state.last_epoch_emission, state.total_distributed);
      }

      if (emission <= 0) {
         sysio::check(false, "treasury exhausted");
      }

      // Category splits
      int64_t compute_amount    = split_bps(emission, cfg.compute_bps);
      int64_t capital_amount    = split_bps(emission, cfg.capital_bps);
      int64_t capex_amount      = split_bps(emission, cfg.capex_bps);
      int64_t governance_amount = emission - compute_amount - capital_amount - capex_amount;

      // Compute sub-split: producers + batch operators
      int64_t producer_pool = split_bps(compute_amount, cfg.producer_bps);
      int64_t batch_pool    = compute_amount - producer_pool;

      // Distribute producer pool with performance-based pay
      int64_t undistributed_producer = 0;
      {
         auto prod_by_rank = _producers.get_index<"prodrank"_n>();

         uint32_t expected_rounds = (elapsed * 2) / TOTAL_BLOCKS_PER_ROUND;
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
            if (it->rank > cfg.standby_end_rank) break;
            if (!it->is_active) continue;

            uint32_t w = 0;
            bool standby = false;
            uint32_t rounds = 0;

            if (it->rank >= 1 && it->rank <= ACTIVE_PRODUCER_COUNT) {
               rounds = it->eligible_rounds;
               if (it->current_round_blocks >= min_blocks_per_round_for_pay) rounds++;
               if (rounds == 0) continue;
               w = 15; // fixed weight > max standby weight (standby weights are 1..standby_end_rank-21)
            } else if (it->rank >= STANDBY_START_RANK && it->rank <= cfg.standby_end_rank) {
               w = cfg.standby_end_rank + 1 - it->rank;
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
         for (auto it = prod_by_rank.begin(); it != prod_by_rank.end(); ++it) {
            if (it->rank > cfg.standby_end_rank) break;
            if (it->unpaid_blocks > 0 || it->eligible_rounds > 0 || it->current_round_blocks > 0) {
               _producers.modify(*it, sysio::same_payer, [&](auto& p) {
                  _gstate.total_unpaid_blocks -= p.unpaid_blocks;
                  p.unpaid_blocks        = 0;
                  p.eligible_rounds      = 0;
                  p.current_round_blocks = 0;
                  p.last_block_num       = no_prev_block;
               });
            }
         }
      }

      // Transfer stubs
      send_wire_transfer(get_self(), BATCH_OP_ACCOUNT,   batch_pool,        "T5 batch operator");
      send_wire_transfer(get_self(), CAPITAL_ACCOUNT,    capital_amount,    "T5 capital");
      send_wire_transfer(get_self(), CAPEX_ACCOUNT,      capex_amount,      "T5 capex");
      send_wire_transfer(get_self(), GOVERNANCE_ACCOUNT, governance_amount, "T5 governance");

      // Update state
      state.epoch_count++;
      state.last_epoch_time     = now;
      state.last_epoch_emission = emission;
      state.total_distributed  += (emission - undistributed_producer);
      t5s.set(state, get_self());

      // Write epoch log
      emissions::epochlog_t epoch_table(get_self(), get_self().value);
      epoch_table.emplace(get_self(), [&](auto& row) {
         row.epoch_num         = state.epoch_count;
         row.timestamp         = now;
         row.total_emission    = emission;
         row.compute_amount    = compute_amount;
         row.capital_amount    = capital_amount;
         row.capex_amount      = capex_amount;
         row.governance_amount = governance_amount;
      });
   }

   emissions::epoch_info_result system_contract::viewepoch() {
      const auto cfg = get_emit_cfg(get_self());

      emissions::t5state_t t5s(get_self(), get_self().value);
      sysio::check(t5s.exists(), "t5 state not initialized");

      const auto state = t5s.get();
      const auto now   = sysio::time_point_sec(sysio::current_time_point());

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
      uint64_t next_epoch_time = static_cast<uint64_t>(state.last_epoch_time.sec_since_epoch()) + cfg.epoch_duration_secs;
      if (now.sec_since_epoch() < next_epoch_time) {
         secs_until = static_cast<uint32_t>(next_epoch_time - now.sec_since_epoch());
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

   emissions::emission_config system_contract::viewemitcfg() {
      return get_emit_cfg(get_self());
   }

}
