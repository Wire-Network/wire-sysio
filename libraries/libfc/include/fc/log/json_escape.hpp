#pragma once
#include <fc/spdlog.hpp>

#include <iterator>
#include <string_view>

namespace fc::log::detail {

/// Append a string_view to an spdlog memory buffer without an intermediate std::string.
inline void append_sv(spdlog::memory_buf_t& out, std::string_view s) {
   out.append(s.data(), s.data() + s.size());
}

/// JSON-escape @p s into @p out: quote, backslash, \b \f \n \r \t, and \u00xx for any
/// other control byte. Bytes >= 0x20 (including UTF-8 sequences) pass through untouched.
inline void json_escape_into(spdlog::memory_buf_t& out, std::string_view s) {
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

} // namespace fc::log::detail
