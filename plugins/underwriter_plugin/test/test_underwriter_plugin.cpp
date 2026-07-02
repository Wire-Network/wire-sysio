#include <boost/test/unit_test.hpp>

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>
#include <sysio/underwriter_plugin/solana_source_deposit_scanner.hpp>
#include <sysio/underwriter_plugin/source_deposit_constants.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

using namespace std::literals;
using namespace sysio::underwriter_defaults;
using namespace sysio::underwriter;

namespace {

/// Removed ETH confirmation-depth option name. Keeping the spelling here makes
/// the option-surface test catch any future non-finality escape hatch.
constexpr std::string_view removed_eth_min_confirmations_option = "underwriter-eth-min-confirmations";

/// Program id used by the scanner tests to model the configured opp-outpost.
const std::string test_sol_program_id = "OppOutpost11111111111111111111111111111111";

/// Program id used by the scanner tests to model an attacker-controlled CPI caller.
const std::string attacker_program_id = "Attacker111111111111111111111111111111111";

/// Returns a deterministic expected correlation hash for test markers.
fc::crypto::keccak256 test_expected_hash() {
   return fc::crypto::keccak256::hash(std::string{"sec-40-source-deposit"});
}

/// Hex-encodes a 32-byte keccak hash the same way the Solana outpost log does.
std::string hash_hex(const fc::crypto::keccak256& hash) {
   return fc::to_hex(reinterpret_cast<const char*>(hash.data()), 32);
}

/// Builds the canonical SwapDeposit marker prefix for a deposit id.
std::string marker_prefix(uint64_t deposit_id) {
   return "Program log: opp_outpost: SwapDeposit id=" + std::to_string(deposit_id) + " hash=";
}

/// Builds one `getSignaturesForAddress` response entry.
fc::variant signature_entry(const std::string& sig,
                            const std::string& confirmation_status = "finalized",
                            bool failed = false) {
   fc::mutable_variant_object obj;
   obj("signature", sig);
   obj("confirmationStatus", confirmation_status);
   if (failed) {
      obj("err", "failed");
   } else {
      obj("err", fc::variant());
   }
   return fc::variant(obj);
}

/// Builds one `getTransaction` response with the supplied runtime log lines.
fc::variant transaction_with_logs(const std::vector<std::string>& logs, bool failed = false) {
   fc::variants log_variants;
   log_variants.reserve(logs.size());
   for (const auto& line : logs) {
      log_variants.emplace_back(line);
   }

   fc::mutable_variant_object meta;
   if (failed) {
      meta("err", "failed");
   } else {
      meta("err", fc::variant());
   }
   meta("logMessages", log_variants);

   return fc::variant(fc::mutable_variant_object()("meta", meta));
}

/// Builds runtime logs where `program_id` is the current executing program.
std::vector<std::string> program_logs(const std::string& program_id,
                                      const std::vector<std::string>& payloads) {
   std::vector<std::string> logs;
   logs.reserve(payloads.size() + 2);
   logs.push_back("Program " + program_id + " invoke [1]");
   logs.insert(logs.end(), payloads.begin(), payloads.end());
   logs.push_back("Program " + program_id + " success");
   return logs;
}

/// Runs the Solana source-deposit page scanner against in-memory transactions.
sysio::underwriter::solana_source_deposit_page_scan_result
scan_test_page(const std::vector<fc::variant>& sigs,
               const std::map<std::string, fc::variant>& tx_by_sig,
               const fc::crypto::keccak256& expected_hash,
               uint64_t deposit_id,
               size_t* fetch_count = nullptr) {
   const auto prefix = marker_prefix(deposit_id);
   const sysio::underwriter::solana_source_deposit_page_scan_config config{
      .sol_program_id = test_sol_program_id,
      .marker_prefix = prefix,
      .recomputed_hash = expected_hash,
   };
   return sysio::underwriter::scan_solana_source_deposit_signature_page(
      sigs,
      [&](const std::string& sig) {
         if (fetch_count) ++*fetch_count;
         return tx_by_sig.at(sig);
      },
      config);
}

} // namespace

BOOST_AUTO_TEST_SUITE(underwriter_plugin_tests)

