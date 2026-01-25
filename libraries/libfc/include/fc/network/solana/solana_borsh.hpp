// SPDX-License-Identifier: MIT
#pragma once

#include <fc/network/solana/solana_types.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/variant.hpp>

namespace fc::network::solana::borsh {

/**
 * @brief Borsh binary serialization encoder
 *
 * Borsh (Binary Object Representation Serializer for Hashing) is a deterministic
 * serialization format used extensively in Solana programs, particularly with Anchor.
 * It uses little-endian byte order for all numeric types.
 */
class encoder {
public:
   encoder() = default;
   explicit encoder(size_t reserve_size) { _buffer.reserve(reserve_size); }

   // Unsigned integers (little-endian)
   void write_u8(uint8_t v);
   void write_u16(uint16_t v);
   void write_u32(uint32_t v);
   void write_u64(uint64_t v);
   void write_u128(const fc::uint128& v);

   // Signed integers (little-endian, two's complement)
   void write_i8(int8_t v);
   void write_i16(int16_t v);
   void write_i32(int32_t v);
   void write_i64(int64_t v);
   void write_i128(const fc::int128& v);

   // Floating point (IEEE 754, little-endian)
   void write_f32(float v);
   void write_f64(double v);

   // Boolean (single byte: 0 for false, 1 for true)
   void write_bool(bool v);

   // String (u32 length prefix + UTF-8 bytes, no null terminator)
   void write_string(const std::string& v);

   // Dynamic bytes (u32 length prefix + bytes)
   void write_bytes(const std::vector<uint8_t>& v);

   // Fixed-size bytes (no length prefix)
   void write_fixed_bytes(const uint8_t* data, size_t len);
   void write_fixed_bytes(const std::vector<uint8_t>& v) { write_fixed_bytes(v.data(), v.size()); }

   // Solana pubkey (32 bytes, no length prefix)
   void write_pubkey(const pubkey& pk);

   /**
    * @brief Write Option<T> - 0 byte for None, 1 byte + value for Some
    */
   template <typename T>
   void write_option(const std::optional<T>& v);

   /**
    * @brief Write Vec<T> - u32 length prefix + elements
    */
   template <typename T>
   void write_vec(const std::vector<T>& v);

   /**
    * @brief Write fixed-size array [T; N] - elements only, no length prefix
    */
   template <typename T, size_t N>
   void write_array(const std::array<T, N>& v);

   /**
    * @brief Get the serialized buffer
    */
   std::vector<uint8_t> finish() { return std::move(_buffer); }

   /**
    * @brief Get a reference to the current buffer
    */
   const std::vector<uint8_t>& data() const { return _buffer; }

   /**
    * @brief Get current buffer size
    */
   size_t size() const { return _buffer.size(); }

private:
   std::vector<uint8_t> _buffer;

   // Helper to write primitive type
   template <typename T>
   void write_primitive(T value);
};

/**
 * @brief Borsh binary deserialization decoder
 */
class decoder {
public:
   explicit decoder(const std::vector<uint8_t>& data) : _data(data.data()), _size(data.size()), _pos(0) {}
   decoder(const uint8_t* data, size_t len) : _data(data), _size(len), _pos(0) {}

   // Unsigned integers
   uint8_t read_u8();
   uint16_t read_u16();
   uint32_t read_u32();
   uint64_t read_u64();
   fc::uint128 read_u128();

   // Signed integers
   int8_t read_i8();
   int16_t read_i16();
   int32_t read_i32();
   int64_t read_i64();
   fc::int128 read_i128();

   // Floating point
   float read_f32();
   double read_f64();

   // Boolean
   bool read_bool();

   // String
   std::string read_string();

   // Dynamic bytes
   std::vector<uint8_t> read_bytes();

   // Fixed-size bytes
   void read_fixed_bytes(uint8_t* out, size_t len);
   std::vector<uint8_t> read_fixed_bytes(size_t len);

   // Solana pubkey
   pubkey read_pubkey();

   /**
    * @brief Read Option<T>
    */
   template <typename T>
   std::optional<T> read_option();

   /**
    * @brief Read Vec<T>
    */
   template <typename T>
   std::vector<T> read_vec();

   /**
    * @brief Read fixed-size array [T; N]
    */
   template <typename T, size_t N>
   std::array<T, N> read_array();

   /**
    * @brief Get number of remaining bytes
    */
   size_t remaining() const { return _size - _pos; }

