#include <fc/log/json_formatter.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/time.hpp>

#include <chrono>
#include <ctime>
#include <iterator>

namespace fc::log {

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

} // anonymous namespace

json_formatter::json_formatter(std::map<std::string, std::string> extra_fields)
   : extra_fields_(std::move(extra_fields)) {}

void json_formatter::format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) {
   auto oi = std::back_inserter(dest);

   auto tp   = msg.time;
   auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
   auto us   = std::chrono::duration_cast<std::chrono::microseconds>(tp - secs).count();
   std::time_t tt = std::chrono::system_clock::to_time_t(secs);
   std::tm tm_utc = fc::to_utc_tm(tt);

   append_sv(dest, R"({"ts":")");
   fmt::format_to(oi, "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z",
      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, us);
   dest.push_back('"');

   const auto& lvl_sv = spdlog::level::to_string_view(msg.level);
   append_sv(dest, R"(,"lvl":")");
   append_sv(dest, std::string_view{lvl_sv.data(), lvl_sv.size()});
   dest.push_back('"');

   append_sv(dest, R"(,"thread":")");
   json_escape_into(dest, fc::get_thread_name());
   dest.push_back('"');

   append_sv(dest, R"(,"logger":")");
   json_escape_into(dest, std::string_view{msg.logger_name.data(), msg.logger_name.size()});
   dest.push_back('"');

   append_sv(dest, R"(,"file":")");
   if (msg.source.filename) {
      json_escape_into(dest, std::string_view{msg.source.filename});
   }
   dest.push_back('"');

   append_sv(dest, R"(,"line":)");
   fmt::format_to(oi, "{}", msg.source.line);

   append_sv(dest, R"(,"func":")");
   if (msg.source.funcname) {
      json_escape_into(dest, std::string_view{msg.source.funcname});
   }
   dest.push_back('"');

   append_sv(dest, R"(,"msg":")");
   json_escape_into(dest, std::string_view{msg.payload.data(), msg.payload.size()});
   dest.push_back('"');

   if (!extra_fields_.empty()) {
      append_sv(dest, R"(,"extra":{)");
      bool first = true;
      for (const auto& kv : extra_fields_) {
         if (!first) dest.push_back(',');
         first = false;
         dest.push_back('"');
         json_escape_into(dest, kv.first);
         append_sv(dest, R"(":")");
         json_escape_into(dest, kv.second);
         dest.push_back('"');
      }
      dest.push_back('}');
   }

   dest.push_back('}');
   dest.push_back('\n');
}

std::unique_ptr<spdlog::formatter> json_formatter::clone() const {
   return std::make_unique<json_formatter>(extra_fields_);
}

} // namespace fc::log
