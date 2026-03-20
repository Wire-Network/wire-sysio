#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/url.hpp>
#include <optional>
#include <unordered_map>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
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

   ethereum_contract_tx_fn<fc::variant> finalizeEpoch;
   OPP(const ethereum_client_ptr& client,
       const address_compat_type& contract_address_compat,
       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , finalizeEpoch(create_tx<fc::variant>(get_abi("finalizeEpoch"))) {

      };
};

struct deposit_manager : fc::network::ethereum::ethereum_contract_client {

   ethereum_contract_tx_fn<fc::variant, uint64_t> setEntryQueue;
   deposit_manager(const ethereum_client_ptr& client,
                   const address_compat_type& contract_address_compat,
                   const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , setEntryQueue(create_tx<fc::variant, uint64_t>(get_abi("setEntryQueue"))) {

      };
};

struct withdrawal_queue : fc::network::ethereum::ethereum_contract_client {

   ethereum_contract_tx_fn<fc::variant, uint64_t> setWithdrawalDelay;
   withdrawal_queue(const ethereum_client_ptr& client,
                    const address_compat_type& contract_address_compat,
                    const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , setWithdrawalDelay(create_tx<fc::variant, uint64_t>(get_abi("setWithdrawalDelay"))) {

      };
};
namespace {
   constexpr auto beacon_chain_queue_url                = "beacon-chain-queue-url";
   constexpr auto beacon_chain_default_queue_url        = "https://beaconcha.in/api/v2/ethereum/queues";
   constexpr auto beacon_chain_apy_url                  = "beacon-chain-apy-url";
   constexpr auto beacon_chain_default_apy_url          = "https://beaconcha.in/api/v1/ethstore/latest";
   constexpr auto beacon_chain_api_key                  = "beacon-chain-api-key";
   constexpr auto beacon_chain_deployer                 = "beacon-chain-deployer";
   constexpr auto beacon_chain_contracts_addrs          = "beacon-chain-contracts-addrs";
   constexpr auto beacon_chain_update_queue             = "beacon-chain-update-queue";
   constexpr auto beacon_chain_update_apy               = "beacon-chain-update-apy";
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

