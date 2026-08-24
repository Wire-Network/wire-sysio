#pragma once

#include <sysio/chain/block_handle.hpp>
#include <sysio/chain/snapshot_scheduler.hpp>

#include <fc/reflect/reflect.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

namespace sysio {

/// Recovery-sidecar schema understood by this node version.
inline constexpr uint32_t snapshot_attestation_recovery_schema_version = 1;

/// Filename used for durable snapshot-provider recovery state.
inline constexpr auto snapshot_attestation_recovery_filename = "snapshot-provider-recovery.json";

/// Fixed interval and first exact block height for automatic provider snapshots.
inline constexpr uint32_t snapshot_provider_block_spacing = 25000;

/** Snapshot contract operation submitted by the provider recovery loop. */
enum class snapshot_attestation_operation {
   /// Provider submission of its own locally computed tuple.
   votesnaphash,
   /// Permissionless re-evaluation of an on-chain pending tuple.
   evalsnapvote,
};

/** On-chain final tuple used to classify recovery of one local snapshot. */
struct snapshot_attestation_final_record {
   /// Finalized block identifier bound to the attestation.
   chain::block_id_type block_id;
   /// Finalized snapshot root hash.
   chain::digest_type snapshot_hash;
   /// Chain block that included the attestation record.
   uint32_t attested_at_block = 0;
};

/** Result of comparing a pending local tuple with the on-chain final table. */
enum class snapshot_attestation_record_status {
   /// No final row exists, so the local vote must be retried.
   retry,
   /// A final row exists but is still reversible, so the retained tuple must not be replaced.
   awaiting_irreversibility,
   /// The final row is identical to the local tuple.
   matching,
   /// The block identifier or snapshot hash differs.
   conflicting,
};

/** Work permitted after the mandatory pending-tuple check for one irreversible block. */
enum class snapshot_attestation_recovery_work {
   /// Pending state is not yet safe, so snapshot promotion must remain blocked.
   blocked,
   /// Pending state is safe and cadence-based maintenance is not due.
   skip_periodic,
   /// Pending state is safe and cadence-based maintenance should run.
   run_periodic,
};

/** Durable restart state for the snapshot-attestation provider loop. */
struct snapshot_attestation_recovery_state {
   /// Version of the sidecar representation.
   uint32_t schema_version = snapshot_attestation_recovery_schema_version;
   /// Chain identity that owns the persisted tuple and cursor.
   chain::chain_id_type chain_id = chain::chain_id_type::empty_chain_id();
   /// Snapshot-provider account that owns the persisted tuple and cursor.
   chain::account_name provider_account;
   /// Newest finalized local tuple that still needs an identical on-chain record.
   std::optional<chain::snapshot_scheduler::snapshot_information> pending_vote;
   /// Inclusive primary-id lower bound for the next on-chain pending-vote page.
   uint64_t pending_vote_cursor = 0;
   /// Whether a local tuple disagreed with a final record and requires operator intervention.
   bool disagreement_detected = false;

   /** Return true when no local tuple or on-chain page cursor needs recovery. */
   bool empty() const {
      return !pending_vote && pending_vote_cursor == 0 && !disagreement_detected;
   }
};

/** Return the stable log label for a provider operation. */
std::string_view snapshot_attestation_operation_name(snapshot_attestation_operation operation);

/** Return whether a local vote reported a snapshot-attestation disagreement. */
bool is_snapshot_attestation_disagreement_error(
   snapshot_attestation_operation operation,
   const std::optional<uint64_t>& error_code);

/**
 * Run snapshot promotion only after recovery confirms that replacing the retained tuple is safe.
 *
 * @return true when promotion ran; false when recovery blocked it
 */
template <typename Recover, typename Promote>
bool promote_snapshot_after_recovery(Recover&& recover, Promote&& promote) {
   if (!std::forward<Recover>(recover)()) {
      return false;
   }
   std::forward<Promote>(promote)();
   return true;
}

/**
 * Resolve retained safety state on every irreversible block before applying periodic cadence.
 *
 * @return blocked when promotion is unsafe, otherwise whether periodic maintenance is due
 */
template <typename ResolvePending>
snapshot_attestation_recovery_work prepare_snapshot_attestation_recovery(
   bool periodic_recovery_due,
   ResolvePending&& resolve_pending) {
   if (!std::forward<ResolvePending>(resolve_pending)()) {
      return snapshot_attestation_recovery_work::blocked;
   }
   return periodic_recovery_due ? snapshot_attestation_recovery_work::run_periodic
                                : snapshot_attestation_recovery_work::skip_periodic;
}

/** Return the irreversible height represented by the startup fork-db root. */
uint32_t snapshot_attestation_startup_irreversible_block_num(
   const chain::block_handle& fork_db_root);

/** Build the canonical automatic snapshot schedule used by every provider node. */
chain::snapshot_scheduler::snapshot_request_information
make_snapshot_provider_auto_schedule_request();

/** Classify an optional final record against the complete pending local tuple. */
snapshot_attestation_record_status classify_snapshot_attestation_record(
   const chain::snapshot_scheduler::snapshot_information& pending_vote,
   const std::optional<snapshot_attestation_final_record>& final_record,
   uint32_t irreversible_block_num);

/** Require nonempty recovery state to belong to the configured chain and provider account. */
void validate_snapshot_attestation_recovery_identity(
   const snapshot_attestation_recovery_state& state,
   const chain::chain_id_type& expected_chain_id,
   chain::account_name expected_provider_account);

/** Load snapshot-attestation recovery state, or return an empty state when no file exists. */
snapshot_attestation_recovery_state
load_snapshot_attestation_recovery_state(const std::filesystem::path& state_path);

/** Atomically persist recovery state, removing the sidecar when the state is empty. */
void save_snapshot_attestation_recovery_state(
   const std::filesystem::path& state_path,
   const snapshot_attestation_recovery_state& state);

} // namespace sysio

FC_REFLECT(sysio::snapshot_attestation_recovery_state,
           (schema_version)(chain_id)(provider_account)(pending_vote)(pending_vote_cursor)(disagreement_detected))
