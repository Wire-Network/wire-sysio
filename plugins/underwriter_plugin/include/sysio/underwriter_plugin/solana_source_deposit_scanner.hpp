#pragma once

#include <sysio/underwriter_plugin/source_deposit_constants.hpp>

#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>

#include <boost/algorithm/string/predicate.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sysio::underwriter {

/// Result categories for scanning one Solana `getSignaturesForAddress` page
/// for an authenticated `SwapDeposit` source-deposit marker.
enum class solana_source_deposit_page_status {
   matched,
   not_found,
   deferred,
   hard_mismatch
};

/// Inputs shared by every transaction inspected in one signature page.
struct solana_source_deposit_page_scan_config {
   /// Deployed opp-outpost program id resolved from the loaded Anchor IDL.
   std::string_view sol_program_id;
   /// Full log prefix ending immediately before the 64-character hash field.
   std::string_view marker_prefix;
   /// Expected correlation hash recomputed from the depot UWREQ row.
   const fc::crypto::keccak256& recomputed_hash;
};

/// Outcome details from scanning a single Solana signature page.
struct solana_source_deposit_page_scan_result {
   solana_source_deposit_page_status status = solana_source_deposit_page_status::not_found;
   /// Matching transaction signature when `status == matched`.
   std::string matched_signature;
   /// Last valid signature in the page. Use as the next `before` cursor only
   /// when `status == not_found` and `page_exhausted == false`.
   std::optional<std::string> next_before;
   /// True when the page had fewer results than the RPC page limit, so there
   /// is no older page to request for this address history.
   bool page_exhausted = false;
   /// Human-readable reason for hard mismatches or deferred scans.
   std::string reason;
};

/// Fetches one decoded Solana transaction for the supplied base58 signature.
using solana_transaction_fetcher = std::function<fc::variant(const std::string&)>;

/// Returns true when a Solana log line opens, closes, or otherwise describes
/// an invocation frame rather than carrying a user `Program log:` payload.
inline bool is_solana_program_frame_line(const std::string& line) {
   return boost::algorithm::starts_with(line, "Program ") &&
          !boost::algorithm::starts_with(line, "Program log:") &&
          !boost::algorithm::starts_with(line, "Program data:") &&
          !boost::algorithm::starts_with(line, "Program return:");
}

/// Updates the executing-program stack from one Solana runtime frame line.
inline void update_solana_program_stack(const std::string& line,
                                        std::vector<std::string>& program_stack) {
   std::string_view rest{line};
   rest.remove_prefix(8); // strip "Program "
   const size_t sp1 = rest.find(' ');
   if (sp1 == std::string_view::npos) return;

   const std::string_view prog = rest.substr(0, sp1);
   std::string_view after = rest.substr(sp1 + 1);
   const size_t sp2 = after.find(' ');
   const std::string_view verb = (sp2 == std::string_view::npos) ? after : after.substr(0, sp2);
   if (verb == "invoke") {
      program_stack.emplace_back(prog);
   } else if (verb.starts_with("success") || verb.starts_with("failed")) {
      if (!program_stack.empty()) program_stack.pop_back();
   }
}

/// Decodes one lower-case hex digit from the canonical marker hash.
inline std::optional<uint8_t> decode_lowercase_hex_digit(char c) {
   if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
   if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
   return std::nullopt;
}

/// Parses the 64-character lower-case hex hash suffix emitted by
/// `opp-outpost::request_swap` after the canonical marker prefix.
inline std::optional<std::array<uint8_t, 32>> parse_solana_swap_deposit_hash(std::string_view hash_hex) {
   if (hash_hex.size() != 64) return std::nullopt;

   std::array<uint8_t, 32> parsed{};
   for (size_t i = 0; i < parsed.size(); ++i) {
      const auto high = decode_lowercase_hex_digit(hash_hex[i * 2]);
      const auto low = decode_lowercase_hex_digit(hash_hex[i * 2 + 1]);
      if (!high || !low) return std::nullopt;
      parsed[i] = static_cast<uint8_t>((*high << 4) | *low);
   }
   return parsed;
}

