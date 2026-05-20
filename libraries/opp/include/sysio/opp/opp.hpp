#pragma once
#pragma GCC diagnostic ignored "-Wswitch"

/**
 * @file opp.hpp
 * @brief OPP library header — FC reflection for host-side protobuf enums.
 *
 * Provides FC_REFLECT_ENUM for all OPP protobuf enum types so they work with
 * fc::variant serialization, ABI serializer, and the testing framework.
 *
 * Used by: contract unit tests, batch_operator_plugin, underwriter_plugin,
 * and any host-side code that needs to serialize OPP enum values.
 */

#include <sysio/opp/debugging/debugging.pb.h>
#include <sysio/opp/types/types.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <fc/reflect/reflect.hpp>

// ---------------------------------------------------------------------------
//  Chain identity enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::ChainKind,
   (CHAIN_KIND_UNKNOWN)
   (CHAIN_KIND_WIRE)
   (CHAIN_KIND_EVM)
   (CHAIN_KIND_SVM))

FC_REFLECT_ENUM(sysio::opp::types::ChainKeyType,
   (CHAIN_KEY_TYPE_UNKNOWN)
   (CHAIN_KEY_TYPE_WIRE)
   (CHAIN_KEY_TYPE_WIRE_BLS)
   (CHAIN_KEY_TYPE_ETHEREUM)
   (CHAIN_KEY_TYPE_SOLANA)
   (CHAIN_KEY_TYPE_SUI))

// ---------------------------------------------------------------------------
//  Operator enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::OperatorType,
   (OPERATOR_TYPE_UNKNOWN)
   (OPERATOR_TYPE_PRODUCER)
   (OPERATOR_TYPE_BATCH)
   (OPERATOR_TYPE_UNDERWRITER)
   (OPERATOR_TYPE_CHALLENGER))

FC_REFLECT_ENUM(sysio::opp::types::OperatorStatus,
   (OPERATOR_STATUS_UNKNOWN)
   (OPERATOR_STATUS_WARMUP)
   (OPERATOR_STATUS_COOLDOWN)
   (OPERATOR_STATUS_ACTIVE)
   (OPERATOR_STATUS_TERMINATED)
   (OPERATOR_STATUS_SLASHED))

// ---------------------------------------------------------------------------
//  Token enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::TokenKind,
   (TOKEN_KIND_UNKNOWN)
   (TOKEN_KIND_NATIVE)
   (TOKEN_KIND_ERC20)
   (TOKEN_KIND_ERC721)
   (TOKEN_KIND_ERC1155)
   (TOKEN_KIND_SPL)
   (TOKEN_KIND_SPL_NFT)
   (TOKEN_KIND_LIQ))

// v6 — Reserve lifecycle
FC_REFLECT_ENUM(sysio::opp::types::ReserveStatus,
   (RESERVE_STATUS_UNKNOWN)
   (RESERVE_STATUS_PENDING)
   (RESERVE_STATUS_ACTIVE)
   (RESERVE_STATUS_CANCELLED))

// ---------------------------------------------------------------------------
//  Attestation enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::AttestationType,
   (ATTESTATION_TYPE_UNSPECIFIED)
   (ATTESTATION_TYPE_OPERATOR_ACTION)
   (ATTESTATION_TYPE_STAKE)
   (ATTESTATION_TYPE_UNSTAKE)
   (ATTESTATION_TYPE_PRETOKEN_PURCHASE)
   (ATTESTATION_TYPE_PRETOKEN_YIELD)
   (ATTESTATION_TYPE_RESERVE_BALANCE_SHEET)
   (ATTESTATION_TYPE_STAKE_UPDATE)
   (ATTESTATION_TYPE_WIRE_TOKEN_PURCHASE)
   (ATTESTATION_TYPE_CHALLENGE_RESPONSE)
   (ATTESTATION_TYPE_SWAP_REQUEST)
   (ATTESTATION_TYPE_SWAP_REMIT)
   (ATTESTATION_TYPE_CHALLENGE_REQUEST)
   (ATTESTATION_TYPE_OPERATORS)
   (ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS)
   (ATTESTATION_TYPE_NODE_OWNER_REG)
   (ATTESTATION_TYPE_STAKING_REWARD)
   (ATTESTATION_TYPE_STAKE_RESULT)
   (ATTESTATION_TYPE_ATTESTATION_PROCESSING_ERROR)
   (ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT)
   (ATTESTATION_TYPE_SWAP_REVERT)
   (ATTESTATION_TYPE_DEPOSIT_REVERT)
   (ATTESTATION_TYPE_SWAP_REJECTED)
   (ATTESTATION_TYPE_RESERVE_CREATE)
   (ATTESTATION_TYPE_RESERVE_CREATE_CANCEL)
   (ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED)
   (ATTESTATION_TYPE_RESERVE_READY))

