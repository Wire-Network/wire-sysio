#pragma once
/**
 * @file fc/slug_name.hpp
 * @brief 8-byte packed identifier for Chain/Token/Reserve `code` fields.
 *
 * fc::slug_name is fc::basic_name specialised to the slug alphabet — up to 8
 * characters over [A-Z0-9_], packed most-significant-symbol-first into a
 * uint64. It mirrors the contract-side sysio::slug_name; both sides must use
 * the identical encoding so a packed value is byte-identical on the wire
 * (protobuf `uint64`).
 *
 * Encoded values live in [0, 2^48) — under JS Number's 2^53 safe-integer
 * limit, so TS/JS code can use a plain `number` rather than bigint.
 *
 * @see fc::basic_name, sysio::slug_name (contract-side mirror)
 */

#include <fc/basic_name.hpp>
#include <fc/exception/exception.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace fc {

struct slug_name_traits {
   // max_len is 8, not the 11 (= ceil(64/6)) that would fill all 64 bits:
   // 8 symbols x 6 bits = 48, keeping every encoded value in [0, 2^48) —
   // under JS Number's 2^53 safe-integer limit, so the TS/JS side can use a
   // plain `number` instead of bigint. 11 symbols would span the full 64 bits.
   static constexpr int max_len = 8;

   // Symbol 0 is the '\0' pad/terminator; 1-26 = A-Z; 27-36 = 0-9; 37 = '_'.
   // Stored as a named array so the length comes from the literal via sizeof —
   // a leading NUL otherwise defeats std::string_view's auto-length.
   static constexpr char alphabet_storage[] =
      "\0ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
   static constexpr std::string_view alphabet{ alphabet_storage,
                                               sizeof(alphabet_storage) - 1 };

   [[noreturn]] static void throw_invalid( std::string_view in, const char* why ) {
      FC_ASSERT( false, "invalid slug_name '{}': {}", std::string(in), why );
      __builtin_unreachable();
   }
};

/// 8-byte packed identifier — alphabet [A-Z0-9_], <= 8 chars, MSB-first.
/// Byte-identical with the contract-side sysio::slug_name.
using slug_name = basic_name<slug_name_traits>;

namespace slug_name_literals {

/// Compile-time slug_name literal: `"ETH"_s`, `"USDC"_s`. A character outside
/// the alphabet, or more than 8 characters, is a compile error.
#if defined(__clang__)
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wgnu-string-literal-operator-template"
#endif
template <typename T, T... Str>
inline constexpr slug_name operator""_s() {
   constexpr char buf[] = {Str...};
   static_assert(slug_name::is_valid_literal(std::string_view{buf, sizeof(buf)}),
                 "invalid _s literal: character not in [A-Z0-9_], or longer than 8");
   return slug_name{ std::integral_constant<uint64_t, slug_name::pack(std::string_view{buf, sizeof(buf)})>::value };
}
#if defined(__clang__)
# pragma clang diagnostic pop
#endif

} // namespace slug_name_literals

using slug_name_literals::operator""_s;

} // namespace fc
