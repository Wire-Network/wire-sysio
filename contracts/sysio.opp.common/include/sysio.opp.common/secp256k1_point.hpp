#pragma once
/**
 * @file secp256k1_point.hpp
 * @brief Curve-membership checks for SEC1-encoded secp256k1 public keys.
 *
 * A contract that stores an external EVM key without a signature by that key (the trusted OPP
 * node-owner path) must still refuse bytes that are not a curve point: such a key has no private
 * key, so it can never sign, yet the depot would copy it verbatim into every roster it emits.
 * The field arithmetic runs through the `mod_exp` host function, so each verdict costs a few
 * bounded 256-bit exponentiations.
 *
 * secp256k1 is y^2 = x^3 + 7 over F_p with p = 2^256 - 2^32 - 977. Its group order is prime, so
 * no affine point has y = 0, and x^3 + 7 is never zero for a point's x-coordinate.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <sysio/crypto.hpp>
#include <sysio/crypto_ext.hpp>

#include <sysio.opp.common/evm_address.hpp>

namespace sysio::opp::secp256k1 {

/** Bytes in one big-endian field element (an x or y coordinate). */
inline constexpr std::size_t coordinate_size = 32;

/** Bytes in a compressed SEC1 public key: a parity prefix followed by x. */
inline constexpr std::size_t compressed_key_size = 1 + coordinate_size;

/** SEC1 prefix of a compressed key whose y-coordinate is even. */
inline constexpr std::uint8_t compressed_even_y_prefix = 0x02;

/** SEC1 prefix of a compressed key whose y-coordinate is odd. */
inline constexpr std::uint8_t compressed_odd_y_prefix = 0x03;

/** The curve constant b in y^2 = x^3 + b. */
inline constexpr std::uint8_t curve_b = 7;

/** One big-endian field element. */
using field_element = std::array<std::uint8_t, coordinate_size>;

/** The field prime p = 2^256 - 2^32 - 977, big-endian. */
inline constexpr std::array<std::uint8_t, coordinate_size> field_prime = {
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xfc, 0x2f};

/** Euler's-criterion exponent (p - 1) / 2, big-endian: a^((p-1)/2) is 1 exactly for nonzero squares. */
inline constexpr std::array<std::uint8_t, coordinate_size> euler_exponent = {
   0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xfe, 0x17};

namespace detail {

/** Exponent 1: `mod_exp` with it reduces a wider value mod p. */
inline constexpr char reduce_exponent = 1;

/** Exponent 2, for y^2. */
inline constexpr char square_exponent = 2;

/** Exponent 3, for x^3. */
inline constexpr char cube_exponent = 3;

/** View bytes as the `char` buffer `mod_exp` takes. */
inline const char* as_chars(const std::uint8_t* bytes) { return reinterpret_cast<const char*>(bytes); }

/**
 * Compute `base^exponent mod p` for a big-endian `base` of any width.
 *
 * @return The reduced field element, or nullopt when the host reports an error.
 */
inline std::optional<field_element> pow_mod_p(const std::uint8_t* base, std::size_t base_size,
                                              const char* exponent, std::size_t exponent_size) {
   field_element result{};
   if (mod_exp(as_chars(base), base_size, exponent, exponent_size, as_chars(field_prime.data()), field_prime.size(),
               reinterpret_cast<char*>(result.data()), result.size()) != 0) {
      return std::nullopt;
   }
   return result;
}

/** Return whether a big-endian 32-byte value is a canonical field element, i.e. strictly below p. */
inline bool is_field_element(const std::uint8_t* value) {
   return std::lexicographical_compare(value, value + coordinate_size, field_prime.begin(), field_prime.end());
}

/**
 * Compute the curve's right-hand side x^3 + 7 mod p for a canonical x.
 *
 * `mod_exp` reduces x^3; adding 7 can carry past p (never past 2^256 + 7), so the sum is widened by
 * one byte and reduced again with exponent 1.
 */
inline std::optional<field_element> curve_rhs(const std::uint8_t* x) {
   const auto x_cubed = pow_mod_p(x, coordinate_size, &cube_exponent, sizeof(cube_exponent));
   if (!x_cubed) return std::nullopt;

   std::array<std::uint8_t, coordinate_size + 1> sum{};
   unsigned carry = curve_b;
   for (std::size_t i = coordinate_size; i-- > 0;) {
      carry += (*x_cubed)[i];
      sum[i + 1] = static_cast<std::uint8_t>(carry & 0xffu);
      carry >>= 8;
   }
   sum[0] = static_cast<std::uint8_t>(carry);
   return pow_mod_p(sum.data(), sum.size(), &reduce_exponent, sizeof(reduce_exponent));
}

} // namespace detail

/**
 * Return whether 33 bytes are the compressed SEC1 encoding of a secp256k1 point.
 *
 * Requires a 0x02/0x03 prefix, a canonical x below p, and x^3 + 7 to be a nonzero square mod p
 * (Euler's criterion), which holds exactly when some y completes the point. Each prefix then names
 * one of the two such y values, so a key that passes decompresses to a real point.
 *
 * @param key The prefix byte followed by the big-endian x-coordinate, as the EM alternative of
 *            `sysio::public_key` stores it.
 */
inline bool is_compressed_key(const ecc_public_key& key) {
   static_assert(std::tuple_size_v<ecc_public_key> == compressed_key_size);
   if (key[0] != compressed_even_y_prefix && key[0] != compressed_odd_y_prefix) return false;

   const std::uint8_t* x = key.data() + 1;
   if (!detail::is_field_element(x)) return false;

   const auto rhs = detail::curve_rhs(x);
   if (!rhs) return false;
   const auto legendre = detail::pow_mod_p(rhs->data(), rhs->size(), detail::as_chars(euler_exponent.data()),
                                           euler_exponent.size());
   if (!legendre) return false;

   field_element one{};
   one.back() = 1;
   return *legendre == one;
}

/**
 * Return whether bytes are the uncompressed SEC1 encoding of a secp256k1 point.
 *
 * Requires `evm_uncompressed_public_key_size` bytes with the `evm_uncompressed_public_key_prefix`,
 * canonical x and y below p, and y^2 = x^3 + 7 mod p.
 *
 * @param key Pointer to the encoded key.
 * @param key_size Number of bytes available at `key`.
 */
inline bool is_uncompressed_key(const char* key, std::size_t key_size) {
   const auto* bytes = reinterpret_cast<const std::uint8_t*>(key);
   static_assert(evm_uncompressed_public_key_size == 1 + 2 * coordinate_size);
   if (key_size != evm_uncompressed_public_key_size || bytes[0] != evm_uncompressed_public_key_prefix) return false;

   const std::uint8_t* x = bytes + 1;
   const std::uint8_t* y = x + coordinate_size;
   if (!detail::is_field_element(x) || !detail::is_field_element(y)) return false;

   const auto y_squared = detail::pow_mod_p(y, coordinate_size, &detail::square_exponent,
                                            sizeof(detail::square_exponent));
   const auto rhs = detail::curve_rhs(x);
   return y_squared && rhs && *y_squared == *rhs;
}

} // namespace sysio::opp::secp256k1
