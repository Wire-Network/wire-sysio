#include <sysio/wire_eth_maintenance_plugin.hpp>

#include <sysio/beacon_chain_config_updates.hpp>
#include <sysio/beacon_chain_update_detail.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/services/cron_parser.hpp>
#include <sysio/services/cron_service.hpp>

#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/url.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <curl/curl.h>

#include <optional>
#include <unordered_map>

namespace bpo = boost::program_options;
using namespace appbase;
using namespace sysio;

namespace sysio {
struct OPP : fc::network::ethereum::ethereum_contract_client {
   static constexpr auto contract_name = "OPP";

   ethereum_contract_tx_fn<fc::variant> finalizeEpoch;
   OPP(const ethereum_client_ptr& client,
       const address_compat_type& contract_address_compat,
       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , finalizeEpoch(create_tx<fc::variant>(get_abi("finalizeEpoch"))) {

   }
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

   }
};

struct withdrawal_queue : fc::network::ethereum::ethereum_contract_client {
   static constexpr auto contract_name = "WithdrawalQueue";

   ethereum_contract_tx_fn<fc::variant, uint64_t> setWithdrawDelay;
   withdrawal_queue(const ethereum_client_ptr& client,
                    const address_compat_type& contract_address_compat,
                    const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts)
      , setWithdrawDelay(create_tx<fc::variant, uint64_t>(get_abi("setWithdrawDelay"))) {

   }
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
   constexpr auto beacon_chain_network                  = "beacon-chain-network";
   constexpr auto beacon_chain_exit_buffer_days         = "beacon-chain-exit-buffer-days";

   constexpr auto client_target_chain                   = fc::crypto::chain_kind_t::chain_kind_ethereum;
   constexpr auto default_interval_schedule             = "* */1 * * *"; // every hour
   constexpr auto default_interval_name                 = "default";
   constexpr auto just_once_interval_name               = "once";

   fc::variant https_request(const std::string& url_str,
                             boost::beast::http::verb method,
                             const std::string& request_body,
                             const std::string& api_key,
                             std::chrono::seconds timeout = std::chrono::seconds(120)) {
      namespace beast = boost::beast;
      namespace http  = beast::http;
      namespace asio  = boost::asio;
      using tcp       = asio::ip::tcp;

      fc::url url(url_str);
      SYS_ASSERT(url.proto() == "https", sysio::chain::plugin_config_exception,
                 "Only https:// URLs are supported here; got `{}` with proto=`{}`",
                 url_str, url.proto());
      SYS_ASSERT(url.host().has_value(), sysio::chain::plugin_config_exception,
                 "URL `{}` has no host component", url_str);
      auto    host = *url.host();
      auto    port = std::to_string(url.port().value_or(443));
      auto    path = url.path().value_or(std::filesystem::path("/")).string();
      ilog("host = {}, port = {}, path = {}", host, port, path);

      asio::io_context   ioc;
      asio::ssl::context ssl_ctx{asio::ssl::context::tlsv12_client};
      tcp::resolver      resolver{ioc};
      auto               dest = resolver.resolve(host, port);
      if (method == boost::beast::http::verb::get) {
         std::unique_ptr<char, decltype(&curl_free)> escaped{
            curl_easy_escape(nullptr, api_key.c_str(), static_cast<int>(api_key.size())),
            &curl_free};
         SYS_ASSERT(escaped != nullptr,
                  sysio::chain::plugin_config_exception,
                  "curl error occurred while performing curl_easy_escape");
         path += "?apikey=";
         path += escaped.get();
      }

      ssl_ctx.set_default_verify_paths();

      uint retry = 0;
      bool valid = false;
      while(true) {

         http::request<http::string_body> req{method, path, 11};
         req.set(http::field::host, host);
         req.set(http::field::content_type, "application/json");
         if (method == boost::beast::http::verb::post)
            req.set(http::field::authorization, "Bearer " + api_key);
         if (!request_body.empty())
            req.body() = request_body;
         req.prepare_payload();

         beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
         stream.set_verify_mode(asio::ssl::verify_peer);
         stream.set_verify_callback(asio::ssl::host_name_verification(host));
         if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
            throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                                      asio::error::get_ssl_category()));

         beast::get_lowest_layer(stream).expires_after(timeout);
         beast::get_lowest_layer(stream).connect(dest);
         stream.handshake(asio::ssl::stream_base::client);
         http::write(stream, req);

