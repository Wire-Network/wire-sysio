#include <sysio/chain/application.hpp>

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>
#include <sysio/version/version.hpp>

#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/log/appender.hpp>
#include <fc/exception/exception.hpp>
#include <fc/scoped_exit.hpp>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/exception/diagnostic_information.hpp>

#include <filesystem>
#include <string>
#include <vector>
#include <iterator>

#include "config.hpp"

using namespace appbase;
using namespace sysio;

namespace detail {

void log_non_default_options(const std::vector<bpo::basic_option<char>>& options) {
   using namespace std::string_literals;
   string result;
   for (const auto& op : options) {
      bool mask = false;
      if (op.string_key == "signature-provider"s
          || op.string_key == "peer-private-key"s
          || op.string_key == "p2p-auto-bp-peer"s) {
         mask = true;
      }
      std::string v;
      for (auto i = op.value.cbegin(), b = op.value.cbegin(), e = op.value.cend(); i != e; ++i) {
         if (i != b)
            v += ", ";
         if (mask)
            v += "***";
         else
            v += *i;
      }

      if (!result.empty())
         result += ", ";

      if (v.empty()) {
         result += op.string_key;
      } else {
         result += op.string_key;
         result += " = ";
         result += v;
      }
   }
   ilog("Non-default options: ${v}", ("v", result));
}

fc::logging_config& add_deep_mind_logger(fc::logging_config& config) {
   config.appenders.push_back(
      fc::appender_config( "deep-mind", "dmlog" )
   );

   fc::logger_config dmlc;
   dmlc.name = "deep-mind";
   dmlc.level = fc::log_level::debug;
   dmlc.enabled = true;
   dmlc.appenders.push_back("deep-mind");

   config.loggers.push_back( dmlc );

   return config;
}

void configure_logging(const std::filesystem::path& config_path)
{
   try {
      try {
         if( std::filesystem::exists( config_path ) ) {
            fc::configure_logging( config_path );
         } else {
            auto cfg = fc::logging_config::default_config();

            fc::configure_logging( ::detail::add_deep_mind_logger(cfg) );
         }
      } catch (...) {
         elog("Error reloading logging.json");
         throw;
      }
   } catch (const fc::exception& e) {
      elog("${e}", ("e",e.to_detail_string()));
   } catch (const boost::exception& e) {
      elog("${e}", ("e",boost::diagnostic_information(e)));
   } catch (const std::exception& e) {
      elog("${e}", ("e",e.what()));
   } catch (...) {
      // empty
   }
}

} // namespace detail

void logging_conf_handler()
{
   auto config_path = app().get_logging_conf();
   if( std::filesystem::exists( config_path ) ) {
      ilog( "Received HUP.  Reloading logging configuration from ${p}.", ("p", config_path.string()) );
   } else {
      ilog( "Received HUP.  No log config found at ${p}, setting to default.", ("p", config_path.string()) );
   }
   ::detail::configure_logging( config_path );
   fc::log_config::initialize_appenders();
}

void initialize_logging()
{
   auto config_path = app().get_logging_conf();
   if(std::filesystem::exists(config_path))
     fc::configure_logging(config_path); // intentionally allowing exceptions to escape
   else {
      auto cfg = fc::logging_config::default_config();

      fc::configure_logging( ::detail::add_deep_mind_logger(cfg) );
   }

   fc::log_config::initialize_appenders();

   app().set_sighup_callback(logging_conf_handler);
}

enum return_codes {
   OTHER_FAIL        = -2,
   INITIALIZE_FAIL   = -1,
   SUCCESS           = 0,
   BAD_ALLOC         = 1,
   DATABASE_DIRTY    = 2,
   FIXED_REVERSIBLE  = SUCCESS,
   EXTRACTED_GENESIS = SUCCESS,
   NODE_MANAGEMENT_SUCCESS = 5
};

