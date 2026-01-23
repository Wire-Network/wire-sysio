#pragma once

#include <sysio/chain/priority_queue_executor.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/version/version.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>
#include <fc/filesystem.hpp>
#include <fc/process.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/exception/exception.hpp>

/*
 * Standard setup for wire applications.
*/
namespace sysio::chain {

struct application_config {
   bool enable_logging_config = true;
   bool enable_resource_monitor = true;
   bool sighup_loads_logging_config = true;
   uint16_t default_http_port = 8888;
};

// use namespace exit_code instead of enum class to allow simple conversion to int in main return statements
namespace exit_code {
   enum exit_code {
      OTHER_FAIL = -2,
      INITIALIZE_FAIL = -1,
      SUCCESS = 0,
      BAD_ALLOC = 1,
      DATABASE_DIRTY = 2,
      FIXED_REVERSIBLE = SUCCESS,
      EXTRACTED_GENESIS = SUCCESS,
      NODE_MANAGEMENT_SUCCESS = 5
   };
};

namespace detail {

using namespace appbase;

void log_non_default_options(const std::vector<bpo::basic_option<char>>& options) {
   using namespace std::string_literals;
   auto mask_private = [](const string& v) -> std::string {
      if (auto parts = fc::split(v, ','); parts.size() > 1) {
         return std::accumulate(std::next(parts.begin()), std::prev(parts.end()), parts[0],
                                [](const string& acc, const string& part) { return acc + "," + part; }) + ",***";
      }
      return "***"s;
   };

   string result;
   for (const auto& op : options) {
      bool mask = false;
      if (op.string_key == "peer-private-key"s
          || op.string_key == "p2p-auto-bp-peer"s) {
         mask = true;
          }
      std::string v;
      for (auto i = op.value.cbegin(), b = op.value.cbegin(), e = op.value.cend(); i != e; ++i) {
         if (i != b)
            v += ", ";
         if (op.string_key == "signature-provider"s)
            v += mask_private(*i);
         else if (mask)
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
   ilog("Non-default options: {}", result);
}

void configure_logging(const std::filesystem::path& config_path) {
   try {
      if (std::filesystem::exists(config_path)) {
         fc::configure_logging(config_path);
      } else {
         auto cfg = fc::logging_config::default_config();
         fc::configure_logging(cfg);
      }
   } catch (const fc::exception& e) {
      std::cerr << "\nError reloading logging.json: " << e.to_detail_string() << std::endl;
   } catch (const boost::exception& e) {
      std::cerr << "\nError reloading logging.json: " << boost::diagnostic_information(e) << std::endl;
   } catch (const std::exception& e) {
      std::cerr << "\nError reloading logging.json: " << e.what() << std::endl;
   } catch (...) {
      std::cerr << "\nError reloading logging.json: unknown" << std::endl;
   }
}

} // namespace detail

void logging_conf_handler() {
   auto config_path = appbase::app().get_logging_conf();
   if (std::filesystem::exists(config_path)) {
      ilog("Received HUP.  Reloading logging configuration from {}.", config_path.string());
   } else {
      ilog("Received HUP.  No log config found at {}, setting to default.", config_path.string());
   }
   detail::configure_logging(config_path);
}

void initialize_logging(const application_config& cfg) {
   if (!cfg.enable_logging_config) {
      appbase::app().set_sighup_callback([]{});
      return;
   }

   auto config_path = appbase::app().get_logging_conf();
   if (std::filesystem::exists(config_path)) {
      fc::configure_logging(config_path); // intentionally allowing exceptions to escape
   } else {
      auto cfg = fc::logging_config::default_config();
      fc::configure_logging(cfg);
   }

   appbase::app().set_sighup_callback(logging_conf_handler);
}

class application {
public:
   explicit application(const application_config& cfg) : cfg_(cfg) {
      exe_name_ = fc::program_name();
      ilog("{} started", exe_name_);

      uint32_t short_hash = 0;
      fc::from_hex(sysio::version::version_hash(), reinterpret_cast<char*>(&short_hash), sizeof(short_hash));

      app_->set_version(htonl(short_hash));
      app_->set_version_string(sysio::version::version_client());
      app_->set_full_version_string(sysio::version::version_full());

      auto root = fc::app_path();
      app_->set_default_data_dir(root / "sysio" / exe_name_ / "data");
      app_->set_default_config_dir(root / "sysio" / exe_name_ / "config");
      http_plugin::set_defaults({
         .default_unix_socket_path = "",
         .default_http_port = cfg_.default_http_port,
         .server_header = exe_name_ + "/" + app_->version_string()
      });
   }
   application(const application&) = delete;
   application& operator=(const application&) = delete;
   application(application&&) = delete;
   application& operator=(application&&) = delete;
   ~application() {
      if (last_result_ != exit_code::NODE_MANAGEMENT_SUCCESS) {
         detail::log_non_default_options(app_->get_parsed_options());
         auto full_ver = app_->version_string() == app_->full_version_string() ? "" : app_->full_version_string();
         ilog("{} version {} {}", exe_name_, app_->version_string(), full_ver);
         ilog("{} successfully exiting", exe_name_);
      }
   }

   template <typename... Plugin>
   exit_code::exit_code init(int argc, char** argv) {
      try {
         auto init_logging = [cfg=cfg_]() { initialize_logging(cfg); };
         if (!app_->initialize<Plugin...>(argc, argv, init_logging)) {
            const auto& opts = app_->get_options();
            if (opts.contains("help") || opts.contains("version") || opts.contains("full-version") || opts.contains("print-default-config")) {
               last_result_ = exit_code::NODE_MANAGEMENT_SUCCESS;
            } else {
               last_result_ = exit_code::INITIALIZE_FAIL;
            }
            return last_result_;
         }

         set_stop_executor_cb([]{});

         if (cfg_.enable_resource_monitor) {
            if (auto resmon_plugin = app_->find_plugin<resource_monitor_plugin>()) {
               resmon_plugin->initialize(app_->get_options());
               resmon_plugin->monitor_directory(app_->data_dir());
            } else {
               elog("resource_monitor_plugin failed to initialize");
               last_result_ = exit_code::INITIALIZE_FAIL;
               return last_result_;
            }
         }

         ilog("{} version {} {}", exe_name_, app_->version_string(),
              app_->version_string() == app_->full_version_string() ? "" : app_->full_version_string());
         ilog("{} using configuration file {}", exe_name_, app_->full_config_file_path().string());
         ilog("{} data directory is {}", exe_name_, app_->data_dir().string());
         detail::log_non_default_options(app_->get_parsed_options());
      } catch (...) {
         return handle_exception();
      }
      return exit_code::SUCCESS;
   }

   // Must call after init
   template <typename Function>
   void set_stop_executor_cb(Function&& f) {
      auto cb = [f=std::forward<Function>(f)]() {
         ilog("appbase quit called");
         f();
         appbase::app().get_io_context().stop();
      };
      app_->set_stop_executor_cb(cb);
   }

   exit_code::exit_code exec() {
      try {
         app_->startup();
         app_->set_thread_priority_max();
         app_->exec();
      } catch (...) {
         return handle_exception();
      }
      return exit_code::SUCCESS;
   }

   exit_code::exit_code handle_exception() {
      try {
         last_result_ = exit_code::OTHER_FAIL;
         throw;
      } catch (const fc::exception& e) {
         if (e.code() == fc::std_exception_code) {
            if (e.top_message().find("atabase dirty flag set") != std::string::npos) {
               elog("database dirty flag set (likely due to unclean shutdown): replay required");
               last_result_ = exit_code::DATABASE_DIRTY;
            } else {
               elog("{}", e.to_detail_string());
               last_result_ = exit_code::OTHER_FAIL;
            }
         } else if (e.code() == interrupt_exception::code_value) {
            ilog("Interrupted, successfully exiting");
            last_result_ = exit_code::SUCCESS;
         } else {
            elog("{}", e.to_detail_string());
            last_result_ = exit_code::OTHER_FAIL;
         }
      } catch (const boost::interprocess::bad_alloc& e) {
         elog("bad alloc");
         last_result_ = exit_code::BAD_ALLOC;
      } catch (const boost::exception& e) {
         elog("{}", boost::diagnostic_information(e));
         last_result_ = exit_code::OTHER_FAIL;
      } catch (const std::runtime_error& e) {
         if (std::string(e.what()).find("atabase dirty flag set") != std::string::npos) {
            elog("database dirty flag set (likely due to unclean shutdown): replay required");
            last_result_ = exit_code::DATABASE_DIRTY;
         } else {
            elog("{}", e.what());
            last_result_ = exit_code::OTHER_FAIL;
         }
      } catch (const std::exception& e) {
         elog("{}", e.what());
         last_result_ = exit_code::OTHER_FAIL;
      } catch (...) {
         elog("unknown exception");
         last_result_ = exit_code::OTHER_FAIL;
      }
      return last_result_;
   }

private:
   appbase::scoped_app app_;
   std::string exe_name_;
   application_config cfg_;
   exit_code::exit_code last_result_ = exit_code::SUCCESS;
};

} // namespace sysio::chain