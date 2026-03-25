#include <curl/curl.h>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/url.hpp>
#include <optional>
#include <unordered_map>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <regex>
#include <sysio/chain/exceptions.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/services/cron_parser.hpp>
#include <sysio/services/cron_service.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>


#include <sysio/beacon_chain_update_plugin.hpp>

namespace bpo = boost::program_options;
using namespace appbase;
using namespace sysio;

namespace sysio {
// using namespace outpost_client::ethereum;

struct OPP : fc::network::ethereum::ethereum_contract_client {
   static constexpr auto contract_name = "OPP";

   ethereum_contract_tx_fn<fc::variant> finalizeEpoch;
   OPP(const ethereum_client_ptr& client,
       const address_compat_type& contract_address_compat,
       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , finalizeEpoch(create_tx<fc::variant>(get_abi("finalizeEpoch"))) {

      };
};

struct deposit_manager : fc::network::ethereum::ethereum_contract_client {
   static constexpr auto contract_name = "DepositManager";

   ethereum_contract_tx_fn<fc::variant, uint64_t> setEntryQueue;
   ethereum_contract_tx_fn<fc::variant, uint64_t> updateApyBPS;
   deposit_manager(const ethereum_client_ptr& client,
                   const address_compat_type& contract_address_compat,
                   const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , setEntryQueue(create_tx<fc::variant, uint64_t>(get_abi("setEntryQueue")))
      , updateApyBPS(create_tx<fc::variant, uint64_t>(get_abi("updateApyBPS"))) {

      };
};

struct withdrawal_queue : fc::network::ethereum::ethereum_contract_client {
   static constexpr auto contract_name = "WithdrawalQueue";

   ethereum_contract_tx_fn<fc::variant, uint64_t> setWithdrawDelay;
   withdrawal_queue(const ethereum_client_ptr& client,
                    const address_compat_type& contract_address_compat,
                    const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , setWithdrawDelay(create_tx<fc::variant, uint64_t>(get_abi("setWithdrawDelay"))) {

      };
};
namespace {
   constexpr auto beacon_chain_queue_url                = "beacon-chain-queue-url";
   constexpr auto beacon_chain_default_queue_url        = "https://beaconcha.in/api/v2/ethereum/queues";
   constexpr auto beacon_chain_apy_url                  = "beacon-chain-apy-url";
   constexpr auto beacon_chain_default_apy_url          = "https://beaconcha.in/api/v1/ethstore/latest";
   constexpr auto beacon_chain_api_key                  = "beacon-chain-api-key";
   constexpr auto beacon_chain_contracts_addrs          = "beacon-chain-contracts-addrs";
   constexpr auto beacon_chain_update_interval          = "beacon-chain-update-interval";
   constexpr auto beacon_chain_interval                 = "beacon-chain-interval";
   constexpr auto beacon_chain_finalize_epoch_interval  = "beacon-chain-finalize-epoch-interval";

   constexpr auto client_target_chain                   = fc::crypto::chain_kind_t::chain_kind_ethereum;
   constexpr auto abi_contract_name_field               = "contractName";

   constexpr auto default_interval_schedule             = "* */1 * * *"; // every hour
   constexpr auto default_interval_name                 = "default";

   const std::regex regex(R"(^(.+?)(?:V\d+)?$)");

   [[maybe_unused]] inline fc::logger& logger() {
      static fc::logger log{"beacon_chain_update_plugin"};
      return log;
   }

   fc::variant get_queues_mainnet(const std::string& queue_url, const std::string& api_key) {
      namespace beast = boost::beast;
      namespace http  = beast::http;
      namespace asio  = boost::asio;
      using tcp       = asio::ip::tcp;

      SYS_ASSERT(!api_key.empty(), sysio::chain::plugin_config_exception,
                 "beacon-chain-api-key is required for queues API");

      fc::url url(queue_url);
      auto    host = url.host().value();
      auto    port = std::to_string(url.port().value_or(443));
      auto    path = url.path().value_or(std::filesystem::path("/")).string();

      asio::io_context    ioc;
      asio::ssl::context  ssl_ctx{asio::ssl::context::tlsv12_client};
      tcp::resolver       resolver{ioc};
      auto                dest = resolver.resolve(host, port);

      http::request<http::string_body> req{http::verb::post, path, 11};
      req.set(http::field::host, host);
      req.set(http::field::content_type, "application/json");
      req.set(http::field::authorization, "Bearer " + api_key);
      req.body() = R"({"chain":"mainnet"})";
      req.prepare_payload();

      beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
      if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
         throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                                     asio::error::get_ssl_category()));

