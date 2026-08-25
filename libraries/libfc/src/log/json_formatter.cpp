#include <fc/log/json_formatter.hpp>
#include <fc/log/json_escape.hpp>

namespace fc::log {

namespace {

using detail::append_sv;
using detail::json_escape_into;

/// Build the ${extra_object} fragment: `,"extra":{"k":"v",...}` -- empty when no extras.
std::string to_extra_object_fragment(const std::map<std::string, std::string>& extra_fields) {
   if (extra_fields.empty())
      return {};
   spdlog::memory_buf_t buf;
   append_sv(buf, R"(,"extra":{)");
   bool first = true;
   for (const auto& kv : extra_fields) {
      if (!first)
         buf.push_back(',');
      first = false;
      buf.push_back('"');
      json_escape_into(buf, kv.first);
      append_sv(buf, R"(":")");
      json_escape_into(buf, kv.second);
      buf.push_back('"');
   }
   buf.push_back('}');
   return std::string{buf.data(), buf.size()};
}

/// Build the ${extra_flat} fragment: `,"k":"v",...` (leading comma per entry) -- empty
/// when no extras.
std::string to_extra_flat_fragment(const std::map<std::string, std::string>& extra_fields) {
   spdlog::memory_buf_t buf;
   for (const auto& kv : extra_fields) {
      append_sv(buf, R"(,")");
      json_escape_into(buf, kv.first);
      append_sv(buf, R"(":")");
      json_escape_into(buf, kv.second);
      buf.push_back('"');
   }
   return std::string{buf.data(), buf.size()};
}

} // anonymous namespace

json_formatter::json_formatter(std::map<std::string, std::string> extra_fields, std::string layout_template)
   : extra_fields_(std::move(extra_fields))
   , layout_template_(std::move(layout_template))
   , compiled_(json_layout::compile(layout_template_.empty() ? default_layout : std::string_view{layout_template_}))
   , extra_object_fragment_(to_extra_object_fragment(extra_fields_))
   , extra_flat_fragment_(to_extra_flat_fragment(extra_fields_)) {}

void json_formatter::format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) {
   compiled_.render(msg, extra_object_fragment_, extra_flat_fragment_, dest);
   dest.push_back('\n');
}

std::unique_ptr<spdlog::formatter> json_formatter::clone() const {
   return std::make_unique<json_formatter>(extra_fields_, layout_template_);
}

} // namespace fc::log
