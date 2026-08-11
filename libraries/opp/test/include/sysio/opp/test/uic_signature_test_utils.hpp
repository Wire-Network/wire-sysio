#pragma once

#include <fc/crypto/private_key.hpp>
#include <sysio/opp/uic_signature_canonical.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sysio::opp::test {

/**
 * Replace a canonical recovery byte with libfc's equivalent noncanonical
 * alias while preserving the recovery id.
 *
 * @param packed Canonically packed recoverable signature.
 * @param key_type Concrete signing-key variant.
 * @return Mutated packed signature, or nullopt for an unsupported type or
 *         malformed input.
 */
inline std::optional<std::vector<char>> create_recovery_alias(
   std::vector<char> packed,
   fc::crypto::private_key::key_type key_type) {
   constexpr size_t signature_tag_size = 1;
   constexpr size_t scalar_size = detail::uic_scalar{}.size();
   constexpr size_t prefixed_recovery_offset = signature_tag_size;
   constexpr size_t suffixed_recovery_offset =
      signature_tag_size + 2 * scalar_size;
   constexpr uint8_t compact_recovery_base = 31;
   constexpr uint8_t ethereum_recovery_base = 27;
   constexpr uint8_t recovery_value_count = 4;

   size_t recovery_offset = 0;
   uint8_t recovery_base = 0;
   int alias_delta = 0;
   using key_variant = fc::crypto::private_key::key_type;
   switch (key_type) {
      case key_variant::k1:
      case key_variant::r1:
         recovery_offset = prefixed_recovery_offset;
         recovery_base = compact_recovery_base;
         alias_delta = -static_cast<int>(recovery_value_count);
         break;
      case key_variant::em:
         recovery_offset = suffixed_recovery_offset;
         recovery_base = ethereum_recovery_base;
         alias_delta = recovery_value_count;
         break;
      default:
         return std::nullopt;
   }

   if (recovery_offset >= packed.size()) return std::nullopt;
   const auto recovery = static_cast<uint8_t>(packed[recovery_offset]);
   if (recovery < recovery_base ||
       recovery >= recovery_base + recovery_value_count) {
      return std::nullopt;
   }
   packed[recovery_offset] = static_cast<char>(
      static_cast<int>(recovery) + alias_delta);
   return packed;
}

/**
 * Convert a canonically packed low-s K1, R1, or EM signature into its
 * cryptographically equivalent high-s form.
 *
 * @param packed Canonically packed recoverable signature.
 * @param key_type Concrete signing-key variant.
 * @return Mutated packed signature, or nullopt for an unsupported type or
 *         malformed input.
 */
inline std::optional<std::vector<char>> create_high_s_alternate(
   std::vector<char> packed,
   fc::crypto::private_key::key_type key_type) {
   constexpr size_t signature_tag_size = 1;
   constexpr size_t recovery_field_size = 1;
   constexpr size_t scalar_size = detail::uic_scalar{}.size();
   constexpr size_t prefixed_recovery_offset = signature_tag_size;
   constexpr size_t prefixed_s_offset =
      signature_tag_size + recovery_field_size + scalar_size;
   constexpr size_t suffixed_s_offset = signature_tag_size + scalar_size;
   constexpr size_t suffixed_recovery_offset =
      signature_tag_size + 2 * scalar_size;
   constexpr uint8_t compact_recovery_base = 31;
   constexpr uint8_t ethereum_recovery_base = 27;

   size_t s_offset = 0;
   size_t recovery_offset = 0;
   uint8_t recovery_base = 0;
   const detail::uic_scalar* order = nullptr;
   using key_variant = fc::crypto::private_key::key_type;
   switch (key_type) {
      case key_variant::k1:
         s_offset = prefixed_s_offset;
         recovery_offset = prefixed_recovery_offset;
         recovery_base = compact_recovery_base;
         order = &detail::secp256k1_order;
         break;
      case key_variant::r1:
         s_offset = prefixed_s_offset;
         recovery_offset = prefixed_recovery_offset;
         recovery_base = compact_recovery_base;
         order = &detail::p256_order;
         break;
      case key_variant::em:
         s_offset = suffixed_s_offset;
         recovery_offset = suffixed_recovery_offset;
         recovery_base = ethereum_recovery_base;
         order = &detail::secp256k1_order;
         break;
      default:
         return std::nullopt;
   }

   if (s_offset + scalar_size > packed.size() || recovery_offset >= packed.size()) {
      return std::nullopt;
   }

   detail::uic_scalar high_s{};
   unsigned borrow = 0;
   for (size_t offset = 0; offset < scalar_size; ++offset) {
      const size_t i = scalar_size - 1 - offset;
      const unsigned minuend = (*order)[i];
      const unsigned subtrahend =
         static_cast<uint8_t>(packed[s_offset + i]) + borrow;
      if (minuend < subtrahend) {
         high_s[i] = static_cast<uint8_t>(minuend + 256u - subtrahend);
         borrow = 1;
      } else {
         high_s[i] = static_cast<uint8_t>(minuend - subtrahend);
         borrow = 0;
      }
   }
   if (borrow != 0) return std::nullopt;

   const auto recovery = static_cast<uint8_t>(packed[recovery_offset]);
   if (recovery < recovery_base) return std::nullopt;
   std::copy(high_s.begin(), high_s.end(), packed.begin() + s_offset);
   packed[recovery_offset] = static_cast<char>(
      recovery_base + ((recovery - recovery_base) ^ 1u));
   return packed;
}

} // namespace sysio::opp::test
