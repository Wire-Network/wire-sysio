#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <string>
#include <span>
#include <vector>
#include <fc/serialize_as_string.hpp>

namespace fc { namespace crypto {

/**
 * @brief Keccak-256 hash (used by Ethereum)
 *
 * Keccak-256 is used by Ethereum for hashing and signing operations.
 */
class keccak256 {
public:
   static constexpr size_t byte_size = 256 / (8 * sizeof(uint8_t));

   keccak256();
   explicit keccak256(std::string_view hex_str);

   // in hex
   std::string str() const;
   std::string to_string() const { return str(); }
   /// Validating parse used by the FC_SERIALIZE_AS_STRING trait: rejects odd-length
   /// hex (the strictness the pre-trait from_variant vector<char> path enforced).
   static keccak256 from_string(std::string_view s);

   const uint8_t* data() const { return _hash; }
   constexpr size_t data_size() const { return byte_size; }

   std::span<const uint8_t, byte_size> to_uint8_span() const {
      return std::span<const uint8_t, byte_size>{data(), data() + data_size()};
   }

   std::span<const char, byte_size> to_char_span() const {
      return std::span<const char, byte_size>{reinterpret_cast<const char*>(data()), reinterpret_cast<const char*>(data()) + data_size()};
   }

   static keccak256 hash(std::span<const uint8_t> bytes);
   static keccak256 hash(const std::string& s);

   class encoder {
   public:
      encoder();
      ~encoder();

      void write(const char* d, uint32_t dlen);
      void put(char c) { write(&c, 1); }
      void reset();
      keccak256 result();
   private:
      std::vector<uint8_t> data;
   };

   template<typename T>
   friend T& operator<<(T& ds, const keccak256& ep) {
      ds.write(ep._hash, sizeof(ep._hash));
      return ds;
   }

   template<typename T>
   friend T& operator>>(T& ds, keccak256& ep) {
      ds.read(ep._hash, sizeof(ep._hash));
      return ds;
   }

   friend bool operator==(const keccak256& h1, const keccak256& h2);
   friend std::strong_ordering operator <=> ( const keccak256& h1, const keccak256& h2 );

private:
   uint8_t _hash[byte_size];
};
} // namespace crypto

} // namespace fc

#include <fc/reflect/reflect.hpp>
FC_REFLECT_TYPENAME(fc::crypto::keccak256)
FC_SERIALIZE_AS_STRING(fc::crypto::keccak256)
