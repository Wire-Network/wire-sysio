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

#include <sysio/opp/types/types.pb.h>
#include <fc/reflect/reflect.hpp>

// ---------------------------------------------------------------------------
//  Chain identity enums
// ---------------------------------------------------------------------------

FC_REFLECT_ENUM(sysio::opp::types::ChainKind,
   (CHAIN_KIND_UNKNOWN)
   (CHAIN_KIND_WIRE)
   (CHAIN_KIND_ETHEREUM)
   (CHAIN_KIND_SOLANA)
   (CHAIN_KIND_SUI))

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
   (TOKEN_KIND_WIRE)
   (TOKEN_KIND_ETH)
   (TOKEN_KIND_ERC20)
   (TOKEN_KIND_ERC721)
   (TOKEN_KIND_ERC1155)
   (TOKEN_KIND_LIQETH)
   (TOKEN_KIND_SOL)
   (TOKEN_KIND_LIQSOL))

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
   (ATTESTATION_TYPE_NATIVE_YIELD_REWARD)
   (ATTESTATION_TYPE_WIRE_TOKEN_PURCHASE)
   (ATTESTATION_TYPE_CHALLENGE_RESPONSE)
   (ATTESTATION_TYPE_SLASH_OPERATOR)
   (ATTESTATION_TYPE_SWAP)
   (ATTESTATION_TYPE_UNDERWRITE_INTENT)
   (ATTESTATION_TYPE_UNDERWRITE_CONFIRM)
   (ATTESTATION_TYPE_UNDERWRITE_REJECT)
   (ATTESTATION_TYPE_UNDERWRITE_UNLOCK)
   (ATTESTATION_TYPE_REMIT)
   (ATTESTATION_TYPE_CHALLENGE_REQUEST)
   (ATTESTATION_TYPE_EPOCH_SYNC)
   (ATTESTATION_TYPE_ROSTER_UPDATE)
   (ATTESTATION_TYPE_REMIT_CONFIRM)
   (ATTESTATION_TYPE_BATCH_OPERATOR_NEXT_GROUP))

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

namespace sysio::opp {
   struct opp {};
}
