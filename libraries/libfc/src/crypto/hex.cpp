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

size_t from_hex(const std::string& hex_str, char* out_data, size_t out_data_len) {
   std::string::const_iterator i       = hex_str.begin();
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


std::vector<uint8_t> from_hex(const std::string& hex, bool trim_prefix) {
   auto cleaned_hex = trim_prefix ? trim_hex_prefix(hex) : hex;
   if (cleaned_hex.size() % 2) {
      cleaned_hex = "0" + cleaned_hex;
   }
   std::vector<uint8_t> out;
   out.reserve(cleaned_hex.size() / 2);

   auto from_hex = [](char c) -> int {
      if (c >= '0' && c <= '9')
         return c - '0';
      if (c >= 'a' && c <= 'f')
         return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
         return c - 'A' + 10;
      throw std::runtime_error("Invalid hex char");
   };

   for (size_t i = 0; i < cleaned_hex.size(); i += 2) {
      uint8_t byte =
         (from_hex(cleaned_hex[i]) << 4) | from_hex(cleaned_hex[i + 1]);
      out.push_back(byte);
   }

   return out;
}

std::string trim_hex_prefix(const std::string& hex) {
   if (hex.starts_with("0x") || hex.starts_with("0X")) {
      return hex.substr(2);
   }
   return hex;
}


}