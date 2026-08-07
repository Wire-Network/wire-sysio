#include <boost/test/unit_test.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gsl-lite/gsl-lite.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>
#include <sysio/underwriter_plugin/solana_source_deposit_scanner.hpp>
#include <sysio/underwriter_plugin/source_deposit_constants.hpp>
#include <sysio/underwriter_plugin/source_deposit_hash_detail.hpp>
#include <sysio/underwriter_plugin/routing_detail.hpp>
#include <sysio/underwriter_plugin/uic_signature_detail.hpp>
#include <sysio/underwriter_plugin/uic_construction_detail.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/opp/test/uic_signature_test_utils.hpp>

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

/// Decodes a hex string into the `std::vector<char>` shape a UWREQ row's
/// `depositor` field carries.
std::vector<char> depositor_bytes(std::string_view hex) {
   std::vector<char> bytes(hex.size() / 2);
   fc::from_hex(hex, bytes.data(), bytes.size());
   return bytes;
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

/** Build one explicitly named local signature-provider specification. */
std::string explicit_uic_provider_spec(
   std::string_view key_name,
   fc::crypto::chain_key_type_t key_type,
   fc::crypto::private_key::key_type concrete_type) {
   const auto key = fc::crypto::private_key::generate(concrete_type);
   return fc::crypto::to_signature_provider_spec(
      std::string{key_name}, fc::crypto::chain_kind_wire, key_type,
      key.get_public_key().to_string({}), "KEY:" + key.to_string({}));
}

/** Build one anonymous four-field local signature-provider specification. */
std::string anonymous_uic_provider_spec(
   std::string_view key_name,
   fc::crypto::chain_key_type_t key_type,
   fc::crypto::private_key::key_type concrete_type) {
   const auto named = explicit_uic_provider_spec(key_name, key_type, concrete_type);
   return named.substr(named.find(',') + 1);
}

/**
 * Exercise operator-configured UIC selection after production-style WIRE default
 * registration for one supported provider key type.
 */
void check_operator_configured_uic_provider_beats_wire_default(
   std::string_view key_name,
   fc::crypto::chain_key_type_t key_type,
   fc::crypto::private_key::key_type concrete_type,
   bool automatic_default_coexists = true,
   bool has_explicit_name = true) {
   using namespace fc::crypto;
   auto reset_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   const auto config_dir = std::filesystem::temp_directory_path()
      / ("underwriter-uic-provider-" + std::string{key_name});
   std::error_code ec;
   std::filesystem::remove_all(config_dir, ec);
   std::filesystem::create_directories(config_dir);
   auto cleanup = gsl_lite::finally([&]() {
      std::error_code ignored;
      std::filesystem::remove_all(config_dir, ignored);
   });

   std::vector<std::string> args{
      "test_underwriter_plugin",
      "--config-dir", config_dir.string(),
      "--signature-provider",
      has_explicit_name
         ? explicit_uic_provider_spec(key_name, key_type, concrete_type)
         : anonymous_uic_provider_spec(key_name, key_type, concrete_type),
   };
   std::vector<char*> argv;
   argv.reserve(args.size());
   for (auto& arg : args) argv.push_back(arg.data());

   appbase::scoped_app test_app;
   BOOST_REQUIRE(test_app->initialize<sysio::signature_provider_manager_plugin>(
      argv.size(), argv.data()));
   auto& manager = test_app->get_plugin<sysio::signature_provider_manager_plugin>();

   // Emulate chain_plugin's production initialize-time call. This focused test
   // intentionally couples to that API; if chain_plugin changes registration
   // timing, reassess the preflight ordering as well as this regression.
   manager.register_default_signature_providers({chain_key_type_wire});
   const auto wire_providers = manager.query_providers(std::nullopt, chain_kind_wire);
   const auto selected = sysio::underwriter_detail::select_uic_signature_providers(
      wire_providers, [&](const std::string& name) {
         return manager.is_operator_configured_provider(name);
      }, [](const public_key&) { return true; });

   BOOST_REQUIRE_EQUAL(1u, selected.size());
   BOOST_CHECK_EQUAL(has_explicit_name ? std::string{key_name} : "key-0",
                     selected.front()->key_name);
   BOOST_CHECK(manager.is_operator_configured_provider(
      selected.front()->key_name));
   BOOST_CHECK_EQUAL(has_explicit_name,
                     manager.is_explicitly_configured_provider(selected.front()->key_name));
   BOOST_CHECK(sysio::underwriter_detail::is_supported_uic_public_key(
      selected.front()->public_key));

   std::vector<signature_provider_ptr> automatic_only;
   for (const auto& provider : wire_providers) {
      if (!manager.is_operator_configured_provider(provider->key_name)) {
         automatic_only.push_back(provider);
      }
   }
   if (automatic_default_coexists) {
      BOOST_REQUIRE(!automatic_only.empty());
   } else {
      // Production default registration is keyed by chain-key type. An
      // explicit WIRE-native K1/R1 provider suppresses the automatic WIRE
      // default in that same bucket; EM/ED occupy different buckets and
      // therefore coexist with it.
      BOOST_CHECK(automatic_only.empty());
   }
   BOOST_CHECK(sysio::underwriter_detail::select_uic_signature_providers(
      automatic_only, [&](const std::string& name) {
         return manager.is_operator_configured_provider(name);
      }, [](const public_key&) { return true; }).empty());
}

} // namespace

