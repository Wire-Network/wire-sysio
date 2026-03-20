#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <optional>
#include <unordered_map>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/services/cron_parser.hpp>
#include <sysio/services/cron_service.hpp>


#include <sysio/beacon_chain_update_plugin.hpp>

namespace bpo = boost::program_options;
using namespace appbase;
using namespace sysio;

namespace sysio {
// using namespace outpost_client::ethereum;

struct OPP : fc::network::ethereum::ethereum_contract_client {

   ethereum_contract_tx_fn<fc::variant> finalizeEpoch;
   OPP(const ethereum_client_ptr& client,
       const address_compat_type& contract_address_compat,
       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , finalizeEpoch(create_tx<fc::variant>(get_abi("finalizeEpoch"))) {

      };
};

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
      constexpr auto OPP = "OPP";
      constexpr auto deposit_manager = "DepositManager";
      constexpr auto withdrawal_queue = "WithdrawalQueue";
   }

   [[maybe_unused]] inline fc::logger& logger() {
      static fc::logger log{"beacon_chain_update_plugin"};
      return log;
   }
}

using namespace std;
using addr_map_t = std::map<std::string, std::string>;
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
   ilog("initializing beacon chain plugin");
   auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();

   if( options.contains(beacon_chain_outpost_addrs) ) {
      ilog("found beacon chain outpost addresses");
      auto& outpost_addrs_file = options.at(beacon_chain_outpost_addrs).as<std::string>();
      fc::variant addrs = fc::json::from_file<fc::variant>(outpost_addrs_file);
      ilog("got it");
      const auto addrs_obj = addrs.get_object();
      for(const auto& entry : addrs_obj) {
         const auto& name = entry.key();
         const auto& addr = entry.value().as_string();
         ilog("outpost address - {}: {}", name, addr);
         my->outpost_addrs.emplace(name, addr);
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
   else {
      if (my->schedules.empty()) {
         ilog("No beacon chain intervals provided, using `default` interval of every 1 hour");
         my->schedules.emplace("default", services::parse_cron_schedule_or_throw("* */1 * * *"));
      }
   }

   if( options.contains(beacon_chain_finalize_epoch_interval) ) {
      ilog("initializing beacon chain finalize epoch interval");
      SYS_ASSERT( my->outpost_addrs.size() > 0, sysio::chain::plugin_config_exception,
         "finalize epoch option is only valid if outpost address file is provided" );
      SYS_ASSERT( my->outpost_addrs.count(contracts::OPP) > 0, sysio::chain::plugin_config_exception,
         "finalize epoch option is only valid if outpost address file is provided" );
      auto& finalize_epoch_interval = options.at(beacon_chain_finalize_epoch_interval).as<std::string>();
      auto& actions = my->intervals[finalize_epoch_interval];
      auto action = [&sig_plug, &opp_addr = my->outpost_addrs.at(contracts::OPP)]() {
         ilog("finalizing OPP epoch");
      };
//      actions.emplace_back(std::move(action));
      
   }
}

void beacon_chain_update_plugin::plugin_startup() {
   ilog("Starting beacon chain update plugin");
   auto& cron = app().get_plugin<sysio::cron_plugin>();
   auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
   auto& oec_plug = app().get_plugin<outpost_ethereum_client_plugin>();
   const auto& clients = oec_plug.get_clients();
   SYS_ASSERT(clients.size() > 0, sysio::chain::plugin_config_exception,
      "At least one ethereum client must be configured for beacon chain update plugin");
   const auto client = clients.front()->client;
   for (const auto& [name, schedule] : my->schedules) {
      ilog("Scheduling beacon chain update for interval {}", name);
      const auto opp_addr = my->outpost_addrs[contracts::OPP];
      ilog("opp_addr={}", opp_addr);
      auto abis = oec_plug.get_abi_files();
      size_t reserve_size = 0;
      std::for_each(abis.begin(), abis.end(), [&reserve_size](const auto& abi_file_and_contracts) {
         const auto& [abi_file, abi_contracts] = abi_file_and_contracts;
         reserve_size += abi_contracts.size();
      });
      std::vector<fc::network::ethereum::abi::contract> opp_contract_abis;
      opp_contract_abis.reserve(reserve_size);
      std::for_each(abis.begin(), abis.end(), [&](const auto& abi_file_and_contracts) {
         const auto& [abi_file, abi_contracts] = abi_file_and_contracts;
         opp_contract_abis.insert(opp_contract_abis.end(), abi_contracts.begin(), abi_contracts.end());
      });
      auto contract = client->get_contract<OPP>(opp_addr, opp_contract_abis);
      cron.add_job(schedule, [&my_ = *my, contract, client, count=0]() mutable {
         const auto bn = client->get_block_number();
         ilog("Executing beacon chain update for interval bn {}", (uint64_t)bn);
         try {
            ilog("Sending finalizeEpoch transaction to OPP contract at address {}", fc::to_hex(client->get_address(), true));
            auto res = contract->finalizeEpoch();
            ilog("finalizeEpoch tx sent, hash: {}", res.as_string());
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
         }
         // REMOVE AFTER TESTING
         if (++count == 5) {
            throw std::runtime_error("Test exception to stop cron job after 5 executions");
            return false;
         }
      },
      cron_service::job_metadata_t{
         .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "cron_1min_heartbeat"
      });
   }
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
       " where cron-spec is in standard cron format (e.g. `*/5 * * * *` for every 5 minutes)."
       " If none are provided, a default interval with name `default` and schedule of every"
       " 1 hour will be used (e.g. `default, * */1 * * *`).")
      (beacon_chain_finalize_epoch_interval,
       bpo::value<std::string>(),
       "flag to indicate to finalize the OPP epoch, using the named interval.");
}


void beacon_chain_update_plugin::plugin_shutdown() {
   ilog("Shutdown beacon chain update plugin");
}

} // namespace sysio