         beast::flat_buffer                        buffer;
         http::response_parser<http::string_body>  parser;
         parser.body_limit(8ull * 1024 * 1024); // 8 MiB cap on response body
         http::read(stream, buffer, parser);
         auto& res = parser.get();

         beast::error_code ec;
         stream.shutdown(ec);
         // eof/stream_truncated are benign - many servers close without a clean TLS shutdown.
         if (ec && ec != asio::error::eof && ec != asio::ssl::error::stream_truncated)
            dlog("TLS shutdown returned non-benign error: {}", ec.message());

         uint64_t sec_sleep = 0;

         valid = res.result() == http::status::ok;
         if (valid) {
            dlog("res.body=\n{}", res.body());
            auto response = fc::json::from_string(res.body());
            return response["data"];
         }

         // if we already did one retry, then give up
         if (retry > 0) {
            return {};
         }

         for (auto const& field : res.base()) {
            if (field.name_string() == "Retry-After:") {
               const auto sec_sleep_str = field.value();
               auto [ptr, ec] = std::from_chars(sec_sleep_str.data(), sec_sleep_str.data() + sec_sleep_str.size(), sec_sleep);
               if (ec == std::errc() && ptr == sec_sleep_str.data() + sec_sleep_str.size()) {
                  // identified a valid reason to retry
                  valid = true;
                  std::this_thread::sleep_for(std::chrono::milliseconds(sec_sleep * 1000));
                  break;
               }
            }
         }
         ++retry;
      }

   }

   fc::variant get_queues_network(const std::string& queue_url, const std::string& api_key,
                                  const std::string& network) {
      SYS_ASSERT(!api_key.empty(), sysio::chain::plugin_config_exception,
                 "beacon-chain-api-key is required for queues API");
      const auto body = fc::json::to_string(
         fc::mutable_variant_object("chain", network), fc::time_point::maximum());
      return https_request(queue_url, boost::beast::http::verb::post, body, api_key);
   }

   fc::variant get_ethstore_latest(const std::string& apy_url, const std::string& api_key) {
      // Build the full URL with apikey query param — fc::url::query() is broken and never
      // stores the query string during parsing, so we construct the URL string directly.
      return https_request(apy_url, boost::beast::http::verb::get,
                           {}, api_key, std::chrono::seconds{180});
   }
}

namespace beacon_chain_detail {

   std::optional<fc::variant> get_field_from_object(const fc::variant& expected_obj,
                                                     const std::string& expected_field) {
      if (!expected_obj.is_object())
         return {};

      const auto& actual_obj = expected_obj.get_object();
      if (!actual_obj.contains(expected_field))
         return {};

      return actual_obj[expected_field];
   }

   // reported in seconds
   std::optional<uint64_t> get_queue_length(const fc::variant& queues, const std::string& queue_branch) {
      const auto deposit_queue = get_field_from_object(queues, queue_branch);
      SYS_ASSERT(!!deposit_queue, sysio::chain::plugin_config_exception,
                 "Returned api request:\n{}\n doesn't contain the field {}",
                 fc::json::to_string(queues, fc::time_point::maximum()), queue_branch);
      const auto epa_var = get_field_from_object(*deposit_queue, epa_field);
      SYS_ASSERT(!!epa_var, sysio::chain::plugin_config_exception,
                 "{}:\n{}\n doesn't contain a key of {}",
                 queue_branch, fc::json::to_string(queues, fc::time_point::maximum()), epa_field);
      SYS_ASSERT(epa_var->is_uint64() || epa_var->is_int64(),
                 sysio::chain::plugin_config_exception,
                 "queues[{}][{}]:\n{}\n is not an integer",
                 queue_branch, epa_field,
                 fc::json::to_string(queues, fc::time_point::maximum()));
      if (epa_var->is_int64()) {
         const auto signed_epa = epa_var->as_int64();
         SYS_ASSERT(signed_epa >= 0, sysio::chain::plugin_config_exception,
                    "queues[{}][{}] is negative: {}", queue_branch, epa_field, signed_epa);
      }

      const auto now_sec = fc::time_point::now().sec_since_epoch();
      const auto epa = epa_var->as_uint64();
      if (epa <= now_sec) {
         wlog("queue {} epa={} is in the past (now={}), returning nullopt", queue_branch, epa, now_sec);
         return std::nullopt;
      }
      const auto eta = epa - now_sec;
      ilog("Determined eta={} from now={} and epa={} on branch={}",
           eta, now_sec, epa, queue_branch);
      return eta;
   }

} // namespace beacon_chain_detail