int main(int argc, char** argv)
{
   try {
      appbase::scoped_app app;
      fc::scoped_exit<std::function<void()>> on_exit = [&]() {
         ilog("${name} version ${ver} ${fv}",
              ("name", nodeop::config::node_executable_name)("ver", app->version_string())
              ("fv", app->version_string() == app->full_version_string() ? "" : app->full_version_string()) );
         ::detail::log_non_default_options(app->get_parsed_options());
      };
      uint32_t short_hash = 0;
      fc::from_hex(sysio::version::version_hash(), (char*)&short_hash, sizeof(short_hash));

      app->set_version(htonl(short_hash));
      app->set_version_string(sysio::version::version_client());
      app->set_full_version_string(sysio::version::version_full());

      auto root = fc::app_path();
      app->set_default_data_dir(root / "sysio" / nodeop::config::node_executable_name / "data" );
      app->set_default_config_dir(root / "sysio" / nodeop::config::node_executable_name / "config" );
      http_plugin::set_defaults({
         .default_unix_socket_path = "",
         .default_http_port = 8888,
         .server_header = nodeop::config::node_executable_name + "/" + app->version_string()
      });
      if(!app->initialize<chain_plugin, net_plugin, producer_plugin, resource_monitor_plugin>(argc, argv, initialize_logging)) {
         const auto& opts = app->get_options();
         if( opts.count("help") || opts.count("version") || opts.count("full-version") || opts.count("print-default-config") ) {
            on_exit.cancel();
            return SUCCESS;
         }
         return INITIALIZE_FAIL;
      }
      if (auto resmon_plugin = app->find_plugin<resource_monitor_plugin>()) {
         resmon_plugin->monitor_directory(app->data_dir());
      } else {
         elog("resource_monitor_plugin failed to initialize");
         return INITIALIZE_FAIL;
      }
      ilog("${name} version ${ver} ${fv}",
            ("name", nodeop::config::node_executable_name)("ver", app->version_string())
            ("fv", app->version_string() == app->full_version_string() ? "" : app->full_version_string()) );
      ilog("${name} using configuration file ${c}", ("name", nodeop::config::node_executable_name)("c", app->full_config_file_path().string()));
      ilog("${name} data directory is ${d}", ("name", nodeop::config::node_executable_name)("d", app->data_dir().string()));
      ::detail::log_non_default_options(app->get_parsed_options());
      app->startup();
      app->set_thread_priority_max();
      app->exec();
   } catch( const extract_genesis_state_exception& e ) {
      return EXTRACTED_GENESIS;
   } catch( const fixed_reversible_db_exception& e ) {
      return FIXED_REVERSIBLE;
   } catch( const node_management_success& e ) {
      return NODE_MANAGEMENT_SUCCESS;
   } catch( const fc::exception& e ) {
      if( e.code() == fc::std_exception_code ) {
         if( e.top_message().find( "atabase dirty flag set" ) != std::string::npos ) {
            elog( "database dirty flag set (likely due to unclean shutdown): replay required" );
            return DATABASE_DIRTY;
         }
      }
      elog( "${e}", ("e", e.to_detail_string()));
      return OTHER_FAIL;
   } catch( const boost::interprocess::bad_alloc& e ) {
      elog("bad alloc");
      return BAD_ALLOC;
   } catch( const boost::exception& e ) {
      elog("${e}", ("e",boost::diagnostic_information(e)));
      return OTHER_FAIL;
   } catch( const std::runtime_error& e ) {
      if( std::string(e.what()).find("atabase dirty flag set") != std::string::npos ) {
         elog( "database dirty flag set (likely due to unclean shutdown): replay required" );
         return DATABASE_DIRTY;
      } else {
         elog( "${e}", ("e",e.what()));
      }
      return OTHER_FAIL;
   } catch( const std::exception& e ) {
      elog("${e}", ("e",e.what()));
      return OTHER_FAIL;
   } catch( ... ) {
      elog("unknown exception");
      return OTHER_FAIL;
   }

   ilog("${name} successfully exiting", ("name", nodeop::config::node_executable_name));
   return SUCCESS;
}