   fc::variant get_queues_mainnet(const std::string& queue_url, const std::string& api_key) {
      namespace beast = boost::beast;
      namespace http  = beast::http;
      namespace asio  = boost::asio;
      using tcp       = asio::ip::tcp;

      FC_ASSERT(!api_key.empty(), "beacon-chain-api-key is required for queues API");

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

      FC_ASSERT(res.result() == http::status::ok,
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

      std::string full_url = apy_url;
      if (api_key && !api_key->empty())
         full_url += "?apikey=" + *api_key;

      fc::url url(full_url);
      auto    host = url.host().value();
      auto    port = std::to_string(url.port().value_or(443));
      auto    path = url.path().value_or(std::filesystem::path("/")).string();
      if (auto query = url.query(); query && !query->empty())
         path += "?" + *query;

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

      FC_ASSERT(res.result() == http::status::ok,
                "get_ethstore_latest HTTP error: {} {}",
                static_cast<unsigned>(res.result()), std::string(res.reason()));

      auto response = fc::json::from_string(res.body());
      return response["data"];
   }
}

using namespace std;
using addr_map_t = std::map<std::string, std::string>;
using action = std::function<bool()>;
using interval_actions_t = vector<action>;
using schedules_t = unordered_map<string, services::cron_service::job_schedule>;

class beacon_chain_update_plugin_impl {

public:
   string beacon_chain_queue_url;
   string beacon_chain_apy_url;
   optional<string> beacon_chain_api_key;
   optional<string> beacon_chain_deployer;
   bool update_queue{false};
   bool update_apy{false};
   schedules_t schedules;
   unordered_map<string, interval_actions_t> intervals;
   addr_map_t outpost_addrs;

};


void beacon_chain_update_plugin::plugin_initialize(const variables_map& options) {
   ilog("initializing beacon chain plugin");

   if( options.contains(beacon_chain_contracts_addrs) ) {
      auto client_specs    = options.at(beacon_chain_contracts_addrs).as<std::vector<std::string>>();
      for(const auto& client_spec : client_specs) {
         ilog("found beacon chain outpost addresses: {}", client_spec);
         // auto& addrs_file = options.at(client_spec).as<std::string>();
         // ilog("found - {}", addrs_file);
         fc::variant addrs = fc::json::from_file<fc::variant>(client_spec);
         ilog("got it");
         const auto addrs_obj = addrs.get_object();
         for(const auto& entry : addrs_obj) {
            const auto& name = entry.key();
            const auto& addr = entry.value().as_string();
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
                     "Repeated interval spec name: {}, schedule: {}", parts[0], parts[1]);
      }
   }
   else {
      if (my->schedules.empty()) {
         ilog("No beacon chain intervals provided, using `default` interval of every 1 hour");
         my->schedules.emplace("default", services::parse_cron_schedule_or_throw("*/6 * * * *"));
      }
   }

   auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
   auto& oec_plug = app().get_plugin<outpost_ethereum_client_plugin>();
   const auto& clients = oec_plug.get_clients();
   SYS_ASSERT(clients.size() > 0, sysio::chain::plugin_config_exception,
      "At least one ethereum client must be configured for beacon chain update plugin");
   const auto ethClient = clients.front()->client;
   const auto dm_addr = my->outpost_addrs[contracts::deposit_manager];
   ilog("dm_addr={}", dm_addr);
   const auto wq_addr = my->outpost_addrs[contracts::withdrawal_queue];
   ilog("wq_addr={}", wq_addr);
   ilog("reading abis");
   ilog("done reading abis");
   ilog("oppContract");
   auto dmContract = ethClient->get_contract<deposit_manager>(dm_addr, contract_abis);
   ilog("dmContract");
   auto wqContract = ethClient->get_contract<withdrawal_queue>(wq_addr, contract_abis);
   ilog("wqContract");

   if( options.contains(beacon_chain_finalize_epoch_interval) ) {
      ilog("initializing beacon chain finalize epoch interval");
      SYS_ASSERT( my->outpost_addrs.size() > 0, sysio::chain::plugin_config_exception,
         "finalize epoch option is only valid if outpost address file is provided" );
      SYS_ASSERT( my->outpost_addrs.count(contracts::OPP) > 0, sysio::chain::plugin_config_exception,
         "finalize epoch option is only valid if outpost address file is provided" );

      const auto opp_addr = my->outpost_addrs[contracts::OPP];
      ilog("opp_addr={}", opp_addr);
      auto abis = oec_plug.get_abi_files();
      ilog("determine size");
      const auto add_size = [](std::size_t a, const auto& abi_file_and_contracts) {
         const auto& [abi_file, abi_contracts] = abi_file_and_contracts;
         return a + abi_contracts.size();
      };

      std::vector<fc::network::ethereum::abi::contract> contract_abis;
      const auto collect_abis = [&contract_abis](const auto& abi_file_and_contracts) {
         const auto& [abi_file, abi_contracts] = abi_file_and_contracts;
         contract_abis.insert(contract_abis.end(), abi_contracts.begin(), abi_contracts.end());
      };
      const auto reserve_size = std::accumulate(abis.begin(), abis.end(), 0, add_size);
      ilog("total={}", reserve_size);
      contract_abis.clear();
      contract_abis.reserve(reserve_size);
      std::transform(abis.begin(), abis.end(), std::back_inserter(contract_abis), add_size);
      std::for_each(abis.begin(), abis.end(), collect_abis);
      auto oppContract = ethClient->get_contract<OPP>(opp_addr, contract_abis);

      auto& finalize_epoch_interval = options.at(beacon_chain_finalize_epoch_interval).as<std::string>();
      auto& actions = my->intervals[finalize_epoch_interval];
      auto action = [&my_ = *my, oppContract, ethClient]() {
         ilog("finalizing OPP epoch");
         const auto bn = ethClient->get_block_number();
         ilog("Executing beacon chain update for interval bn {}", (uint64_t)bn);
         try {
            ilog("Sending finalizeEpoch transaction to OPP contract at address {}", fc::to_hex(ethClient->get_address(), true));
            auto res = oppContract->finalizeEpoch();
            ilog("finalizeEpoch tx sent, hash: {}", res.as_string());
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
         }
         return true;
      };
      actions.emplace_back(std::move(action));
      
   }

   my->beacon_chain_queue_url = options.at(beacon_chain_queue_url).as<std::string>();
   my->beacon_chain_apy_url   = options.at(beacon_chain_apy_url).as<std::string>();

   const optional<string> update_queue = options.contains(beacon_chain_update_queue)
    ? optional<string>{options.at(beacon_chain_update_queue).as<string>()}
    : optional<string>{};
   const optional<string> update_apy   = options.contains(beacon_chain_update_apy)
    ? optional<string>{options.at(beacon_chain_update_apy).as<string>()}
    : optional<string>{};
   my->beacon_chain_api_key = options.contains(beacon_chain_api_key)
    ? optional<string>{options.at(beacon_chain_api_key).as<std::string>()}
    : optional<string>{};

   if( update_queue.has_value() ) {
      ilog("beacon chain queue update enabled");
      FC_ASSERT(my->beacon_chain_api_key.has_value(), "beacon-chain-api-key is required for queue update");
      auto& actions = my->intervals[*my->beacon_chain_api_key];
      auto action = [&my_ = *my, oppContract, ethClient]() {
         ilog("update Queue");
         auto queues = get_queues_mainnet(my_.beacon_chain_queue_url, *(my_.beacon_chain_api_key));
         ilog("queues: {}", fc::json::to_string(queues, fc::time_point::maximum()));
         try {
            ilog("Sending finalizeEpoch transaction to OPP contract at address {}", fc::to_hex(ethClient->get_address(), true));
//            ilog("finalizeEpoch tx sent, hash: {}", res.as_string());
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
         }
         return true;
      };
      actions.emplace_back(std::move(action));
   }
   if( update_apy.has_value() ) {
      ilog("beacon chain APY update enabled");
      auto ethstore = get_ethstore_latest(my->beacon_chain_apy_url, my->beacon_chain_api_key);
//      ilog("ethstore: {}", fc::json::to_string(ethstore, fc::time_point::maximum()));
   }
   ilog("initializing beacon chain plugin DONE");
}

void beacon_chain_update_plugin::plugin_startup() {
   ilog("Starting beacon chain update plugin");
   auto& cron = app().get_plugin<sysio::cron_plugin>();
   auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
   auto& oec_plug = app().get_plugin<outpost_ethereum_client_plugin>();
   const auto& clients = oec_plug.get_clients();
   SYS_ASSERT(clients.size() > 0, sysio::chain::plugin_config_exception,
      "At least one ethereum client must be configured for beacon chain update plugin");
   const auto ethClient = clients.front()->client;
   for (const auto& [name, schedule] : my->schedules) {
      ilog("Scheduling beacon chain update for interval {}", name);
      cron.add_job(schedule, []() {
         ilog("Executing beacon chain update for");
         try {
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
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
      (beacon_chain_update_queue,
       bpo::bool_switch()->default_value(false),
       "Enable fetching the beacon chain deposit/exit queue data and updating on-chain contracts.")
      (beacon_chain_update_apy,
       bpo::bool_switch()->default_value(false),
       "Enable fetching the beacon chain APY data and updating on-chain contracts.")
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
