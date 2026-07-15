#pragma once
/**
 * @file opp_canonical_codec.hpp
 * @brief Field-complete canonical protobuf encoder and keccak256 epoch digest for OPP envelopes.
 *
 * Cross-chain OPP epoch envelopes chain via `Envelope.previous_envelope_hash`: each envelope carries
 * the digest of the previous envelope emitted on the same (depot, outpost) stream. Every chain that
 * verifies the link must compute the identical digest, defined as
 *
 *     epoch_digest = keccak256( canonical_encode( envelope with envelope_hash := empty ) )
 *
 * where `canonical_encode` is the FIELD-COMPLETE protobuf wire encoding produced by the generated
 * Solidity codec (`protoc-gen-solidity`, see `libraries/opp/tools/protoc-gen-solidity` and the
 * generated `EnvelopeCodec` in the `@wireio/opp-solidity-models` bundle). It differs from standard
 * proto3 encoding in exactly one way: EVERY singular field is written unconditionally, including
 * proto3 defaults; empty `bytes` as `tag + varint(0)`, zero varints and enums as `tag + 0x00`, and
 * nested messages always present (length-prefixed), recursively. Repeated fields write one tag per
 * element (never packed); an empty repeated field writes nothing. Fields are written in ascending
 * field-number order with standard LEB128 varints and `varint((field << 3) | wire_type)` tags.
 *
 * The zpp::bits pb encoder used for the contracts' decode path is NOT suitable here: it writes
 * default scalars, enums and nested messages, but OMITS empty `bytes`/`string` fields, and
 * `envelope_hash` is empty on the wire by construction, so a zpp encoding can never equal the
 * digest preimage.
 *
 * Implementation is two-pass per message: a bottom-up `encoded_size` pass, then one sequential
 * write into a single pre-reserved buffer. This is byte-identical to the Solidity codec's nested
 * concatenation but avoids re-copying nested payloads once per nesting level; this encoder runs
 * on consensus-billed CPU.
 *
 * MAINTENANCE: this encoder must stay in lock-step with `libraries/opp/proto/sysio/opp/opp.proto`
 * (and `types.proto`) for every message reachable from `Envelope`. The `static_assert`s at the
 * bottom of this file fail the build when a regenerated pb header changes a member count; a field
 * RENUMBERING is not detectable at compile time and is caught by the cross-language golden-vector
 * tests (`contracts/tests/sysio.msgch_chain_tests.cpp`). Field numbers are pinned once in
 * `sysio::opp::canonical::field` below.
 */

#include <sysio/crypto.hpp>
#include <sysio/crypto_ext.hpp>
#include <sysio/opp/opp.pb.hpp>
#include <sysio/opp/types/types.pb.hpp>

#include <magic_enum/magic_enum.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace sysio::opp::canonical {

/// Protobuf wire types used by the OPP envelope tree (no fixed32/fixed64 fields exist in it).
enum class wire_type : uint32_t {
   varint           = 0,   ///< int32/int64/uint32/uint64/bool/enum
   length_delimited = 2,   ///< bytes/string/embedded message/repeated element
};

/// Proto field numbers for every message reachable from `Envelope`, pinned to
/// `libraries/opp/proto/sysio/opp/opp.proto` and `.../types/types.proto`. A renumbering there must
/// be mirrored here (and in every other OPP codec); the proto files mark retired slots
/// "do not reuse" for exactly this reason.
namespace field {
   namespace envelope {
      constexpr uint32_t envelope_hash          = 1;
      constexpr uint32_t endpoints              = 2;
      constexpr uint32_t epoch_timestamp        = 5;
      constexpr uint32_t epoch_index            = 6;
      constexpr uint32_t epoch_envelope_index   = 7;
      constexpr uint32_t previous_envelope_hash = 20;
      constexpr uint32_t messages               = 40;
   } // namespace envelope
   namespace endpoints {
      constexpr uint32_t start = 1;
      constexpr uint32_t end   = 2;
   } // namespace endpoints
   namespace chain_id {
      constexpr uint32_t kind = 1;
      constexpr uint32_t id   = 2;
   } // namespace chain_id
   namespace message {
      constexpr uint32_t header  = 1;
      constexpr uint32_t payload = 2;
   } // namespace message
   namespace message_header {
      constexpr uint32_t endpoints           = 1;
      constexpr uint32_t message_id          = 2;
      constexpr uint32_t previous_message_id = 3;
      // Slot 4 was `encoding_flags` — removed from opp.proto; reserved, do not reuse.
      constexpr uint32_t payload_size        = 5;
      constexpr uint32_t payload_checksum    = 6;
      constexpr uint32_t timestamp           = 7;
      constexpr uint32_t header_checksum     = 8;
   } // namespace message_header
   namespace message_payload {
      constexpr uint32_t version      = 1;
      constexpr uint32_t attestations = 2;
   } // namespace message_payload
   namespace attestation_entry {
      constexpr uint32_t type      = 1;
      constexpr uint32_t data_size = 2;
      constexpr uint32_t data      = 3;
   } // namespace attestation_entry
} // namespace field

