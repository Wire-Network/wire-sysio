#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <ethash/keccak.hpp>

namespace fc {

keccak256::keccak256() {
   memset(_hash, 0, sizeof(_hash));
}

keccak256::keccak256(const std::string& hex_str) {
   auto bytes = from_hex(hex_str);
   FC_ASSERT(bytes.size() == sizeof(_hash),
             "Invalid keccak256 hex string length: {}", hex_str.size());
   memcpy(_hash, bytes.data(), sizeof(_hash));
}

std::string keccak256::str() const {
   return to_hex(to_char_span());
}

keccak256 keccak256::hash(std::span<const uint8_t> bytes) {
   keccak256 h;
   auto result = ethash::keccak256(bytes.data(), bytes.size());
   static_assert(sizeof(h._hash) == sizeof(result.bytes), "Invalid keccak256 size");
   memcpy(h._hash, result.bytes, sizeof(h._hash));
   return h;
}

keccak256 keccak256::hash(const std::string& s) {
   return hash(std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

bool operator==(const keccak256& h1, const keccak256& h2) {
   return memcmp(h1._hash, h2._hash, sizeof(h1._hash)) == 0;
}

std::strong_ordering operator<=>( const keccak256& h1, const keccak256& h2 ) {
   return memcmp(h1._hash, h2._hash, sizeof(h1._hash)) <=> 0;
}

keccak256::encoder::encoder() {
   reset();
}

keccak256::encoder::~encoder() {}

void keccak256::encoder::write(const char* d, uint32_t dlen) {
   data.insert(data.end(), d, d + dlen);
}

keccak256 keccak256::encoder::result() {
   return keccak256::hash(std::span(data.data(), data.size()));
}

void keccak256::encoder::reset() {
   data.clear();
}

void to_variant(const keccak256& bi, variant& v) {
   v = bi.str();
}

void from_variant(const variant& v, keccak256& bi) {
   bi = keccak256(v.as_string());
}

} // namespace fc
