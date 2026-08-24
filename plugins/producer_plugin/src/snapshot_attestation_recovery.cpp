#include <sysio/producer_plugin/snapshot_attestation_recovery.hpp>

#include <sysio/protocol/snapshot_attestation.hpp>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>

#include <magic_enum/magic_enum.hpp>

#include <limits>
#include <system_error>

namespace sysio {
namespace {

/// Suffix used for the same-directory atomic-write staging file.
constexpr auto snapshot_recovery_temp_suffix = ".tmp";

/// Diagnostic emitted when recovery state belongs to another chain.
constexpr auto recovery_chain_mismatch_error =
   "Snapshot-attestation recovery state belongs to a different chain";

/// Diagnostic emitted when recovery state belongs to another configured provider.
constexpr auto recovery_provider_mismatch_error =
   "Snapshot-attestation recovery state belongs to a different provider account";

/// Diagnostic emitted when startup has no fork-db root from which to establish finality.
constexpr auto missing_fork_db_root_error =
   "Snapshot-attestation recovery requires a valid startup fork-db root";

/// Description persisted for the canonical automatic provider schedule.
constexpr char snapshot_provider_auto_schedule_description[] = "snapshot-provider auto";

} // namespace

std::string_view snapshot_attestation_operation_name(snapshot_attestation_operation operation) {
   return magic_enum::enum_name(operation);
}

bool is_snapshot_attestation_disagreement_error(
   snapshot_attestation_operation operation,
   const std::optional<uint64_t>& error_code) {
   return operation == snapshot_attestation_operation::votesnaphash
          && error_code == protocol::snapshot_attestation::disagreement_error_code;
}

uint32_t snapshot_attestation_startup_irreversible_block_num(
   const chain::block_handle& fork_db_root) {
   FC_ASSERT(fork_db_root.is_valid(), "{}", missing_fork_db_root_error);
   return fork_db_root.block_num();
}

chain::snapshot_scheduler::snapshot_request_information
make_snapshot_provider_auto_schedule_request() {
   return {
      .block_spacing = snapshot_provider_block_spacing,
      .start_block_num = snapshot_provider_block_spacing,
      .end_block_num = std::numeric_limits<uint32_t>::max(),
      .snapshot_description = snapshot_provider_auto_schedule_description,
   };
}

snapshot_attestation_record_status classify_snapshot_attestation_record(
   const chain::snapshot_scheduler::snapshot_information& pending_vote,
   const std::optional<snapshot_attestation_final_record>& final_record,
   uint32_t irreversible_block_num) {
   if (!final_record) {
      return snapshot_attestation_record_status::retry;
   }
   if (final_record->attested_at_block > irreversible_block_num) {
      return snapshot_attestation_record_status::awaiting_irreversibility;
   }
   if (final_record->block_id == pending_vote.head_block_id
       && final_record->snapshot_hash.str() == pending_vote.root_hash.str()) {
      return snapshot_attestation_record_status::matching;
   }
   return snapshot_attestation_record_status::conflicting;
}

void validate_snapshot_attestation_recovery_identity(
   const snapshot_attestation_recovery_state& state,
   const chain::chain_id_type& expected_chain_id,
   chain::account_name expected_provider_account) {
   if (state.empty()) {
      return;
   }
   FC_ASSERT(state.chain_id == expected_chain_id, "{}", recovery_chain_mismatch_error);
   FC_ASSERT(state.provider_account == expected_provider_account, "{}", recovery_provider_mismatch_error);
}

snapshot_attestation_recovery_state
load_snapshot_attestation_recovery_state(const std::filesystem::path& state_path) {
   if (!std::filesystem::exists(state_path)) {
      return {};
   }

   auto state = fc::json::from_file(state_path).as<snapshot_attestation_recovery_state>();
   FC_ASSERT(state.schema_version == snapshot_attestation_recovery_schema_version,
             "Unsupported snapshot-attestation recovery schema version {}",
             state.schema_version);
   return state;
}

void save_snapshot_attestation_recovery_state(
   const std::filesystem::path& state_path,
   const snapshot_attestation_recovery_state& state) {
   FC_ASSERT(state.schema_version == snapshot_attestation_recovery_schema_version,
             "Unsupported snapshot-attestation recovery schema version {}",
             state.schema_version);

   auto temp_path = state_path;
   temp_path += snapshot_recovery_temp_suffix;
   if (state.empty()) {
      std::error_code ignored;
      std::filesystem::remove(temp_path, ignored);
      ignored.clear();
      std::filesystem::remove(state_path, ignored);
      return;
   }

   FC_ASSERT(fc::json::save_to_file(state, temp_path, true),
             "Failed to write snapshot-attestation recovery state to {}",
             temp_path.string());
   std::filesystem::rename(temp_path, state_path);
}

} // namespace sysio
