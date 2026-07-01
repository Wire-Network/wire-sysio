#pragma once

#include <cstddef>

namespace sysio::msgch_svm_terminal_budget {

/// Solana raw transaction packet ceiling used by legacy transactions.
inline constexpr size_t SVM_TERMINAL_PACKET_LIMIT_BYTES = 1'232;

/// Packet safety margin reserved for future IDL/layout drift.
inline constexpr size_t SVM_TERMINAL_PACKET_SAFETY_MARGIN_BYTES = 80;

/// Effective Solana terminal packet budget used by `buildenv` prefix packing.
inline constexpr size_t SVM_TERMINAL_PACKET_BUDGET_BYTES =
   SVM_TERMINAL_PACKET_LIMIT_BYTES - SVM_TERMINAL_PACKET_SAFETY_MARGIN_BYTES;

/// Current Solana loaded-account runtime cap for one terminal transaction.
inline constexpr size_t SVM_TERMINAL_RUNTIME_ACCOUNT_LIMIT = 64;

/// Legacy transaction account-key ceiling from one-byte instruction indices.
inline constexpr size_t SVM_TERMINAL_ACCOUNT_KEY_LIMIT = 256;

/// Account-key margin reserved for static account drift.
inline constexpr size_t SVM_TERMINAL_ACCOUNT_KEY_SAFETY_MARGIN = 16;

/// Effective legacy account-key budget used by the WIRE estimator.
inline constexpr size_t SVM_TERMINAL_ACCOUNT_KEY_BUDGET =
   SVM_TERMINAL_ACCOUNT_KEY_LIMIT - SVM_TERMINAL_ACCOUNT_KEY_SAFETY_MARGIN;

/// Pessimistic packet-byte cost for adding one dynamic remaining account.
inline constexpr size_t SVM_DYNAMIC_ACCOUNT_PACKET_BYTES = 33;

/// Conservative zero-data terminal-finalize static packet size, including
/// the terminal `request_heap_frame` compute-budget pre-instruction.
///
/// This is a consensus parameter coupled to wire-solana's terminal `epoch_in`
/// account list and terminal pre-instruction set. Keep it greater than or
/// equal to the measured static packet bytes in
/// `sec-94-solana-terminal-budget.json`.
inline constexpr size_t SVM_TERMINAL_STATIC_PACKET_BYTES_WITH_MARGIN = 624;

/// Conservative static loaded-account count for the zero-data terminal
/// finalization transaction.
inline constexpr size_t SVM_TERMINAL_STATIC_LOADED_ACCOUNTS = 46;

/// Conservative static legacy account-key count for the zero-data terminal
/// finalization transaction.
inline constexpr size_t SVM_TERMINAL_STATIC_ACCOUNT_KEYS = 46;

/// Return the remaining budget without underflow when a static estimate has
/// already consumed the whole cap.
constexpr size_t checked_budget_remainder(size_t budget, size_t used) {
   return used < budget ? budget - used : 0;
}

/// Dynamic-account budget implied by Solana packet bytes.
constexpr size_t svm_packet_dynamic_account_budget() {
   return checked_budget_remainder(SVM_TERMINAL_PACKET_BUDGET_BYTES,
                                   SVM_TERMINAL_STATIC_PACKET_BYTES_WITH_MARGIN) /
          SVM_DYNAMIC_ACCOUNT_PACKET_BYTES;
}

/// Dynamic-account budget implied by the Solana runtime loaded-account cap.
constexpr size_t svm_runtime_dynamic_account_budget() {
   return checked_budget_remainder(SVM_TERMINAL_RUNTIME_ACCOUNT_LIMIT,
                                   SVM_TERMINAL_STATIC_LOADED_ACCOUNTS);
}

/// Dynamic-account budget implied by the legacy account-key cap.
constexpr size_t svm_key_dynamic_account_budget() {
   return checked_budget_remainder(SVM_TERMINAL_ACCOUNT_KEY_BUDGET,
                                   SVM_TERMINAL_STATIC_ACCOUNT_KEYS);
}

/// Hard dynamic-account budget for the SVM terminal transaction.
constexpr size_t svm_hard_dynamic_account_budget() {
   constexpr size_t packet_budget = svm_packet_dynamic_account_budget();
   constexpr size_t runtime_budget = svm_runtime_dynamic_account_budget();
   constexpr size_t key_budget = svm_key_dynamic_account_budget();
   constexpr size_t packet_runtime_min = packet_budget < runtime_budget ? packet_budget : runtime_budget;
   return packet_runtime_min < key_budget ? packet_runtime_min : key_budget;
}

/// Estimate terminal transaction bytes from the dynamic account count.
constexpr size_t svm_estimated_terminal_packet_bytes(size_t dynamic_accounts) {
   return SVM_TERMINAL_STATIC_PACKET_BYTES_WITH_MARGIN +
          dynamic_accounts * SVM_DYNAMIC_ACCOUNT_PACKET_BYTES;
}

/// Check all SVM terminal caps against one dynamic account count.
constexpr bool svm_terminal_budget_fits(size_t dynamic_accounts) {
   return dynamic_accounts <= svm_hard_dynamic_account_budget() &&
          svm_estimated_terminal_packet_bytes(dynamic_accounts) <= SVM_TERMINAL_PACKET_BUDGET_BYTES &&
          SVM_TERMINAL_STATIC_LOADED_ACCOUNTS + dynamic_accounts <= SVM_TERMINAL_RUNTIME_ACCOUNT_LIMIT &&
          SVM_TERMINAL_STATIC_ACCOUNT_KEYS + dynamic_accounts <= SVM_TERMINAL_ACCOUNT_KEY_BUDGET;
}

} // namespace sysio::msgch_svm_terminal_budget