namespace detail {

   /// Number of bytes the LEB128 varint encoding of `v` occupies (1..10).
   constexpr size_t varint_size(uint64_t v) {
      size_t n = 1;
      while (v >= 0x80) {
         v >>= 7;
         ++n;
      }
      return n;
   }

   /// Append the LEB128 varint encoding of `v` to `out`.
   inline void put_varint(std::vector<char>& out, uint64_t v) {
      while (v >= 0x80) {
         out.push_back(static_cast<char>(static_cast<uint8_t>(v) | 0x80));
         v >>= 7;
      }
      out.push_back(static_cast<char>(static_cast<uint8_t>(v)));
   }

   /// Protobuf tag value for `field_number` with wire type `t` (the value that gets varint-encoded).
   constexpr uint64_t make_tag(uint32_t field_number, wire_type t) {
      return (static_cast<uint64_t>(field_number) << 3) | static_cast<uint64_t>(t);
   }

   /// Encoded size of a varint-typed field (tag + value), written unconditionally.
   constexpr size_t varint_field_size(uint32_t field_number, uint64_t v) {
      return varint_size(make_tag(field_number, wire_type::varint)) + varint_size(v);
   }

   /// Append a varint-typed field (tag + value), written unconditionally.
   inline void put_varint_field(std::vector<char>& out, uint32_t field_number, uint64_t v) {
      put_varint(out, make_tag(field_number, wire_type::varint));
      put_varint(out, v);
   }

   /// Proto enums encode as the varint of their (sign-extended) integer value. Every enum in the
   /// OPP tree is non-negative, but sign-extending through int64_t matches proto semantics exactly.
   template <typename E>
   constexpr uint64_t enum_wire_value(E e) {
      return static_cast<uint64_t>(static_cast<int64_t>(magic_enum::enum_integer(e)));
   }

   /// Encoded size of a bytes/string field (tag + length + payload), written unconditionally;
   /// an empty value still contributes `tag + varint(0)`.
   inline size_t bytes_field_size(uint32_t field_number, const std::vector<char>& v) {
      return varint_size(make_tag(field_number, wire_type::length_delimited)) + varint_size(v.size()) + v.size();
   }

   /// Append a bytes/string field (tag + length + payload), written unconditionally.
   inline void put_bytes_field(std::vector<char>& out, uint32_t field_number, const std::vector<char>& v) {
      put_varint(out, make_tag(field_number, wire_type::length_delimited));
      put_varint(out, v.size());
      out.insert(out.end(), v.begin(), v.end());
   }

   /// Append an explicitly empty bytes field (`tag + varint(0)`), used when blanking a field.
   inline void put_empty_bytes_field(std::vector<char>& out, uint32_t field_number) {
      put_varint(out, make_tag(field_number, wire_type::length_delimited));
      put_varint(out, 0);
   }

   /// Encoded size of an embedded-message field (tag + length + body) given the body size.
   inline size_t submessage_field_size(uint32_t field_number, size_t body_size) {
      return varint_size(make_tag(field_number, wire_type::length_delimited)) + varint_size(body_size) + body_size;
   }

   /// Append the tag + length prefix of an embedded-message field; the caller writes the body next.
   inline void put_submessage_prefix(std::vector<char>& out, uint32_t field_number, size_t body_size) {
      put_varint(out, make_tag(field_number, wire_type::length_delimited));
      put_varint(out, body_size);
   }

} // namespace detail

// -------------------------------------------------------------------------------------------------
//  Per-message body sizing + writing. `encoded_size(m)` returns the size of the message BODY
//  (no outer tag/length); `encode_into(out, m)` appends exactly that body. Field order and
//  presence rules match the generated Solidity codec; see the file header.
// -------------------------------------------------------------------------------------------------

