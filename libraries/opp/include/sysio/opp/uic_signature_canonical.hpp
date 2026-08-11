#pragma once
/**
 * @file uic_signature_canonical.hpp
 * @brief Host/contract-shared recovery-byte and scalar canonicality policy for
 *        UIC ECDSA signatures.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sysio::opp {

/// Curve used by one recoverable UIC ECDSA signature variant.
enum class uic_ecdsa_curve {
   secp256k1,
   p256,
};

/// Position of the recovery byte inside a 65-byte compact signature body.
enum class uic_ecdsa_recovery_position {
   prefix,
   suffix,
};

/// Canonical numeric encoding of the recovery byte.
enum class uic_ecdsa_recovery_encoding {
   compact,
   ethereum,
};

namespace detail {

using uic_scalar = std::array<uint8_t, 32>;

inline constexpr uic_scalar secp256k1_order{
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
   0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
   0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41,
};

inline constexpr uic_scalar secp256k1_half_order{
   0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d,
   0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0,
};

inline constexpr uic_scalar p256_order{
   0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
   0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

inline constexpr uic_scalar p256_half_order{
   0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00,
   0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xde, 0x73, 0x7d, 0x56, 0xd3, 0x8b, 0xcf, 0x42,
   0x79, 0xdc, 0xe5, 0x61, 0x7e, 0x31, 0x92, 0xa8,
};

/// Compare a 32-byte big-endian scalar with a fixed 32-byte bound.
inline int compare_uic_scalar(std::span<const char, 32> scalar,
                              const uic_scalar& bound) {
   for (size_t i = 0; i < scalar.size(); ++i) {
      const auto value = static_cast<uint8_t>(scalar[i]);
      if (value < bound[i]) return -1;
      if (value > bound[i]) return 1;
   }
   return 0;
}

/// Return true when a 32-byte big-endian scalar is nonzero.
inline bool is_nonzero_uic_scalar(std::span<const char, 32> scalar) {
   for (const char byte : scalar) {
      if (byte != 0) return true;
   }
   return false;
}

} // namespace detail

/**
 * @brief Validate the recovery byte, `r`/`s` scalars, and low-s form.
 *
 * K1 and R1 use `[recovery || r || s]`; EM uses `[r || s || recovery]`.
 * K1/R1 use compact recovery headers 31..34, while EM uses Ethereum recovery
 * values 27..30. Accepting the opposite range would create a second byte
 * representation that libfc normalizes to the same recovered key.
 *
 * @param compact_body Exact 65-byte compact signature body, without the variant tag.
 * @param curve Curve order governing the scalar fields.
 * @param recovery_position Location of the recovery byte in `compact_body`.
 * @param recovery_encoding Required numeric recovery-byte representation.
 * @return True only for the required recovery range, `0 < r < n`, and
 *         `0 < s <= n/2`.
 */
inline bool is_canonical_uic_ecdsa_signature(
   std::span<const char> compact_body,
   uic_ecdsa_curve curve,
   uic_ecdsa_recovery_position recovery_position,
   uic_ecdsa_recovery_encoding recovery_encoding) {
   constexpr size_t scalar_size = 32;
   constexpr size_t recovery_field_size = 1;
   constexpr size_t prefixed_recovery_offset = 0;
   constexpr size_t suffixed_recovery_offset = 2 * scalar_size;
   constexpr size_t compact_size =
      2 * scalar_size + recovery_field_size;
   constexpr uint8_t compact_recovery_base = 31;
   constexpr uint8_t ethereum_recovery_base = 27;
   constexpr uint8_t recovery_value_count = 4;
   if (compact_body.size() != compact_size) return false;

   const size_t recovery_offset =
      recovery_position == uic_ecdsa_recovery_position::prefix
         ? prefixed_recovery_offset : suffixed_recovery_offset;
   const size_t r_offset =
      recovery_position == uic_ecdsa_recovery_position::prefix
         ? prefixed_recovery_offset + recovery_field_size
         : prefixed_recovery_offset;
   const size_t s_offset = r_offset + scalar_size;
   const std::span<const char, scalar_size> r{compact_body.data() + r_offset, scalar_size};
   const std::span<const char, scalar_size> s{compact_body.data() + s_offset, scalar_size};

   const uint8_t recovery_base =
      recovery_encoding == uic_ecdsa_recovery_encoding::compact
         ? compact_recovery_base : ethereum_recovery_base;
   const auto recovery = static_cast<uint8_t>(compact_body[recovery_offset]);
   if (recovery < recovery_base ||
       recovery >= recovery_base + recovery_value_count) {
      return false;
   }

   const auto& order = curve == uic_ecdsa_curve::p256
      ? detail::p256_order : detail::secp256k1_order;
   const auto& half_order = curve == uic_ecdsa_curve::p256
      ? detail::p256_half_order : detail::secp256k1_half_order;

   return detail::is_nonzero_uic_scalar(r)
       && detail::compare_uic_scalar(r, order) < 0
       && detail::is_nonzero_uic_scalar(s)
       && detail::compare_uic_scalar(s, half_order) <= 0;
}

} // namespace sysio::opp
