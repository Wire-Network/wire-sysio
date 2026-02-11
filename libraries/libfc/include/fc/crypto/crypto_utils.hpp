#pragma once
#include <fc/utility.hpp>
#include <fc/crypto/ripemd160.hpp>

namespace fc::crypto {
template <typename DataType>
struct checksum_data {
   checksum_data() {}
   uint32_t check = 0;
   DataType data;

   static auto calculate_checksum(const DataType& data, const char* prefix = nullptr) {
      auto encoder = ripemd160::encoder();
      raw::pack(encoder, data);

      if (prefix != nullptr) {
         encoder.write(prefix, const_strlen(prefix));
      }
      return encoder.result()._hash[0];
   }
};

inline bool prefix_matches(const char* prefix, std::string_view str) {
   auto prefix_len = const_strlen(prefix);
   return str.size() > prefix_len && str.substr(0, prefix_len).compare(prefix) == 0;
}
}