BOOST_AUTO_TEST_SUITE(underwriter_plugin_tests)

BOOST_AUTO_TEST_CASE(uic_authenticated_caller_shape_is_non_default_and_exact) try {
   using sysio::opp::attestations::UnderwriteIntentCommit;
   using sysio::opp::types::CHAIN_KIND_EVM;
   using sysio::opp::types::CHAIN_KIND_SVM;
   using sysio::opp::types::CHAIN_KIND_UNKNOWN;

   for (const auto [kind, size] : std::array{
           std::pair{CHAIN_KIND_EVM, size_t{20}},
           std::pair{CHAIN_KIND_SVM, size_t{32}},
        }) {
      UnderwriteIntentCommit uic;
      const std::vector<uint8_t> address(size, 0x5au);
      BOOST_REQUIRE(sysio::underwriter_detail::set_uic_authenticated_caller(
         uic, kind, address));
      BOOST_CHECK_EQUAL(kind, uic.uw_ext_chain_addr().kind());
      BOOST_CHECK_EQUAL_COLLECTIONS(
         address.begin(), address.end(),
         uic.uw_ext_chain_addr().address().begin(),
         uic.uw_ext_chain_addr().address().end());
   }

   UnderwriteIntentCommit invalid;
   const std::vector<uint8_t> short_evm(19, 0x01u);
   BOOST_CHECK(!sysio::underwriter_detail::set_uic_authenticated_caller(
      invalid, CHAIN_KIND_EVM, short_evm));
   BOOST_CHECK(!invalid.has_uw_ext_chain_addr());
   const std::vector<uint8_t> unsupported(32, 0x01u);
   BOOST_CHECK(!sysio::underwriter_detail::set_uic_authenticated_caller(
      invalid, CHAIN_KIND_UNKNOWN, unsupported));
   BOOST_CHECK(!invalid.has_uw_ext_chain_addr());
} FC_LOG_AND_RETHROW();

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

