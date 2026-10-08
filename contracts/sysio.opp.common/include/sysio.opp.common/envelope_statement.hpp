#pragma once
/**
 * @file envelope_statement.hpp
 * @brief The statement `sysio.synd` asks `sysio.bond` to underwrite for one token of one accepted
 *        inbound OPP envelope: "the envelope of outpost `chain_code` at epoch `epoch_index`, whose
 *        canonical digest is `digest`, carried these syndications and yield reports of `token_code`".
 *
 * `sysio.bond` stores a statement as opaque bytes under a schema name and refuses a second request
 * for the same bytes. This header is the ONE definition of that schema's name and byte layout, so
 * the issuer that packs a statement and any reader that checks one agree by construction.
 *
 * The bytes are the zpp::bits encoding of `wire_statement` with no size prefix: the chain code and
 * the token code as 8-byte little-endian integers, the epoch as a 4-byte little-endian integer, and
 * the digest as its 32 raw bytes, in declaration order.
 */

#include <sysio/crypto.hpp>
#include <sysio/name.hpp>

#include <zpp_bits.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sysio::opp::envelope_statement {

/// Schema name every envelope statement is registered under on `sysio.bond`.
inline constexpr name schema = "oppenvelope"_n;

/// One envelope statement.
struct statement {
   uint64_t    chain_code  = 0;   ///< registry code value of the outpost that sent the envelope
   uint32_t    epoch_index = 0;   ///< depot epoch the envelope was accepted for
   checksum256 digest;            ///< the envelope's canonical epoch digest, as `sysio.msgch` accepted it
   uint64_t    token_code  = 0;   ///< registry code value of the liq token the statement covers
};

/// Size of a digest's raw bytes.
inline constexpr size_t DIGEST_BYTES = 32;

/// The serialized form of a `statement`: every field a plain integer or byte array, so the
/// encoding is fixed-width and needs no custom serializer for `checksum256`.
struct wire_statement {
   uint64_t                            chain_code  = 0;   ///< see `statement::chain_code`
   uint32_t                            epoch_index = 0;   ///< see `statement::epoch_index`
   std::array<uint8_t, DIGEST_BYTES>   digest{};          ///< `statement::digest` as raw bytes
   uint64_t                            token_code  = 0;   ///< see `statement::token_code`
};

/// Encode `s` as the statement bytes `sysio.bond::request` receives: zpp::bits, no size prefix.
inline std::vector<char> pack(const statement& s) {
   const wire_statement wire{
      .chain_code  = s.chain_code,
      .epoch_index = s.epoch_index,
      .digest      = s.digest.extract_as_byte_array(),
      .token_code  = s.token_code,
   };
   std::vector<char> bytes;
   auto              out = zpp::bits::out{bytes, zpp::bits::no_size{}};
   (void)out(wire);
   return bytes;
}

/// Decode statement bytes packed by `pack`, or `std::nullopt` when `bytes` is not exactly one encoded
/// statement. Never throws.
inline std::optional<statement> unpack(const std::vector<char>& bytes) {
   wire_statement wire;
   auto           in = zpp::bits::in{std::span{bytes.data(), bytes.size()}, zpp::bits::no_size{}};
   if (in(wire) != zpp::bits::errc{}) return std::nullopt;
   if (in.position() != bytes.size()) return std::nullopt;
   return statement{
      .chain_code  = wire.chain_code,
      .epoch_index = wire.epoch_index,
      .digest      = checksum256{wire.digest},
      .token_code  = wire.token_code,
   };
}

} // namespace sysio::opp::envelope_statement
