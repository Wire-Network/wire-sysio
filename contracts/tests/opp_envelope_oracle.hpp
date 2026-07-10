/// Shared host-side oracle for the canonical field-complete OPP envelope encoding and the
/// semantic MessageHeader derivation (opp.proto). An independent reimplementation of
/// `contracts/sysio.opp.common/include/sysio.opp.common/opp_canonical_codec.hpp` over the Google
/// protobuf classes, used by every contract test that builds or verifies inbound/outbound OPP
/// envelopes. Inbound envelopes delivered to `sysio.msgch` MUST carry a spec-derived header
/// (`oracle::finalize_header`) or `apply_consensus` drops them before dispatch.
#pragma once

#include <sysio/opp/opp.pb.h>

#include <fc/crypto/keccak256.hpp>

#include <magic_enum/magic_enum.hpp>

#include <span>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Canonical field-complete encoding oracle (host side, over the Google
//  protobuf classes). Field numbers and presence rules mirror
//  opp_canonical_codec.hpp; see that header for the encoding definition.
// ---------------------------------------------------------------------------
namespace oracle {

   inline void put_varint(std::vector<char>& out, uint64_t v) {
      while (v >= 0x80) {
         out.push_back(static_cast<char>(static_cast<uint8_t>(v) | 0x80));
         v >>= 7;
      }
      out.push_back(static_cast<char>(static_cast<uint8_t>(v)));
   }

   /// wire type 0 = varint, 2 = length-delimited
   inline void put_tag(std::vector<char>& out, uint32_t field, uint32_t wire) {
      put_varint(out, (static_cast<uint64_t>(field) << 3) | wire);
   }

   inline void put_varint_field(std::vector<char>& out, uint32_t field, uint64_t v) {
      put_tag(out, field, 0);
      put_varint(out, v);
   }

   inline void put_bytes_field(std::vector<char>& out, uint32_t field, const std::string& v) {
      put_tag(out, field, 2);
      put_varint(out, v.size());
      out.insert(out.end(), v.begin(), v.end());
   }

   inline void put_submessage(std::vector<char>& out, uint32_t field, const std::vector<char>& body) {
      put_tag(out, field, 2);
      put_varint(out, body.size());
      out.insert(out.end(), body.begin(), body.end());
   }

   inline std::vector<char> encode(const sysio::opp::types::ChainId& m) {
      std::vector<char> out;
      put_varint_field(out, 1, magic_enum::enum_integer(m.kind()));
      put_varint_field(out, 2, m.id());
      return out;
   }

   inline std::vector<char> encode(const sysio::opp::Endpoints& m) {
      std::vector<char> out;
      put_submessage(out, 1, encode(m.start()));
      put_submessage(out, 2, encode(m.end()));
      return out;
   }

   inline std::vector<char> encode(const sysio::opp::MessageHeader& m) {
      std::vector<char> out;
      put_submessage(out, 1, encode(m.endpoints()));
      put_bytes_field(out, 2, m.message_id());
      put_bytes_field(out, 3, m.previous_message_id());
      // Slot 4 was `encoding_flags` — removed from opp.proto; reserved, do not reuse.
      put_varint_field(out, 5, m.payload_size());
      put_bytes_field(out, 6, m.payload_checksum());
      put_varint_field(out, 7, m.timestamp());
      put_bytes_field(out, 8, m.header_checksum());
      return out;
   }

   inline std::vector<char> encode(const sysio::opp::AttestationEntry& m) {
      std::vector<char> out;
      put_varint_field(out, 1, magic_enum::enum_integer(m.type()));
      put_varint_field(out, 2, m.data_size());
      put_bytes_field(out, 3, m.data());
      return out;
   }

   inline std::vector<char> encode(const sysio::opp::MessagePayload& m) {
      std::vector<char> out;
      put_varint_field(out, 1, m.version());
      for (const auto& a : m.attestations())
         put_submessage(out, 2, encode(a));
      return out;
   }

   inline std::vector<char> encode(const sysio::opp::Message& m) {
      std::vector<char> out;
      put_submessage(out, 1, encode(m.header()));
      put_submessage(out, 2, encode(m.payload()));
      return out;
   }

