#pragma once
#include <fc/spdlog.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fc::log {

/// Placeholder tokens accepted inside a json_formatter layout template (see json_layout).
/// Token names and modifiers are parsed by name via magic_enum -- the logging config
/// carries the template STRING; these enums are internal parse artifacts, deliberately
/// not config-reflected types.
enum class json_layout_token : uint8_t {
   timestamp,    ///< record time; modifiers: iso8601 (default) | epoch_millis (bare number)
   level,        ///< spdlog level name; modifiers: upper | lower (default preserves spdlog's spelling)
   message,      ///< the log payload (JSON-escaped)
   logger,       ///< the logger name (JSON-escaped)
   file,         ///< source filename, empty when unknown (JSON-escaped)
   line,         ///< source line as a bare number
   func,         ///< source function name, empty when unknown (JSON-escaped)
   thread,       ///< fc::get_thread_name() (JSON-escaped)
   extra_object, ///< renders `,"extra":{"k":"v",...}` -- or NOTHING when extra_fields is empty
   extra_flat    ///< renders `,"k":"v",...` (leading comma per entry) -- or NOTHING when empty
};

/// ${timestamp} rendering choices.
enum class json_layout_timestamp_format : uint8_t { iso8601, epoch_millis };

/// ${level} case choices. `preserve` keeps spdlog's spelling (trace/debug/info/warn/error/crit).
enum class json_layout_level_case : uint8_t { preserve, upper, lower };

/// A compiled json_formatter layout template.
///
/// A template is arbitrary literal text (the author writes the JSON punctuation and
/// key names themselves) interleaved with `${token}` / `${token:modifier}` placeholders
/// drawn from json_layout_token. `$${` escapes a literal `${`. Compile once
/// (configuration time); render per log record. String-valued tokens are JSON-escaped
/// on emit; `line` and `timestamp:epoch_millis` emit bare numbers.
class json_layout {
public:
   json_layout() = default;

   /// Compile @p template_text into a segment sequence.
   /// FC_ASSERTs (failing configure_logging loudly) on an unknown token name, an
   /// unknown or misplaced modifier, or an unterminated `${` placeholder.
   static json_layout compile(std::string_view template_text);

   /// Render one record into @p dest (no trailing newline -- the formatter owns that).
   /// The extras fragments are pre-escaped by the caller (json_formatter builds them
   /// once from its extra_fields) and are emitted verbatim wherever ${extra_object} /
   /// ${extra_flat} sit in the template.
   void render(const spdlog::details::log_msg& msg,
               const std::string& extra_object_fragment,
               const std::string& extra_flat_fragment,
               spdlog::memory_buf_t& dest) const;

private:
   /// One compiled template piece: literal text, or a token with its modifiers.
   struct segment {
      std::string                  literal;                                                  ///< used when !is_token
      bool                         is_token = false;                                         ///< literal vs token discriminator
      json_layout_token            token = json_layout_token::message;                       ///< valid when is_token
      json_layout_timestamp_format timestamp_format = json_layout_timestamp_format::iso8601; ///< timestamp token only
      json_layout_level_case       level_case = json_layout_level_case::preserve;            ///< level token only
   };

   std::vector<segment> segments_;
};

/// Historical fc JSONL document shape -- compiled when a json format block carries no
/// `layout` value. Renders byte-identically to the pre-template json_formatter output
/// (pinned by json_formatter_tests).
inline constexpr std::string_view default_layout =
   R"({"ts":"${timestamp}","lvl":"${level}","thread":"${thread}","logger":"${logger}","file":"${file}","line":${line},"func":"${func}","msg":"${message}"${extra_object}})";

/// Elasticsearch/OpenSearch log-document shape -- the es_sink's default formatter
/// template. Fields: @timestamp (epoch millis, matching a `date`/`epoch_millis`
/// mapping), level (uppercase keyword), message, category (logger name),
/// sourceLocation, data.thread; extra_fields (env/app/principal/logStream/...) land
/// at the document top level via ${extra_flat}. Note: spdlog's `critical` renders as
/// `CRIT` under ${level:upper} (the repo's SPDLOG_LEVEL_NAMES spelling).
inline constexpr std::string_view es_default_layout =
   R"({"@timestamp":${timestamp:epoch_millis},"level":"${level:upper}","message":"${message}","category":"${logger}","sourceLocation":"${file}:${line} ${func}","data":{"thread":"${thread}"}${extra_flat}})";

} // namespace fc::log