/// Scans one page of Solana program signatures for a source-deposit marker.
///
/// The function is deliberately free of plugin state: production code supplies
/// the RPC-backed transaction fetcher, and unit tests supply an in-memory
/// fetcher. Pagination is page-at-a-time; callers persist `next_before` only
/// after a clean `not_found` result so transient transaction fetch failures
/// cannot advance past an uninspected candidate.
inline solana_source_deposit_page_scan_result
scan_solana_source_deposit_signature_page(const std::vector<fc::variant>& sigs,
                                          const solana_transaction_fetcher& get_transaction,
                                          const solana_source_deposit_page_scan_config& config) {
   solana_source_deposit_page_scan_result result;
   result.page_exhausted = sigs.size() < SOL_SIGNATURE_SCAN_PAGE_SIZE;
   bool saw_uninspected_candidate = false;
   std::string deferred_reason;

   for (const auto& sig_var : sigs) {
      if (!sig_var.is_object()) continue;
      const auto sig_obj = sig_var.get_object();
      if (!sig_obj.contains("signature") || !sig_obj["signature"].is_string()) continue;
      const std::string sig_b58 = sig_obj["signature"].as_string();
      result.next_before = sig_b58;

      // Skip failed txs at the listing level. They cannot have emitted the
      // successful source-deposit marker.
      if (sig_obj.contains("err") && !sig_obj["err"].is_null()) {
         continue;
      }

      fc::variant tx;
      try {
         tx = get_transaction(sig_b58);
      } catch (const fc::exception& e) {
         saw_uninspected_candidate = true;
         if (deferred_reason.empty()) {
            deferred_reason = "getTransaction(" + sig_b58 + ") RPC error: " + e.to_detail_string();
         }
         continue;
      }
      if (tx.is_null() || !tx.is_object()) continue;
      const auto tx_obj = tx.get_object();
      if (!tx_obj.contains("meta") || !tx_obj["meta"].is_object()) continue;
      const auto meta_obj = tx_obj["meta"].get_object();
      if (meta_obj.contains("err") && !meta_obj["err"].is_null()) continue;
      if (!meta_obj.contains("logMessages") || !meta_obj["logMessages"].is_array()) continue;

      std::vector<std::string> program_stack;
      for (const auto& line_var : meta_obj["logMessages"].get_array()) {
         if (!line_var.is_string()) continue;
         const std::string line = line_var.as_string();

         if (is_solana_program_frame_line(line)) {
            update_solana_program_stack(line, program_stack);
            continue;
         }

         if (!boost::algorithm::starts_with(line, config.marker_prefix)) {
            continue;
         }

         // Attribute "Program log:" to the program currently executing.
         if (program_stack.empty() || program_stack.back() != config.sol_program_id) {
            continue;
         }

         const auto hash_hex = std::string_view{line}.substr(config.marker_prefix.size());
         const auto on_chain_hash = parse_solana_swap_deposit_hash(hash_hex);
         if (!on_chain_hash) {
            result.status = solana_source_deposit_page_status::hard_mismatch;
            result.next_before.reset();
            result.reason = "marker hash is malformed";
            return result;
         }

         if (std::memcmp(config.recomputed_hash.data(), on_chain_hash->data(), on_chain_hash->size()) != 0) {
            const std::string want_hex =
               fc::to_hex(reinterpret_cast<const char*>(config.recomputed_hash.data()), 32);
            result.status = solana_source_deposit_page_status::hard_mismatch;
            result.next_before.reset();
            result.reason = "SwapDeposit hash mismatch: recomputed=" + want_hex +
                            " on-chain=" + std::string{hash_hex};
            return result;
         }

         std::string conf_status;
         if (sig_obj.contains("confirmationStatus") && sig_obj["confirmationStatus"].is_string()) {
            conf_status = sig_obj["confirmationStatus"].as_string();
         }
         if (conf_status != "finalized") {
            result.status = solana_source_deposit_page_status::deferred;
            result.next_before.reset();
            result.reason = "matching tx is not finalized";
            return result;
         }

         result.status = solana_source_deposit_page_status::matched;
         result.matched_signature = sig_b58;
         result.next_before.reset();
         return result;
      }
   }

   if (saw_uninspected_candidate) {
      result.status = solana_source_deposit_page_status::deferred;
      result.next_before.reset();
      result.reason = std::move(deferred_reason);
   }

   return result;
}

} // namespace sysio::underwriter