   inline std::vector<char> encode(const sysio::opp::Envelope& m, bool blank_envelope_hash = false) {
      std::vector<char> out;
      put_bytes_field(out, 1, blank_envelope_hash ? std::string{} : m.envelope_hash());
      put_submessage(out, 2, encode(m.endpoints()));
      put_varint_field(out, 5, m.epoch_timestamp());
      put_varint_field(out, 6, m.epoch_index());
      put_varint_field(out, 7, m.epoch_envelope_index());
      put_bytes_field(out, 20, m.previous_envelope_hash());
      for (const auto& msg : m.messages())
         put_submessage(out, 40, encode(msg));
      return out;
   }

   /// keccak256 over the canonical encoding with `envelope_hash` blanked: the cross-chain epoch
   /// digest (`OPPCommon.epochHash` on the outposts, `opp::canonical::epoch_digest` in the depot).
   inline fc::crypto::keccak256 epoch_digest(const sysio::opp::Envelope& env) {
      const auto preimage = encode(env, /*blank_envelope_hash=*/true);
      return fc::crypto::keccak256::hash(std::span<const uint8_t>(
         reinterpret_cast<const uint8_t*>(preimage.data()), preimage.size()));
   }

   /// The digest as the raw 32-byte string a protobuf `bytes` field carries.
   inline std::string digest_bytes(const fc::crypto::keccak256& d) {
      return std::string(reinterpret_cast<const char*>(d.data()), d.data_size());
   }

   inline fc::crypto::keccak256 keccak_of(const std::vector<char>& bytes) {
      return fc::crypto::keccak256::hash(std::span<const uint8_t>(
         reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
   }

   /// The spec derivation of `MessageHeader.header_checksum`: keccak256 over the canonical
   /// header with `message_id` and `header_checksum` blanked (mirrors
   /// `opp::canonical::header_digest` and Solidity `OPPCommon.headerChecksum`).
   inline fc::crypto::keccak256 header_checksum(const sysio::opp::MessageHeader& header) {
      sysio::opp::MessageHeader blanked = header;
      blanked.set_message_id("");
      blanked.set_header_checksum("");
      return keccak_of(encode(blanked));
   }

   /// The spec derivation of `MessageHeader.message_id`: the header checksum with its first 8
   /// bytes replaced by the message's big-endian sequence number (mirrors
   /// `opp::canonical::derive_message_id` and Solidity `OPPCommon.getMessageID`).
   inline std::string derive_message_id(const fc::crypto::keccak256& checksum, uint64_t sequence) {
      std::string id = digest_bytes(checksum);
      for (size_t i = 0; i < 8; ++i) {
         id[i] = static_cast<char>(static_cast<uint8_t>(sequence >> (8 * (7 - i))));
      }
      return id;
   }

   /// Big-endian sequence number in the first 8 bytes of a wire message id; 0 when empty
   /// (stream genesis) or out-of-spec short.
   inline uint64_t message_sequence(const std::string& message_id) {
      if (message_id.size() < 8) {
         return 0;
      }
      uint64_t sequence = 0;
      for (size_t i = 0; i < 8; ++i) {
         sequence = (sequence << 8) | static_cast<uint8_t>(message_id[i]);
      }
      return sequence;
   }

   /// Populate `msg`'s semantic header per the spec derivation from its payload and stream
   /// predecessor: `payload_size` / `payload_checksum` over the payload's canonical bytes, then
   /// `header_checksum` over the blanked header, then `message_id` at the predecessor's sequence
   /// number + 1. Mirrors what `sysio.msgch::buildenv` derives on emit.
   inline void finalize_header(sysio::opp::Message& msg, const std::string& prev_message_id,
                        uint64_t timestamp_ms) {
      auto* header = msg.mutable_header();
      header->set_previous_message_id(prev_message_id);
      const auto payload_bytes = encode(msg.payload());
      header->set_payload_size(static_cast<uint32_t>(payload_bytes.size()));
      header->set_payload_checksum(digest_bytes(keccak_of(payload_bytes)));
      header->set_timestamp(timestamp_ms);
      const auto checksum = header_checksum(*header);
      header->set_header_checksum(digest_bytes(checksum));
      header->set_message_id(derive_message_id(checksum, message_sequence(prev_message_id) + 1));
   }

} // namespace oracle