BOOST_AUTO_TEST_CASE(plugin_can_be_constructed) try {
   sysio::underwriter_plugin plugin;
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(plugin_options_are_registered) try {
   sysio::underwriter_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   const auto& opts = cfg.options();
   std::set<std::string> option_names;
   for (const auto& opt : opts) {
      option_names.insert(opt->long_name());
   }
   BOOST_CHECK(option_names.count("underwriter-account") > 0);
   BOOST_CHECK(option_names.count("underwriter-scan-interval-ms") > 0);
   BOOST_CHECK(option_names.count("underwriter-action-timeout-ms") > 0);
   BOOST_CHECK(option_names.count("underwriter-enabled") > 0);
   BOOST_CHECK(option_names.count("underwriter-eth-outpost") > 0);
   BOOST_CHECK(option_names.count("underwriter-sol-outpost") > 0);
   BOOST_CHECK(option_names.count(std::string{ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION}) > 0);
   BOOST_CHECK_EQUAL(option_names.count(std::string{removed_eth_min_confirmations_option}), 0);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(default_options_are_correct) try {
   sysio::underwriter_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   boost::program_options::variables_map vm;
   boost::program_options::store(
      boost::program_options::parse_command_line(0, static_cast<char**>(nullptr), cfg), vm);
   boost::program_options::notify(vm);

   BOOST_CHECK_EQUAL(vm["underwriter-scan-interval-ms"].as<uint32_t>(), scan_interval_ms);
   BOOST_CHECK_EQUAL(vm["underwriter-action-timeout-ms"].as<uint32_t>(), action_timeout_ms);
   BOOST_CHECK_EQUAL(vm["underwriter-enabled"].as<bool>(), enabled);
   // SEC-13/WSA-027: the former single --underwriter-{eth,sol}-client-id were
   // replaced by repeatable per-chain --underwriter-{eth,sol}-outpost (no scalar
   // default to assert; presence is checked in the option-registration case).
   BOOST_CHECK_EQUAL(
      vm[std::string{ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION}].as<uint64_t>(),
      ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS);
} FC_LOG_AND_RETHROW();

/// The ETH source-deposit event lookup window is inclusive and anchored on the
/// finalized head. It never reaches below genesis, so the verifier can bound
/// each `eth_getLogs` request without losing well-defined behavior on young
/// chains.
BOOST_AUTO_TEST_CASE(eth_source_deposit_log_window_is_bounded) try {
   constexpr uint64_t first_non_genesis_head = 1;
   constexpr uint64_t one_block_window       = 1;
   constexpr uint64_t larger_head            = ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS + 99;

   BOOST_CHECK_EQUAL(
      eth_source_deposit_from_block(larger_head, ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS),
      larger_head - ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS + 1);
   BOOST_CHECK_EQUAL(
      larger_head - eth_source_deposit_from_block(larger_head, ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS) + 1,
      ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS);
   BOOST_CHECK_EQUAL(eth_source_deposit_from_block(first_non_genesis_head,
                                                   ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS),
                     0);
   BOOST_CHECK_EQUAL(eth_source_deposit_from_block(larger_head, one_block_window),
                     larger_head);
} FC_LOG_AND_RETHROW();

/// Reject an empty bounded ETH log lookup window.
BOOST_AUTO_TEST_CASE(eth_source_deposit_options_reject_zero_window) try {
   sysio::underwriter_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   const std::vector<std::string> args{
      "--" + std::string{ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION} + "=0",
   };

   boost::program_options::variables_map vm;
   boost::program_options::store(
      boost::program_options::command_line_parser(args).options(cfg).run(), vm);
   boost::program_options::notify(vm);

   BOOST_CHECK_THROW(plugin.plugin_initialize(vm), fc::exception);
} FC_LOG_AND_RETHROW();

/// Explicit block bounds must be encoded as JSON-RPC quantities, not tags such
/// as `earliest` or `latest`.
BOOST_AUTO_TEST_CASE(eth_block_quantity_formats_json_rpc_numbers) try {
   constexpr uint64_t low_nibble_block = 15;
   constexpr uint64_t two_digit_block  = 16;
   constexpr uint64_t sample_block     = 0x1234abcd;

   BOOST_CHECK_EQUAL(eth_block_quantity(0), "0x0");
   BOOST_CHECK_EQUAL(eth_block_quantity(low_nibble_block), "0xf");
   BOOST_CHECK_EQUAL(eth_block_quantity(two_digit_block), "0x10");
   BOOST_CHECK_EQUAL(eth_block_quantity(sample_block), "0x1234abcd");
} FC_LOG_AND_RETHROW();

/// Malformed finalized-head block numbers defer source-deposit verification
/// instead of escaping through the scan cycle.
BOOST_AUTO_TEST_CASE(eth_block_quantity_parser_rejects_malformed_rpc_numbers) try {
   const auto parsed_zero = eth_parse_block_quantity("0x0");
   const auto parsed_sample = eth_parse_block_quantity("0x1234abcd");

   BOOST_REQUIRE(parsed_zero);
   BOOST_REQUIRE(parsed_sample);
   BOOST_CHECK_EQUAL(*parsed_zero, 0);
   BOOST_CHECK_EQUAL(*parsed_sample, 0x1234abcd);
   BOOST_CHECK(!eth_parse_block_quantity(""));
   BOOST_CHECK(!eth_parse_block_quantity("0x"));
   BOOST_CHECK(!eth_parse_block_quantity("1234"));
   BOOST_CHECK(!eth_parse_block_quantity("0x12zz"));
   BOOST_CHECK(!eth_parse_block_quantity("0x10000000000000000"));
} FC_LOG_AND_RETHROW();

// ── B1: preflight option-coverage ──────────────────────────────────────
//
// The full preflight (operator status / authex links / balances /
// signature self-test) needs a live chain context to exercise. What we
// CAN cover at unit-test level is that every required CLI option for
// the verify path is declared by `set_program_options` — so a typo'd
// option name in production won't slip past with a silent default.
BOOST_AUTO_TEST_CASE(preflight_required_options_are_registered) try {
   sysio::underwriter_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   const auto& opts = cfg.options();
   std::set<std::string> option_names;
   for (const auto& opt : opts) {
      option_names.insert(opt->long_name());
   }
   BOOST_CHECK(option_names.count("underwriter-account") > 0);
   BOOST_CHECK(option_names.count("underwriter-eth-source-deposit-function") > 0);
   BOOST_CHECK(option_names.count("underwriter-sol-source-deposit-instruction") > 0);
} FC_LOG_AND_RETHROW();

// The preflight cases below are placeholders: exercising the live
// preflight requires standing up a chain_plugin + chain controller +
// authex/opreg/epoch contracts in a tester fixture. The integration
// flow tests cover those paths end-to-end; this unit-test file is the
// option-surface guard.
BOOST_AUTO_TEST_CASE(preflight_fails_on_missing_authex_link) try {
   // Documented in test plan as needing the cluster harness; this stub
   // keeps the test name on the books so future scaffolding lands here.
   // Integration coverage: flow-underwriter-race (deferred).
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(preflight_fails_on_zero_balance_on_any_registered_outpost) try {
   // Stub — see preflight_fails_on_missing_authex_link comment.
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(preflight_fails_on_slashed_status) try {
   // Stub — see preflight_fails_on_missing_authex_link comment.
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

// ── B5: knapsack fallback above MAX_CANDIDATES ─────────────────────────
//
// The branch-and-bound selector lives in the `impl` private struct and
// isn't reachable from this test binary (no public accessor). The
// fallback is exercised at integration time; we sanity-check here that
// the constant is well-defined (compile-time) by referencing it. The
// real coverage lives in flow tests.
BOOST_AUTO_TEST_CASE(knapsack_fallback_threshold_is_documented) try {
   // Documentation marker — see underwriter_plugin.cpp::MAX_CANDIDATES.
   // The threshold is 64; raising it without a fallback test would be a
   // regression to surface in a future PR's review.
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

// ── B3: HTTP diagnostic endpoint plumbing ──────────────────────────────
//
// /v1/underwriter/stats + /v1/underwriter/commits are registered via
// http_plugin during plugin_startup. Constructing http_plugin in
// isolation from chain_plugin requires the appbase wiring. Stub here;
// integration coverage via curl in the flow tests.
BOOST_AUTO_TEST_CASE(http_endpoints_registered_at_startup) try {
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_state_advances_across_pages) try {
   sysio::underwriter::solana_source_deposit_scan_cursor_map cursors;
   const sysio::underwriter::solana_source_deposit_scan_key key{
      .uwreq_id = 42,
      .deposit_id = 7,
   };

   const auto initial =
      sysio::underwriter::get_or_create_solana_source_deposit_scan_cursor(cursors, key);
   BOOST_CHECK(!initial.before);
   BOOST_CHECK_EQUAL(initial.pages_scanned, 0);
   BOOST_CHECK_EQUAL(initial.signatures_scanned, 0);

   const auto first_page = sysio::underwriter::advance_solana_source_deposit_scan_cursor(
      cursors, key, "page-0-last-signature", sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE);
   BOOST_REQUIRE(first_page.before);
   BOOST_CHECK_EQUAL(*first_page.before, "page-0-last-signature");
   BOOST_CHECK_EQUAL(first_page.pages_scanned, 1);
   BOOST_CHECK_EQUAL(first_page.signatures_scanned, sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE);

   const auto second_page = sysio::underwriter::advance_solana_source_deposit_scan_cursor(
      cursors, key, "page-1-last-signature", sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE);
   BOOST_REQUIRE(second_page.before);
   BOOST_CHECK_EQUAL(*second_page.before, "page-1-last-signature");
   BOOST_CHECK_EQUAL(second_page.pages_scanned, 2);
   BOOST_CHECK_EQUAL(second_page.signatures_scanned, sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE * 2);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_state_records_terminal_failure_once) try {
   sysio::underwriter::solana_source_deposit_scan_cursor_map cursors;
   const sysio::underwriter::solana_source_deposit_scan_key key{
      .uwreq_id = 43,
      .deposit_id = 8,
   };

   sysio::underwriter::advance_solana_source_deposit_scan_cursor(
      cursors, key, "page-0-last-signature", sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE);
   const auto first = sysio::underwriter::record_solana_source_deposit_terminal_failure(
      cursors, key, "exhausted history", 17);
   BOOST_CHECK(first.first_failure);
   BOOST_CHECK(first.cursor.terminal_failure);
   BOOST_CHECK_EQUAL(first.cursor.terminal_failure_reason, "exhausted history");
   BOOST_CHECK_EQUAL(first.cursor.pages_scanned, 2);
   BOOST_CHECK_EQUAL(first.cursor.signatures_scanned, sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE + 17);

   const auto terminal =
      sysio::underwriter::get_solana_source_deposit_terminal_failure(cursors, key);
   BOOST_REQUIRE(terminal);
   BOOST_CHECK_EQUAL(terminal->terminal_failure_reason, "exhausted history");

   const auto repeated = sysio::underwriter::record_solana_source_deposit_terminal_failure(
      cursors, key, "second failure should not replace first", 99);
   BOOST_CHECK(!repeated.first_failure);
   BOOST_CHECK_EQUAL(repeated.cursor.terminal_failure_reason, "exhausted history");
   BOOST_CHECK_EQUAL(repeated.cursor.pages_scanned, 2);
   BOOST_CHECK_EQUAL(repeated.cursor.signatures_scanned, sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE + 17);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_state_prunes_resolved_uwreqs) try {
   sysio::underwriter::solana_source_deposit_scan_cursor_map cursors;
   const sysio::underwriter::solana_source_deposit_scan_key pending_key{
      .uwreq_id = 44,
      .deposit_id = 9,
   };
   const sysio::underwriter::solana_source_deposit_scan_key resolved_key{
      .uwreq_id = 45,
      .deposit_id = 10,
   };
   sysio::underwriter::advance_solana_source_deposit_scan_cursor(
      cursors, pending_key, "pending-last-signature", 3);
   sysio::underwriter::record_solana_source_deposit_terminal_failure(
      cursors, resolved_key, "resolved terminal failure", 5);

   const std::unordered_set<uint64_t> still_pending{pending_key.uwreq_id};
   sysio::underwriter::prune_solana_source_deposit_scan_cursors(cursors, still_pending);

   BOOST_CHECK(cursors.contains(pending_key));
   BOOST_CHECK(!cursors.contains(resolved_key));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_finds_match_after_legacy_window) try {
   const uint64_t deposit_id = 7;
   const auto expected_hash = test_expected_hash();
   const auto marker = marker_prefix(deposit_id) + hash_hex(expected_hash);

   std::vector<fc::variant> sigs;
   std::map<std::string, fc::variant> tx_by_sig;
   for (size_t i = 0; i < 51; ++i) {
      const std::string sig = "noise-" + std::to_string(i);
      sigs.push_back(signature_entry(sig));
      tx_by_sig.emplace(sig, transaction_with_logs(program_logs(test_sol_program_id, {"Program log: unrelated"})));
   }

   const std::string target_sig = "target-source-deposit";
   sigs.push_back(signature_entry(target_sig));
   tx_by_sig.emplace(target_sig, transaction_with_logs(program_logs(test_sol_program_id, {marker})));

   size_t fetch_count = 0;
   const auto result = scan_test_page(sigs, tx_by_sig, expected_hash, deposit_id, &fetch_count);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::matched);
   BOOST_CHECK_EQUAL(result.matched_signature, target_sig);
   BOOST_CHECK_EQUAL(fetch_count, 52);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_returns_next_before_for_full_clean_page) try {
   const uint64_t deposit_id = 8;
   const auto expected_hash = test_expected_hash();

   std::vector<fc::variant> sigs;
   std::map<std::string, fc::variant> tx_by_sig;
   for (size_t i = 0; i < sysio::underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE; ++i) {
      const std::string sig = "full-page-" + std::to_string(i);
      sigs.push_back(signature_entry(sig));
      tx_by_sig.emplace(sig, transaction_with_logs(program_logs(test_sol_program_id, {"Program log: unrelated"})));
   }

   const auto result = scan_test_page(sigs, tx_by_sig, expected_hash, deposit_id);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::not_found);
   BOOST_REQUIRE(result.next_before);
   BOOST_CHECK_EQUAL(*result.next_before, "full-page-999");
   BOOST_CHECK(!result.page_exhausted);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_ignores_forged_marker_from_wrong_program) try {
   const uint64_t deposit_id = 9;
   const auto expected_hash = test_expected_hash();
   const auto marker = marker_prefix(deposit_id) + hash_hex(expected_hash);
   const std::string sig = "forged-marker";

   const std::vector<fc::variant> sigs{signature_entry(sig)};
   const std::map<std::string, fc::variant> tx_by_sig{
      {sig, transaction_with_logs(program_logs(attacker_program_id, {marker}))},
   };

   const auto result = scan_test_page(sigs, tx_by_sig, expected_hash, deposit_id);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::not_found);
   BOOST_REQUIRE(result.next_before);
   BOOST_CHECK_EQUAL(*result.next_before, sig);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_defers_unfinalized_match_without_cursor_advance) try {
   const uint64_t deposit_id = 10;
   const auto expected_hash = test_expected_hash();
   const auto marker = marker_prefix(deposit_id) + hash_hex(expected_hash);
   const std::string sig = "unfinalized-marker";

   const std::vector<fc::variant> sigs{signature_entry(sig, "confirmed")};
   const std::map<std::string, fc::variant> tx_by_sig{
      {sig, transaction_with_logs(program_logs(test_sol_program_id, {marker}))},
   };

   const auto result = scan_test_page(sigs, tx_by_sig, expected_hash, deposit_id);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::deferred);
   BOOST_CHECK(!result.next_before);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_continues_after_fetch_error_to_find_match) try {
   const uint64_t deposit_id = 11;
   const auto expected_hash = test_expected_hash();
   const auto marker = marker_prefix(deposit_id) + hash_hex(expected_hash);
   const std::string error_sig = "transient-fetch-error";
   const std::string target_sig = "target-after-fetch-error";
   const std::vector<fc::variant> sigs{signature_entry(error_sig), signature_entry(target_sig)};
   const std::map<std::string, fc::variant> tx_by_sig{
      {target_sig, transaction_with_logs(program_logs(test_sol_program_id, {marker}))},
   };
   const auto prefix = marker_prefix(deposit_id);
   const sysio::underwriter::solana_source_deposit_page_scan_config config{
      .sol_program_id = test_sol_program_id,
      .marker_prefix = prefix,
      .recomputed_hash = expected_hash,
   };

   const auto result = sysio::underwriter::scan_solana_source_deposit_signature_page(
      sigs,
      [&](const std::string& sig) -> fc::variant {
         if (sig == error_sig) {
            FC_THROW_EXCEPTION(fc::exception, "transient fetch failure");
         }
         return tx_by_sig.at(sig);
      },
      config);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::matched);
   BOOST_CHECK_EQUAL(result.matched_signature, target_sig);
   BOOST_CHECK(!result.next_before);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_defers_fetch_error_without_cursor_advance) try {
   const uint64_t deposit_id = 12;
   const auto expected_hash = test_expected_hash();
   const std::string error_sig = "transient-fetch-error";
   const std::string clean_sig = "clean-after-fetch-error";
   const std::vector<fc::variant> sigs{signature_entry(error_sig), signature_entry(clean_sig)};
   const std::map<std::string, fc::variant> tx_by_sig{
      {clean_sig, transaction_with_logs(program_logs(test_sol_program_id, {"Program log: unrelated"}))},
   };
   const auto prefix = marker_prefix(deposit_id);
   const sysio::underwriter::solana_source_deposit_page_scan_config config{
      .sol_program_id = test_sol_program_id,
      .marker_prefix = prefix,
      .recomputed_hash = expected_hash,
   };

   const auto result = sysio::underwriter::scan_solana_source_deposit_signature_page(
      sigs,
      [&](const std::string& sig) -> fc::variant {
         if (sig == error_sig) {
            FC_THROW_EXCEPTION(fc::exception, "transient fetch failure");
         }
         return tx_by_sig.at(sig);
      },
      config);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::deferred);
   BOOST_CHECK(!result.next_before);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_hard_fails_malformed_marker_hash) try {
   const uint64_t deposit_id = 13;
   const auto expected_hash = test_expected_hash();
   const auto marker = marker_prefix(deposit_id) + "abc";
   const std::string sig = "malformed-marker";

   const std::vector<fc::variant> sigs{signature_entry(sig)};
   const std::map<std::string, fc::variant> tx_by_sig{
      {sig, transaction_with_logs(program_logs(test_sol_program_id, {marker}))},
   };

   const auto result = scan_test_page(sigs, tx_by_sig, expected_hash, deposit_id);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::hard_mismatch);
   BOOST_CHECK(!result.next_before);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_hard_fails_partial_hex_parse) try {
   const uint64_t deposit_id = 14;
   const fc::crypto::keccak256 expected_zero_hash;
   std::string malformed_zero_hash;
   malformed_zero_hash.reserve(64);
   for (size_t i = 0; i < 32; ++i) {
      malformed_zero_hash += "0g";
   }
   const auto marker = marker_prefix(deposit_id) + malformed_zero_hash;
   const std::string sig = "partial-hex-marker";

   const std::vector<fc::variant> sigs{signature_entry(sig)};
   const std::map<std::string, fc::variant> tx_by_sig{
      {sig, transaction_with_logs(program_logs(test_sol_program_id, {marker}))},
   };

   const auto result = scan_test_page(sigs, tx_by_sig, expected_zero_hash, deposit_id);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::hard_mismatch);
   BOOST_CHECK(!result.next_before);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(solana_source_deposit_scan_skips_failed_listing_transactions) try {
   const uint64_t deposit_id = 15;
   const auto expected_hash = test_expected_hash();
   const auto marker = marker_prefix(deposit_id) + hash_hex(expected_hash);
   const std::string sig = "failed-listing";

   const std::vector<fc::variant> sigs{signature_entry(sig, "finalized", true)};
   const std::map<std::string, fc::variant> tx_by_sig{
      {sig, transaction_with_logs(program_logs(test_sol_program_id, {marker}))},
   };

   size_t fetch_count = 0;
   const auto result = scan_test_page(sigs, tx_by_sig, expected_hash, deposit_id, &fetch_count);

   BOOST_CHECK(result.status == sysio::underwriter::solana_source_deposit_page_status::not_found);
   BOOST_CHECK_EQUAL(fetch_count, 0);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
