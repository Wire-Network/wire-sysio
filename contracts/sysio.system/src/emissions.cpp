#include <sysio.system/sysio.system.hpp>
#include <sysio.system/emissions.hpp>

namespace sysiosystem {

   using namespace emissions;

   // - - - - LOCAL EMISSIONS CONSTANTS - - - -

   // Adjust precision here to match your core symbol
   static constexpr sysio::symbol WIRE_SYMBOL = sysio::symbol("WIRE", 8); // TODO: Set precision once we know.
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

}