      beast::get_lowest_layer(stream).connect(dest);
      stream.handshake(asio::ssl::stream_base::client);
      http::write(stream, req);

      beast::flat_buffer                buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ec;
      stream.shutdown(ec);

      SYS_ASSERT(res.result() == http::status::ok,
                 sysio::chain::plugin_config_exception,
                 "get_queues_mainnet HTTP error: {} {}",
                 static_cast<unsigned>(res.result()), std::string(res.reason()));

      auto response = fc::json::from_string(res.body());
      return response["data"];
   }

   fc::variant get_ethstore_latest(const std::string& apy_url, const std::optional<std::string>& api_key) {
      namespace beast = boost::beast;
      namespace http  = beast::http;
      namespace asio  = boost::asio;
      using tcp       = asio::ip::tcp;

      // Parse the base URL only — fc::url::query() is broken and never stores the query string
      // during parsing, so appending ?apikey= before parsing would silently discard the key.
      fc::url url(apy_url);
      auto    host = url.host().value();
      auto    port = std::to_string(url.port().value_or(443));
      auto    path = url.path().value_or(std::filesystem::path("/")).string();
      if (api_key && !api_key->empty()) {
         char* escaped = curl_easy_escape(nullptr, api_key->c_str(), static_cast<int>(api_key->size()));
         path += "?apikey=";
         path += escaped;
         curl_free(escaped);
      }

      asio::io_context    ioc;
      asio::ssl::context  ssl_ctx{asio::ssl::context::tlsv12_client};
      tcp::resolver       resolver{ioc};
      auto                dest = resolver.resolve(host, port);

      http::request<http::string_body> req{http::verb::get, path, 11};
      req.set(http::field::host, host);
      req.set(http::field::content_type, "application/json");
      req.prepare_payload();

      beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
      if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
         throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                                     asio::error::get_ssl_category()));

      beast::get_lowest_layer(stream).connect(dest);
      stream.handshake(asio::ssl::stream_base::client);
      http::write(stream, req);

      beast::flat_buffer                buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ec;
      stream.shutdown(ec);

      SYS_ASSERT(res.result() == http::status::ok,
                 sysio::chain::plugin_config_exception,
                 "get_ethstore_latest HTTP error: {} {}",
                 static_cast<unsigned>(res.result()), std::string(res.reason()));

      auto response = fc::json::from_string(res.body());
      return response["data"];
   }
}

using namespace std;
using addr_map_t = std::map<std::string, std::string>;
using action = std::function<void()>;
using interval_actions_t = vector<action>;
using schedules_t = unordered_map<string, services::cron_service::job_schedule>;
using ethereum_client_ptr = fc::network::ethereum::ethereum_client_ptr;

class beacon_chain_update_plugin_impl {

public:
   string beacon_chain_queue_url;
   string beacon_chain_queue_interval;
   string beacon_chain_apy_url;
   string beacon_chain_apy_interval;
   optional<string> beacon_chain_api_key;
   schedules_t schedules;
   string actual_default_schedule;
   unordered_map<string, interval_actions_t> intervals;

   addr_map_t outpost_addrs;

   interval_actions_t& find_interval_actions(string interval_name) {
      // if the interval actions are already created, we can just use it
      if(intervals.count(interval_name) > 0) {
         return intervals[interval_name];
      }

      // This is used to make sure that there is a corresponding cron schedule associated with each collection of actions
      if(schedules.count(interval_name) == 0) {
         ilog("Could not find a schedule named {}, using {} interval", interval_name, default_interval_name);
         interval_name = actual_default_schedule;
      }

      return intervals[interval_name];
   }

