#pragma once

#include <filesystem>
#include <fc/log/logger.hpp>
#include <spdlog/spdlog.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc {
   class path;
   struct sink_config {
       sink_config(const std::string& name = "", const std::string& type = "", variant args = variant())
          : name(name), type(type), args(fc::move(args)), enabled(true) {}
       std::string name;
       std::string type;
       variant args;
       bool enabled;
   };

   namespace sink {
      struct level_color {
          level_color ( std::string l = "trace", std::string c = "yellow")
                  : level(l), color(c) {}

          std::string  level;
          std::string  color;
      };

      struct stderr_color_sink_config {
          std::vector<level_color>      level_colors;
      };

      struct stdout_color_sink_config {
         std::vector<level_color>      level_colors;
      };

      struct daily_file_sink_config {
         std::string    base_filename;
         int32_t        rotation_hour = 0;
         int32_t        rotation_minute = 0;
         bool           truncate = false;
         uint32_t       max_files = 0;
      };

      struct rotating_file_sink_config {
         std::string    base_filename;
         uint32_t       max_size = 1048576*10; // 10MB
         uint32_t       max_files = 10;
      };

      struct dmlog_sink_config {
         std::string file = "-";
      };
   } // namespace sink

   struct logger_config {
      explicit logger_config(std::string name = {}):name(std::move(name)){}
      std::string                      name;
      std::optional<log_level>         level;
      /// if not set, then parents enabled is used.
      std::optional<bool>              enabled;
      std::vector<std::string>         sinks;
   };

   struct logging_config {
      static logging_config default_config();
      std::vector<std::string>     includes;
      std::vector<sink_config>     sinks;
      std::vector<logger_config>   loggers;
   };

   struct log_config {
      static logger get_logger( const std::string& name );
      static void update_logger( const std::string& name, logger& log );
      static void update_logger_with_default( const std::string& name, logger& log, const std::string& default_name );

      static bool configure_logging( const logging_config& l );

   private:
      static log_config& get();

      friend class logger;

      std::mutex                                                             log_mutex;
      std::unordered_map<std::string, std::shared_ptr<spdlog::sinks::sink>>  sink_map;
      std::unordered_map<std::string, logger>                                logger_map;
   };

   void configure_logging( const std::filesystem::path& log_config );
   bool configure_logging( const logging_config& l );

   void set_os_thread_name( const std::string& name );
   void set_thread_name( const std::string& name );
   const std::string& get_thread_name();
}

#include <fc/reflect/reflect.hpp>
FC_REFLECT( fc::sink_config, (name)(type)(args)(enabled) )
FC_REFLECT( fc::sink::level_color, (level)(color) )
FC_REFLECT( fc::sink::stderr_color_sink_config, (level_colors) )
FC_REFLECT( fc::sink::stdout_color_sink_config, (level_colors) )
FC_REFLECT( fc::sink::daily_file_sink_config, (base_filename)(rotation_hour)(rotation_minute)(truncate)(max_files) )
FC_REFLECT( fc::sink::rotating_file_sink_config, (base_filename)(max_size)(max_files) )
FC_REFLECT( fc::sink::dmlog_sink_config, (file) )
FC_REFLECT( fc::logger_config, (name)(level)(enabled)(sinks) )
FC_REFLECT( fc::logging_config, (includes)(sinks)(loggers) )
