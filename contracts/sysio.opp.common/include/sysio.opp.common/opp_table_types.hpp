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

// TokenAmount: { uint64 token_code; vint64_t amount; }  (v6 — codename-keyed)
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

// ---------------------------------------------------------------------------
//  v6 registry-entity messages (Chain / Token / ChainToken / Reserve / ReserveAmount)
// ---------------------------------------------------------------------------

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

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const Reserve& t) {
   return ds << t.chain_code << t.token_code << t.code << t.name << t.description
             << t.status << t.reserve_chain_amount << t.reserve_wire_amount
             << t.connector_weight_bps << t.creator_addr
             << t.requested_wire_amount << t.external_token_amount
             << t.registered_at_ms << t.activated_at_ms << t.cancelled_at_ms;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, Reserve& t) {
   return ds >> t.chain_code >> t.token_code >> t.code >> t.name >> t.description
             >> t.status >> t.reserve_chain_amount >> t.reserve_wire_amount
             >> t.connector_weight_bps >> t.creator_addr
             >> t.requested_wire_amount >> t.external_token_amount
             >> t.registered_at_ms >> t.activated_at_ms >> t.cancelled_at_ms;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveAmount& t) {
   return ds << t.chain_code << t.reserve_code << t.amount;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveAmount& t) {
   return ds >> t.chain_code >> t.reserve_code >> t.amount;
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

// ReserveBalanceSheet (v6, renamed from ChainReserveBalanceSheet; the
// parallel amounts[]/reserve_codes[] arrays were replaced by
// self-describing depot-frame `ReserveAmount` entries).
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveBalanceSheet& t) {
   return ds << t.chain_code << t.reserves;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveBalanceSheet& t) {
   return ds >> t.chain_code >> t.reserves;
}

// PretokenStakeChange (deprecated; pre-launch only)
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const PretokenStakeChange& t) {
   return ds << t.actor << t.amount << t.index_at_mint << t.index_at_burn;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, PretokenStakeChange& t) {
   return ds >> t.actor >> t.amount >> t.index_at_mint >> t.index_at_burn;
}

// PretokenPurchase (deprecated; pre-launch only)
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const PretokenPurchase& t) {
   return ds << t.actor << t.amount << t.pretoken_count << t.index_at_mint;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, PretokenPurchase& t) {
   return ds >> t.actor >> t.amount >> t.pretoken_count >> t.index_at_mint;
}

// PretokenYield (deprecated; pre-launch only)
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const PretokenYield& t) {
   return ds << t.actor << t.amount << t.index_at_mint;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, PretokenYield& t) {
   return ds >> t.actor >> t.amount >> t.index_at_mint;
}

// StakeUpdate
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const StakeUpdate& t) {
   return ds << t.actor << t.status << t.amount;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, StakeUpdate& t) {
   return ds >> t.actor >> t.status >> t.amount;
}

// WireTokenPurchase
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const WireTokenPurchase& t) {
   return ds << t.actor << t.amounts;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, WireTokenPurchase& t) {
   return ds >> t.actor >> t.amounts;
}

// OperatorAction — v6: chain_code (codename uint64), and SLASH carries reserve_code.
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

// ReserveDisbursement
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveDisbursement& t) {
   return ds << t.actor << t.amount << t.signature;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveDisbursement& t) {
   return ds >> t.actor >> t.amount >> t.signature;
}

// ProtocolState — v6: chain_code (codename uint64) replaces ChainId chain_id.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ProtocolState& t) {
   return ds << t.chain_code << t.current_message_id << t.processed_message_id
             << t.incoming_messages << t.outgoing_messages;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ProtocolState& t) {
   return ds >> t.chain_code >> t.current_message_id >> t.processed_message_id
             >> t.incoming_messages >> t.outgoing_messages;
}

// SwapRequest — v6: full codename triples for source + target.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const SwapRequest& t) {
   return ds << t.actor << t.source_amount
             << t.source_chain_code << t.source_reserve_code
             << t.target_chain_code << t.target_token_code << t.target_reserve_code
             << t.recipient << t.target_amount
             << t.target_tolerance_bps << t.target_timestamp_ms
             << t.source_tx_id;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, SwapRequest& t) {
   return ds >> t.actor >> t.source_amount
             >> t.source_chain_code >> t.source_reserve_code
             >> t.target_chain_code >> t.target_token_code >> t.target_reserve_code
             >> t.recipient >> t.target_amount
             >> t.target_tolerance_bps >> t.target_timestamp_ms
             >> t.source_tx_id;
}

// UnderwriteIntentCommit — v6: (token_code, chain_code, reserve_code) triple
// disambiguates same-chain swap legs. Field order MUST match the generated proto struct
// (attestations.proto): uw_account, uw_ext_chain_addr, uw_request_id, signature,
// token_code, chain_code, reserve_code. (The legacy v5 `outpost_id` field — a per-outpost
// numeric id redundant with `chain_code` and read by no one — was removed in SEC-13/WSA-027.)
// These operators are currently unreached (UIC always travels as raw zpp::bits proto bytes).
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const UnderwriteIntentCommit& t) {
   return ds << t.uw_account << t.uw_ext_chain_addr << t.uw_request_id
             << t.signature
             << t.token_code << t.chain_code << t.reserve_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, UnderwriteIntentCommit& t) {
   return ds >> t.uw_account >> t.uw_ext_chain_addr >> t.uw_request_id
             >> t.signature
             >> t.token_code >> t.chain_code >> t.reserve_code;
}