   template <typename C>
   std::pair<std::shared_ptr<C>, ethereum_client_ptr> get_contract(const outpost_ethereum_client_plugin& oec_plugin) const {
      constexpr auto desired_contract_name = C::contract_name;
      const auto clients = oec_plugin.get_clients();
      ethereum_client_ptr client;
      for(const auto& client_entry : clients) {
         ilog("id={}", client_entry->id);
         if(client_target_chain == client_entry->signature_provider->target_chain) {
            SYS_ASSERT(!client, sysio::chain::plugin_config_exception,
                       "There should only be one ethereum client provided, but there were at least 2");
            client = client_entry->client;
            break;
         }
      }
      SYS_ASSERT(!!client, sysio::chain::plugin_config_exception,
                 "could not find any ethereum client for {}", desired_contract_name);

      auto itr = outpost_addrs.find(desired_contract_name);
      SYS_ASSERT(itr != outpost_addrs.end(), sysio::chain::plugin_config_exception,
                 "contract {} address was not provided in an abi file", desired_contract_name );

      const auto contract_addr = itr->second;
      const auto abis = oec_plugin.get_abi_files();
      std::vector<fc::network::ethereum::abi::contract> contract_abis;
      for(const auto& abi_file_and_contracts : abis) {
         const auto& [json_abi_file, abi_contracts] = abi_file_and_contracts;
         auto json_var = fc::json::from_file(json_abi_file);
         if(!json_var.is_object())
            continue;

         const auto var_obj = json_var.get_object();
         if(!var_obj.contains(abi_contract_name_field))
            continue;

         const auto contract_name_var = var_obj[abi_contract_name_field];
         if(contract_name_var.is_array())
            continue;

         const auto contract_name = contract_name_var.as<std::string>();

         std::smatch matches;
         if(!std::regex_search(contract_name, matches, regex))
            continue;

         if(matches[1].str() != desired_contract_name)
            continue;

         contract_abis.insert(contract_abis.end(), abi_contracts.begin(), abi_contracts.end());
         break;
      }

      std::shared_ptr<C> contract;
      if(contract_abis.size()) {
         contract = client->get_contract<C>(contract_addr, contract_abis);
      }

      return { contract, client };
   }

   constexpr static auto epa_field = "estimated_processed_at";

   static optional<fc::variant> get_field_from_object(const fc::variant& expected_obj, const string& expected_field) {
      if (!expected_obj.is_object())
         return {};

      const auto actual_obj = expected_obj.get_object();
      if (!actual_obj.contains(expected_field))
         return {};

      return actual_obj[expected_field];
   }

   // reported in seconds
   static optional<uint64_t> get_queue_length(const fc::variant& queues, const string& queue_branch) {
      const auto deposit_queue = get_field_from_object(queues, queue_branch);
      SYS_ASSERT(!!deposit_queue, sysio::chain::plugin_config_exception,
                 "Returned api request:\n{}\n doesn't contain the field {}",
                 fc::json::to_string(queues, fc::time_point::maximum()), queue_branch);
      const auto epa_var = get_field_from_object(*deposit_queue, epa_field);
      SYS_ASSERT(!!epa_var, sysio::chain::plugin_config_exception,
                 "{}:\n{}\n doesn't contain a key of {}",
                 queue_branch, fc::json::to_string(queues, fc::time_point::maximum()), epa_field);
      SYS_ASSERT(epa_var->is_numeric(), sysio::chain::plugin_config_exception,
                 "queues[{}][{}]:\n{}\n doesn't contain a number",
                 queue_branch, epa_field,
                 fc::json::to_string(queues, fc::time_point::maximum()));

      const auto now_sec = fc::time_point::now().sec_since_epoch();
      const auto epa = epa_var->as_uint64();
      return now_sec - epa;
   }
};

