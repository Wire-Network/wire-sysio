#pragma once

#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>

namespace sysio {

   using fc::crypto::chain_kind_t;
   using fc::crypto::chain_key_type_t;

   // ── Message chain direction ──────────────────────────────────────────────
   enum message_direction_t : uint8_t {
      message_direction_inbound  = 0, // Outpost -> Depot
      message_direction_outbound = 1, // Depot -> Outpost
   };

   // ── Message chain lifecycle status ───────────────────────────────────────
   enum chain_status_t : uint8_t {
      chain_status_created    = 0,
      chain_status_pending    = 1,
      chain_status_challenged = 2,
      chain_status_valid      = 3,
      chain_status_slashed    = 4, // terminal
   };

   // ── Individual message processing status ─────────────────────────────────
   enum message_status_t : uint8_t {
      message_status_pending = 0, // needs underwriting
      message_status_ready   = 1, // ready for processing
   };

   // ── Operator type ────────────────────────────────────────────────────────
   enum operator_type_t : uint8_t {
      operator_type_node        = 0, // node / producer
      operator_type_batch       = 1,
      operator_type_underwriter = 2,
      operator_type_challenger  = 3,
   };

   // ── Operator lifecycle status ────────────────────────────────────────────
   enum operator_status_t : uint8_t {
      operator_status_warmup   = 0,
      operator_status_active   = 1,
      operator_status_cooldown = 2,
      operator_status_exited   = 3, // graceful exit after cooldown
      operator_status_slashed  = 4, // terminal, collateral collapsed
   };

   // ── Challenge status ─────────────────────────────────────────────────────
   enum challenge_status_t : uint8_t {
      challenge_status_none            = 0,
      challenge_status_round1_pending  = 1,
      challenge_status_round1_complete = 2,
      challenge_status_round2_pending  = 3,
      challenge_status_round2_complete = 4,
      challenge_status_paused          = 5, // global pause, awaiting manual
      challenge_status_resolved        = 6,
   };

   // ── Underwriting ledger entry status ─────────────────────────────────────
   enum underwrite_status_t : uint8_t {
      underwrite_status_intent_submitted = 0,
      underwrite_status_intent_confirmed = 1,
      underwrite_status_expired          = 2,
      underwrite_status_cancelled        = 3,
   };

   // ── Depot global state ───────────────────────────────────────────────────
   enum depot_state_t : uint8_t {
      depot_state_active    = 0,
      depot_state_challenge = 1, // normal msg processing suspended
      depot_state_paused    = 2, // global pause, manual intervention required
   };

   // ── OPP Assertion type IDs (from OPP Assertion Catalog) ──────────────────
   enum assertion_type_t : uint16_t {
      assertion_type_balance_sheet         = 0xAA00,
      assertion_type_stake_update          = 0xEE00,
      assertion_type_yield_reward          = 0xEE01,
      assertion_type_wire_purchase         = 0xEE02,
      assertion_type_operator_registration = 0xEE03,
      assertion_type_challenge_response    = 0xEE04,
      assertion_type_slash_operator        = 0xEE05,
   };

   // ── Constants ────────────────────────────────────────────────────────────
   static constexpr uint32_t MAX_BATCH_OPERATORS_PER_EPOCH = 7;
   static constexpr uint32_t TOTAL_BATCH_OPERATORS         = 21;
   static constexpr uint32_t CONSENSUS_MAJORITY            = 4; // of 7
   static constexpr uint32_t MAX_CHALLENGE_ROUNDS          = 2;

   static constexpr uint32_t INTENT_LOCK_SECONDS    = 6 * 3600;  // 6 hours
   static constexpr uint32_t CONFIRMED_LOCK_SECONDS  = 24 * 3600; // 24 hours

   static constexpr uint64_t UNDERWRITE_FEE_BPS = 10; // 0.1% = 10 basis points

   // Slash distribution (basis points out of 10 000)
   static constexpr uint64_t SLASH_CHALLENGER_BPS      = 5000; // 50 %
   static constexpr uint64_t SLASH_UNDERWRITERS_BPS    = 2500; // 25 %
   static constexpr uint64_t SLASH_BATCH_OPERATORS_BPS = 2500; // 25 %

   // Manual resolution vote threshold
   static constexpr uint64_t RESOLUTION_VOTE_THRESHOLD_BPS = 6667; // ≈ 2/3

} // namespace sysio
