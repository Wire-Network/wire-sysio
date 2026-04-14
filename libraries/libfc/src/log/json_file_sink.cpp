#include <fc/log/json_file_sink.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/time.hpp>

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>

#include <chrono>
#include <ctime>
#include <iterator>

namespace fc {

namespace {

inline void append_sv(spdlog::memory_buf_t& out, std::string_view s) {
   out.append(s.data(), s.data() + s.size());
}

void json_escape_into(spdlog::memory_buf_t& out, std::string_view s) {
   for (unsigned char c : s) {
      switch (c) {
         case '"':  append_sv(out, R"(\")"); break;
         case '\\': append_sv(out, R"(\\)"); break;
         case '\b': append_sv(out, R"(\b)"); break;
         case '\f': append_sv(out, R"(\f)"); break;
         case '\n': append_sv(out, R"(\n)"); break;
         case '\r': append_sv(out, R"(\r)"); break;
         case '\t': append_sv(out, R"(\t)"); break;
         default:
            if (c < 0x20) {
               fmt::format_to(std::back_inserter(out), R"(\u{:04x})", static_cast<unsigned>(c));
            } else {
               out.push_back(static_cast<char>(c));
            }
      }
   }
}

void format_json_line(const spdlog::details::log_msg& msg,
                      const std::map<std::string, std::string>& extra,
                      spdlog::memory_buf_t& out) {
   out.clear();
   auto oi = std::back_inserter(out);

   auto tp   = msg.time;
   auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
   auto us   = std::chrono::duration_cast<std::chrono::microseconds>(tp - secs).count();
   std::time_t tt = std::chrono::system_clock::to_time_t(secs);
   std::tm tm_utc = fc::to_utc_tm(tt);

   append_sv(out, R"({"ts":")");
   fmt::format_to(oi, "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z",
      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, us);
   out.push_back('"');

   const auto& lvl_sv = spdlog::level::to_string_view(msg.level);
   append_sv(out, R"(,"lvl":")");
   append_sv(out, std::string_view{lvl_sv.data(), lvl_sv.size()});
   out.push_back('"');

   append_sv(out, R"(,"thread":")");
   json_escape_into(out, fc::get_thread_name());
   out.push_back('"');

   append_sv(out, R"(,"logger":")");
   json_escape_into(out, std::string_view{msg.logger_name.data(), msg.logger_name.size()});
   out.push_back('"');

   append_sv(out, R"(,"file":")");
   if (msg.source.filename) {
      json_escape_into(out, std::string_view{msg.source.filename});
   }
   out.push_back('"');

   append_sv(out, R"(,"line":)");
   fmt::format_to(oi, "{}", msg.source.line);

   append_sv(out, R"(,"func":")");
   if (msg.source.funcname) {
      json_escape_into(out, std::string_view{msg.source.funcname});
   }
   out.push_back('"');

   append_sv(out, R"(,"msg":")");
   json_escape_into(out, std::string_view{msg.payload.data(), msg.payload.size()});
   out.push_back('"');

   if (!extra.empty()) {
      append_sv(out, R"(,"extra":{)");
      bool first = true;
      for (const auto& kv : extra) {
         if (!first) out.push_back(',');
         first = false;
         out.push_back('"');
         json_escape_into(out, kv.first);
         append_sv(out, R"(":")");
         json_escape_into(out, kv.second);
         out.push_back('"');
      }
      out.push_back('}');
   }

   out.push_back('}');
   out.push_back('\n');
}

std::unique_ptr<spdlog::pattern_formatter> make_passthrough_formatter() {
   // Inner sink writes payload verbatim; we already include the trailing newline.
   return std::make_unique<spdlog::pattern_formatter>(
      "%v", spdlog::pattern_time_type::utc, std::string{});
}

} // anonymous namespace

json_rotating_file_sink_mt::json_rotating_file_sink_mt(
      const std::string& base_filename,
      std::size_t        max_size_bytes,
      std::size_t        max_files,
      std::map<std::string, std::string> extra_fields)
   : inner_(std::make_unique<spdlog::sinks::rotating_file_sink_st>(
               base_filename, max_size_bytes, max_files))
   , extra_fields_(std::move(extra_fields))
{
   inner_->set_formatter(make_passthrough_formatter());
}

json_rotating_file_sink_mt::~json_rotating_file_sink_mt() = default;

void json_rotating_file_sink_mt::sink_it_(const spdlog::details::log_msg& msg) {
   // buf backs synthetic.payload; inner_->log() must consume it synchronously.
   thread_local spdlog::memory_buf_t buf;
   format_json_line(msg, extra_fields_, buf);
   spdlog::details::log_msg synthetic = msg;
   synthetic.payload = spdlog::string_view_t{buf.data(), buf.size()};
   inner_->log(synthetic);
}

void json_rotating_file_sink_mt::flush_() {
   inner_->flush();
}

void json_rotating_file_sink_mt::set_formatter_(std::unique_ptr<spdlog::formatter>) {
}

json_daily_file_sink_mt::json_daily_file_sink_mt(
      const std::string& base_filename,
      int                rotation_hour,
      int                rotation_minute,
      bool               truncate,
      uint16_t           max_files,
      std::map<std::string, std::string> extra_fields)
   : inner_(std::make_unique<spdlog::sinks::daily_file_sink_st>(
               base_filename, rotation_hour, rotation_minute, truncate, max_files))
   , extra_fields_(std::move(extra_fields))
{
   inner_->set_formatter(make_passthrough_formatter());
}

json_daily_file_sink_mt::~json_daily_file_sink_mt() = default;

void json_daily_file_sink_mt::sink_it_(const spdlog::details::log_msg& msg) {
   // buf backs synthetic.payload; inner_->log() must consume it synchronously.
   thread_local spdlog::memory_buf_t buf;
   format_json_line(msg, extra_fields_, buf);
   spdlog::details::log_msg synthetic = msg;
   synthetic.payload = spdlog::string_view_t{buf.data(), buf.size()};
   inner_->log(synthetic);
}

void json_daily_file_sink_mt::flush_() {
   inner_->flush();
}

void json_daily_file_sink_mt::set_formatter_(std::unique_ptr<spdlog::formatter>) {
}

} // namespace fc