/// @{ sysio.opp.types.ChainId
inline size_t encoded_size(const types::ChainId& m) {
   return detail::varint_field_size(field::chain_id::kind, detail::enum_wire_value(m.kind)) +
          detail::varint_field_size(field::chain_id::id, static_cast<uint32_t>(m.id));
}
inline void encode_into(std::vector<char>& out, const types::ChainId& m) {
   detail::put_varint_field(out, field::chain_id::kind, detail::enum_wire_value(m.kind));
   detail::put_varint_field(out, field::chain_id::id, static_cast<uint32_t>(m.id));
}
/// @}

/// @{ sysio.opp.Endpoints
inline size_t encoded_size(const Endpoints& m) {
   return detail::submessage_field_size(field::endpoints::start, encoded_size(m.start)) +
          detail::submessage_field_size(field::endpoints::end, encoded_size(m.end));
}
inline void encode_into(std::vector<char>& out, const Endpoints& m) {
   detail::put_submessage_prefix(out, field::endpoints::start, encoded_size(m.start));
   encode_into(out, m.start);
   detail::put_submessage_prefix(out, field::endpoints::end, encoded_size(m.end));
   encode_into(out, m.end);
}
/// @}

/// @{ sysio.opp.MessageHeader
inline size_t encoded_size(const MessageHeader& m) {
   return detail::submessage_field_size(field::message_header::endpoints, encoded_size(m.endpoints)) +
          detail::bytes_field_size(field::message_header::message_id, m.message_id) +
          detail::bytes_field_size(field::message_header::previous_message_id, m.previous_message_id) +
          detail::varint_field_size(field::message_header::payload_size, static_cast<uint32_t>(m.payload_size)) +
          detail::bytes_field_size(field::message_header::payload_checksum, m.payload_checksum) +
          detail::varint_field_size(field::message_header::timestamp, static_cast<uint64_t>(m.timestamp)) +
          detail::bytes_field_size(field::message_header::header_checksum, m.header_checksum);
}
inline void encode_into(std::vector<char>& out, const MessageHeader& m) {
   detail::put_submessage_prefix(out, field::message_header::endpoints, encoded_size(m.endpoints));
   encode_into(out, m.endpoints);
   detail::put_bytes_field(out, field::message_header::message_id, m.message_id);
   detail::put_bytes_field(out, field::message_header::previous_message_id, m.previous_message_id);
   detail::put_varint_field(out, field::message_header::payload_size, static_cast<uint32_t>(m.payload_size));
   detail::put_bytes_field(out, field::message_header::payload_checksum, m.payload_checksum);
   detail::put_varint_field(out, field::message_header::timestamp, static_cast<uint64_t>(m.timestamp));
   detail::put_bytes_field(out, field::message_header::header_checksum, m.header_checksum);
}
/// @}

/// @{ sysio.opp.AttestationEntry
inline size_t encoded_size(const AttestationEntry& m) {
   return detail::varint_field_size(field::attestation_entry::type, detail::enum_wire_value(m.type)) +
          detail::varint_field_size(field::attestation_entry::data_size, static_cast<uint32_t>(m.data_size)) +
          detail::bytes_field_size(field::attestation_entry::data, m.data);
}
inline void encode_into(std::vector<char>& out, const AttestationEntry& m) {
   detail::put_varint_field(out, field::attestation_entry::type, detail::enum_wire_value(m.type));
   detail::put_varint_field(out, field::attestation_entry::data_size, static_cast<uint32_t>(m.data_size));
   detail::put_bytes_field(out, field::attestation_entry::data, m.data);
}
/// @}

/// @{ sysio.opp.MessagePayload
inline size_t encoded_size(const MessagePayload& m) {
   size_t n = detail::varint_field_size(field::message_payload::version, static_cast<uint32_t>(m.version));
   for (const auto& a : m.attestations)
      n += detail::submessage_field_size(field::message_payload::attestations, encoded_size(a));
   return n;
}
inline void encode_into(std::vector<char>& out, const MessagePayload& m) {
   detail::put_varint_field(out, field::message_payload::version, static_cast<uint32_t>(m.version));
   for (const auto& a : m.attestations) {
      detail::put_submessage_prefix(out, field::message_payload::attestations, encoded_size(a));
      encode_into(out, a);
   }
}
/// @}

