#include <fc/exception/exception.hpp>
#include <fc/log/json_escape.hpp>
#include <fc/log/json_layout.hpp>
#include <fc/log/logger_config.hpp> // fc::get_thread_name
#include <iterator>
#include <magic_enum/magic_enum.hpp>

namespace fc::log {

namespace {

using detail::append_sv;
using detail::json_escape_into;

} // anonymous namespace

json_layout json_layout::compile(std::string_view template_text) {
   json_layout compiled;
   for_each_template_part(
      template_text,
      [&](std::string_view literal) {
         segment seg;
         seg.literal = std::string{literal};
         compiled.segments_.push_back(std::move(seg));
      },
      [&](std::string_view name, std::string_view modifier) {
         const auto token = magic_enum::enum_cast<json_layout_token>(name);
         FC_ASSERT(token.has_value(), "json layout: unknown token '{}'", std::string(name));

         segment seg;
         seg.is_token = true;
         seg.token = *token;
         if (*token == json_layout_token::timestamp) {
            if (!modifier.empty()) {
               const auto format = magic_enum::enum_cast<json_template_timestamp_format>(modifier);
               FC_ASSERT(format.has_value(), "json layout: unknown timestamp modifier '{}'", std::string(modifier));
               seg.timestamp_format = *format;
            }
         } else if (*token == json_layout_token::level) {
            if (!modifier.empty()) {
               const auto level_case = magic_enum::enum_cast<json_template_level_case>(modifier);
               FC_ASSERT(level_case.has_value(), "json layout: unknown level modifier '{}'", std::string(modifier));
               seg.level_case = *level_case;
            }
         } else {
            FC_ASSERT(modifier.empty(), "json layout: token '{}' accepts no modifier", std::string(name));
         }
         compiled.segments_.push_back(std::move(seg));
      });
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
      case json_layout_token::timestamp:
         format_timestamp_to(oi, msg.time, seg.timestamp_format);
         break;
      case json_layout_token::level: {
         const auto& lvl_sv = spdlog::level::to_string_view(msg.level);
         format_level_name_to(oi, std::string_view{lvl_sv.data(), lvl_sv.size()}, seg.level_case);
         break;
      }
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
      case json_layout_token::line:
         fmt::format_to(oi, "{}", msg.source.line);
         break;
      case json_layout_token::func:
         if (msg.source.funcname) {
            json_escape_into(dest, std::string_view{msg.source.funcname});
         }
         break;
      case json_layout_token::thread:
         json_escape_into(dest, fc::get_thread_name());
         break;
      case json_layout_token::extra_object:
         append_sv(dest, extra_object_fragment);
         break;
      case json_layout_token::extra_flat:
         append_sv(dest, extra_flat_fragment);
         break;
      }
   }
}

} // namespace fc::log
