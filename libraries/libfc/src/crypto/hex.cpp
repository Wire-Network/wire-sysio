#include <fc/crypto/hex.hpp>
#include <fc/string.hpp>
#include <fc/exception/exception.hpp>

namespace fc {

uint8_t from_hex(char c) {
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   FC_THROW_EXCEPTION(exception, "Invalid hex character '{}'", std::string(&c,1) );
}

std::string to_hex(const char* d, uint32_t s, bool add_prefix) {
   std::string r;
   r.reserve(s * 2 + (add_prefix ? 2 : 0));
   if (add_prefix) r += "0x";
   auto to_hex = "0123456789abcdef";
   auto c = reinterpret_cast<const uint8_t*>(d);
   for (uint32_t i = 0; i < s; ++i)
      (r += to_hex[(c[i] >> 4)]) += to_hex[(c[i] & 0x0f)];
   return r;
}

size_t from_hex(std::string_view hex_str, char* out_data, size_t out_data_len) {
   auto                        i       = hex_str.begin();
   uint8_t*                    out_pos = (uint8_t*)out_data;
   uint8_t*                    out_end = out_pos + out_data_len;
   while (i != hex_str.end() && out_end != out_pos) {
      *out_pos = from_hex(*i) << 4;
      ++i;
      if (i != hex_str.end()) {
         *out_pos |= from_hex(*i);
         ++i;
      }
      ++out_pos;
   }
   return out_pos - reinterpret_cast<uint8_t*>(out_data);
}


std::vector<uint8_t> from_hex(std::string_view hex, bool trim_prefix) {
   if (trim_prefix) hex = trim_hex_prefix(hex);

   auto from_hex = [](char c) -> int {
      if (c >= '0' && c <= '9')
         return c - '0';
      if (c >= 'a' && c <= 'f')
         return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
         return c - 'A' + 10;
      throw std::runtime_error("Invalid hex char");
   };

   std::vector<uint8_t> out;
   out.reserve((hex.size() + 1) / 2);

   size_t i = 0;
   if (hex.size() % 2) {
      // Odd-length hex: the first nibble decodes to a single byte with high nibble = 0.
      out.push_back(static_cast<uint8_t>(from_hex(hex[0])));
      i = 1;
   }
   for (; i < hex.size(); i += 2) {
      out.push_back(static_cast<uint8_t>((from_hex(hex[i]) << 4) | from_hex(hex[i + 1])));
   }

   return out;
}

std::string_view trim_hex_prefix(std::string_view hex) {
   if (hex.starts_with("0x") || hex.starts_with("0X")) {
      hex.remove_prefix(2);
   }
   return hex;
}


}