// SwapRevert — v6 adds source_chain_code + source_reserve_code.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const SwapRevert& t) {
   return ds << t.original_swap_message_id << t.depositor
             << t.refund_amount << t.reason
             << t.source_chain_code << t.source_reserve_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, SwapRevert& t) {
   return ds >> t.original_swap_message_id >> t.depositor
             >> t.refund_amount >> t.reason
             >> t.source_chain_code >> t.source_reserve_code;
}

// SwapRemit — v6: adds chain_code + reserve_code (destination identity).
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const SwapRemit& t) {
   return ds << t.recipient << t.amount << t.original_message_id
             << t.underwriter << t.unlock_timestamp
             << t.chain_code << t.reserve_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, SwapRemit& t) {
   return ds >> t.recipient >> t.amount >> t.original_message_id
             >> t.underwriter >> t.unlock_timestamp
             >> t.chain_code >> t.reserve_code;
}

// SwapRejected DataStream operators removed — the SwapRejected message no longer
// exists (no post-underwriting rejection path; every REMIT is depot-initiated).

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

// ReserveAmount — v6: (chain_code, reserve_code, TokenAmount), depot-frame.
// (NOTE: ReserveAmount lives in `sysio::opp::types` per v6 types.proto;
//  the DataStream overloads are above in the types namespace.)

// DepositRevert — v6: adds chain_code.
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const DepositRevert& t) {
   return ds << t.original_deposit_message_id << t.depositor
             << t.refund_amount << t.reason << t.chain_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, DepositRevert& t) {
   return ds >> t.original_deposit_message_id >> t.depositor
             >> t.refund_amount >> t.reason >> t.chain_code;
}

// NodeOwnerReg
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const NodeOwnerReg& t) {
   return ds << t.owner_address << t.token_id << t.nft_address;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, NodeOwnerReg& t) {
   return ds >> t.owner_address >> t.token_id >> t.nft_address;
}

// StakingReward: the single staker-reward feedback path. `sysio.msgch`
// dispatches the per-staker body to `sysio.dclaim::onreward` (the WIRE-side
// claim ledger); the amount is already WIRE-denominated, so there is no
// reserve leg. Field order mirrors the proto (1..7).
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const StakingReward& t) {
   return ds << t.chain_code << t.staker_wire_account << t.share_bps
             << t.reward_epoch_index << t.external_epoch_ref
             << t.reward_amount << t.staker_native_address;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, StakingReward& t) {
   return ds >> t.chain_code >> t.staker_wire_account >> t.share_bps
             >> t.reward_epoch_index >> t.external_epoch_ref
             >> t.reward_amount >> t.staker_native_address;
}

// ---------------------------------------------------------------------------
//  v6 reserve-flow attestations
// ---------------------------------------------------------------------------

// ReserveCreate — reserve identity + custodied amount travel together in
// `external_amount` (a depot-frame `ReserveAmount`); field order mirrors
// the generated struct (proto fields 4,5,7,8,9,10,11,12).
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveCreate& t) {
   return ds << t.name << t.description
             << t.requested_wire_amount << t.connector_weight_bps
             << t.creator_addr << t.is_private << t.creator_pub_key
             << t.external_amount;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveCreate& t) {
   return ds >> t.name >> t.description
             >> t.requested_wire_amount >> t.connector_weight_bps
             >> t.creator_addr >> t.is_private >> t.creator_pub_key
             >> t.external_amount;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveCreateCancel& t) {
   return ds << t.chain_code << t.token_code << t.reserve_code << t.creator_addr;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveCreateCancel& t) {
   return ds >> t.chain_code >> t.token_code >> t.reserve_code >> t.creator_addr;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveCreateCancelled& t) {
   return ds << t.chain_code << t.token_code << t.reserve_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveCreateCancelled& t) {
   return ds >> t.chain_code >> t.token_code >> t.reserve_code;
}

template <typename DataStream>
DataStream& operator<<(DataStream& ds, const ReserveReady& t) {
   return ds << t.chain_code << t.token_code << t.reserve_code;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, ReserveReady& t) {
   return ds >> t.chain_code >> t.token_code >> t.reserve_code;
}

// StakeResult
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const StakeResult& t) {
   return ds << t.owner_address << t.amount << t.success << t.error_reason;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, StakeResult& t) {
   return ds >> t.owner_address >> t.amount >> t.success >> t.error_reason;
}

// AttestationProcessingError
template <typename DataStream>
DataStream& operator<<(DataStream& ds, const AttestationProcessingError& t) {
   return ds << t.attestation_id << t.original_type << t.original_data
             << t.error_message;
}
template <typename DataStream>
DataStream& operator>>(DataStream& ds, AttestationProcessingError& t) {
   return ds >> t.attestation_id >> t.original_type >> t.original_data
             >> t.error_message;
}

} // namespace sysio::opp::attestations

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