   /**
    * @brief Check if there are more bytes to read
    */
   bool has_remaining() const { return _pos < _size; }

   /**
    * @brief Get current position
    */
   size_t position() const { return _pos; }

   /**
    * @brief Skip bytes
    */
   void skip(size_t n);

private:
   const uint8_t* _data;
   size_t _size;
   size_t _pos;

   void ensure_remaining(size_t n) const {
      FC_ASSERT(_pos + n <= _size, "Borsh decoder: not enough data, need ${n} bytes, have ${r}",
                ("n", n)("r", remaining()));
   }

   // Helper to read primitive type
   template <typename T>
   T read_primitive();
};

//=============================================================================
// Encoder template implementations
//=============================================================================

template <typename T>
void encoder::write_primitive(T value) {
   static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
   const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
   _buffer.insert(_buffer.end(), bytes, bytes + sizeof(T));
}

template <typename T>
void encoder::write_option(const std::optional<T>& v) {
   if (!v.has_value()) {
      write_u8(0);
   } else {
      write_u8(1);
      if constexpr (std::is_same_v<T, uint8_t>) {
         write_u8(*v);
      } else if constexpr (std::is_same_v<T, uint16_t>) {
         write_u16(*v);
      } else if constexpr (std::is_same_v<T, uint32_t>) {
         write_u32(*v);
      } else if constexpr (std::is_same_v<T, uint64_t>) {
         write_u64(*v);
      } else if constexpr (std::is_same_v<T, int8_t>) {
         write_i8(*v);
      } else if constexpr (std::is_same_v<T, int16_t>) {
         write_i16(*v);
      } else if constexpr (std::is_same_v<T, int32_t>) {
         write_i32(*v);
      } else if constexpr (std::is_same_v<T, int64_t>) {
         write_i64(*v);
      } else if constexpr (std::is_same_v<T, bool>) {
         write_bool(*v);
      } else if constexpr (std::is_same_v<T, std::string>) {
         write_string(*v);
      } else if constexpr (std::is_same_v<T, pubkey>) {
         write_pubkey(*v);
      } else {
         static_assert(sizeof(T) == 0, "Unsupported type for write_option");
      }
   }
}

template <typename T>
void encoder::write_vec(const std::vector<T>& v) {
   write_u32(static_cast<uint32_t>(v.size()));
   for (const auto& elem : v) {
      if constexpr (std::is_same_v<T, uint8_t>) {
         write_u8(elem);
      } else if constexpr (std::is_same_v<T, uint16_t>) {
         write_u16(elem);
      } else if constexpr (std::is_same_v<T, uint32_t>) {
         write_u32(elem);
      } else if constexpr (std::is_same_v<T, uint64_t>) {
         write_u64(elem);
      } else if constexpr (std::is_same_v<T, int8_t>) {
         write_i8(elem);
      } else if constexpr (std::is_same_v<T, int16_t>) {
         write_i16(elem);
      } else if constexpr (std::is_same_v<T, int32_t>) {
         write_i32(elem);
      } else if constexpr (std::is_same_v<T, int64_t>) {
         write_i64(elem);
      } else if constexpr (std::is_same_v<T, bool>) {
         write_bool(elem);
      } else if constexpr (std::is_same_v<T, std::string>) {
         write_string(elem);
      } else if constexpr (std::is_same_v<T, pubkey>) {
         write_pubkey(elem);
      } else {
         static_assert(sizeof(T) == 0, "Unsupported type for write_vec");
      }
   }
}

template <typename T, size_t N>
void encoder::write_array(const std::array<T, N>& v) {
   for (const auto& elem : v) {
      if constexpr (std::is_same_v<T, uint8_t>) {
         write_u8(elem);
      } else if constexpr (std::is_same_v<T, uint16_t>) {
         write_u16(elem);
      } else if constexpr (std::is_same_v<T, uint32_t>) {
         write_u32(elem);
      } else if constexpr (std::is_same_v<T, uint64_t>) {
         write_u64(elem);
      } else if constexpr (std::is_same_v<T, pubkey>) {
         write_pubkey(elem);
      } else {
         static_assert(sizeof(T) == 0, "Unsupported type for write_array");
      }
   }
}

//=============================================================================
// Decoder template implementations
//=============================================================================

template <typename T>
T decoder::read_primitive() {
   static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
   ensure_remaining(sizeof(T));
   T value;
   std::memcpy(&value, _data + _pos, sizeof(T));
   _pos += sizeof(T);
   return value;
}

template <typename T>
std::optional<T> decoder::read_option() {
   uint8_t has_value = read_u8();
   if (has_value == 0) {
      return std::nullopt;
   }
   FC_ASSERT(has_value == 1, "Invalid option discriminator: ${v}", ("v", has_value));

   if constexpr (std::is_same_v<T, uint8_t>) {
      return read_u8();
   } else if constexpr (std::is_same_v<T, uint16_t>) {
      return read_u16();
   } else if constexpr (std::is_same_v<T, uint32_t>) {
      return read_u32();
   } else if constexpr (std::is_same_v<T, uint64_t>) {
      return read_u64();
   } else if constexpr (std::is_same_v<T, int8_t>) {
      return read_i8();
   } else if constexpr (std::is_same_v<T, int16_t>) {
      return read_i16();
   } else if constexpr (std::is_same_v<T, int32_t>) {
      return read_i32();
   } else if constexpr (std::is_same_v<T, int64_t>) {
      return read_i64();
   } else if constexpr (std::is_same_v<T, bool>) {
      return read_bool();
   } else if constexpr (std::is_same_v<T, std::string>) {
      return read_string();
   } else if constexpr (std::is_same_v<T, pubkey>) {
      return read_pubkey();
   } else {
      static_assert(sizeof(T) == 0, "Unsupported type for read_option");
   }
}

template <typename T>
std::vector<T> decoder::read_vec() {
   uint32_t len = read_u32();
   std::vector<T> result;
   result.reserve(len);

   for (uint32_t i = 0; i < len; ++i) {
      if constexpr (std::is_same_v<T, uint8_t>) {
         result.push_back(read_u8());
      } else if constexpr (std::is_same_v<T, uint16_t>) {
         result.push_back(read_u16());
      } else if constexpr (std::is_same_v<T, uint32_t>) {
         result.push_back(read_u32());
      } else if constexpr (std::is_same_v<T, uint64_t>) {
         result.push_back(read_u64());
      } else if constexpr (std::is_same_v<T, int8_t>) {
         result.push_back(read_i8());
      } else if constexpr (std::is_same_v<T, int16_t>) {
         result.push_back(read_i16());
      } else if constexpr (std::is_same_v<T, int32_t>) {
         result.push_back(read_i32());
      } else if constexpr (std::is_same_v<T, int64_t>) {
         result.push_back(read_i64());
      } else if constexpr (std::is_same_v<T, bool>) {
         result.push_back(read_bool());
      } else if constexpr (std::is_same_v<T, std::string>) {
         result.push_back(read_string());
      } else if constexpr (std::is_same_v<T, pubkey>) {
         result.push_back(read_pubkey());
      } else {
         static_assert(sizeof(T) == 0, "Unsupported type for read_vec");
      }
   }

   return result;
}

template <typename T, size_t N>
std::array<T, N> decoder::read_array() {
   std::array<T, N> result;

   for (size_t i = 0; i < N; ++i) {
      if constexpr (std::is_same_v<T, uint8_t>) {
         result[i] = read_u8();
      } else if constexpr (std::is_same_v<T, uint16_t>) {
         result[i] = read_u16();
      } else if constexpr (std::is_same_v<T, uint32_t>) {
         result[i] = read_u32();
      } else if constexpr (std::is_same_v<T, uint64_t>) {
         result[i] = read_u64();
      } else if constexpr (std::is_same_v<T, pubkey>) {
         result[i] = read_pubkey();
      } else {
         static_assert(sizeof(T) == 0, "Unsupported type for read_array");
      }
   }

   return result;
}

//=============================================================================
// Helper functions for instruction data encoding/decoding
//=============================================================================

/**
 * @brief Compute Anchor instruction discriminator
 *
 * The discriminator is the first 8 bytes of sha256("global:<instruction_name>")
 * for regular instructions, or sha256("state:<instruction_name>") for state methods.
 */
std::array<uint8_t, 8> compute_discriminator(const std::string& namespace_prefix, const std::string& name);

/**
 * @brief Compute Anchor account discriminator
 *
 * The discriminator is the first 8 bytes of sha256("account:<account_name>")
 */
std::array<uint8_t, 8> compute_account_discriminator(const std::string& name);

}  // namespace fc::network::solana::borsh
