#pragma once

#include <fc/crypto/hex.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <compare>
#include <string>
#include <span>

namespace fc { namespace crypto {

/**
 * @brief BLAKE3 hash (32 bytes)
 *
 * Storage type for BLAKE3 digests. Does not include the hashing implementation
 * itself — callers use the LLVM BLAKE3 C API (llvm-c/blake3.h) directly.
 */
class blake3 {
public:
   static constexpr size_t byte_size = 32;

   blake3() { memset(_hash, 0, sizeof(_hash)); }

   explicit blake3(const std::string& hex_str) {
      auto bytes = from_hex(hex_str);
      FC_ASSERT(bytes.size() == sizeof(_hash),
                "Invalid blake3 hex string length: {}", hex_str.size());
      memcpy(_hash, bytes.data(), sizeof(_hash));
   }

   std::string str() const { return to_hex(to_char_span()); }

   uint8_t*       data()       { return _hash; }
   const uint8_t* data() const { return _hash; }
   char*          cdata()       { return reinterpret_cast<char*>(_hash); }
   const char*    cdata() const { return reinterpret_cast<const char*>(_hash); }
   constexpr size_t data_size() const { return byte_size; }

   std::span<const uint8_t, byte_size> to_uint8_span() const {
      return std::span<const uint8_t, byte_size>{_hash, byte_size};
   }

   std::span<const char, byte_size> to_char_span() const {
      return std::span<const char, byte_size>{cdata(), byte_size};
   }

   template<typename T>
   friend T& operator<<(T& ds, const blake3& ep) {
      ds.write(ep.cdata(), byte_size);
      return ds;
   }

   template<typename T>
   friend T& operator>>(T& ds, blake3& ep) {
      ds.read(ep.cdata(), byte_size);
      return ds;
   }

   friend bool operator==(const blake3& h1, const blake3& h2) {
      return memcmp(h1._hash, h2._hash, sizeof(h1._hash)) == 0;
   }
   friend bool operator!=(const blake3& h1, const blake3& h2) {
      return !(h1 == h2);
   }
   friend std::strong_ordering operator<=>(const blake3& h1, const blake3& h2) {
      return memcmp(h1._hash, h2._hash, sizeof(h1._hash)) <=> 0;
   }

private:
   uint8_t _hash[byte_size];
};

} // namespace crypto

inline void to_variant( const crypto::blake3& bi, variant& v ) {
   v = std::vector<char>( bi.cdata(), bi.cdata() + bi.data_size() );
}

inline void from_variant( const variant& v, crypto::blake3& bi ) {
   std::vector<char> ve = v.as< std::vector<char> >();
   FC_ASSERT(ve.size() == crypto::blake3::byte_size, "Invalid blake3 data size: {}", ve.size());
   memcpy(bi.cdata(), ve.data(), crypto::blake3::byte_size);
}

} // namespace fc

#include <fc/reflect/reflect.hpp>
FC_REFLECT_TYPENAME(fc::crypto::blake3)
