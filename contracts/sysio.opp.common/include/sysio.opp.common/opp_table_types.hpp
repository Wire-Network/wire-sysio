#pragma once
/**
 * @file opp_table_types.hpp
 * @brief CDT multi_index serialization wrappers for zpp_bits protobuf types.
 *
 * The generated CDT protobuf headers use `zpp::bits::pb_members<N>` for wire-format
 * serialization but lack `SYSLIB_SERIALIZE` / DataStream operators required by CDT
 * multi_index tables and action argument serialization.
 *
 * This header provides non-member DataStream operators for all OPP protobuf types
 * used in contract tables and actions. It must be included in any contract that
 * stores protobuf types in tables.
 *
 * NOTE: Generated headers are overwritten on every CMake build. Non-member
 * operators here survive regeneration since all struct members are public.
 */

#include <sysio/serialize.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio/opp/opp.pb.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>

// ─────────────────────────────────────────────────────────────────────────────
//  zpp::bits varint types — CDT DataStream operators
//
//  varint<T> has implicit conversion to/from T, so ds << varint_val resolves
//  to ds << T via implicit conversion. However, ds >> varint_val needs an
//  explicit overload because the implicit conversion returns T& not varint&.
// ─────────────────────────────────────────────────────────────────────────────
namespace zpp::bits {

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const vuint32_t& v) {
   return ds << static_cast<uint32_t>(v);
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, vuint32_t& v) {
   uint32_t tmp;
   ds >> tmp;
   v = vuint32_t{tmp};
   return ds;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const vint64_t& v) {
   return ds << static_cast<int64_t>(v);
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, vint64_t& v) {
   int64_t tmp;
   ds >> tmp;
   v = vint64_t{tmp};
   return ds;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const vuint64_t& v) {
   return ds << static_cast<uint64_t>(v);
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, vuint64_t& v) {
   uint64_t tmp;
   ds >> tmp;
   v = vuint64_t{tmp};
   return ds;
}

} // namespace zpp::bits

// ─────────────────────────────────────────────────────────────────────────────
//  sysio::opp::types — CDT DataStream operators for protobuf struct types
// ─────────────────────────────────────────────────────────────────────────────
namespace sysio::opp::types {

// ChainId: { ChainKind kind; vuint32_t id; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ChainId& t) {
   return ds << t.kind << t.id;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ChainId& t) {
   return ds >> t.kind >> t.id;
}

// TokenAmount: { TokenKind kind; vint64_t amount; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const TokenAmount& t) {
   return ds << t.kind << t.amount;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, TokenAmount& t) {
   return ds >> t.kind >> t.amount;
}

// ChainAddress: { ChainKind kind; vector<char> address; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ChainAddress& t) {
   return ds << t.kind << t.address;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ChainAddress& t) {
   return ds >> t.kind >> t.address;
}

// ChainSignature: { ChainAddress actor; ChainKeyType key_type; vector<char> signature; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ChainSignature& t) {
   return ds << t.actor << t.key_type << t.signature;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ChainSignature& t) {
   return ds >> t.actor >> t.key_type >> t.signature;
}

// WireAccount: { string name; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const WireAccount& t) {
   return ds << t.name;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, WireAccount& t) {
   return ds >> t.name;
}

// WirePermission: { WireAccount account; string permission; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const WirePermission& t) {
   return ds << t.account << t.permission;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, WirePermission& t) {
   return ds >> t.account >> t.permission;
}

// EncodingFlags: { Endianness; HashAlgorithm; LengthEncoding; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const EncodingFlags& t) {
   return ds << t.endianness << t.hash_algorithm << t.length_encoding;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, EncodingFlags& t) {
   return ds >> t.endianness >> t.hash_algorithm >> t.length_encoding;
}

} // namespace sysio::opp::types

// ─────────────────────────────────────────────────────────────────────────────
//  sysio::opp — CDT DataStream operators for OPP message/envelope types
// ─────────────────────────────────────────────────────────────────────────────
namespace sysio::opp {

// Endpoints: { ChainId start; ChainId end; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Endpoints& t) {
   return ds << t.start << t.end;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Endpoints& t) {
   return ds >> t.start >> t.end;
}

// AttestationEntry: { AttestationType type; vuint32_t data_size; vector<char> data; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const AttestationEntry& t) {
   return ds << t.type << t.data_size << t.data;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, AttestationEntry& t) {
   return ds >> t.type >> t.data_size >> t.data;
}

// MessagePayload: { vuint32_t version; vector<AttestationEntry> attestations; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const MessagePayload& t) {
   return ds << t.version << t.attestations;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, MessagePayload& t) {
   return ds >> t.version >> t.attestations;
}

// MessageHeader: all 8 fields
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const MessageHeader& t) {
   return ds << t.endpoints << t.message_id << t.previous_message_id
             << t.encoding_flags << t.payload_size << t.payload_checksum
             << t.timestamp << t.header_checksum;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, MessageHeader& t) {
   return ds >> t.endpoints >> t.message_id >> t.previous_message_id
             >> t.encoding_flags >> t.payload_size >> t.payload_checksum
             >> t.timestamp >> t.header_checksum;
}

// Message: { MessageHeader header; MessagePayload payload; }
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Message& t) {
   return ds << t.header << t.payload;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Message& t) {
   return ds >> t.header >> t.payload;
}

// Envelope: all fields (signatures field removed per protocol spec)
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Envelope& t) {
   return ds << t.envelope_hash << t.endpoints << t.epoch_timestamp
             << t.epoch_index << t.epoch_envelope_index << t.merkle
             << t.previous_envelope_hash << t.start_message_id
             << t.end_message_id;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Envelope& t) {
   return ds >> t.envelope_hash >> t.endpoints >> t.epoch_timestamp
             >> t.epoch_index >> t.epoch_envelope_index >> t.merkle
             >> t.previous_envelope_hash >> t.start_message_id
             >> t.end_message_id;
}

} // namespace sysio::opp

// ─────────────────────────────────────────────────────────────────────────────
//  Contract-local types with SYSLIB_SERIALIZE for multi_index table storage
// ─────────────────────────────────────────────────────────────────────────────
namespace sysio::opp_table {

/// Locked amount entry for underwrite requests (uwreqs table).
/// Uses zpp protobuf types directly with CDT serialization via the
/// DataStream operators defined above.
struct locked_amount_t {
   opp::types::ChainId     chain_id;
   opp::types::TokenAmount amount;
   uint128_t               lock_id        = 0;
   uint64_t                lock_timestamp = 0;

   SYSLIB_SERIALIZE(locked_amount_t,
      (chain_id)(amount)(lock_id)(lock_timestamp))
};

} // namespace sysio::opp_table
