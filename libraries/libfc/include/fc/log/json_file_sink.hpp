#pragma once
#include <fc/log/logger.hpp>
#include <fc/variant_object.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace fc {

   namespace sink {
      // extra_fields is a variant_object so operators can write `{"env":"prod"}`
      // natively in logging.json. (fc serializes std::map as an array of pairs,
      // which is unfriendly in hand-edited config.) Primitive values (string,
      // number, bool, null) are coerced to string at load time; nested objects
      // or arrays throw on configure.
      struct json_rotating_file_sink_config {
         std::string          base_filename;
         uint32_t             max_size  = 10;  // megabytes (multiplied by 1024*1024 at load time)
         uint32_t             max_files = 10;
         fc::variant_object   extra_fields;
      };

      // Note: rotation_hour/rotation_minute are interpreted in the node's local
      // time (spdlog uses localtime_r for rotation scheduling), but the emitted
      // "ts" field is always UTC. A line with ts=2026-04-14T23:30:00Z may land
      // in the file dated 2026-04-15 if the operator's timezone is east of UTC.
      struct json_daily_file_sink_config {
         std::string          base_filename;
         int32_t              rotation_hour   = 0;
         int32_t              rotation_minute = 0;
         bool                 truncate        = false;
         uint32_t             max_files       = 0;
         fc::variant_object   extra_fields;
      };
   } // namespace sink

   /// JSONL (one JSON object per line) sink with size-based rotation.
   /// Inner rotator uses null_mutex -- the outer std::mutex serializes all access.
   class json_rotating_file_sink_mt : public spdlog::sinks::base_sink<std::mutex> {
   public:
      /// @param max_size_bytes rotation threshold in bytes (config converts MB -> bytes)
      json_rotating_file_sink_mt(const std::string& base_filename,
                                 std::size_t        max_size_bytes,
                                 std::size_t        max_files,
                                 std::map<std::string, std::string> extra_fields);
      ~json_rotating_file_sink_mt() override;

   protected:
      void sink_it_(const spdlog::details::log_msg& msg) override;
      void flush_() override;
      // no-op: logger-level pattern_formatter must not clobber our JSON output
      void set_formatter_(std::unique_ptr<spdlog::formatter> sink_formatter) override;

   private:
      std::unique_ptr<spdlog::sinks::rotating_file_sink_st> inner_;
      std::map<std::string, std::string>                    extra_fields_;
   };

   /// JSONL sink with daily time-based rotation.
   class json_daily_file_sink_mt : public spdlog::sinks::base_sink<std::mutex> {
   public:
      json_daily_file_sink_mt(const std::string& base_filename,
                              int                rotation_hour,
                              int                rotation_minute,
                              bool               truncate,
                              uint16_t           max_files,
                              std::map<std::string, std::string> extra_fields);
      ~json_daily_file_sink_mt() override;

   protected:
      void sink_it_(const spdlog::details::log_msg& msg) override;
      void flush_() override;
      void set_formatter_(std::unique_ptr<spdlog::formatter> sink_formatter) override;

   private:
      std::unique_ptr<spdlog::sinks::daily_file_sink_st> inner_;
      std::map<std::string, std::string>                 extra_fields_;
   };

} // namespace fc
