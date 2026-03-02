#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <optional>
#include <unordered_map>
#include <sysio/chain/exceptions.hpp>
#include <sysio/services/cron_parser.hpp>
#include <sysio/services/cron_service.hpp>


#include <sysio/beacon_chain_update_plugin.hpp>

namespace bpo = boost::program_options;

namespace sysio {
// using namespace outpost_client::ethereum;

namespace {
   constexpr auto beacon_chain_queue_url                = "beacon-chain-queue-url";
   constexpr auto beacon_chain_apy_url                  = "beacon-chain-apy-url";
   constexpr auto beacon_chain_api_key                  = "beacon-chain-api-key";
   constexpr auto beacon_chain_deployer                 = "beacon-chain-deployer";
   constexpr auto beacon_chain_outpost_addrs            = "beacon-chain-outpost-addrs";
   constexpr auto beacon_chain_liqeth_addrs             = "beacon-chain-liqeth-addrs";
   constexpr auto beacon_chain_interval                 = "beacon-chain-interval";
   constexpr auto beacon_chain_finalize_epoch_interval  = "beacon-chain-finalize-epoch-interval";

   namespace contracts {
      constexpr auto opp = "OPP";
      constexpr auto deposit_manager = "DepositManager";
      constexpr auto withdrawal_queue = "WithdrawalQueue";
   }

   [[maybe_unused]] inline fc::logger& logger() {
      static fc::logger log{"beacon_chain_update_plugin"};
      return log;
   }
}

using namespace std;
using addr_map_t = unordered_map<string, string>;
using action = bool (*)();
using interval_actions_t = vector<action>;
using schedules_t = unordered_map<string, services::cron_service::job_schedule>;

class beacon_chain_update_plugin_impl {

public:
   string beacon_chain_queue_url;
   string beacon_chain_apy_url;
   optional<string> beacon_chain_api_key;
   optional<string> beacon_chain_deployer;
   schedules_t schedules;
   unordered_map<string, interval_actions_t> intervals;
   addr_map_t outpost_addrs;
   addr_map_t liqeth_addrs;

};


void beacon_chain_update_plugin::plugin_initialize(const variables_map& options) {
   ilog("initializing chain plugin");
   auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();

   if( options.contains(beacon_chain_outpost_addrs) ) {
      ilog("found beacon chain outpost addresses");
      auto& outpost_addrs_file = options.at(beacon_chain_outpost_addrs).as<std::string>();
      ilog("found {}", outpost_addrs_file);
      my->outpost_addrs = fc::json::from_file<addr_map_t>(outpost_addrs_file);
      for(const auto& [name, addr] : my->outpost_addrs) {
         ilog("outpost address - {}: {}", name, addr);
      }
   }
   if( options.contains(beacon_chain_interval) ) {
      auto client_specs    = options.at(beacon_chain_interval).as<std::vector<std::string>>();
      for (auto& client_spec : client_specs) {
          auto parts = fc::split(client_spec, ',', 1);
          auto schedule_inserted = my->schedules.emplace(parts[0], services::parse_cron_schedule_or_throw(parts[1]));
          SYS_ASSERT(schedule_inserted.second, chain::plugin_config_exception,
                     "Repeated interval spec name: {}, schedule: {}", parts[0], parts[1]);
      }

   }
   if( options.contains(beacon_chain_finalize_epoch_interval) ) {
      SYS_ASSERT( my->outpost_addrs.size() > 0, sysio::chain::plugin_config_exception,
         "finalize epoch option is only valid if outpost address file is provided" );
      SYS_ASSERT( my->outpost_addrs.count(contracts::opp) > 0, sysio::chain::plugin_config_exception,
         "finalize epoch option is only valid if outpost address file is provided" );
      auto& finalize_epoch_interval = options.at(beacon_chain_finalize_epoch_interval).as<std::string>();
      
   }
}

void beacon_chain_update_plugin::plugin_startup() {
   ilog("Starting eth queues apy plugin");
}


beacon_chain_update_plugin::beacon_chain_update_plugin() : my(
   std::make_unique<beacon_chain_update_plugin_impl>()) {}

void beacon_chain_update_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()
      (beacon_chain_queue_url,
       bpo::value<std::string>()->default_value("https://beaconcha.in/api/v2/ethereum/queues"),
       "URL for the beacon chain queues endpoint to obtain the current queue duration.")
      (beacon_chain_apy_url,
       bpo::value<std::string>()->default_value("https://beaconcha.in/api/v1/ethstore/latest"),
       "URL for the beacon chain APY endpoint to obtain the current APY value.")
      (beacon_chain_outpost_addrs,
       bpo::value<std::string>(),
       "filename for the beacon chain outpost addresses endpoint to obtain the current outpost addresses.")
      (beacon_chain_liqeth_addrs,
       bpo::value<std::string>(),
       "filename for the beacon chain liqeth addresses endpoint to obtain the current liqeth addresses.")
      (beacon_chain_interval,
       boost::program_options::value<std::vector<std::string>>()->multitoken(),
       "Interval specification. Format is `<interval-name>,<cron-spec>`"
       " where cron-spec is in standard cron format (e.g. `*/5 * * * *` for every 5 minutes).")
      (beacon_chain_finalize_epoch_interval,
       bpo::value<std::string>(),
       "flag to indicate to finalize the OPP epoch, using the named interval.");
}


void beacon_chain_update_plugin::plugin_shutdown() {
   ilog("Shutdown beacon chain update plugin");
}

} // namespace sysio
