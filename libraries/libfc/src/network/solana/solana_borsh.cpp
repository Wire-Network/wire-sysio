// SPDX-License-Identifier: MIT
#include <fc/network/solana/solana_borsh.hpp>

#include <cstring>

#include <fc/crypto/sha256.hpp>

namespace fc::network::solana::borsh {

//=============================================================================
// Encoder implementation
//=============================================================================

void encoder::write_u8(uint8_t v) { _buffer.push_back(v); }

void encoder::write_u16(uint16_t v) { write_primitive(v); }

void encoder::write_u32(uint32_t v) { write_primitive(v); }

void encoder::write_u64(uint64_t v) { write_primitive(v); }

void encoder::write_u128(const fc::uint128& v) {
   // Write as little-endian: low 64 bits first, then high 64 bits
   write_u64(static_cast<uint64_t>(v & fc::uint128(UINT64_MAX)));
   write_u64(static_cast<uint64_t>(v >> 64));
}

void encoder::write_u256(const fc::uint256& v) {
   // Write as little-endian: 4 x 64-bit chunks, lowest first
   // Extract 64-bit chunks using boost multiprecision
   fc::uint256 mask64 = std::numeric_limits<uint64_t>::max();
   write_u64(static_cast<uint64_t>(v & mask64));
   write_u64(static_cast<uint64_t>((v >> 64) & mask64));
   write_u64(static_cast<uint64_t>((v >> 128) & mask64));
   write_u64(static_cast<uint64_t>((v >> 192) & mask64));
}

void encoder::write_i8(int8_t v) { _buffer.push_back(static_cast<uint8_t>(v)); }

void encoder::write_i16(int16_t v) { write_primitive(v); }

void encoder::write_i32(int32_t v) { write_primitive(v); }

void encoder::write_i64(int64_t v) { write_primitive(v); }

void encoder::write_i128(const fc::int128& v) {
   // Write as little-endian using two's complement
   uint64_t low = static_cast<uint64_t>(v & fc::int128(UINT64_MAX));
   uint64_t high = static_cast<uint64_t>(v >> 64);
   write_u64(low);
   write_u64(high);
}

void encoder::write_i256(const fc::int256& v) {
   // Write as little-endian using two's complement: 4 x 64-bit chunks
   // Convert to unsigned for bit manipulation, preserving two's complement representation
   fc::uint256 uv;
   if (v >= 0) {
      uv = fc::uint256(v);
   } else {
      // For negative values, we need the two's complement representation
      // boost::multiprecision handles this correctly when casting
      uv = fc::uint256(v);
   }
   fc::uint256 mask64 = std::numeric_limits<uint64_t>::max();
   write_u64(static_cast<uint64_t>(uv & mask64));
   write_u64(static_cast<uint64_t>((uv >> 64) & mask64));
   write_u64(static_cast<uint64_t>((uv >> 128) & mask64));
   write_u64(static_cast<uint64_t>((uv >> 192) & mask64));
}

void encoder::write_f32(float v) { write_primitive(v); }

void encoder::write_f64(double v) { write_primitive(v); }

void encoder::write_bool(bool v) { _buffer.push_back(v ? 1 : 0); }

void encoder::write_string(const std::string& v) {
   write_u32(static_cast<uint32_t>(v.size()));
   _buffer.insert(_buffer.end(), v.begin(), v.end());
}

void encoder::write_bytes(const std::vector<uint8_t>& v) {
   write_u32(static_cast<uint32_t>(v.size()));
   _buffer.insert(_buffer.end(), v.begin(), v.end());
}

void encoder::write_fixed_bytes(const uint8_t* data, size_t len) {
   _buffer.insert(_buffer.end(), data, data + len);
}

void encoder::write_pubkey(const solana_public_key& pk) {
   _buffer.insert(_buffer.end(), pk.data.begin(), pk.data.end());
}

//=============================================================================
// Decoder implementation
//=============================================================================

uint8_t decoder::read_u8() {
   ensure_remaining(1);
   return _data[_pos++];
}

uint16_t decoder::read_u16() { return read_primitive<uint16_t>(); }

uint32_t decoder::read_u32() { return read_primitive<uint32_t>(); }

uint64_t decoder::read_u64() { return read_primitive<uint64_t>(); }

fc::uint128 decoder::read_u128() {
   uint64_t low = read_u64();
   uint64_t high = read_u64();
   return (fc::uint128(high) << 64) | fc::uint128(low);
}

fc::uint256 decoder::read_u256() {
   // Read 4 x 64-bit chunks in little-endian order
   uint64_t chunk0 = read_u64();
   uint64_t chunk1 = read_u64();
   uint64_t chunk2 = read_u64();
   uint64_t chunk3 = read_u64();
   fc::uint256 result = fc::uint256(chunk3);
   result = (result << 64) | fc::uint256(chunk2);
   result = (result << 64) | fc::uint256(chunk1);
   result = (result << 64) | fc::uint256(chunk0);
   return result;
}

int8_t decoder::read_i8() {
   ensure_remaining(1);
   return static_cast<int8_t>(_data[_pos++]);
}

int16_t decoder::read_i16() { return read_primitive<int16_t>(); }

int32_t decoder::read_i32() { return read_primitive<int32_t>(); }

int64_t decoder::read_i64() { return read_primitive<int64_t>(); }

fc::int128 decoder::read_i128() {
   uint64_t low = read_u64();
   uint64_t high = read_u64();
   // Reconstruct two's complement signed value
   fc::int128 result = (fc::int128(static_cast<int64_t>(high)) << 64) | fc::int128(low);
   return result;
}

fc::int256 decoder::read_i256() {
   // Read 4 x 64-bit chunks in little-endian order
   uint64_t chunk0 = read_u64();
   uint64_t chunk1 = read_u64();
   uint64_t chunk2 = read_u64();
   uint64_t chunk3 = read_u64();

   // Reconstruct the value, treating the highest chunk as signed for sign extension
   // Similar to how read_i128 handles sign via int64_t cast of high bits
   fc::int256 result = fc::int256(static_cast<int64_t>(chunk3));
   result = (result << 64) | fc::int256(chunk2);
   result = (result << 64) | fc::int256(chunk1);
   result = (result << 64) | fc::int256(chunk0);
   return result;
}

float decoder::read_f32() { return read_primitive<float>(); }

double decoder::read_f64() { return read_primitive<double>(); }

bool decoder::read_bool() {
   uint8_t v = read_u8();
   FC_ASSERT(v <= 1, "Invalid boolean value: {}", v);
   return v != 0;
}

std::string decoder::read_string() {
   uint32_t len = read_u32();
   ensure_remaining(len);
   std::string result(reinterpret_cast<const char*>(_data + _pos), len);
   _pos += len;
   return result;
}

std::vector<uint8_t> decoder::read_bytes() {
   uint32_t len = read_u32();
   ensure_remaining(len);
   std::vector<uint8_t> result(_data + _pos, _data + _pos + len);
   _pos += len;
   return result;
}

void decoder::read_fixed_bytes(uint8_t* out, size_t len) {
   ensure_remaining(len);
   std::memcpy(out, _data + _pos, len);
   _pos += len;
}

std::vector<uint8_t> decoder::read_fixed_bytes(size_t len) {
   ensure_remaining(len);
   std::vector<uint8_t> result(_data + _pos, _data + _pos + len);
   _pos += len;
   return result;
}

solana_public_key decoder::read_pubkey() {
   solana_public_key pk;
   read_fixed_bytes(pk.data.data(), solana_public_key::SIZE);
   return pk;
}

void decoder::skip(size_t n) {
   ensure_remaining(n);
   _pos += n;
}

//=============================================================================
// Helper functions
//=============================================================================

std::array<uint8_t, 8> compute_discriminator(const std::string& namespace_prefix, const std::string& name) {
   // Anchor discriminator is sha256(namespace:name)[0..8]
   std::string preimage = namespace_prefix + ":" + name;
   fc::sha256 hash = fc::sha256::hash(preimage.data(), preimage.size());

   std::array<uint8_t, 8> discriminator;
   std::memcpy(discriminator.data(), hash.data(), 8);
   return discriminator;
}

std::array<uint8_t, 8> compute_account_discriminator(const std::string& name) {
   return compute_discriminator("account", name);
}

}  // namespace fc::network::solana::borsh