using std::optional;
using std::string;
using std::unordered_map;
using std::vector;

using addr_map_t = std::map<std::string, std::string>;
using action = std::function<void()>;
using interval_actions_t = vector<action>;
using job_schedule = services::cron_service::job_schedule;
using schedules_t = unordered_map<string, job_schedule>;
using ethereum_client_ptr = fc::network::ethereum::ethereum_client_ptr;

class wire_eth_maintenance_plugin_impl {

public:
   schedules_t schedules;
   string actual_default_schedule;
   unordered_map<string, interval_actions_t> intervals;
   interval_actions_t                        just_once_actions;
   optional<cron_service::job_id_t>          just_once_jid;

   addr_map_t outpost_addrs;

   interval_actions_t& find_interval_actions(string interval_name) {
      // if the interval actions are already created, we can just use it
      if(intervals.count(interval_name) > 0) {
         return intervals[interval_name];
      }

      if(interval_name == just_once_interval_name) {
         return just_once_actions;
      }

      // This is used to make sure that there is a corresponding cron schedule associated with each collection of actions
      if(schedules.count(interval_name) == 0) {
         ilog("Could not find a schedule named {}, using {} interval", interval_name, default_interval_name);
         interval_name = actual_default_schedule;
      }

      return intervals[interval_name];
   }

   template <typename C>
   std::pair<std::shared_ptr<C>, ethereum_client_ptr> get_contract(const outpost_ethereum_client_plugin& oec_plugin,
                                                                   ethereum_client_ptr client = ethereum_client_ptr{}) const {
      constexpr auto desired_contract_name = C::contract_name;
      if(!client)
         client = oec_plugin.get_client_for_chain(client_target_chain);

      auto itr = outpost_addrs.find(desired_contract_name);
      SYS_ASSERT(itr != outpost_addrs.end(), sysio::chain::plugin_config_exception,
                 "contract {} address was not provided in an abi file", desired_contract_name);

      auto contract_abis = oec_plugin.get_abis_for_contract(desired_contract_name);

      std::shared_ptr<C> contract;
      if(!contract_abis.empty())
         contract = client->get_contract<C>(itr->second, contract_abis);

      return {contract, client};
   }

};