// ---------------------------------------------------------------------------
//  Nested enums on attestation messages
//
//  protoc-cpp prefixes nested-enum values with the enum type name (e.g.
//  `OperatorAction_ActionType_ACTION_TYPE_UNKNOWN`), so the reflection uses
//  the full prefixed identifier. The string form on `to_string` will carry
//  the prefix too — JSON consumers that want the bare proto value name
//  (e.g. "ACTION_TYPE_SLASH") should strip the `OperatorAction_ActionType_`
//  prefix at their boundary.
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::attestations::OperatorAction_ActionType,
   (OperatorAction_ActionType_ACTION_TYPE_UNKNOWN)
   (OperatorAction_ActionType_ACTION_TYPE_DEPOSIT_REQUEST)
   (OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REQUEST)
   (OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT)
   (OperatorAction_ActionType_ACTION_TYPE_SLASH))

// ReserveTarget_Kind removed in v6 — ReserveTarget is now (chain_code, reserve_code, TokenAmount).

FC_REFLECT_ENUM(sysio::opp::types::AttestationStatus,
   (ATTESTATION_STATUS_PENDING)
   (ATTESTATION_STATUS_READY)
   (ATTESTATION_STATUS_PROCESSED))

// ---------------------------------------------------------------------------
//  Message channel / depot enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::ChainRequestStatus,
   (CHAIN_REQUEST_STATUS_PENDING)
   (CHAIN_REQUEST_STATUS_COLLECTING)
   (CHAIN_REQUEST_STATUS_CONSENSUS_OK)
   (CHAIN_REQUEST_STATUS_CONSENSUS_FAIL)
   (CHAIN_REQUEST_STATUS_CHALLENGED))

FC_REFLECT_ENUM(sysio::opp::types::MessageDirection,
   (MESSAGE_DIRECTION_INBOUND)
   (MESSAGE_DIRECTION_OUTBOUND))

FC_REFLECT_ENUM(sysio::opp::types::MessageStatus,
   (MESSAGE_STATUS_PENDING)
   (MESSAGE_STATUS_READY)
   (MESSAGE_STATUS_PROCESSED)
   (MESSAGE_STATUS_CANCELLED))

FC_REFLECT_ENUM(sysio::opp::types::EnvelopeStatus,
   (ENVELOPE_STATUS_PENDING_DELIVERY)
   (ENVELOPE_STATUS_DELIVERED)
   (ENVELOPE_STATUS_CONFIRMED))

// ---------------------------------------------------------------------------
//  Underwriting enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::UnderwriteRequestStatus,
   (UNDERWRITE_REQUEST_STATUS_PENDING)
   (UNDERWRITE_REQUEST_STATUS_CONFIRMED)
   (UNDERWRITE_REQUEST_STATUS_REJECTED)
   (UNDERWRITE_REQUEST_STATUS_COMPLETED)
   (UNDERWRITE_REQUEST_STATUS_EXPIRED))

FC_REFLECT_ENUM(sysio::opp::types::UnderwriteStatus,
   (UNDERWRITE_STATUS_INTENT_CREATED)
   (UNDERWRITE_STATUS_INTENT_SUBMITTED)
   (UNDERWRITE_STATUS_INTENT_CONFIRMED)
   (UNDERWRITE_STATUS_READY)
   (UNDERWRITE_STATUS_RELEASED)
   (UNDERWRITE_STATUS_SLASHED))

FC_REFLECT_ENUM(sysio::opp::types::ChallengeStatus,
   (CHALLENGE_STATUS_CHALLENGE_SENT)
   (CHALLENGE_STATUS_RESPONSE_RECEIVED)
   (CHALLENGE_STATUS_RESOLVED)
   (CHALLENGE_STATUS_ESCALATED))

// ---------------------------------------------------------------------------
//  Encoding enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::Endianness,
   (ENDIANNESS_BIG)
   (ENDIANNESS_LITTLE))

FC_REFLECT_ENUM(sysio::opp::types::HashAlgorithm,
   (HASH_ALGORITHM_KECCAK256)
   (HASH_ALGORITHM_SHA256)
   (HASH_ALGORITHM_RESERVED_1)
   (HASH_ALGORITHM_RESERVED_2))

FC_REFLECT_ENUM(sysio::opp::types::LengthEncoding,
   (LENGTH_ENCODING_VARUINT)
   (LENGTH_ENCODING_UINT32))

FC_REFLECT_ENUM(sysio::opp::types::StakeStatus,
   (STAKE_STATUS_UNKNOWN)
   (STAKE_STATUS_WARMUP)
   (STAKE_STATUS_COOLDOWN)
   (STAKE_STATUS_ACTIVE)
   (STAKE_STATUS_TERMINATED)
   (STAKE_STATUS_SLASHED))

FC_REFLECT_ENUM(sysio::opp::debugging::DebugOutpostEndpointsType,
   (DEBUG_OUTPOST_ENDPOINTS_TYPE_UNKNOWN)
   (DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT)
   (DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT)
   (DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM)
   (DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA));

namespace sysio::opp {
   struct opp {};
}