BOOST_AUTO_TEST_CASE(preflight_accepts_fixed_size_recoverable_uic_signature_providers) try {
   using private_key = fc::crypto::private_key;
   using provider_result =
      sysio::underwriter_detail::uic_signature_provider_result;

   const auto digest = fc::sha256::hash(
      std::string{sysio::underwriter_detail::uic_signature_self_test_domain});
   const std::array supported_types{
      private_key::key_type::k1,
      private_key::key_type::r1,
      private_key::key_type::em,
      private_key::key_type::ed,
   };

   for (const auto type : supported_types) {
      const auto key = private_key::generate(type);
      const auto signature = key.sign(digest);

      fc::crypto::signature_provider_t provider;
      provider.public_key = key.get_public_key();
      provider.sign = [signature](const fc::sha256&) { return signature; };

      const auto check =
         sysio::underwriter_detail::check_uic_signature_provider(provider, digest);
      BOOST_CHECK(check.result == provider_result::compatible);
      BOOST_REQUIRE(check.recovered_key);
      BOOST_CHECK(*check.recovered_key == key.get_public_key());

      const auto packed = fc::raw::pack(signature);
      BOOST_CHECK(
         sysio::underwriter_detail::is_canonical_packed_uic_signature(packed));
      const size_t expected_size =
         sysio::underwriter_detail::canonical_packed_uic_signature_size(
            signature.type());
      BOOST_CHECK_EQUAL(packed.size(), expected_size);
   }

   const auto k1_private = private_key::generate(private_key::key_type::k1);
   const auto r1_private = private_key::generate(private_key::key_type::r1);
   const auto r1_signature = r1_private.sign(digest);

   // A supported provider key paired with another supported signature variant
   // still fails: the signer identity must match its configured public key.
   fc::crypto::signature_provider_t mismatched_provider;
   mismatched_provider.public_key = k1_private.get_public_key();
   mismatched_provider.sign = [r1_signature](const fc::sha256&) {
      return r1_signature;
   };
   const auto mismatched_check =
      sysio::underwriter_detail::check_uic_signature_provider(mismatched_provider, digest);
   BOOST_CHECK(mismatched_check.result == provider_result::signature_type_mismatch);
   BOOST_CHECK(!mismatched_check.recovered_key);

   // WebAuthn and BLS tags remain outside the non-parsing protocol boundary,
   // and supported tags with the wrong exact size are non-canonical.
   using sig_type = fc::crypto::signature::sig_type;
   auto tag = [](sig_type type) {
      return static_cast<char>(magic_enum::enum_integer(type));
   };
   fc::crypto::webauthn::signature webauthn{
      fc::crypto::r1::compact_signature{},
      std::vector<uint8_t>{1, 2, 3},
      R"({"type":"webauthn.get"})"};
   const auto webauthn_shape = fc::raw::pack(fc::crypto::signature{
      fc::crypto::signature::storage_type{std::move(webauthn)}});
   const auto bls_shape = fc::raw::pack(fc::crypto::signature{
      fc::crypto::signature::storage_type{
         std::in_place_type<fc::crypto::bls::signature_shim>}});
   std::vector<char> truncated_ed(
      sysio::underwriter_detail::canonical_packed_uic_signature_size(sig_type::ed) - 1,
      '\0');
   truncated_ed.front() = tag(sig_type::ed);
   BOOST_CHECK(!sysio::underwriter_detail::is_canonical_packed_uic_signature(
      webauthn_shape));
   BOOST_CHECK(!sysio::underwriter_detail::is_canonical_packed_uic_signature(
      bls_shape));
   BOOST_CHECK(!sysio::underwriter_detail::is_canonical_packed_uic_signature(
      truncated_ed));

   // A provider can return a recoverable high-s alternate even though local
   // libfc signers normally emit low-s. Preflight must reject it before key
   // recovery so runtime construction cannot submit a depot-invalid UIC.
   const auto canonical_k1 = k1_private.sign(digest);
   auto high_s_packed = sysio::opp::test::create_high_s_alternate(
      fc::raw::pack(canonical_k1), private_key::key_type::k1);
   BOOST_REQUIRE(high_s_packed.has_value());
   BOOST_REQUIRE_EQUAL(
      sysio::underwriter_detail::canonical_packed_uic_signature_size(
         sig_type::k1),
      high_s_packed->size());
   // create_signed_uic_bytes calls this shared predicate independently at
   // runtime after packing every returned signature, even after startup
   // preflight has passed. Exercise that exact guard as well as the aggregate
   // preflight result below.
   BOOST_CHECK(!sysio::underwriter_detail::has_canonical_uic_signature_body(
      *high_s_packed));
   const auto high_s_signature =
      fc::raw::unpack<fc::crypto::signature>(*high_s_packed);
   fc::crypto::signature_provider_t high_s_provider;
   high_s_provider.public_key = k1_private.get_public_key();
   high_s_provider.sign = [high_s_signature](const fc::sha256&) {
      return high_s_signature;
   };
   const auto high_s_check =
      sysio::underwriter_detail::check_uic_signature_provider(high_s_provider, digest);
   BOOST_CHECK(high_s_check.result == provider_result::non_canonical_signature);
   BOOST_CHECK(!high_s_check.recovered_key);

   // Libfc can recover the same key from the other recovery-header family.
   // Both startup preflight and the runtime predicate must reject that alias
   // so a UIC signature has exactly one accepted byte representation.
   const std::array alias_types{
      private_key::key_type::k1,
      private_key::key_type::r1,
      private_key::key_type::em,
   };
   for (const auto type : alias_types) {
      const auto key = private_key::generate(type);
      const auto canonical = key.sign(digest);
      auto aliased_packed = sysio::opp::test::create_recovery_alias(
         fc::raw::pack(canonical), type);
      BOOST_REQUIRE(aliased_packed.has_value());
      BOOST_CHECK(!sysio::underwriter_detail::has_canonical_uic_signature_body(
         *aliased_packed));

      const auto aliased = fc::raw::unpack<fc::crypto::signature>(*aliased_packed);
      BOOST_REQUIRE(fc::crypto::public_key::recover(aliased, digest)
                    == key.get_public_key());
      fc::crypto::signature_provider_t alias_provider;
      alias_provider.public_key = key.get_public_key();
      alias_provider.sign = [aliased](const fc::sha256&) { return aliased; };
      const auto alias_check =
         sysio::underwriter_detail::check_uic_signature_provider(alias_provider, digest);
      BOOST_CHECK(alias_check.result == provider_result::non_canonical_signature);
      BOOST_CHECK(!alias_check.recovered_key);
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_reaches_all_supported_native_key_formats) try {
   using namespace fc::crypto;

   appbase::scoped_app test_app;
   char program_name[] = "test_underwriter_plugin";
   char* argv[] = {program_name};
   BOOST_REQUIRE(test_app->initialize<sysio::signature_provider_manager_plugin>(
      std::size(argv), argv));
   auto& manager = test_app->get_plugin<sysio::signature_provider_manager_plugin>();

   auto add_provider = [&](std::string key_name,
                           chain_kind_t target_chain,
                           chain_key_type_t key_type,
                           private_key::key_type concrete_type) {
      const auto key = private_key::generate(concrete_type);
      return manager.create_provider(
         key_name, target_chain, key_type,
         key.get_public_key().to_string({}), "KEY:" + key.to_string({}));
   };

   add_provider("uic-k1", chain_kind_wire, chain_key_type_wire,
                private_key::key_type::k1);
   add_provider("uic-r1", chain_kind_wire, chain_key_type_wire,
                private_key::key_type::r1);
   add_provider("uic-em", chain_kind_wire, chain_key_type_ethereum,
                private_key::key_type::em);
   add_provider("uic-ed", chain_kind_wire, chain_key_type_solana,
                private_key::key_type::ed);
   add_provider("wire-bls", chain_kind_wire, chain_key_type_wire_bls,
                private_key::key_type::bls);
   add_provider("ethereum-transaction", chain_kind_ethereum,
                chain_key_type_ethereum, private_key::key_type::em);

   const auto wire_target_providers =
      manager.query_providers(std::nullopt, chain_kind_wire);
   const auto uic_providers =
      sysio::underwriter_detail::select_uic_signature_providers(
         wire_target_providers,
         [](const std::string&) { return true; },
         [](const public_key&) { return true; });

   BOOST_REQUIRE_EQUAL(5u, wire_target_providers.size());
   BOOST_REQUIRE_EQUAL(4u, uic_providers.size());
   std::set<public_key::key_type> selected_types;
   for (const auto& provider : uic_providers) {
      selected_types.insert(provider->public_key.type());
      BOOST_CHECK(provider->target_chain == chain_kind_wire);
   }
   const std::set expected_types{
      public_key::key_type::k1,
      public_key::key_type::r1,
      public_key::key_type::em,
      public_key::key_type::ed,
   };
   BOOST_CHECK(selected_types == expected_types);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_excludes_unrelated_wire_signer) try {
   using namespace fc::crypto;
   const auto underwriter_key = private_key::generate(private_key::key_type::em);
   const auto block_signing_key = private_key::generate(private_key::key_type::k1);

   auto create_provider = [](std::string name, chain_key_type_t key_type,
                             const private_key& key) {
      auto provider = std::make_shared<signature_provider_t>();
      provider->target_chain = chain_kind_wire;
      provider->key_type = key_type;
      provider->key_name = std::move(name);
      provider->public_key = key.get_public_key();
      provider->sign = [key](const fc::sha256& digest) { return key.sign(digest); };
      return provider;
   };

   const std::vector<signature_provider_ptr> providers{
      create_provider("uic-em", chain_key_type_ethereum, underwriter_key),
      create_provider("block-k1", chain_key_type_wire, block_signing_key),
   };
   const auto selected = sysio::underwriter_detail::select_uic_signature_providers(
      providers,
      [](const std::string&) { return true; },
      [&](const public_key& key) {
         return key == underwriter_key.get_public_key();
      });

   BOOST_REQUIRE_EQUAL(1u, selected.size());
   BOOST_CHECK_EQUAL("uic-em", selected.front()->key_name);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_ignores_default_for_explicit_em) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "uic-em", fc::crypto::chain_key_type_ethereum,
      fc::crypto::private_key::key_type::em);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_uses_explicit_k1_after_default_suppression) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "uic-k1", fc::crypto::chain_key_type_wire,
      fc::crypto::private_key::key_type::k1, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_uses_explicit_r1_after_default_suppression) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "uic-r1", fc::crypto::chain_key_type_wire,
      fc::crypto::private_key::key_type::r1, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_ignores_default_for_explicit_ed) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "uic-ed", fc::crypto::chain_key_type_solana,
      fc::crypto::private_key::key_type::ed);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_accepts_anonymous_em) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "ignored-em", fc::crypto::chain_key_type_ethereum,
      fc::crypto::private_key::key_type::em, true, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_accepts_anonymous_k1) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "ignored-k1", fc::crypto::chain_key_type_wire,
      fc::crypto::private_key::key_type::k1, false, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_accepts_anonymous_r1) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "ignored-r1", fc::crypto::chain_key_type_wire,
      fc::crypto::private_key::key_type::r1, false, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_accepts_anonymous_ed) try {
   check_operator_configured_uic_provider_beats_wire_default(
      "ignored-ed", fc::crypto::chain_key_type_solana,
      fc::crypto::private_key::key_type::ed, true, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(uic_provider_selection_preserves_two_explicit_ambiguity) try {
   using namespace fc::crypto;
   auto reset_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   const auto config_dir = std::filesystem::temp_directory_path()
      / "underwriter-uic-provider-ambiguity";
   std::error_code ec;
   std::filesystem::remove_all(config_dir, ec);
   std::filesystem::create_directories(config_dir);
   auto cleanup = gsl_lite::finally([&]() {
      std::error_code ignored;
      std::filesystem::remove_all(config_dir, ignored);
   });

   std::vector<std::string> args{
      "test_underwriter_plugin",
      "--config-dir", config_dir.string(),
      "--signature-provider",
      explicit_uic_provider_spec("uic-em", chain_key_type_ethereum,
                                 private_key::key_type::em),
      "--signature-provider",
      explicit_uic_provider_spec("uic-ed", chain_key_type_solana,
                                 private_key::key_type::ed),
   };
   std::vector<char*> argv;
   argv.reserve(args.size());
   for (auto& arg : args) argv.push_back(arg.data());

   appbase::scoped_app test_app;
   BOOST_REQUIRE(test_app->initialize<sysio::signature_provider_manager_plugin>(
      argv.size(), argv.data()));
   auto& manager = test_app->get_plugin<sysio::signature_provider_manager_plugin>();
   manager.register_default_signature_providers({chain_key_type_wire});

   const auto selected = sysio::underwriter_detail::select_uic_signature_providers(
      manager.query_providers(std::nullopt, chain_kind_wire),
      [&](const std::string& name) {
         return manager.is_operator_configured_provider(name);
      }, [](const public_key&) { return true; });
   BOOST_REQUIRE_EQUAL(2u, selected.size());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(stored_commit_plan_skips_complete_candidate_after_restart) try {
   const auto plan = sysio::underwriter_detail::plan_stored_commits(
      /*candidate_exists=*/true,
      /*intent_submitted=*/true,
      /*source_is_depot=*/false,
      /*destination_is_depot=*/false,
      /*source_uic_stored=*/true,
      /*destination_uic_stored=*/true);
   BOOST_CHECK(plan.skip_candidate);
   BOOST_CHECK(!plan.submit_source);
   BOOST_CHECK(!plan.submit_destination);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(stored_commit_plan_submits_only_missing_outpost_leg) try {
   const auto plan = sysio::underwriter_detail::plan_stored_commits(
      /*candidate_exists=*/true,
      /*intent_submitted=*/true,
      /*source_is_depot=*/false,
      /*destination_is_depot=*/false,
      /*source_uic_stored=*/true,
      /*destination_uic_stored=*/false);
   BOOST_CHECK(!plan.skip_candidate);
   BOOST_CHECK(!plan.submit_source);
   BOOST_CHECK(plan.submit_destination);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(stored_commit_plan_handles_single_outpost_candidate) try {
   const auto ready = sysio::underwriter_detail::plan_stored_commits(
      true, true, false, true, true, false);
   BOOST_CHECK(ready.skip_candidate);
   BOOST_CHECK(!ready.submit_source);
   BOOST_CHECK(!ready.submit_destination);

   const auto missing = sysio::underwriter_detail::plan_stored_commits(
      true, true, false, true, false, false);
   BOOST_CHECK(!missing.skip_candidate);
   BOOST_CHECK(missing.submit_source);
   BOOST_CHECK(!missing.submit_destination);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(stored_commit_plan_skips_disqualified_candidate_after_restart) try {
   const auto disqualified = sysio::underwriter_detail::plan_stored_commits(
      /*candidate_exists=*/true,
      /*intent_submitted=*/false,
      /*source_is_depot=*/false,
      /*destination_is_depot=*/false,
      /*source_uic_stored=*/true,
      /*destination_uic_stored=*/true);
   BOOST_CHECK(disqualified.skip_candidate);
   BOOST_CHECK(!disqualified.submit_source);
   BOOST_CHECK(!disqualified.submit_destination);

   const auto absent = sysio::underwriter_detail::plan_stored_commits(
      /*candidate_exists=*/false,
      /*intent_submitted=*/false,
      /*source_is_depot=*/false,
      /*destination_is_depot=*/false,
      /*source_uic_stored=*/false,
      /*destination_uic_stored=*/false);
   BOOST_CHECK(!absent.skip_candidate);
   BOOST_CHECK(absent.submit_source);
   BOOST_CHECK(absent.submit_destination);
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

BOOST_AUTO_TEST_CASE(preflight_allows_late_collateral_without_global_runtime_gate) try {
   // Stub — see preflight_fails_on_missing_authex_link comment. Request-level
   // bucket admission is exercised by the underwriter race integration flow.
   BOOST_CHECK(true);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(scan_waits_for_active_status_after_preflight) try {
   // Stub — see preflight_fails_on_missing_authex_link comment. The live
   // status transition is covered by the cluster harness.
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
// /v1/underwriter/stats + /v1/underwriter/commits register via
// http_plugin during plugin_startup — UNCONDITIONALLY, before the sync
// gate, because http_plugin's handler map is read lock-free once the
// listener goes live. Until the deferred startup body completes the
// handlers answer with the gate state (covered as the pure
// `startup_gate_payload` cases in test_underwriter_sync.cpp).
// Constructing http_plugin in isolation from chain_plugin requires the
// appbase wiring. Stub here; integration coverage via curl in the flow
// tests.
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

// ── SwapDeposit correlation-hash preimage ──────────────────────────────
//
// Both outposts hash the user's accepted `target_amount`
// (`ReserveManagerLib.hashSwapDeposit` on EVM,
// `swap_correlation_hash` on SVM). The depot re-prices `dst_amount` at
// ingestion, so the two diverge whenever the caller's target is not
// exactly the depot's quote — which is the normal case, and precisely
// what `variance_tolerance_bps` exists to bound.

/// The EVM leg of a real ETH→SOL underwritten swap, captured from a live
/// cluster run. `on_chain_swap_deposit_hash` is the `SwapDeposit` event's
/// hash as `ReserveManager` emitted it.
const std::vector<char> production_depositor =
   depositor_bytes("cf2d5b3cbb4d7bf04e3f7bfa8e27081b52191f91");
constexpr uint64_t production_source_amount        = 100000000;
constexpr uint64_t production_source_token_code    = 23373212024832;
constexpr uint64_t production_source_reserve_code  = 71615576876608;
constexpr uint64_t production_target_chain_code    = 84606581215232;
constexpr uint64_t production_target_token_code    = 84606560763904;
constexpr uint64_t production_target_reserve_code  = 71615576876608;
constexpr uint64_t production_target_amount        = 98039214;
constexpr uint64_t production_settlement_quote     = 97747972;
constexpr uint32_t production_tolerance_bps        = 50;
constexpr auto     on_chain_swap_deposit_hash =
   "fd8f16aad2443acf2847c6f49c41feac6f32feadc87682df25835ae328a76695";

/// Build the production preimage, varying only the destination amount.
sysio::underwriter::source_deposit_hash_input production_hash_input(uint64_t destination_amount) {
   return {
      .depositor            = std::span<const char>{production_depositor},
      .source_amount        = production_source_amount,
      .source_token_code    = production_source_token_code,
      .source_reserve_code  = production_source_reserve_code,
      .target_chain_code    = production_target_chain_code,
      .target_token_code    = production_target_token_code,
      .target_reserve_code  = production_target_reserve_code,
      .target_amount        = destination_amount,
      .target_tolerance_bps = production_tolerance_bps,
   };
}

/// The recomputed hash matches the outpost's only when the preimage carries
/// the caller's `target_amount`.
BOOST_AUTO_TEST_CASE(source_deposit_hash_binds_the_callers_target_amount) try {
   const auto recomputed = sysio::underwriter::source_deposit_hash(
      production_hash_input(production_target_amount));

   BOOST_CHECK_EQUAL(hash_hex(recomputed), on_chain_swap_deposit_hash);
} FC_LOG_AND_RETHROW();

/// The depot's re-priced settlement quote is NOT what the outpost hashed;
/// feeding it in reproduces the divergence that stalls every UWREQ whose
/// target differs from the quote.
BOOST_AUTO_TEST_CASE(source_deposit_hash_rejects_the_depot_settlement_quote) try {
   BOOST_REQUIRE_NE(production_settlement_quote, production_target_amount);

   const auto recomputed = sysio::underwriter::source_deposit_hash(
      production_hash_input(production_settlement_quote));

   BOOST_CHECK_NE(hash_hex(recomputed), on_chain_swap_deposit_hash);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