/// @{ sysio.opp.Message
inline size_t encoded_size(const Message& m) {
   return detail::submessage_field_size(field::message::header, encoded_size(m.header)) +
          detail::submessage_field_size(field::message::payload, encoded_size(m.payload));
}
inline void encode_into(std::vector<char>& out, const Message& m) {
   detail::put_submessage_prefix(out, field::message::header, encoded_size(m.header));
   encode_into(out, m.header);
   detail::put_submessage_prefix(out, field::message::payload, encoded_size(m.payload));
   encode_into(out, m.payload);
}
/// @}

/// @{ sysio.opp.Envelope
/// `encoded_size`/`encode_into` encode the envelope exactly as given; the `*_epoch_preimage`
/// variants force `envelope_hash` (field 1) to empty without copying the envelope, per the digest
/// definition. `previous_envelope_hash` (field 20) is part of the preimage.
namespace detail {
   inline size_t envelope_body_size(const Envelope& m, bool blank_envelope_hash) {
      size_t n = blank_envelope_hash
                    ? bytes_field_size(field::envelope::envelope_hash, {})
                    : bytes_field_size(field::envelope::envelope_hash, m.envelope_hash);
      n += submessage_field_size(field::envelope::endpoints, encoded_size(m.endpoints));
      n += varint_field_size(field::envelope::epoch_timestamp, static_cast<uint64_t>(m.epoch_timestamp));
      n += varint_field_size(field::envelope::epoch_index, static_cast<uint32_t>(m.epoch_index));
      n += varint_field_size(field::envelope::epoch_envelope_index, static_cast<uint32_t>(m.epoch_envelope_index));
      n += bytes_field_size(field::envelope::previous_envelope_hash, m.previous_envelope_hash);
      for (const auto& msg : m.messages)
         n += submessage_field_size(field::envelope::messages, encoded_size(msg));
      return n;
   }
   inline void envelope_encode_into(std::vector<char>& out, const Envelope& m, bool blank_envelope_hash) {
      if (blank_envelope_hash) {
         put_empty_bytes_field(out, field::envelope::envelope_hash);
      } else {
         put_bytes_field(out, field::envelope::envelope_hash, m.envelope_hash);
      }
      put_submessage_prefix(out, field::envelope::endpoints, encoded_size(m.endpoints));
      encode_into(out, m.endpoints);
      put_varint_field(out, field::envelope::epoch_timestamp, static_cast<uint64_t>(m.epoch_timestamp));
      put_varint_field(out, field::envelope::epoch_index, static_cast<uint32_t>(m.epoch_index));
      put_varint_field(out, field::envelope::epoch_envelope_index, static_cast<uint32_t>(m.epoch_envelope_index));
      put_bytes_field(out, field::envelope::previous_envelope_hash, m.previous_envelope_hash);
      for (const auto& msg : m.messages) {
         put_submessage_prefix(out, field::envelope::messages, encoded_size(msg));
         encode_into(out, msg);
      }
   }
} // namespace detail

inline size_t encoded_size(const Envelope& m) { return detail::envelope_body_size(m, false); }
inline void encode_into(std::vector<char>& out, const Envelope& m) {
   detail::envelope_encode_into(out, m, false);
}
/// @}

/// Canonical field-complete encoding of `env`, exactly as it should appear on the wire.
inline std::vector<char> encode(const Envelope& env) {
   std::vector<char> out;
   out.reserve(encoded_size(env));
   encode_into(out, env);
   return out;
}

/// Canonical epoch-digest preimage of `env`: the field-complete encoding with `envelope_hash`
/// forced empty. Identical to `encode(env)` when `env.envelope_hash` is already empty (the only
/// shape the depot and outposts emit).
inline std::vector<char> encode_epoch_preimage(const Envelope& env) {
   std::vector<char> out;
   out.reserve(detail::envelope_body_size(env, true));
   detail::envelope_encode_into(out, env, true);
   return out;
}

/// The cross-chain OPP epoch digest of `env`:
/// `keccak256(canonical_encode(env with envelope_hash := empty))`. Matches the generated Solidity
/// `OPPCommon.epochHash` on every outpost; the value each envelope's successor must carry in
/// `previous_envelope_hash`.
inline sysio::checksum256 epoch_digest(const Envelope& env) {
   const std::vector<char> preimage = encode_epoch_preimage(env);
   return sysio::keccak(preimage.data(), preimage.size());
}