void beacon_chain_update_plugin::plugin_initialize(const variables_map& options) {
   ilog("initializing beacon chain plugin");

   if( options.contains(beacon_chain_contracts_addrs) ) {
      auto client_specs    = options.at(beacon_chain_contracts_addrs).as<std::vector<std::string>>();
      for(const auto& client_spec : client_specs) {
         ilog("found beacon chain outpost addresses: {}", client_spec);
         fc::variant addrs = fc::json::from_file<fc::variant>(client_spec);
         const auto addrs_obj = addrs.get_object();
         for(const auto& entry : addrs_obj) {
            const auto name = entry.key();
            const auto addr = entry.value().as_string();
            ilog("outpost address - {}: {}", name, addr);
            my->outpost_addrs.emplace(name, addr);
         }
      }
   }

   if( options.contains(beacon_chain_interval) ) {
      ilog("initializing beacon chain intervals");
      auto client_specs    = options.at(beacon_chain_interval).as<std::vector<std::string>>();
      for (auto& client_spec : client_specs) {
         auto parts = fc::split(client_spec, ',', 1);
         auto schedule_inserted = my->schedules.emplace(parts[0], services::parse_cron_schedule_or_throw(parts[1]));
         SYS_ASSERT(schedule_inserted.second, chain::plugin_config_exception,
                    "Repeated interval spec name: `{}`, schedule: `{}`", parts[0], parts[1]);
         if(my->actual_default_schedule.empty()) {
            my->actual_default_schedule = parts[0];
            ilog("Interval schedule name: `{}`, with schedule: `{}`, will be used for `{}`",
                 parts[0], parts[1], default_interval_name);
         }
      }
   }
   else {
      ilog("No beacon chain interval schedules provided, using `{}` schedule with name `{}`", default_interval_schedule, default_interval_name);
      my->schedules.emplace(default_interval_name, services::parse_cron_schedule_or_throw(default_interval_schedule));
   }

   auto& oec_plugin = app().get_plugin<outpost_ethereum_client_plugin>();

   if( options.contains(beacon_chain_finalize_epoch_interval) ) {
      ilog("initializing beacon chain finalize epoch interval");
      auto [ opp_contract, eth_client ] = my->get_contract<OPP>(oec_plugin);

      auto& finalize_epoch_interval = options.at(beacon_chain_finalize_epoch_interval).as<std::string>();
      auto& actions = my->find_interval_actions(finalize_epoch_interval);
      auto action = [&my_ = *my, opp_contract, eth_client]() {
         ilog("finalizing OPP epoch");
         const auto bn = eth_client->get_block_number();
         ilog("Executing beacon chain update for interval bn {}", (uint64_t)bn);
         try {
            ilog("Sending finalizeEpoch transaction to OPP contract at address {}", fc::to_hex(eth_client->get_address(), true));
            auto res = opp_contract->finalizeEpoch();
            ilog("finalizeEpoch tx sent, hash: {}", res.as_string());
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
         }
         return true;
      };
      actions.emplace_back(std::move(action));
      ilog("There are {} actions currently registered.", actions.size());
      
   }

   my->beacon_chain_api_key = options.contains(beacon_chain_api_key)
    ? optional<string>{options.at(beacon_chain_api_key).as<std::string>()}
    : optional<string>{};

   if( options.contains(beacon_chain_api_key) ) {
      ilog("beacon chain queue/apy update enabled");
      auto [ wq_contract, eth_client ] = my->get_contract<withdrawal_queue>(oec_plugin);
      auto [ dm_contract, eth_client2 ] = my->get_contract<deposit_manager>(oec_plugin);
      SYS_ASSERT(eth_client == eth_client2, sysio::chain::plugin_config_exception,
                 "get_contract should be returning the same ethereum client for both contracts");
      SYS_ASSERT(!!wq_contract || !!dm_contract, sysio::chain::plugin_config_exception,
                 "If {} is set, then must provide at least {}'s or {}'s contract address",
                 beacon_chain_api_key, withdrawal_queue::contract_name, deposit_manager::contract_name);
      my->beacon_chain_queue_url = options.at(beacon_chain_queue_url).as<std::string>();
      my->beacon_chain_queue_interval = options.at(beacon_chain_update_interval).as<std::string>();
      my->beacon_chain_apy_url = options.at(beacon_chain_apy_url).as<std::string>();
      auto& actions = my->find_interval_actions(my->beacon_chain_queue_interval);
      auto action = [&my_ = *my, wq_contract, dm_contract, eth_client]() {
         try {
            ilog("update Queue");
            auto queues = get_queues_mainnet(my_.beacon_chain_queue_url, *(my_.beacon_chain_api_key));
            ilog("queues: {}", fc::json::to_string(queues, fc::time_point::maximum()));
            constexpr auto exit_queue = "exit_queue";

            const auto exit_queue_len_sec = my_.get_queue_length(queues, exit_queue);

            constexpr auto nine_days = 9;
            constexpr auto nine_days_in_sec = 60 * 60 * 24 * nine_days;
            if(!exit_queue_len_sec)
               wlog("defaulting the {} withdrawal delay to {} days since {}::{} was not a finite number",
                     withdrawal_queue::contract_name, nine_days, exit_queue,
                     beacon_chain_update_plugin_impl::epa_field);
            auto exit_queue_delay_len_sec = nine_days_in_sec +
               (!!exit_queue_len_sec ? *exit_queue_len_sec : 0);
            ilog("Sending setWithdrawDelay transaction to {} contract at address {}",
                 withdrawal_queue::contract_name, fc::to_hex(eth_client->get_address(), true));
            if(!!wq_contract) {
               auto res = wq_contract->setWithdrawDelay(exit_queue_delay_len_sec);
               ilog("setWithdrawDelay tx sent, hash: {}", res.as_string());
            }

            if(!dm_contract)
               return;

            constexpr auto deposit_queue = "deposit_queue";
            const auto deposit_queue_len_sec = my_.get_queue_length(queues, deposit_queue);
            const auto default_days = 1;
            uint64_t depositQDaysFl = !deposit_queue_len_sec
               ? default_days
               : *deposit_queue_len_sec / (60 * 60 * 24); // convert sec to min, min to hours, hours to days
            if(!deposit_queue_len_sec)
               wlog("defaulting the {} withdrawal delay to {} day since {} was not a finite number",
                     deposit_manager::contract_name, depositQDaysFl, deposit_queue,
                     beacon_chain_update_plugin_impl::epa_field);

            ilog("Sending setEntryQueue transaction to {} contract at address {}",
                 deposit_manager::contract_name, fc::to_hex(eth_client->get_address(), true));

            auto res1 = dm_contract->setEntryQueue(depositQDaysFl);
            ilog("setEntryQueue tx sent, hash: {}", res1.as_string());

            auto ethstore = get_ethstore_latest(my_.beacon_chain_apy_url, *(my_.beacon_chain_api_key));
            ilog("ethstore: {}", fc::json::to_string(ethstore, fc::time_point::maximum()));
            ilog("Sending setEntryQueue transaction to DepositManager contract at address {}", fc::to_hex(eth_client->get_address(), true));
            constexpr auto avgapr7d_field = "avgapr7d";
            const auto apy = my_.get_field_from_object(ethstore, avgapr7d_field);
            if(!apy) {
               elog("ethstore:\n{}\n did not have a {} field, not setting the {} contract entry queue",
                    fc::json::to_string(ethstore, fc::time_point::maximum()), avgapr7d_field, deposit_manager::contract_name);
               return;
            }
            double aprFraction = 1.0;
            if(apy->is_double())
               aprFraction = apy->as_double();
            auto scaled = static_cast<uint64_t>(aprFraction * 10000.0 + 1e-12);
            auto res2 = dm_contract->updateApyBPS(scaled);
            ilog("updateApyBPS tx sent, hash: {}", res2.as_string());
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
         }
      };
      actions.emplace_back(std::move(action));
      ilog("There are {} actions currently registered.", actions.size());
   }

   ilog("initializing beacon chain plugin DONE");
}

