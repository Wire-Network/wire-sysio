#include <fc/log/logger_config.hpp>
#include <fc/io/json.hpp>
#include <fc/filesystem.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/dmlog_sink.hpp>
#include <fc/log/dmlog_formatter.hpp>
#include <fc/log/json_formatter.hpp>
#include <fc/log/pattern_formatter.hpp>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#define BOOST_DLL_USE_STD_FS
#include <boost/dll/runtime_symbol_info.hpp>

namespace fc {

   constexpr std::string DEFAULT_LOGGER = "default";

   log_config& log_config::get() {
      // allocate dynamically which will leak on exit but allow loggers to be used until the very end of execution
      static log_config* the = new log_config;
      return *the;
   }

   logger log_config::get_logger( const std::string& name ) {
      std::lock_guard g( log_config::get().log_mutex );
      return log_config::get().logger_map[name];
   }

   void log_config::update_logger( const std::string& name, logger& log ) {
      update_logger_with_default(name, log, DEFAULT_LOGGER);
   }

   void log_config::update_logger_with_default( const std::string& name, logger& log, const std::string& default_name ) {
      std::lock_guard g( log_config::get().log_mutex );
      if( log_config::get().logger_map.find( name ) != log_config::get().logger_map.end() ) {
         log = log_config::get().logger_map[name];
      } else {
         // no entry for logger, so setup with default logger if it exists, otherwise do nothing since default logger not configured
         if( log_config::get().logger_map.find( default_name ) != log_config::get().logger_map.end() ) {
            log = log_config::get().logger_map[default_name];
            log_config::get().logger_map.emplace( name, log );
         }
      }
   }

   void configure_logging( const std::filesystem::path& lc ) {
      configure_logging( fc::json::from_file<logging_config>(lc) );
   }
   bool configure_logging( const logging_config& cfg ) {
      return log_config::configure_logging( cfg );
   }

   namespace {
      std::unique_ptr<spdlog::formatter> build_formatter(const format_config& f, const std::string& sink_name) {
         if (f.type == "pattern") {
            std::string pattern;
            if (!f.args.is_null()) {
               pattern = f.args.as<format::pattern_config>().pattern;
            }
            return pattern.empty()
               ? fc::make_pattern_formatter()
               : fc::make_pattern_formatter(pattern);
         } else if (f.type == "json") {
            std::map<std::string, std::string> extras;
            if (!f.args.is_null()) {
               auto jc = f.args.as<format::json_config>();
               for (const auto& kv : jc.extra_fields)
                  extras[kv.key()] = kv.value().as_string();
            }
            return std::make_unique<fc::json_formatter>(std::move(extras));
         } else if (f.type == "dmlog") {
            return std::make_unique<fc::dmlog_formatter>();
         }
         // Warn-and-fallback (rather than throw) so a SIGHUP reload with a typo
         // doesn't wipe the running node's logging config.
         std::cerr << "\nWARNING: Unknown format type '" << f.type
                   << "' for sink '" << sink_name
                   << "'; falling back to default pattern" << std::endl;
         return fc::make_pattern_formatter();
      }
   } // anonymous namespace

   bool log_config::configure_logging( const logging_config& cfg ) {
      try {
         std::lock_guard g( log_config::get().log_mutex );
         log_config::get().logger_map.clear();
         log_config::get().sink_map.clear();

         logger::default_logger() = log_config::get().logger_map[DEFAULT_LOGGER];
         logger& default_logger = logger::default_logger();

         // Only construct sinks that at least one logger references. Unreferenced sink definitions in the
         // config act as a menu of available options -- no file handle is opened, no file is created --
         // until an operator attaches them to a logger's sinks[].
         std::set<std::string> referenced_sinks;
         for (const auto& lcfg : cfg.loggers) {
            for (const auto& s : lcfg.sinks)
               referenced_sinks.insert(s);
         }

         for ( size_t i = 0; i < cfg.sinks.size(); ++i ) {
            if (!referenced_sinks.contains(cfg.sinks[i].name)) continue;
            // create sink
            auto config_colors = [](auto& sink, const std::vector<sink::level_color>& colors) {
               for (auto& it : colors) {
                  if (it.color == "yellow")
                     sink->set_color(spdlog::level::from_str(it.level), sink->yellow);
                  else if (it.color == "red")
                     sink->set_color(spdlog::level::from_str(it.level), sink->red);
                  else if (it.color == "green")
                     sink->set_color(spdlog::level::from_str(it.level), sink->green);
                  else
                     sink->set_color(spdlog::level::from_str(it.level), sink->reset);
               }
            };
            std::shared_ptr<spdlog::sinks::sink> sink;
            if (cfg.sinks[i].type == "console_sink") {
               auto config = cfg.sinks[i].args.as<sink::console_sink_config>();
               if (config.color) {
                  if (config.output_type == sink::output_t::stderr) {
                     auto color_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                     config_colors(color_sink, config.level_colors);
                     sink = color_sink;
                  } else {
                     auto color_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                     config_colors(color_sink, config.level_colors);
                     sink = color_sink;
                  }
               } else {
                  if (config.output_type == sink::output_t::stderr) {
                     sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
                  } else {
                     sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
                  }
               }
               assert(!!sink);
               log_config::get().sink_map[cfg.sinks[i].name] = sink;
            } else if (cfg.sinks[i].type == "daily_file_sink") {
               auto config = cfg.sinks[i].args.as<sink::daily_file_sink_config>();
               FC_ASSERT(config.max_files <= std::numeric_limits<uint16_t>::max(),
                         "daily_file_sink max_files {} exceeds uint16_t max", config.max_files);
               auto sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                       config.base_filename, config.rotation_hour, config.rotation_minute,
                       config.truncate, static_cast<uint16_t>(config.max_files));
               log_config::get().sink_map[cfg.sinks[i].name] = sink;
            } else if (cfg.sinks[i].type == "rotating_file_sink") {
               auto config = cfg.sinks[i].args.as<sink::rotating_file_sink_config>();
               auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                       config.base_filename, std::size_t{config.max_size}*1024*1024, config.max_files);
               log_config::get().sink_map[cfg.sinks[i].name] = sink;
            } else if (cfg.sinks[i].type == "dmlog_sink") {
               auto config = cfg.sinks[i].args.as<sink::dmlog_sink_config>();
               auto sink = std::make_shared<fc::dmlog_sink_mt>(config.file);
               log_config::get().sink_map[cfg.sinks[i].name] = sink;
            } else {
               std::cerr << "\nWARNING: Unknown sink type: " << cfg.sinks[i].type << std::endl;
            }

            // Attach formatter. Explicit format overrides the default; absent
            // format means pattern (except for dmlog_sink which ships with
            // dmlog_formatter already attached by its ctor).
            auto sink_it = log_config::get().sink_map.find(cfg.sinks[i].name);
            if (sink_it != log_config::get().sink_map.end()) {
               if (cfg.sinks[i].format) {
                  sink_it->second->set_formatter(build_formatter(*cfg.sinks[i].format, cfg.sinks[i].name));
               } else if (cfg.sinks[i].type != "dmlog_sink") {
                  sink_it->second->set_formatter(fc::make_pattern_formatter());
               }
            }
         }