/// Canonical field-complete bytes of a standalone MessagePayload — the sub-message bytes exactly
/// as embedded inside an Envelope, minus the enclosing field tag + length prefix. Feeds
/// `MessageHeader.payload_size` and `MessageHeader.payload_checksum`.
inline std::vector<char> encode(const MessagePayload& payload) {
   std::vector<char> out;
   out.reserve(encoded_size(payload));
   encode_into(out, payload);
   return out;
}

/// Canonical field-complete bytes of a standalone MessageHeader (same sub-message form as
/// `encode(MessagePayload)`).
inline std::vector<char> encode(const MessageHeader& header) {
   std::vector<char> out;
   out.reserve(encoded_size(header));
   encode_into(out, header);
   return out;
}

/// The `MessageHeader.header_checksum` value: keccak256 over the canonical encoding of `header`
/// with `message_id` and `header_checksum` blanked. Takes a copy so callers can pass the header
/// they are about to finalize without pre-blanking either field. Matches the generated Solidity
/// `OPPCommon.headerChecksum`.
inline sysio::checksum256 header_digest(MessageHeader header) {
   header.message_id.clear();
   header.header_checksum.clear();
   const std::vector<char> preimage = encode(header);
   return sysio::keccak(preimage.data(), preimage.size());
}

/// The `MessageHeader.message_id` value: `header_checksum` with its first 8 bytes replaced by the
/// message's big-endian sequence number (the previous message's sequence number + 1; a stream's
/// first message is sequence number 1). Mirrors the Solidity `OPPCommon.getMessageID` /
/// `setMessageNumber` splice.
inline sysio::checksum256 derive_message_id(const sysio::checksum256& header_checksum,
                                            uint64_t                  sequence) {
   auto bytes = header_checksum.extract_as_byte_array();
   for (size_t i = 0; i < 8; ++i) {
      bytes[i] = static_cast<uint8_t>(sequence >> (8 * (7 - i)));
   }
   return sysio::checksum256{bytes};
}

/// The big-endian sequence number carried in the first 8 bytes of a wire `message_id`, accepting
/// only the two canonical forms: an EMPTY id (stream genesis) reads as 0, so the successor
/// message's sequence number is 1, and a 32-byte id yields its first 8 bytes. Any other length
/// is non-canonical and yields std::nullopt — verifiers treat it as a mismatch, never as genesis
/// or as a sequence source (1-7 bytes would otherwise alias genesis, and any other length would
/// alias a truncated or padded id).
inline std::optional<uint64_t> message_sequence(const std::vector<char>& message_id) {
   if (message_id.empty()) {
      return 0;
   }
   if (message_id.size() != 32) {
      return std::nullopt;
   }
   uint64_t sequence = 0;
   for (size_t i = 0; i < 8; ++i) {
      sequence = (sequence << 8) | static_cast<uint8_t>(message_id[i]);
   }
   return sequence;
}

// -------------------------------------------------------------------------------------------------
//  Drift guards: fail the build when a regenerated pb header changes a member count, forcing this
//  encoder (and the pinned field numbers above) to be revisited. Field renumbering without a count
//  change is caught by the cross-language golden-vector tests instead.
// -------------------------------------------------------------------------------------------------
static_assert(zpp::bits::access::number_of_members<Envelope>() == 7,
              "opp.proto Envelope changed; update opp_canonical_codec.hpp to match");
static_assert(zpp::bits::access::number_of_members<Endpoints>() == 2,
              "opp.proto Endpoints changed; update opp_canonical_codec.hpp to match");
static_assert(zpp::bits::access::number_of_members<Message>() == 2,
              "opp.proto Message changed; update opp_canonical_codec.hpp to match");
static_assert(zpp::bits::access::number_of_members<MessageHeader>() == 7,
              "opp.proto MessageHeader changed; update opp_canonical_codec.hpp to match");
static_assert(zpp::bits::access::number_of_members<MessagePayload>() == 2,
              "opp.proto MessagePayload changed; update opp_canonical_codec.hpp to match");
static_assert(zpp::bits::access::number_of_members<AttestationEntry>() == 3,
              "opp.proto AttestationEntry changed; update opp_canonical_codec.hpp to match");
static_assert(zpp::bits::access::number_of_members<types::ChainId>() == 2,
              "types.proto ChainId changed; update opp_canonical_codec.hpp to match");

} // namespace sysio::opp::canonical