void beacon_chain_update_plugin::plugin_startup() {
   ilog("Starting beacon chain update plugin");
   auto& cron = app().get_plugin<sysio::cron_plugin>();
   auto& oec_plugin = app().get_plugin<outpost_ethereum_client_plugin>();
   const auto clients = oec_plugin.get_clients();
   SYS_ASSERT(clients.size() > 0, sysio::chain::plugin_config_exception,
              "At least one ethereum client must be configured for beacon chain update plugin");
   const auto eth_client = clients.front()->client;
   ilog("There are {} schedule currently available.", my->schedules.size());
   ilog("There are {} actions currently registered.", my->intervals.size());
   for (const auto& [name, schedule] : my->schedules) {
      ilog("Scheduling beacon chain update for interval {}", name);

      auto& actions = my->find_interval_actions(name);
      ilog("There are {} actions currently registered for this interval.", actions.size());
      if(actions.empty()) {
         ilog("No actions to register for interval {}", name);
         continue;
      }
      ilog("{} actions to register for interval {}", actions.size(), name);

      cron.add_job(schedule, [&name, &actions]() {
         ilog("Executing beacon chain update for {}", name);
         for(const auto& action : actions) {
            try {
               action();
            }
            catch (const std::exception& e) {
               elog("Error executing beacon chain update for interval: {}", e.what());
            }
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
       bpo::value<std::string>()->default_value(beacon_chain_default_queue_url),
       "URL for the beacon chain queues endpoint to obtain the current queue duration.")
      (beacon_chain_apy_url,
       bpo::value<std::string>()->default_value(beacon_chain_default_apy_url),
       "URL for the beacon chain APY endpoint to obtain the current APY value.")
      (beacon_chain_api_key,
       bpo::value<std::string>(),
       "API key for authenticating requests to the beacon chain endpoints.")
      (beacon_chain_update_interval,
       bpo::value<std::string>()->default_value(default_interval_name),
       "Enable fetching the beacon chain deposit/exit queue data and updating on-chain contracts, using the indicated interval.")
      (beacon_chain_contracts_addrs,
       bpo::value<std::vector<std::string>>()->multitoken(),
       "filename to provide addresses for any needed contracts.")
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
