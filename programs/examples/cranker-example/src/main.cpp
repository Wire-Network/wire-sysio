
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/appender.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/process.hpp>
#include <fc/scoped_exit.hpp>
#include <filesystem>
#include <iterator>
#include <string>
#include <sysio/chain/application.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>
#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/version/version.hpp>
#include <vector>


using namespace appbase;
using namespace sysio;

namespace detail {


fc::logging_config& add_deep_mind_logger(fc::logging_config& config) {
   config.appenders.push_back(fc::appender_config("deep-mind", "dmlog"));

   fc::logger_config dmlc;
   dmlc.name = "deep-mind";
   dmlc.level = fc::log_level::debug;
   dmlc.enabled = true;
   dmlc.appenders.push_back("deep-mind");

   config.loggers.push_back(dmlc);

   return config;
}

void configure_logging(const std::filesystem::path& config_path) {
   try {
      try {
         if (std::filesystem::exists(config_path)) {
            fc::configure_logging(config_path);
         } else {
            auto cfg = fc::logging_config::default_config();

            fc::configure_logging(::detail::add_deep_mind_logger(cfg));
         }
      } catch (...) {
         elog("Error reloading logging.json");
         throw;
      }
   } catch (const fc::exception& e) {
      elogf("{}", e.to_detail_string());
   } catch (const boost::exception& e) {
      elogf("{}", boost::diagnostic_information(e));
   } catch (const std::exception& e) {
      elogf("{}", e.what());
   } catch (...) {
      // empty
   }
}

} // namespace detail

void logging_conf_handler() {
   auto config_path = app().get_logging_conf();
   if (std::filesystem::exists(config_path)) {
      ilog("Received HUP.  Reloading logging configuration from ${p}.", ("p", config_path.string()));
   } else {
      ilog("Received HUP.  No log config found at ${p}, setting to default.", ("p", config_path.string()));
   }
   ::detail::configure_logging(config_path);
   fc::log_config::initialize_appenders();
}

void initialize_logging() {
   auto config_path = app().get_logging_conf();
   if (std::filesystem::exists(config_path))
      fc::configure_logging(config_path); // intentionally allowing exceptions to escape
   else {
      auto cfg = fc::logging_config::default_config();

      fc::configure_logging(::detail::add_deep_mind_logger(cfg));
   }

   fc::log_config::initialize_appenders();

   app().set_sighup_callback(logging_conf_handler);
}

enum return_codes {
   OTHER_FAIL = -2,
   INITIALIZE_FAIL = -1,
   SUCCESS = 0,
   BAD_ALLOC = 1,
   DATABASE_DIRTY = 2,
   FIXED_REVERSIBLE = SUCCESS,
   EXTRACTED_GENESIS = SUCCESS,
   NODE_MANAGEMENT_SUCCESS = 5
};

int main(int argc, char** argv) {

   auto exe_name = fc::program_name();
   ilogf("{} started", exe_name);

   try {
      appbase::scoped_app app;

      auto on_exit = fc::make_scoped_exit([&]() {
         auto full_ver = app->version_string() == app->full_version_string() ? "" : app->full_version_string();
         ilogf("{} version {} {}", exe_name, app->version_string(), full_ver);
      });
      uint32_t short_hash = 0;
      fc::from_hex(sysio::version::version_hash(), reinterpret_cast<char*>(&short_hash), sizeof(short_hash));

      app->set_version(htonl(short_hash));
      app->set_version_string(sysio::version::version_client());
      app->set_full_version_string(sysio::version::version_full());

      auto root = fc::app_path();
      app->set_default_data_dir(root / "sysio" / exe_name / "data");
      app->set_default_config_dir(root / "sysio" / exe_name / "config");

      // chain_plugin, net_plugin
      if (!app->initialize<outpost_ethereum_client_plugin, cron_plugin>(argc, argv, initialize_logging)) {
         const auto& opts = app->get_options();
         if (opts.contains("help") || opts.contains("version") || opts.contains("full-version") ||
             opts.contains("print-default-config")) {
            on_exit.cancel();
            return SUCCESS;
         }
         return INITIALIZE_FAIL;
      }
      auto& cron_plug = app->get_plugin<cron_plugin>();
      auto& eth_plug = app->get_plugin<outpost_ethereum_client_plugin>();
      auto eth_clients = eth_plug.get_clients();
      FC_ASSERT(!eth_clients.empty(), "At least 1 ethereum client must be configured");
      auto eth_client = eth_clients[0];

      app->set_stop_executor_cb([&app, &cron_plug]() {
         ilog("appbase quit called");
         cron_plug.cron_service().stop();
         app->get_io_context().stop();
      });

      ilogf("{} version {} {}", exe_name, app->version_string(),
            app->version_string() == app->full_version_string() ? "" : app->full_version_string());
      ilogf("{} using configuration file {}", exe_name, app->full_config_file_path().string());
      ilogf("{} data directory is {}", exe_name, app->data_dir().string());

      cron_plug.add_job(
         {
            .milliseconds = {0},
            .seconds = {5, 15, 25, 35, 45, 55}
      },
         [&]() {
            auto now = std::chrono::utc_clock::now();
            auto now_str = std::format("{:%H:%M:%S}", now);
            ilogf("{}: Getting ethereum gas price", now_str);

            auto current_price = eth_client->client->get_gas_price();
            ilogf("{}: Current Price> {}WEI", now_str, current_price.str());
         },
         cron_service::job_metadata_t{
            .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "cron_5s_heartbeat"});

      app->startup();
      app->set_thread_priority_max();
      app->exec();
   } catch (const fc::exception& e) {
      if (e.code() == fc::std_exception_code) {
         if (e.top_message().find("atabase dirty flag set") != std::string::npos) {
            elog("database dirty flag set (likely due to unclean shutdown): replay required");
            return DATABASE_DIRTY;
         }
      } else if (e.code() == interrupt_exception::code_value) {
         ilog("Interrupted, successfully exiting");
         return SUCCESS;
      }
      elogf("{}", e.to_detail_string());
      return OTHER_FAIL;
   } catch (const boost::interprocess::bad_alloc& e) {
      elog("bad alloc");
      return BAD_ALLOC;
   } catch (const boost::exception& e) {
      elogf("{}", boost::diagnostic_information(e));
      return OTHER_FAIL;
   } catch (const std::runtime_error& e) {
      if (std::string(e.what()).find("atabase dirty flag set") != std::string::npos) {
         elog("database dirty flag set (likely due to unclean shutdown): replay required");
         return DATABASE_DIRTY;
      }
      elogf("{}", e.what());
      return OTHER_FAIL;
   } catch (const std::exception& e) {
      elogf("{}", e.what());
      return OTHER_FAIL;
   } catch (...) {
      elog("unknown exception");
      return OTHER_FAIL;
   }

   ilogf("{} successfully exiting", exe_name);
   return SUCCESS;
}
