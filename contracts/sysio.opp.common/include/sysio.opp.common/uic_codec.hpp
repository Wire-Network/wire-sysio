#pragma once
/**
 * @file uic_codec.hpp
 * @brief Bounded decoding of UnderwriteIntentCommit payloads, shared by sysio.msgch and sysio.uwrit.
 */

#include <sysio/opp/attestations/attestations.pb.hpp>

#include <zpp_bits.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace sysio::opp {

/// Largest encoded UnderwriteIntentCommit the depot accepts. A real UIC is a handful of ids and
/// codes plus one fixed-size signature of at most 97 bytes; the outposts refuse a larger one before
/// relaying it, and `sysio.uwrit::rcrdcommit` refuses one before storing it.
inline constexpr uint32_t MAX_UIC_LEG_BYTES = 2048;

namespace detail {

/// True when the only containers anywhere in `T` are byte strings. zpp_bits checks a length-delimited
/// byte string against `alloc_limit` before resizing it, but reserves a packed repeated varint
/// field's declared length without that check. A byte string sent under any other wire type grows
/// one byte at a time unchecked; only `decode_uic`'s input cap bounds that.
template <typename T>
consteval bool alloc_limit_covers() {
   using type = std::remove_cvref_t<T>;
   if constexpr (std::is_same_v<type, std::string> || std::is_same_v<type, std::vector<char>>) {
      return true;
   } else if constexpr (zpp::bits::concepts::container<type>) {
      return false;
   } else if constexpr (std::is_class_v<type> && !zpp::bits::concepts::varint<type>) {
      // visit_members_types yields a default-constructed value of the visitor's return type.
      return decltype(zpp::bits::visit_members_types<type>([]<typename... Members>() {
         return std::bool_constant<(alloc_limit_covers<Members>() && ...)>{};
      }))::value;
   } else {
      return true;
   }
}

} // namespace detail

/// Decode `bytes` as an UnderwriteIntentCommit, with every allocation bounded by
/// `MAX_UIC_LEG_BYTES`.
///
/// Uncapped, zpp_bits resizes a bytes or string field to its declared length before checking that
/// the input holds that many bytes, so a few bytes declaring a huge length trap on memory growth
/// instead of returning an error, and the trap reverts the enclosing consensus delivery. The input
/// cap also bounds the one growth `alloc_limit` does not see: a bytes field sent under a
/// non-length-delimited wire type appends one byte per value.
///
/// @return the decoded UIC, or `std::nullopt` when `bytes` exceeds `MAX_UIC_LEG_BYTES` or does not
///         decode.
inline std::optional<attestations::UnderwriteIntentCommit> decode_uic(std::span<const char> bytes) {
   if (bytes.size() > MAX_UIC_LEG_BYTES) return std::nullopt;
   attestations::UnderwriteIntentCommit uic;
   auto in = zpp::bits::in{bytes, zpp::bits::no_size{}, zpp::bits::alloc_limit<MAX_UIC_LEG_BYTES>{}};
   if (in(uic) != zpp::bits::errc{}) return std::nullopt;
   return uic;
}

static_assert(detail::alloc_limit_covers<attestations::UnderwriteIntentCommit>(),
              "the UnderwriteIntentCommit tree gained a non-byte-string container; decode_uic no longer "
              "bounds every allocation");

} // namespace sysio::opp