void wire_eth_maintenance_plugin::plugin_initialize(const variables_map& options) {
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
         SYS_ASSERT(parts.size() == 2, chain::plugin_config_exception,
                    "Interval spec `{}` must be of form `<name>,<cron-spec>`", client_spec);
         SYS_ASSERT(parts[0] != just_once_interval_name, chain::plugin_config_exception,
                    "Cannot use reserved interval spec name: `{}`, to store schedule: `{}`",
                    just_once_interval_name, parts[1]);
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
      my->actual_default_schedule = default_interval_name;
      my->schedules.emplace(default_interval_name, services::parse_cron_schedule_or_throw(default_interval_schedule));
   }

   auto& oec_plugin = app().get_plugin<outpost_ethereum_client_plugin>();

   auto [ opp_contract, eth_client ] = my->get_contract<OPP>(oec_plugin);
   if( opp_contract ) {
      ilog("initializing beacon chain finalize epoch interval");

      auto& finalize_epoch_interval = options.at(beacon_chain_finalize_epoch_interval).as<std::string>();
      auto& actions = my->find_interval_actions(finalize_epoch_interval);
      auto action = [&my_ = *my, opp_contract, eth_client]() {
         ilog("finalizing OPP epoch");
         const auto bn = eth_client->get_block_number();
         ilog("Executing beacon chain update for interval bn {}", static_cast<uint64_t>(bn));
         try {
            ilog("Sending finalizeEpoch transaction to OPP contract using address {}",
                 fc::to_hex(eth_client->get_address(), true));
            auto res = opp_contract->finalizeEpoch();
            ilog("finalizeEpoch tx sent, hash: {}", res.as_string());
         }
         catch (const std::exception& e) {
            elog("Error executing beacon chain update for interval: {}", e.what());
         }
      };
      actions.emplace_back(std::move(action));
      ilog("There are {} actions currently registered.", actions.size());
      
   }

   if( options.contains(beacon_chain_api_key) ) {
      ilog("beacon chain queue/apy update enabled");
      auto wq_contract = my->get_contract<withdrawal_queue>(oec_plugin, eth_client).first;
      auto dm_contract = my->get_contract<deposit_manager>(oec_plugin, eth_client).first;
      SYS_ASSERT(!!wq_contract || !!dm_contract, sysio::chain::plugin_config_exception,
                 "If {} is set, then must provide at least {}'s or {}'s contract address",
                 beacon_chain_api_key, withdrawal_queue::contract_name, deposit_manager::contract_name);

      auto queue_url = options.at(beacon_chain_queue_url).as<std::string>();
      auto apy_url = options.at(beacon_chain_apy_url).as<std::string>();
      auto api_key_val = options.at(beacon_chain_api_key).as<std::string>();
      SYS_ASSERT(api_key_val.find_first_of("\r\n") == std::string::npos,
                 sysio::chain::plugin_config_exception,
                 "--beacon-chain-api-key must not contain CR/LF characters"
                 " (value would be injected into HTTP headers).");
      auto network_val = options.at(beacon_chain_network).as<std::string>();
      auto update_interval = options.at(beacon_chain_update_interval).as<std::string>();
      auto exit_buffer_days = options.at(beacon_chain_exit_buffer_days).as<uint64_t>();

      auto& actions = my->find_interval_actions(update_interval);
      actions.emplace_back(beacon_chain_config_updates({
         .fetch_queues = [=]() { return get_queues_network(queue_url, api_key_val, network_val); },
         .fetch_apy = [=]() { return get_ethstore_latest(apy_url, api_key_val); },
         .send_set_withdraw_delay = wq_contract
            ? std::function<std::string(uint64_t)>([wq_contract](uint64_t val) {
                 return wq_contract->setWithdrawDelay(val).as_string();
              })
            : std::function<std::string(uint64_t)>{},
         .send_set_entry_queue = dm_contract
            ? std::function<std::string(uint64_t)>([dm_contract](uint64_t val) {
                 return dm_contract->setEntryQueue(val).as_string();
              })
            : std::function<std::string(uint64_t)>{},
         .send_update_apy_bps = dm_contract
            ? std::function<std::string(uint64_t)>([dm_contract](uint64_t val) {
                 auto ret = dm_contract->updateApyBPS(val).as_string();
                 return ret;
              })
            : std::function<std::string(uint64_t)>{},
         .confirm_txs = [eth_client, &app_ref = app()](const std::vector<pending_tx>& txs) {
            auto& cron_svc = app_ref.get_plugin<sysio::cron_plugin>().cron_service();
            auto make_retry_opts = []() -> cron_service::retry_options {
               return cron_service::retry_options{
                  .retry_schedule = job_schedule{.milliseconds = {job_schedule::step_value{5000}}},
                  .max_retries = 600,
                  .on_exhaustion = []() -> fc::exception {
                     return fc::ethereum_abi_decode_exception(
                        FC_LOG_MESSAGE(error, "transaction not mined within retry timeout"),
                        fc::ethereum_abi_decode_exception_code,
                        "ethereum_abi_decode_exception",
                        "transaction not mined within retry timeout");
                  }
               };
            };
            for (const auto& tx : txs) {
               auto bn = eth_client->get_block_for_transaction(tx.tx_hash);
               if (bn) {
                  ilog("tx for {} ({}) in block number {}", tx.method, tx.tx_hash, *bn);
                  continue;
               }
               auto bn_retry = cron_svc.blocking_retry(make_retry_opts(),
                  [&]() { return eth_client->get_block_for_transaction(tx.tx_hash); });
               if (bn_retry.has_value())
                  ilog("tx for {} ({}) in block number {}", tx.method, tx.tx_hash, *bn_retry);
               else
                  elog("failed to identify block for tx {}: {}", tx.tx_hash, bn_retry.error().what());
            }
         }
      }, exit_buffer_days));
      ilog("There are {} actions currently registered.", actions.size());
   }
   else {
      SYS_ASSERT(!!opp_contract, sysio::chain::plugin_config_exception,
                 "Nothing is configured to run in wire_eth_maintenance_plugin");
   }

   auto res = curl_global_init(CURL_GLOBAL_DEFAULT);
   SYS_ASSERT(res == CURLE_OK, chain::http_exception, "{}", curl_easy_strerror(res));

   ilog("initializing beacon chain plugin DONE");
}

