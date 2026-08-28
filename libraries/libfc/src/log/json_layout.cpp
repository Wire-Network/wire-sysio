#include <fc/log/json_layout.hpp>
#include <fc/log/json_escape.hpp>
#include <fc/log/logger_config.hpp> // fc::get_thread_name
#include <fc/exception/exception.hpp>
#include <fc/time.hpp> // fc::to_utc_tm

#include <magic_enum/magic_enum.hpp>

#include <cctype>
#include <chrono>
#include <ctime>
#include <iterator>

namespace fc::log {

namespace {

using detail::append_sv;
using detail::json_escape_into;

constexpr std::string_view token_open         = "${";
constexpr std::string_view token_escape       = "$${";
constexpr char             token_close        = '}';
constexpr char             modifier_separator = ':';

/// Emit msg.time per the segment's timestamp format.
void emit_timestamp(const spdlog::details::log_msg& msg, json_layout_timestamp_format format,
                    spdlog::memory_buf_t& dest) {
   auto oi = std::back_inserter(dest);
   if (format == json_layout_timestamp_format::epoch_millis) {
      fmt::format_to(oi, "{}",
                     std::chrono::duration_cast<std::chrono::milliseconds>(msg.time.time_since_epoch()).count());
      return;
   }
   auto        tp     = msg.time;
   auto        secs   = std::chrono::time_point_cast<std::chrono::seconds>(tp);
   auto        us     = std::chrono::duration_cast<std::chrono::microseconds>(tp - secs).count();
   std::time_t tt     = std::chrono::system_clock::to_time_t(secs);
   std::tm     tm_utc = fc::to_utc_tm(tt);
   fmt::format_to(oi, "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                  tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, us);
}

/// Emit the spdlog level name per the segment's case choice. Level names are known-safe
/// ASCII, so no JSON escaping is required on this path.
void emit_level(const spdlog::details::log_msg& msg, json_layout_level_case level_case, spdlog::memory_buf_t& dest) {
   const auto&      lvl_sv = spdlog::level::to_string_view(msg.level);
   std::string_view name{lvl_sv.data(), lvl_sv.size()};
   if (level_case == json_layout_level_case::preserve) {
      append_sv(dest, name);
      return;
   }
   for (char c : name) {
      dest.push_back(level_case == json_layout_level_case::upper
                        ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                        : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
   }
}

} // anonymous namespace

json_layout json_layout::compile(std::string_view template_text) {
   json_layout compiled;
   std::string literal;

   auto flush_literal = [&]() {
      if (literal.empty())
         return;
      segment seg;
      seg.literal = std::move(literal);
      compiled.segments_.push_back(std::move(seg));
      literal.clear();
   };

   std::size_t pos = 0;
   while (pos < template_text.size()) {
      if (template_text.compare(pos, token_escape.size(), token_escape) == 0) {
         literal += token_open; // "$${" escapes a literal "${"
         pos += token_escape.size();
         continue;
      }
      if (template_text.compare(pos, token_open.size(), token_open) != 0) {
         literal += template_text[pos];
         ++pos;
         continue;
      }

      const auto close = template_text.find(token_close, pos + token_open.size());
      FC_ASSERT(close != std::string_view::npos, "json layout: unterminated '${{' placeholder at offset {}", pos);
      const auto body     = template_text.substr(pos + token_open.size(), close - pos - token_open.size());
      const auto sep      = body.find(modifier_separator);
      const auto name     = sep == std::string_view::npos ? body : body.substr(0, sep);
      const auto modifier = sep == std::string_view::npos ? std::string_view{} : body.substr(sep + 1);

      const auto token = magic_enum::enum_cast<json_layout_token>(name);
      FC_ASSERT(token.has_value(), "json layout: unknown token '{}'", std::string(name));

      segment seg;
      seg.is_token = true;
      seg.token    = *token;
      if (*token == json_layout_token::timestamp) {
         if (!modifier.empty()) {
            const auto format = magic_enum::enum_cast<json_layout_timestamp_format>(modifier);
            FC_ASSERT(format.has_value(), "json layout: unknown timestamp modifier '{}'", std::string(modifier));
            seg.timestamp_format = *format;
         }
      } else if (*token == json_layout_token::level) {
         if (!modifier.empty()) {
            const auto level_case = magic_enum::enum_cast<json_layout_level_case>(modifier);
            FC_ASSERT(level_case.has_value(), "json layout: unknown level modifier '{}'", std::string(modifier));
            seg.level_case = *level_case;
         }
      } else {
         FC_ASSERT(modifier.empty(), "json layout: token '{}' accepts no modifier", std::string(name));
      }

      flush_literal();
      compiled.segments_.push_back(std::move(seg));
      pos = close + 1;
   }
   flush_literal();
   return compiled;
}

void json_layout::render(const spdlog::details::log_msg& msg, const std::string& extra_object_fragment,
                         const std::string& extra_flat_fragment, spdlog::memory_buf_t& dest) const {
   auto oi = std::back_inserter(dest);
   for (const auto& seg : segments_) {
      if (!seg.is_token) {
         append_sv(dest, seg.literal);
         continue;
      }
      switch (seg.token) {
         case json_layout_token::timestamp: emit_timestamp(msg, seg.timestamp_format, dest); break;
         case json_layout_token::level: emit_level(msg, seg.level_case, dest); break;
         case json_layout_token::message:
            json_escape_into(dest, std::string_view{msg.payload.data(), msg.payload.size()});
            break;
         case json_layout_token::logger:
            json_escape_into(dest, std::string_view{msg.logger_name.data(), msg.logger_name.size()});
            break;
         case json_layout_token::file:
            if (msg.source.filename) {
               json_escape_into(dest, std::string_view{msg.source.filename});
            }
            break;
         case json_layout_token::line: fmt::format_to(oi, "{}", msg.source.line); break;
         case json_layout_token::func:
            if (msg.source.funcname) {
               json_escape_into(dest, std::string_view{msg.source.funcname});
            }
            break;
         case json_layout_token::thread: json_escape_into(dest, fc::get_thread_name()); break;
         case json_layout_token::extra_object: append_sv(dest, extra_object_fragment); break;
         case json_layout_token::extra_flat: append_sv(dest, extra_flat_fragment); break;
      }
   }
}

} // namespace fc::log
