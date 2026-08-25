#pragma once

#include <fc/log/logger.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc {
   class path;

   /// Formatter selection for a sink. Applied via sink->set_formatter() after the sink is constructed.
   ///   type == "pattern" -> args: format::pattern_config (or empty/absent for fc::log::DEFAULT_LOG_PATTERN)
   ///   type == "json"    -> args: format::json_config (extra_fields map, may be empty/absent)
   ///   type == "dmlog"   -> no args; dfuse deep-mind wire format, intended for dmlog_sink, not useful on other sinks
   struct format_config {
      std::string type = "pattern";
      variant     args;
   };

   namespace format {
      struct pattern_config {
         std::string pattern; // empty -> fc::log::DEFAULT_LOG_PATTERN
      };
      struct json_config {
         // extra_fields accepts a JSON object whose values are primitives
         // (string/number/bool/null) coerced to string at load time. Nested
         // objects or arrays throw fc::bad_cast_exception on configure_logging.
         fc::variant_object extra_fields;
         // layout is a fc::log::json_layout token template controlling the document
         // shape (see fc/log/json_layout.hpp). Empty/absent renders the historical
         // fc JSONL shape (fc::log::default_layout); fc::log::es_default_layout is
         // the shipped Elasticsearch/OpenSearch document template. A malformed
         // template (unknown token/modifier, unterminated placeholder) fails
         // configure_logging.
         std::string layout;
      };
   } // namespace format

   struct sink_config {
       explicit sink_config(const std::string& name = "", const std::string& type = "", variant args = variant())
          : name(name), type(type), args(fc::move(args)), enabled(true) {}
       std::string                   name;
       std::string                   type;
       variant                       args;
       std::optional<format_config>  format;
       bool                          enabled;
   };

   namespace sink {
      struct level_color {
          explicit level_color ( std::string l = "trace", std::string c = "yellow")
                  : level(std::move(l)), color(std::move(c)) {}

          std::string  level;
          std::string  color;
      };

      enum class output_t { stderr, stdout };

      struct console_sink_config {
         output_t                      output_type = output_t::stderr;
         bool                          color = true;
         std::vector<level_color>      level_colors; // ignored if color = false
      };

      struct daily_file_sink_config {
         std::string    base_filename;
         int32_t        rotation_hour = 0;
         int32_t        rotation_minute = 0;
         bool           truncate = false;
         uint32_t       max_files = 0;
      };

      /// Rotation threshold for rotating_file_sink, in MEGABYTES. configure_logging multiplies max_size by
      /// 1 MiB when constructing the spdlog sink, so this is a count of megabytes and not a byte count.
      inline constexpr uint32_t default_rotating_max_size_mb = 10;
      /// Number of rotated files kept alongside the active one, matching spdlog's max_files semantics.
      inline constexpr uint32_t default_rotating_max_files = 10;

      /// Size-based rotating log file. Rotates once the active file reaches max_size megabytes, keeping at
      /// most max_files rotated copies. max_size must be non-zero -- spdlog rejects a zero rotation size.
      struct rotating_file_sink_config {
         std::string    base_filename;
         uint32_t       max_size = default_rotating_max_size_mb;  // megabytes
         uint32_t       max_files = default_rotating_max_files;
      };

      struct dmlog_sink_config {
         std::string file = "-";
      };

      /// Defaults for es_sink. Worst-case buffered memory is roughly
      /// (max_pending_batches + 2) * max_batch_bytes -- the pending batch, the queued
      /// batches, and one batch in flight.
      inline constexpr uint32_t default_es_batch_size = 100; ///< documents per bulk request
      /// NDJSON bulk-request body cap, in bytes.
      inline constexpr uint32_t default_es_max_batch_bytes = 1024 * 1024;
      /// Single-document cap, in bytes; a larger formatted document is dropped and counted.
      inline constexpr uint32_t default_es_max_doc_bytes = 256 * 1024;
      /// Interval flush cadence for a partially-filled batch.
      inline constexpr uint32_t default_es_flush_interval_ms = 1000;
      /// Delivery-queue bound, in batches; a full queue drops the newest batch.
      inline constexpr uint32_t default_es_max_pending_batches = 8;
      /// ADDITIONAL delivery attempts after the first (total attempts = max_retries + 1).
      inline constexpr uint32_t default_es_max_retries = 2;
      /// Initial retry backoff; doubles per attempt, capped inside the sink.
      inline constexpr uint32_t default_es_retry_backoff_ms = 250;
      inline constexpr uint32_t default_es_connect_timeout_ms = 5000;
      inline constexpr uint32_t default_es_request_timeout_ms = 10000;
      /// App-thread stall budget: the sink destructor runs on the app/SIGHUP thread
      /// during plugin handle_sighup() fan-out -- this bounds that stall, NOT a
      /// background wait.
      inline constexpr uint32_t default_es_shutdown_flush_timeout_ms = 500;
      /// Hard ceiling on max_batch_bytes; bounds sink memory even with an absurd config.
      inline constexpr uint32_t es_max_batch_bytes_ceiling = 16u * 1024 * 1024;

      /// Ships log documents to an OpenSearch/Elasticsearch _bulk endpoint from a
      /// dedicated worker thread. Document shape is owned by the sink's formatter
      /// (fc::log::json_formatter with the fc::log::es_default_layout template by
      /// default); identity fields (env/app/principal/logStream/...) ride the
      /// formatter's extra_fields. This struct configures endpoint, batching, and
      /// delivery only.
      struct es_sink_config {
         std::string url;                     ///< base URL, e.g. "https://elasticsearch.example.com" (required; trailing '/' stripped)
         std::string index;                   ///< target index or write alias (required non-empty)
         std::optional<std::string> username; ///< optional HTTP basic auth user
         std::optional<std::string> password; ///< required iff username is set
         uint32_t batch_size          = default_es_batch_size;
         uint32_t max_batch_bytes     = default_es_max_batch_bytes;
         uint32_t max_doc_bytes       = default_es_max_doc_bytes;
         uint32_t flush_interval_ms   = default_es_flush_interval_ms;
         uint32_t max_pending_batches = default_es_max_pending_batches;
         uint32_t max_retries               = default_es_max_retries;
         uint32_t retry_backoff_ms          = default_es_retry_backoff_ms;
         uint32_t connect_timeout_ms        = default_es_connect_timeout_ms;
         uint32_t request_timeout_ms        = default_es_request_timeout_ms;
         uint32_t shutdown_flush_timeout_ms = default_es_shutdown_flush_timeout_ms;
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
FC_REFLECT( fc::sink_config, (name)(type)(args)(format)(enabled) )
FC_REFLECT( fc::format_config, (type)(args) )
FC_REFLECT( fc::format::pattern_config, (pattern) )
FC_REFLECT( fc::format::json_config, (extra_fields)(layout) )
FC_REFLECT( fc::sink::level_color, (level)(color) )
FC_REFLECT_ENUM( fc::sink::output_t, (stderr)(stdout) )
FC_REFLECT( fc::sink::console_sink_config, (color)(level_colors)(output_type) )
FC_REFLECT( fc::sink::daily_file_sink_config, (base_filename)(rotation_hour)(rotation_minute)(truncate)(max_files) )
FC_REFLECT( fc::sink::rotating_file_sink_config, (base_filename)(max_size)(max_files) )
FC_REFLECT( fc::sink::dmlog_sink_config, (file) )
FC_REFLECT( fc::sink::es_sink_config,
   (url)(index)(username)(password)
   (batch_size)(max_batch_bytes)(max_doc_bytes)(flush_interval_ms)(max_pending_batches)
   (max_retries)(retry_backoff_ms)(connect_timeout_ms)(request_timeout_ms)
   (shutdown_flush_timeout_ms) )
FC_REFLECT( fc::logger_config, (name)(level)(enabled)(sinks) )
FC_REFLECT( fc::logging_config, (includes)(sinks)(loggers) )