void wire_eth_maintenance_plugin::plugin_startup() {
   ilog("Starting beacon chain update plugin");
   auto& cron = app().get_plugin<sysio::cron_plugin>();
   SYS_ASSERT(cron.cron_service().num_threads() > 1, sysio::chain::plugin_config_exception,
              "wire_eth_maintenance_plugin uses cron_service::blocking_retry for tx confirmation;"
              " --cron-threads must be >= 2");
   auto& oec_plugin = app().get_plugin<outpost_ethereum_client_plugin>();
   const auto clients = oec_plugin.get_clients();
   SYS_ASSERT(clients.size() > 0, sysio::chain::plugin_config_exception,
              "At least one ethereum client must be configured for beacon chain update plugin");
   const auto eth_client = clients.front()->client;

   ilog("Scheduling {} to execute right after startup", just_once_interval_name);
   job_schedule jo_schedule = services::parse_cron_schedule_or_throw("*/1 * * * *");
   my->just_once_jid =
      cron.add_job(jo_schedule, [my_=my,cron=&cron]() {
         try {
            if(!!my_->just_once_jid)
               cron->cancel_job(*my_->just_once_jid);
         }
         catch (const std::exception& e) {
            elog("Error cancelling the beacon chain update for the just once actions: {}", e.what());
         }
         ilog("Executing beacon chain update for the processes that run `{}`", just_once_interval_name);
         for(const auto& action : my_->just_once_actions) {
            try {
               action();
            }
            catch (const std::exception& e) {
               elog("Error executing beacon chain update for the just once actions: {}", e.what());
            }
         }
      },
      cron_service::job_metadata_t{
         .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "beacon_chain_startup"
      });

   ilog("There are {} schedules currently available.", my->schedules.size());
   ilog("There are {} intervals currently registered.", my->intervals.size());
   for (const auto& [name, schedule] : my->schedules) {
      ilog("Scheduling beacon chain update for interval {}", name);

      auto& actions = my->find_interval_actions(name);
      ilog("There are {} actions currently registered for this interval.", actions.size());
      if(actions.empty()) {
         ilog("No actions to register for interval {}", name);
         continue;
      }
      ilog("{} actions to register for interval {}", actions.size(), name);

      cron.add_job(schedule, [my_=my, name=std::string{name}]() {
         ilog("Executing beacon chain update for {}", name);
         auto it = my_->intervals.find(name);
         if (it == my_->intervals.end()) return;
         for(const auto& action : it->second) {
            try {
               action();
            }
            catch (const std::exception& e) {
               elog("Error executing beacon chain update for interval: {}", e.what());
            }
         }
      },
      cron_service::job_metadata_t{
         .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "beacon_chain_update:" + name
      });
   }
}


wire_eth_maintenance_plugin::wire_eth_maintenance_plugin() : my(
   std::make_shared<wire_eth_maintenance_plugin_impl>()) {}

void wire_eth_maintenance_plugin::set_program_options(options_description& cli, options_description& cfg) {
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
       bpo::value<std::string>()->default_value(just_once_interval_name),
       "Enable fetching the beacon chain deposit/exit queue data and updating on-chain contracts, using the indicated interval.")
      (beacon_chain_contracts_addrs,
       bpo::value<std::vector<std::string>>()->multitoken(),
       "filename to provide addresses for any needed contracts.")
      (beacon_chain_interval,
       boost::program_options::value<std::vector<std::string>>()->multitoken(),
       "Interval specification. Format is `<interval-name>,<cron-spec>`"
       " where cron-spec is in standard cron format (e.g. `*/5 * * * *` for every 5 minutes)."
       " If none are provided, a default interval with name `default` and schedule of every"
       " 1 hour will be used (e.g. `default, * */1 * * *`). Also, a `once` interval is"
       " automatically provided which will just execute immediately and then not run again.")
      (beacon_chain_finalize_epoch_interval,
       bpo::value<std::string>()->default_value(just_once_interval_name),
       "Name of the interval (defined via --beacon-chain-interval) on which to run OPP finalizeEpoch.")
      (beacon_chain_network,
       bpo::value<std::string>()->default_value("mainnet"),
       "The beacon chain network name passed to the queues API (e.g. mainnet, holesky).")
      (beacon_chain_exit_buffer_days,
       bpo::value<uint64_t>()->default_value(9),
       "Buffer in days added to the exit queue ETA when computing withdraw delay;"
       " also used as the fallback delay when the ETA is unavailable or in the past.");
}


void wire_eth_maintenance_plugin::plugin_shutdown() {
   ilog("Shutdown beacon chain update plugin");
   if (my && my->just_once_jid.has_value()) {
      auto* cron = app().find_plugin<sysio::cron_plugin>();
      if (cron) {
         cron->cancel_job(*my->just_once_jid);
      }
      my->just_once_jid.reset();
   }
   curl_global_cleanup();
}

} // namespace sysio