         for (bool first_pass = true; ; first_pass = false) { // process default first
            for (size_t i = 0; i < cfg.loggers.size(); ++i) {
               auto lgr = log_config::get().logger_map[cfg.loggers[i].name];
               if (first_pass && cfg.loggers[i].name != DEFAULT_LOGGER)
                  continue;
               if (!first_pass && cfg.loggers[i].name == DEFAULT_LOGGER)
                  continue;

               lgr.set_name(cfg.loggers[i].name);
               if (lgr.get_name() != DEFAULT_LOGGER) {
                  lgr.set_parent(default_logger);
               }
               if( cfg.loggers[i].enabled ) {
                  lgr.set_enabled( *cfg.loggers[i].enabled );
               } else {
                  lgr.set_enabled( default_logger.is_enabled() );
               }
               if( cfg.loggers[i].level ) {
                  lgr.set_log_level( *cfg.loggers[i].level );
               } else {
                  lgr.set_log_level( default_logger.get_log_level() );
               }

               for (auto s = cfg.loggers[i].sinks.begin(); s != cfg.loggers[i].sinks.end(); ++s) {
                  auto sink_it = log_config::get().sink_map.find(*s);
                  if (sink_it != log_config::get().sink_map.end()) {
                     lgr.add_sink(sink_it->second);
                  }
               }
               if (cfg.loggers[i].sinks.size() > 0) {
                  auto agent = std::make_unique<spdlog::logger>(cfg.loggers[i].name, lgr.get_sinks().begin(), lgr.get_sinks().end());
                  // Flush file sinks on info+ so operators see output promptly; trace/debug stay buffered for throughput.
                  agent->flush_on(spdlog::level::info);
                  lgr.update_agent_logger(std::move(agent));
               }
            }
            if (!first_pass)
               break;
         }

         return true;
      } catch ( exception& e ) {
         std::cerr<<e.to_detail_string()<<"\n";
      } catch ( std::exception& e ) {
         std::cerr<<"Failed to configure logging: "<<e.what()<<"\n";
      }
      return false;
   }

   logging_config logging_config::default_config() {
      logging_config cfg;

     variants  c;
               c.push_back(  mutable_variant_object( "level","debug")("color", "green") );
               c.push_back(  mutable_variant_object( "level","warn")("color", "brown") );
               c.push_back(  mutable_variant_object( "level","error")("color", "red") );

      logger_config dlc;
      dlc.name = DEFAULT_LOGGER;
      dlc.level = log_level::info;
      cfg.loggers.push_back( dlc );
      return cfg;
   }

   static thread_local std::string thread_name;

   void set_thread_name( const std::string& name ) {
      thread_name = name;
#if defined(__linux__) || defined(__FreeBSD__)
      pthread_setname_np( pthread_self(), name.c_str() );
#elif defined(__APPLE__)
      pthread_setname_np( name.c_str() );
#endif
   }
   const std::string& get_thread_name() {
      if(thread_name.empty()) {
         try {
            thread_name = boost::dll::program_location().filename().generic_string();
         } catch (...) {
            thread_name = "unknown";
         }
      }
      return thread_name;
   }
}
