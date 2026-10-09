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

// TokenAmount: { uint64 token_code; vint64_t amount; }  (codename-keyed)
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const TokenAmount& t) {
   return ds << t.token_code << t.amount;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, TokenAmount& t) {
   return ds >> t.token_code >> t.amount;
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

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Chain& t) {
   return ds << t.kind << t.code << t.external_chain_id << t.name << t.description
             << t.is_depot << t.active << t.registered_at_ms << t.activated_at_ms;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Chain& t) {
   return ds >> t.kind >> t.code >> t.external_chain_id >> t.name >> t.description
             >> t.is_depot >> t.active >> t.registered_at_ms >> t.activated_at_ms;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Token& t) {
   return ds << t.kind << t.code << t.symbol_name << t.description << t.precision
             << t.address << t.active << t.registered_at_ms << t.activated_at_ms;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Token& t) {
   return ds >> t.kind >> t.code >> t.symbol_name >> t.description >> t.precision
             >> t.address >> t.active >> t.registered_at_ms >> t.activated_at_ms;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ChainToken& t) {
   return ds << t.chain_code << t.token_code << t.contract_addr
             << t.is_native << t.active << t.registered_at_ms << t.activated_at_ms;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ChainToken& t) {
   return ds >> t.chain_code >> t.token_code >> t.contract_addr
             >> t.is_native >> t.active >> t.registered_at_ms >> t.activated_at_ms;
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

// MessageHeader: all 7 fields
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const MessageHeader& t) {
   return ds << t.endpoints << t.message_id << t.previous_message_id
             << t.payload_size << t.payload_checksum
             << t.timestamp << t.header_checksum;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, MessageHeader& t) {
   return ds >> t.endpoints >> t.message_id >> t.previous_message_id
             >> t.payload_size >> t.payload_checksum
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

// Envelope: inline-consensus fields. Legacy merkle/range metadata was removed
// from the schema and is not stored in CDT table serialization.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Envelope& t) {
   return ds << t.envelope_hash << t.endpoints << t.epoch_timestamp
             << t.epoch_index << t.epoch_envelope_index
             << t.previous_envelope_hash << t.messages;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Envelope& t) {
   return ds >> t.envelope_hash >> t.endpoints >> t.epoch_timestamp
             >> t.epoch_index >> t.epoch_envelope_index
             >> t.previous_envelope_hash >> t.messages;
}

} // namespace sysio::opp

// ─────────────────────────────────────────────────────────────────────────────
//  sysio::opp::attestations — CDT DataStream operators for every attestation
//                              message type. Required so contracts can store
//                              these directly in `kv::table` rows or pass them
//                              as action arguments (e.g.
//                              `sysio.opreg::operator_entry.recent_actions`
//                              holds `OperatorActionLog` values).
//
//  Generated proto fields use `zpp::bits::vuint*_t` / `vint*_t` for varints;
//  the varint DataStream overloads above bridge them.
// ─────────────────────────────────────────────────────────────────────────────
namespace sysio::opp::attestations {

// OperatorAction — chain_code (codename uint64); reserve_code is historical and left zero.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const OperatorAction& t) {
   return ds << t.action_type << t.op_address << t.type << t.status
             << t.amount << t.request_id << t.chain_code << t.reason << t.reserve_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, OperatorAction& t) {
   return ds >> t.action_type >> t.op_address >> t.type >> t.status
             >> t.amount >> t.request_id >> t.chain_code >> t.reason >> t.reserve_code;
}

// OperatorActionLog — stored in sysio.opreg::operator_entry.recent_actions.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const OperatorActionLog& t) {
   return ds << t.action << t.success << t.timestamp << t.error_message;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, OperatorActionLog& t) {
   return ds >> t.action >> t.success >> t.timestamp >> t.error_message;
}

// ChallengeOperatorHash — field name `operator_` (trailing underscore) because
// `operator` is a C++ keyword.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ChallengeOperatorHash& t) {
   return ds << t.operator_ << t.chain_hash;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ChallengeOperatorHash& t) {
   return ds >> t.operator_ >> t.chain_hash;
}

// ChallengeRequest
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ChallengeRequest& t) {
   return ds << t.epoch_index << t.round << t.original_chain_hash
             << t.operator_hashes;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ChallengeRequest& t) {
   return ds >> t.epoch_index >> t.round >> t.original_chain_hash
             >> t.operator_hashes;
}

// OperatorEntry — one row of the OPERATORS attestation roster.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const OperatorEntry& t) {
   return ds << t.account << t.addresses << t.type << t.status;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, OperatorEntry& t) {
   return ds >> t.account >> t.addresses >> t.type >> t.status;
}

// Operators
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Operators& t) {
   return ds << t.operators;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Operators& t) {
   return ds >> t.operators;
}

// BatchOperatorGroup
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const BatchOperatorGroup& t) {
   return ds << t.operators;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, BatchOperatorGroup& t) {
   return ds >> t.operators;
}

// BatchOperatorGroups
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const BatchOperatorGroups& t) {
   return ds << t.active_group_index << t.epoch_index << t.groups;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, BatchOperatorGroups& t) {
   return ds >> t.active_group_index >> t.epoch_index >> t.groups;
}

} // namespace sysio::opp::attestations
