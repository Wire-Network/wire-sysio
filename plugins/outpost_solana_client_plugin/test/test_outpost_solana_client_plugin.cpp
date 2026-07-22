#include <boost/test/unit_test.hpp>

#include <fc-test/build_info.hpp>
#include <fc-test/crypto_utils.hpp>
#include <fc-test/one_shot_http_server.hpp>
#include <fc/io/json.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_borsh.hpp>

#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>
#include <sysio.msgch/solana_terminal_budget.hpp>

#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

#include <algorithm>
#include <filesystem>
#include <map>

using namespace std::literals;
using namespace fc::network::solana;

namespace {

constexpr std::string_view counter_anchor_idl_fixture = "solana-idl-counter-anchor.json";
constexpr std::string_view opp_outpost_idl_fixture = "solana-idl-opp-outpost-stub.json";
constexpr std::string_view sec94_terminal_budget_fixture = "sec-94-solana-terminal-budget.json";
constexpr std::string_view test_genesis_hash = "11111111111111111111111111111111";
constexpr std::string_view different_genesis_hash = "SysvarC1ock11111111111111111111111111111111";

/** Build a one-shot JSON-RPC endpoint returning the supplied genesis hash. */
fc::test::one_shot_http_server genesis_hash_rpc_server(
   std::string genesis_hash = std::string(test_genesis_hash)) {
   return fc::test::one_shot_http_server{
      R"json({"jsonrpc":"2.0","id":1,"result":")json" + genesis_hash + R"json("})json",
      "getGenesisHash"};
}

/** Build a named Solana signature-provider spec from the canonical fixture. */
std::string named_solana_signature_provider(
   std::string name = "signer-a",
   fc::crypto::chain_kind_t chain_kind = fc::crypto::chain_kind_solana) {
   const auto fixture = fc::test::load_keygen_fixture("solana", 1);
   return fc::crypto::to_signature_provider_spec(
      name,
      chain_kind,
      fixture.chain_key_type,
      fixture.public_key,
      fc::test::to_private_key_spec(fixture.private_key));
}

/** Build a provider with Solana targeting but a valid non-Solana key type. */
std::string solana_target_with_wire_key_provider() {
   const auto fixture = fc::test::load_keygen_fixture("wire", 1);
   return fc::crypto::to_signature_provider_spec(
      "signer-a",
      fc::crypto::chain_kind_solana,
      fixture.chain_key_type,
      fixture.public_key,
      fc::test::to_private_key_spec(fixture.private_key));
}

/** Initialize the complete Solana outpost plugin with supplied configuration arguments. */
void initialize_outpost_plugin(const std::vector<std::string>& configuration_arguments) {
   appbase::scoped_app test_application{};
   std::vector<std::string> arguments{"test_outpost_solana_client_plugin"};
   arguments.insert(arguments.end(), configuration_arguments.begin(), configuration_arguments.end());

   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) {
      argv.emplace_back(argument.data());
   }

   BOOST_REQUIRE(test_application->initialize<sysio::outpost_solana_client_plugin>(argv.size(), argv.data()));
}

/// Measured legacy transaction dimensions for a terminal Solana `epoch_in` call.
struct terminal_tx_measurement {
   size_t declared_idl_accounts = 0;
   size_t instruction_data_bytes = 0;
   size_t required_signatures = 0;
   size_t legacy_account_keys = 0;
   size_t loaded_accounts = 0;
   size_t packet_bytes = 0;
};

/// Load an Anchor IDL fixture from the libfc test fixture directory.
idl::program load_idl_fixture(std::string_view filename) {
   auto path = fc::test::get_test_fixtures_path() / boost::filesystem::path(filename);
   return idl::parse_idl_file(path.generic_string());
}

/// Load a JSON fixture from the libfc test fixture directory.
fc::variant load_json_fixture(std::string_view filename) {
   auto path = fc::test::get_test_fixtures_path() / boost::filesystem::path(filename);
   return fc::json::from_file(std::filesystem::path(path.generic_string()));
}

/// Create deterministic placeholder keys for transaction-size measurements.
solana_public_key measurement_pubkey(uint32_t seed) {
   solana_public_key key;
   std::ranges::fill(key._data, 0);
   key._data[0] = static_cast<uint8_t>(seed & 0xff);
   key._data[1] = static_cast<uint8_t>((seed >> 8) & 0xff);
   key._data[2] = static_cast<uint8_t>((seed >> 16) & 0xff);
   key._data[3] = static_cast<uint8_t>((seed >> 24) & 0xff);
   return key;
}

/// Append an unsigned 16-bit little-endian integer to an instruction buffer.
void write_u16_le(std::vector<uint8_t>& out, uint16_t value) {
   out.push_back(static_cast<uint8_t>(value & 0xff));
   out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

/// Append an unsigned 32-bit little-endian integer to an instruction buffer.
void write_u32_le(std::vector<uint8_t>& out, uint32_t value) {
   out.push_back(static_cast<uint8_t>(value & 0xff));
   out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
   out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
   out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

/// Encode `epoch_in` args used by the SEC-94 packet-budget fixture.
std::vector<uint8_t> epoch_in_data(const idl::instruction& instr,
                                   const fc::variant_object& args) {
   std::vector<uint8_t> data;
   data.reserve(instr.discriminator.size() + 16 + args["chunk_data_bytes"].as_uint64());
   data.insert(data.end(), instr.discriminator.begin(), instr.discriminator.end());
   write_u32_le(data, static_cast<uint32_t>(args["epoch_index"].as_uint64()));
   write_u16_le(data, static_cast<uint16_t>(args["chunk_index"].as_uint64()));
   write_u16_le(data, static_cast<uint16_t>(args["total_chunks"].as_uint64()));
   write_u32_le(data, static_cast<uint32_t>(args["total_bytes"].as_uint64()));
   write_u32_le(data, static_cast<uint32_t>(args["chunk_data_bytes"].as_uint64()));
   data.resize(data.size() + static_cast<size_t>(args["chunk_data_bytes"].as_uint64()));
   return data;
}

/// Build the IDL-declared static account metas for terminal `epoch_in`.
std::vector<account_meta> terminal_static_accounts(const idl::instruction& instr,
                                                   const solana_public_key& fee_payer) {
   std::vector<account_meta> accounts;
   accounts.reserve(instr.accounts.size());
   for (size_t i = 0; i < instr.accounts.size(); ++i) {
      const auto& acct = instr.accounts[i];
      solana_public_key key;
      if (acct.name == "operator") {
         key = fee_payer;
      } else if (acct.name == "system_program") {
         key = system::program_ids::SYSTEM_PROGRAM;
      } else {
         key = measurement_pubkey(static_cast<uint32_t>(100 + i));
      }

      if (acct.is_signer) {
         accounts.push_back(account_meta::signer(key, acct.is_mut));
      } else if (acct.is_mut) {
         accounts.push_back(account_meta::writable(key, false));
      } else {
         accounts.push_back(account_meta::readonly(key, false));
      }
   }
   return accounts;
}

/// Build a legacy Solana transaction using the same account ordering rules as the client.
transaction build_measured_legacy_transaction(const std::vector<instruction>& instructions,
                                              const solana_public_key& fee_payer) {
   transaction tx;
   tx.msg.recent_blockhash = measurement_pubkey(2);

   std::vector<account_meta> all_accounts;
   all_accounts.push_back(account_meta::signer(fee_payer, true));

   auto add_account = [&](const account_meta& meta) {
      auto it = std::find_if(all_accounts.begin(), all_accounts.end(), [&](const auto& existing) {
         return existing.key == meta.key;
      });
      if (it == all_accounts.end()) {
         all_accounts.push_back(meta);
         return;
      }
      it->is_signer = it->is_signer || meta.is_signer;
      it->is_writable = it->is_writable || meta.is_writable;
   };

   for (const auto& instr : instructions) {
      for (const auto& meta : instr.accounts) {
         add_account(meta);
      }
      add_account(account_meta::readonly(instr.program_id, false));
   }

   std::vector<account_meta> writable_signers;
   std::vector<account_meta> readonly_signers;
   std::vector<account_meta> writable_non_signers;
   std::vector<account_meta> readonly_non_signers;
   for (const auto& meta : all_accounts) {
      if (meta.is_signer) {
         (meta.is_writable ? writable_signers : readonly_signers).push_back(meta);
      } else {
         (meta.is_writable ? writable_non_signers : readonly_non_signers).push_back(meta);
      }
   }

   auto append_keys = [&](const std::vector<account_meta>& metas) {
      for (const auto& meta : metas) {
         tx.msg.account_keys.push_back(meta.key);
      }
   };
   append_keys(writable_signers);
   append_keys(readonly_signers);
   append_keys(writable_non_signers);
   append_keys(readonly_non_signers);

   tx.msg.header.num_required_signatures =
      static_cast<uint8_t>(writable_signers.size() + readonly_signers.size());
   tx.msg.header.num_readonly_signed_accounts = static_cast<uint8_t>(readonly_signers.size());
   tx.msg.header.num_readonly_unsigned_accounts = static_cast<uint8_t>(readonly_non_signers.size());

   std::map<solana_public_key, size_t> key_index_map;
   for (size_t i = 0; i < tx.msg.account_keys.size(); ++i) {
      key_index_map[tx.msg.account_keys[i]] = i;
   }

   for (const auto& instr : instructions) {
      compiled_instruction compiled;
      compiled.program_id_index = static_cast<uint8_t>(key_index_map.at(instr.program_id));
      for (const auto& meta : instr.accounts) {
         compiled.account_indices.push_back(static_cast<uint8_t>(key_index_map.at(meta.key)));
      }
      compiled.data = instr.data;
      tx.msg.instructions.push_back(std::move(compiled));
   }

   tx.signatures.resize(tx.msg.header.num_required_signatures);
   validate_legacy_transaction(tx);
   return tx;
}

/// Measure terminal `epoch_in` with distinct dynamic `remaining_accounts`.
terminal_tx_measurement measure_terminal_epoch_in_transaction(const idl::instruction& instr,
                                                              const fc::variant_object& fixture,
                                                              size_t dynamic_accounts) {
   const auto fee_payer = measurement_pubkey(1);
   auto accounts = terminal_static_accounts(instr, fee_payer);
   accounts.reserve(accounts.size() + dynamic_accounts);
   for (size_t i = 0; i < dynamic_accounts; ++i) {
      accounts.push_back(account_meta::writable(measurement_pubkey(static_cast<uint32_t>(1'000 + i)), false));
   }

   const auto program_id = solana_public_key::from_base58_string(fixture["program_id"].as_string());
   const auto heap_bytes =
      fixture["terminal_pre_instructions"].get_array().front()["bytes"].as_uint64();
   std::vector<instruction> instructions = {
      system::compute_budget::request_heap_frame(static_cast<uint32_t>(heap_bytes)),
      instruction{program_id, std::move(accounts), epoch_in_data(instr, fixture["terminal_args"].get_object())},
   };

   auto tx = build_measured_legacy_transaction(instructions, fee_payer);
   const auto packet = tx.serialize();
   return terminal_tx_measurement{
      .declared_idl_accounts = instr.accounts.size(),
      .instruction_data_bytes = instructions.back().data.size(),
      .required_signatures = tx.msg.header.num_required_signatures,
      .legacy_account_keys = tx.msg.account_keys.size(),
      .loaded_accounts = tx.msg.account_keys.size(),
      .packet_bytes = packet.size(),
   };
}

/// Measure a non-terminal data chunk with no ComputeBudget pre-instruction.
terminal_tx_measurement measure_data_chunk_epoch_in_transaction(const idl::instruction& instr,
                                                                const fc::variant_object& fixture) {
   const auto fee_payer = measurement_pubkey(1);
   auto accounts = terminal_static_accounts(instr, fee_payer);
   const auto program_id = solana_public_key::from_base58_string(fixture["program_id"].as_string());
   const auto& data_chunk = fixture["data_chunk"].get_object();

   std::vector<instruction> instructions = {
      instruction{program_id, std::move(accounts), epoch_in_data(instr, data_chunk["args"].get_object())},
   };

   auto tx = build_measured_legacy_transaction(instructions, fee_payer);
   const auto packet = tx.serialize();
   return terminal_tx_measurement{
      .declared_idl_accounts = instr.accounts.size(),
      .instruction_data_bytes = instructions.back().data.size(),
      .required_signatures = tx.msg.header.num_required_signatures,
      .legacy_account_keys = tx.msg.account_keys.size(),
      .loaded_accounts = tx.msg.account_keys.size(),
      .packet_bytes = packet.size(),
   };
}

/// Assert that a measured transaction row exactly matches the JSON fixture.
void check_measurement_matches_fixture(const terminal_tx_measurement& measured,
                                       const fc::variant_object& expected) {
   if (expected.contains("declared_idl_accounts")) {
      BOOST_CHECK_EQUAL(measured.declared_idl_accounts, expected["declared_idl_accounts"].as_uint64());
   }
   if (expected.contains("instruction_data_bytes")) {
      BOOST_CHECK_EQUAL(measured.instruction_data_bytes, expected["instruction_data_bytes"].as_uint64());
   }
   if (expected.contains("required_signatures")) {
      BOOST_CHECK_EQUAL(measured.required_signatures, expected["required_signatures"].as_uint64());
   }
   BOOST_CHECK_EQUAL(measured.legacy_account_keys, expected["legacy_account_keys"].as_uint64());
   BOOST_CHECK_EQUAL(measured.loaded_accounts, expected["loaded_accounts"].as_uint64());
   BOOST_CHECK_EQUAL(measured.packet_bytes, expected["packet_bytes"].as_uint64());
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(outpost_solana_client_plugin)

// ---------------------------------------------------------------------------
//  Startup configuration validation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(startup_accepts_matching_named_signer_and_genesis_hash) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url() + "," + std::string(test_genesis_hash),
   }));
}

BOOST_AUTO_TEST_CASE(startup_validates_rpc_without_configured_genesis_hash) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url(),
   }));
}

BOOST_AUTO_TEST_CASE(startup_rejects_client_without_matching_named_signature_provider) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider("other-signer"),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url() + "," + std::string(test_genesis_hash),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_anonymous_signature_provider_reference) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(""),
      "--outpost-solana-client",
      "client-a,key-0," + rpc_server.url() + "," + std::string(test_genesis_hash),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_named_signer_for_wrong_chain) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider("signer-a", fc::crypto::chain_kind_wire),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url() + "," + std::string(test_genesis_hash),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_named_signer_with_wrong_key_type) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      solana_target_with_wire_key_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url() + "," + std::string(test_genesis_hash),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_genesis_hash_mismatch_with_rpc_endpoint) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url() + "," + std::string(different_genesis_hash),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_unavailable_solana_rpc) {
   fc::test::connection_closing_http_server rpc_server;
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url(),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_invalid_remote_genesis_hash) {
   auto rpc_server = genesis_hash_rpc_server("not-a-genesis-hash");
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url(),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_invalid_configured_genesis_hash) {
   auto rpc_server = genesis_hash_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + rpc_server.url() + ",not-a-genesis-hash",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(can_load_counter_anchor_idl) try {
   auto prog = load_idl_fixture(counter_anchor_idl_fixture);

   bool has_initialize = false, has_increment = false;
   for (auto& instr : prog.instructions) {
      if (instr.name == "initialize") has_initialize = true;
      if (instr.name == "increment") has_increment = true;
   }
   BOOST_CHECK(has_initialize);
   BOOST_CHECK(has_increment);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(can_load_opp_outpost_idl) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   bool has_epoch_in = false, has_emit = false;
   for (auto& instr : prog.instructions) {
      if (instr.name == "epoch_in") has_epoch_in = true;
      if (instr.name == "emit_outbound_envelope") has_emit = true;
   }
   BOOST_CHECK(has_epoch_in);
   BOOST_CHECK(has_emit);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(filter_outpost_program_idls_selects_configured_name) try {
   std::vector<std::pair<std::filesystem::path, std::vector<idl::program>>> idl_files;
   idl_files.emplace_back("counter.json",
                          std::vector<idl::program>{load_idl_fixture(counter_anchor_idl_fixture)});
   idl_files.emplace_back("outpost.json",
                          std::vector<idl::program>{load_idl_fixture(opp_outpost_idl_fixture)});

   // The default constant selects the standalone outpost program's IDL only.
   auto defaults =
      sysio::filter_outpost_program_idls(idl_files, sysio::OPP_SOLANA_OUTPOST_PROGRAM_NAME);
   BOOST_REQUIRE_EQUAL(defaults.size(), 1u);
   BOOST_CHECK_EQUAL(defaults.front().name, "opp_outpost");

   // A configured `--solana-outpost-program-name` override (e.g. liqsol_core
   // hosting the outpost interface) selects by the overridden name instead.
   auto counter = sysio::filter_outpost_program_idls(idl_files, "counter_anchor");
   BOOST_REQUIRE_EQUAL(counter.size(), 1u);
   BOOST_CHECK_EQUAL(counter.front().name, "counter_anchor");

   // No loaded IDL with the configured name -> empty (create_outpost_client
   // FC_ASSERTs on this with a remediation message).
   BOOST_CHECK(sysio::filter_outpost_program_idls(idl_files, "liqsol_core").empty());
   BOOST_CHECK(sysio::filter_outpost_program_idls({}, sysio::OPP_SOLANA_OUTPOST_PROGRAM_NAME).empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(outpost_program_name_default_is_opp_outpost) try {
   // The option default is back-compat load-bearing: pre-cleanroom deployments
   // pass no --solana-outpost-program-name and must keep matching opp_outpost.
   BOOST_CHECK_EQUAL(std::string{sysio::OPP_SOLANA_OUTPOST_PROGRAM_NAME}, "opp_outpost");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(resolve_token_custody_reads_outpost_config_maps) try {
   using sysio::outpost_solana_client_detail::resolve_token_custody;

   const auto spl_mint      = measurement_pubkey(77);
   const auto native_marker = system::program_ids::SYSTEM_PROGRAM;

   fc::variants addresses;
   addresses.push_back(fc::mutable_variant_object("token_code", 111)(
      "mint", spl_mint.to_string(fc::yield_function_t{})));
   addresses.push_back(fc::mutable_variant_object("token_code", 222)(
      "mint", native_marker.to_string(fc::yield_function_t{})));
   fc::variants precisions;
   precisions.push_back(fc::mutable_variant_object("token_code", 111)("decimals", 6));
   precisions.push_back(fc::mutable_variant_object("token_code", 222)("decimals", 9));

   const fc::variant_object config =
      fc::mutable_variant_object("token_addresses_by_code", std::move(addresses))(
         "precision_by_token_code", std::move(precisions));

   // SPL binding: configured mint + configured decimals.
   const auto spl = resolve_token_custody(config, 111);
   BOOST_CHECK(spl.mint == spl_mint);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(spl.decimals), 6u);

   // Explicit zero-mint binding (the harness registers native SOL this way):
   // native custody + its configured precision.
   const auto native = resolve_token_custody(config, 222);
   BOOST_CHECK(native.mint == native_marker);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(native.decimals), 9u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(resolve_token_custody_requires_both_config_entries) try {
   using sysio::outpost_solana_client_detail::resolve_token_custody;

   const auto spl_mint = measurement_pubkey(78);
   fc::variants addresses;
   addresses.push_back(fc::mutable_variant_object("token_code", 111)(
      "mint", spl_mint.to_string(fc::yield_function_t{})));

   // Address bound but precision missing -> throws (wire-ethereum
   // WIRE_TokenPrecisionUnset parity; program PrecisionUnconfigured parity).
   const fc::variant_object address_only =
      fc::mutable_variant_object("token_addresses_by_code", addresses)(
         "precision_by_token_code", fc::variants{});
   BOOST_CHECK_THROW(resolve_token_custody(address_only, 111), fc::assert_exception);

   // Unknown token_code entirely -> throws on the address requirement.
   BOOST_CHECK_THROW(resolve_token_custody(address_only, 999), fc::assert_exception);

   // Config carrying no maps at all -> throws.
   BOOST_CHECK_THROW(
      resolve_token_custody(fc::variant_object{fc::mutable_variant_object{}}, 111),
      fc::assert_exception);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(opp_outpost_epoch_in_has_chunked_args) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   const idl::instruction* epoch_in = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "epoch_in") { epoch_in = &instr; break; }
   }
   BOOST_REQUIRE(epoch_in != nullptr);

   // Chunked signature: (epoch_index, chunk_index, total_chunks, total_bytes,
   // chunk_data). Solana's 1 232-byte tx MTU forces multi-call streaming for
   // production-scale envelopes; the program assembles per-(epoch, signer)
   // staging PDAs and finalizes on a zero-data terminal call where
   // chunk_index == total_chunks.
   BOOST_REQUIRE_EQUAL(epoch_in->args.size(), 5u);
   BOOST_CHECK_EQUAL(epoch_in->args[0].name, "epoch_index");
   BOOST_CHECK_EQUAL(epoch_in->args[1].name, "chunk_index");
   BOOST_CHECK_EQUAL(epoch_in->args[2].name, "total_chunks");
   BOOST_CHECK_EQUAL(epoch_in->args[3].name, "total_bytes");
   BOOST_CHECK_EQUAL(epoch_in->args[4].name, "chunk_data");

   // Accounts: operator (signer), config, operator_registry, epoch_deliveries,
   //           chunk_buffer, inbound_envelopes, outbound emit state, vault,
   //           reserve_aggregate, system_program.
   BOOST_CHECK_EQUAL(epoch_in->accounts.size(), 12u);
   BOOST_CHECK(epoch_in->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(epoch_in->accounts[4].name, "chunk_buffer");
   BOOST_CHECK_EQUAL(epoch_in->accounts[6].name, "outbound_message_buffer");
   BOOST_CHECK_EQUAL(epoch_in->accounts[8].name, "latest_outbound_envelope");
   BOOST_CHECK_EQUAL(epoch_in->accounts[10].name, "reserve_aggregate");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sec94_terminal_budget_fixture_matches_contract_estimator) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   auto fixture = load_json_fixture(sec94_terminal_budget_fixture).get_object();

   const idl::instruction* epoch_in = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "epoch_in") { epoch_in = &instr; break; }
   }
   BOOST_REQUIRE(epoch_in != nullptr);

   BOOST_REQUIRE_EQUAL(fixture["legacy_packet_limit_bytes"].as_uint64(),
                       limits::PACKET_DATA_SIZE);
   BOOST_REQUIRE_EQUAL(fixture["legacy_packet_limit_bytes"].as_uint64(),
                       sysio::msgch_svm_terminal_budget::SVM_TERMINAL_PACKET_LIMIT_BYTES);
   BOOST_REQUIRE_EQUAL(fixture["runtime_account_limit"].as_uint64(),
                       sysio::msgch_svm_terminal_budget::SVM_TERMINAL_RUNTIME_ACCOUNT_LIMIT);
   BOOST_REQUIRE_EQUAL(fixture["legacy_account_key_limit"].as_uint64(),
                       limits::LEGACY_ACCOUNT_KEY_LIMIT);
   BOOST_REQUIRE_EQUAL(fixture["legacy_account_key_limit"].as_uint64(),
                       sysio::msgch_svm_terminal_budget::SVM_TERMINAL_ACCOUNT_KEY_LIMIT);

   const auto data_chunk_measured = measure_data_chunk_epoch_in_transaction(*epoch_in, fixture);
   check_measurement_matches_fixture(data_chunk_measured, fixture["data_chunk"].get_object());
   // Data chunks do not carry terminal remaining_accounts. The hard invariant
   // is that the measured full data-chunk packet fits Solana's raw MTU; future
   // static IDL/account-list drift must update this fixture and keep fitting.
   BOOST_CHECK_LE(data_chunk_measured.packet_bytes,
                  sysio::msgch_svm_terminal_budget::SVM_TERMINAL_PACKET_LIMIT_BYTES);

   const auto static_measured = measure_terminal_epoch_in_transaction(*epoch_in, fixture, 0);
   check_measurement_matches_fixture(static_measured, fixture["static"].get_object());
   BOOST_CHECK_LE(static_measured.packet_bytes,
                  sysio::msgch_svm_terminal_budget::SVM_TERMINAL_STATIC_PACKET_BYTES_WITH_MARGIN);
   BOOST_CHECK_LE(static_measured.legacy_account_keys,
                  sysio::msgch_svm_terminal_budget::SVM_TERMINAL_STATIC_ACCOUNT_KEYS);
   BOOST_CHECK_LE(static_measured.loaded_accounts,
                  sysio::msgch_svm_terminal_budget::SVM_TERMINAL_STATIC_LOADED_ACCOUNTS);
   BOOST_CHECK_EQUAL(sysio::msgch_svm_terminal_budget::svm_hard_dynamic_account_budget(), 16u);

   for (const auto& entry : fixture["cases"].get_array()) {
      const auto& test_case = entry.get_object();
      const auto dynamic_accounts = static_cast<size_t>(test_case["dynamic_remaining_accounts"].as_uint64());
      const auto measured = measure_terminal_epoch_in_transaction(*epoch_in, fixture, dynamic_accounts);
      check_measurement_matches_fixture(measured, test_case);
      BOOST_CHECK(sysio::msgch_svm_terminal_budget::svm_terminal_budget_fits(dynamic_accounts));
      BOOST_CHECK_LE(measured.packet_bytes,
                     sysio::msgch_svm_terminal_budget::svm_estimated_terminal_packet_bytes(dynamic_accounts));
      BOOST_CHECK_LE(measured.packet_bytes,
                     sysio::msgch_svm_terminal_budget::SVM_TERMINAL_PACKET_BUDGET_BYTES);
      BOOST_CHECK_LE(measured.loaded_accounts,
                     sysio::msgch_svm_terminal_budget::SVM_TERMINAL_STATIC_LOADED_ACCOUNTS + dynamic_accounts);
      BOOST_CHECK_LE(measured.legacy_account_keys,
                     sysio::msgch_svm_terminal_budget::SVM_TERMINAL_STATIC_ACCOUNT_KEYS + dynamic_accounts);
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(opp_outpost_cleanup_envelope_chunks_present) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   const idl::instruction* cleanup = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "cleanup_envelope_chunks") { cleanup = &instr; break; }
   }
   BOOST_REQUIRE(cleanup != nullptr);
   BOOST_REQUIRE_EQUAL(cleanup->args.size(), 1u);
   BOOST_CHECK_EQUAL(cleanup->args[0].name, "epoch_index");

   // Accounts: reaper (signer), config, latest_outbound_envelope,
   //           chunk_buffer, uploader.
   BOOST_REQUIRE_EQUAL(cleanup->accounts.size(), 5u);
   BOOST_CHECK_EQUAL(cleanup->accounts[0].name, "reaper");
   BOOST_CHECK(cleanup->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(cleanup->accounts[2].name, "latest_outbound_envelope");
   BOOST_CHECK_EQUAL(cleanup->accounts[3].name, "chunk_buffer");
   BOOST_CHECK_EQUAL(cleanup->accounts[4].name, "uploader");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(envelope_chunk_count_math) try {
   // The relay derives `total_chunks` as `ceil(total / MAX_CHUNK_BYTES)`.
   // Verify the arithmetic at sentinel sizes: empty (rejected by the relay
   // before reaching this math), single-chunk, exact multiple, the captured
   // dev-026 production envelope, and the upper boundary.

   auto chunks_for = [](size_t total) {
      return (total + sysio::SOLANA_MAX_CHUNK_BYTES - 1) / sysio::SOLANA_MAX_CHUNK_BYTES;
   };
   auto epoch_in_calls_for = [&](size_t total) {
      return chunks_for(total) + 1; // data chunks plus zero-data terminal finalize
   };

   BOOST_CHECK_EQUAL(chunks_for(1),                                 1u);
   BOOST_CHECK_EQUAL(epoch_in_calls_for(1),                          2u);
   BOOST_CHECK_EQUAL(chunks_for(sysio::SOLANA_MAX_CHUNK_BYTES),     1u);
   BOOST_CHECK_EQUAL(epoch_in_calls_for(sysio::SOLANA_MAX_CHUNK_BYTES), 2u);
   BOOST_CHECK_EQUAL(chunks_for(sysio::SOLANA_MAX_CHUNK_BYTES + 1), 2u);
   BOOST_CHECK_EQUAL(epoch_in_calls_for(sysio::SOLANA_MAX_CHUNK_BYTES + 1), 3u);
   BOOST_CHECK_EQUAL(chunks_for(2 * sysio::SOLANA_MAX_CHUNK_BYTES), 2u);
   // dev-026 captured 2,526-byte envelope (groups-of-7 batch op delivery).
   BOOST_CHECK_EQUAL(chunks_for(2526), 4u);   // 2526/672 = 3.75 -> 4
   // 64 KiB cap: ceil(65 536 / 672) = 98 chunks. Last chunk is 352 B
   // (65_536 mod 672 = 352), the first 97 are full at MAX_CHUNK_BYTES.
   BOOST_CHECK_EQUAL(chunks_for(sysio::SOLANA_MAX_ENVELOPE_BYTES), 98u);
   BOOST_CHECK_EQUAL(epoch_in_calls_for(sysio::SOLANA_MAX_ENVELOPE_BYTES), 99u);
   BOOST_CHECK_EQUAL(sysio::SOLANA_MAX_ENVELOPE_BYTES % sysio::SOLANA_MAX_CHUNK_BYTES, 352u);

   // Last-chunk size at the dev-026 reproduction: the loop fills the first
   // 3 chunks at MAX_CHUNK_BYTES (= 672) and the last with the remainder.
   const size_t last_chunk_size = 2526 - 3 * sysio::SOLANA_MAX_CHUNK_BYTES;
   BOOST_CHECK_EQUAL(last_chunk_size, 510u);   // 2526 - 2016 = 510
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(opp_outpost_emit_has_wire_epoch_arg) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   const idl::instruction* emit = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "emit_outbound_envelope") { emit = &instr; break; }
   }
   BOOST_REQUIRE(emit != nullptr);
   BOOST_CHECK_EQUAL(emit->args.size(), 1u);
   BOOST_CHECK_EQUAL(emit->args[0].name, "wire_epoch_index");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(opp_outpost_has_initialize_and_add_attestation) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   bool has_initialize = false, has_add = false, has_deposit = false;
   for (auto& instr : prog.instructions) {
      if (instr.name == "initialize")       has_initialize = true;
      if (instr.name == "add_attestation")  has_add        = true;
      if (instr.name == "deposit")          has_deposit    = true;
   }
   BOOST_CHECK(has_initialize);
   BOOST_CHECK(has_add);
   BOOST_CHECK(has_deposit);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(borsh_encode_u32_roundtrip) try {
   borsh::encoder enc;
   enc.write_u32(42);
   BOOST_CHECK_EQUAL(enc.data().size(), 4u);

   borsh::decoder dec(enc.data());
   uint32_t val = dec.read_u32();
   BOOST_CHECK_EQUAL(val, 42u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(borsh_encode_bytes_roundtrip) try {
   std::vector<uint8_t> test_data = {0x12, 0x0c, 0x0a, 0x04, 0x08};
   borsh::encoder enc;
   enc.write_bytes(test_data);

   // Borsh bytes: 4-byte LE length prefix + data
   BOOST_CHECK_EQUAL(enc.data().size(), 4 + test_data.size());

   borsh::decoder dec(enc.data());
   auto decoded = dec.read_bytes();
   BOOST_CHECK(decoded == test_data);
} FC_LOG_AND_RETHROW();

// ── extract_inbound_recipient_pubkeys: envelope-decode + remit/revert
//    pubkey extraction. Verifies the cranker enhancement that walks an
//    inbound envelope's attestations and surfaces the operator /
//    depositor SOL pubkeys that `epoch_in` must declare in its
//    `remaining_accounts` so the on-chain WITHDRAW_REMIT /
//    DEPOSIT_REVERT handlers can do their CPI transfers immediately.

namespace {

/// Build a 32-byte SOLANA `ChainAddress` carrying `pk_bytes` verbatim.
sysio::opp::types::ChainAddress make_sol_addr(const std::array<uint8_t, 32>& pk_bytes) {
   sysio::opp::types::ChainAddress addr;
   addr.set_kind(sysio::opp::types::CHAIN_KIND_SVM);
   addr.set_address(pk_bytes.data(), pk_bytes.size());
   return addr;
}

/// Build a 32-byte ETHEREUM `ChainAddress` (32-byte length is a SOL
/// pubkey, but the kind flag is ETH — the helper must reject this
/// shape rather than misinterpret it).
sysio::opp::types::ChainAddress make_eth_addr_32(const std::array<uint8_t, 32>& bytes) {
   sysio::opp::types::ChainAddress addr;
   addr.set_kind(sysio::opp::types::CHAIN_KIND_EVM);
   addr.set_address(bytes.data(), bytes.size());
   return addr;
}

/// Pack a single `AttestationEntry` (type + data) into a freshly-built
/// `Envelope` and return its serialized bytes.
std::vector<char> envelope_with_entries(
   const std::vector<sysio::opp::AttestationEntry>& entries) {
   sysio::opp::Envelope env;
   auto*                msg = env.add_messages();
   for (const auto& e : entries) {
      auto* out = msg->mutable_payload()->add_attestations();
      *out      = e;
   }
   std::string buf;
   env.SerializeToString(&buf);
   return std::vector<char>(buf.begin(), buf.end());
}

/// Build an `OPERATOR_ACTION(WITHDRAW_REMIT)` entry pointing at
/// `op_addr`. Other proto fields are populated with neutral defaults —
/// the decoder only reads `op_address` + `action_type`.
sysio::opp::AttestationEntry remit_entry(const sysio::opp::types::ChainAddress& op_addr) {
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT);
   *oa.mutable_op_address() = op_addr;
   std::string body;
   oa.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION);
   entry.set_data(std::move(body));
   return entry;
}

/// Same as `remit_entry` but for SLASH (which should NOT be returned —
/// SLASH routes to the Reserve PDA which is in the static account list).
sysio::opp::AttestationEntry slash_entry(const sysio::opp::types::ChainAddress& op_addr) {
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_SLASH);
   *oa.mutable_op_address() = op_addr;
   std::string body;
   oa.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION);
   entry.set_data(std::move(body));
   return entry;
}

/// Build a `DEPOSIT_REVERT` entry pointing at `depositor_addr`.
sysio::opp::AttestationEntry revert_entry(const sysio::opp::types::ChainAddress& depositor_addr) {
   sysio::opp::attestations::DepositRevert dr;
   *dr.mutable_depositor() = depositor_addr;
   std::string body;
   dr.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_DEPOSIT_REVERT);
   entry.set_data(std::move(body));
   return entry;
}

/// Build a `SWAP_REMIT` entry pointing at `recipient_addr`.
sysio::opp::AttestationEntry swap_remit_entry(uint64_t token_code,
                                              uint64_t reserve_code,
                                              const sysio::opp::types::ChainAddress& recipient_addr) {
   sysio::opp::attestations::SwapRemit remit;
   remit.mutable_amount()->set_token_code(token_code);
   remit.mutable_amount()->set_amount(123);
   remit.set_reserve_code(reserve_code);
   *remit.mutable_recipient() = recipient_addr;
   std::string body;
   remit.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_SWAP_REMIT);
   entry.set_data(std::move(body));
   return entry;
}

/// Build a `SWAP_REVERT` entry pointing at `depositor_addr`.
sysio::opp::AttestationEntry swap_revert_entry(uint64_t token_code,
                                               uint64_t reserve_code,
                                               const sysio::opp::types::ChainAddress& depositor_addr) {
   sysio::opp::attestations::SwapRevert revert;
   revert.mutable_refund_amount()->set_token_code(token_code);
   revert.mutable_refund_amount()->set_amount(456);
   revert.set_source_reserve_code(reserve_code);
   *revert.mutable_depositor() = depositor_addr;
   std::string body;
   revert.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_SWAP_REVERT);
   entry.set_data(std::move(body));
   return entry;
}

/// Build a `RESERVE_READY` entry.
sysio::opp::AttestationEntry reserve_ready_entry(uint64_t token_code, uint64_t reserve_code) {
   sysio::opp::attestations::ReserveReady ready;
   ready.set_token_code(token_code);
   ready.set_reserve_code(reserve_code);
   std::string body;
   ready.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_RESERVE_READY);
   entry.set_data(std::move(body));
   return entry;
}

/// Build a `RESERVE_CREATE_CANCELLED` entry.
sysio::opp::AttestationEntry reserve_create_cancelled_entry(uint64_t token_code, uint64_t reserve_code) {
   sysio::opp::attestations::ReserveCreateCancelled cancelled;
   cancelled.set_token_code(token_code);
   cancelled.set_reserve_code(reserve_code);
   std::string body;
   cancelled.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED);
   entry.set_data(std::move(body));
   return entry;
}

std::array<uint8_t, 32> filled_pubkey(uint8_t byte) {
   std::array<uint8_t, 32> arr{};
   arr.fill(byte);
   return arr;
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(extract_pubkeys_empty_envelope_returns_empty) try {
   std::vector<char> envelope = envelope_with_entries({});
   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_CHECK(pks.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_single_withdraw_remit) try {
   auto op_pk = filled_pubkey(0xAA);
   auto envelope = envelope_with_entries({remit_entry(make_sol_addr(op_pk))});

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_REQUIRE_EQUAL(pks.size(), 1u);
   BOOST_CHECK(pks[0].serialize() == op_pk);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_deposit_revert) try {
   auto depositor_pk = filled_pubkey(0xBB);
   auto envelope = envelope_with_entries({revert_entry(make_sol_addr(depositor_pk))});

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_REQUIRE_EQUAL(pks.size(), 1u);
   BOOST_CHECK(pks[0].serialize() == depositor_pk);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_dedupes_repeated_recipient) try {
   auto op_pk = filled_pubkey(0xCC);
   // Two WITHDRAW_REMITs to the same operator (e.g. ETH bond + SOL bond
   // both being returned in one envelope referencing the operator's SOL
   // wallet twice) — only one account slot is needed in the tx.
   auto envelope = envelope_with_entries({
      remit_entry(make_sol_addr(op_pk)),
      remit_entry(make_sol_addr(op_pk)),
   });

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_REQUIRE_EQUAL(pks.size(), 1u);
   BOOST_CHECK(pks[0].serialize() == op_pk);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_skips_slash) try {
   // SLASH attestations target the Reserve PDA, which is already a
   // declared account on `epoch_in`. They MUST NOT bloat the
   // remaining_accounts list — every entry costs ~33 bytes against the
   // 1 232-byte tx MTU.
   auto slash_op = filled_pubkey(0xDD);
   auto remit_op = filled_pubkey(0xEE);
   auto envelope = envelope_with_entries({
      slash_entry(make_sol_addr(slash_op)),
      remit_entry(make_sol_addr(remit_op)),
   });

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_REQUIRE_EQUAL(pks.size(), 1u);
   BOOST_CHECK(pks[0].serialize() == remit_op);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_skips_non_solana_chain) try {
   // A WITHDRAW_REMIT whose `op_address` carries kind=ETHEREUM is not
   // for this outpost and must not contribute a SOL account.
   auto eth_bytes = filled_pubkey(0x01);
   auto envelope  = envelope_with_entries({
      remit_entry(make_eth_addr_32(eth_bytes)),
   });

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_CHECK(pks.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_skips_malformed_address_length) try {
   // 20-byte address with kind=SOLANA — the bytes pass the chain check
   // but fail the length check; the decoder must drop the entry rather
   // than truncate or zero-extend.
   sysio::opp::types::ChainAddress malformed;
   malformed.set_kind(sysio::opp::types::CHAIN_KIND_SVM);
   std::vector<uint8_t> short_addr(20, 0xAB);
   malformed.set_address(short_addr.data(), short_addr.size());

   auto envelope = envelope_with_entries({remit_entry(malformed)});

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_CHECK(pks.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_returns_empty_on_garbage_envelope) try {
   std::vector<char> garbage = {char(0xFF), char(0xFF), char(0xFF), char(0xFF)};
   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(garbage);
   BOOST_CHECK(pks.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_mixed_remit_and_revert_preserved_order) try {
   auto op_a       = filled_pubkey(0x10);
   auto depositor  = filled_pubkey(0x20);
   auto op_b       = filled_pubkey(0x30);
   auto envelope   = envelope_with_entries({
      remit_entry(make_sol_addr(op_a)),
      revert_entry(make_sol_addr(depositor)),
      remit_entry(make_sol_addr(op_b)),
   });

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_REQUIRE_EQUAL(pks.size(), 3u);
   BOOST_CHECK(pks[0].serialize() == op_a);
   BOOST_CHECK(pks[1].serialize() == depositor);
   BOOST_CHECK(pks[2].serialize() == op_b);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_pubkeys_includes_native_swap_effect_wallets) try {
   auto swap_recipient = filled_pubkey(0x41);
   auto swap_depositor = filled_pubkey(0x42);
   auto withdraw_op    = filled_pubkey(0x43);
   auto envelope       = envelope_with_entries({
      swap_remit_entry(10, 20, make_sol_addr(swap_recipient)),
      swap_revert_entry(11, 21, make_sol_addr(swap_depositor)),
      remit_entry(make_sol_addr(withdraw_op)),
   });

   auto pks = sysio::outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope);
   BOOST_REQUIRE_EQUAL(pks.size(), 3u);
   BOOST_CHECK(pks[0].serialize() == swap_recipient);
   BOOST_CHECK(pks[1].serialize() == swap_depositor);
   BOOST_CHECK(pks[2].serialize() == withdraw_op);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_reserve_seeds_includes_all_terminal_reserve_effects_and_dedupes) try {
   auto recipient = filled_pubkey(0x51);
   auto depositor = filled_pubkey(0x52);
   auto envelope  = envelope_with_entries({
      swap_remit_entry(100, 200, make_sol_addr(recipient)),
      swap_revert_entry(101, 201, make_sol_addr(depositor)),
      reserve_ready_entry(102, 202),
      reserve_create_cancelled_entry(103, 203),
      reserve_ready_entry(102, 202),
   });

   auto seeds = sysio::outpost_solana_client_detail::extract_inbound_swap_remit_reserve_seeds(envelope);
   BOOST_REQUIRE_EQUAL(seeds.size(), 4u);
   BOOST_CHECK_EQUAL(seeds[0].token_code, 100u);
   BOOST_CHECK_EQUAL(seeds[0].reserve_code, 200u);
   BOOST_CHECK_EQUAL(seeds[1].token_code, 101u);
   BOOST_CHECK_EQUAL(seeds[1].reserve_code, 201u);
   BOOST_CHECK_EQUAL(seeds[2].token_code, 102u);
   BOOST_CHECK_EQUAL(seeds[2].reserve_code, 202u);
   BOOST_CHECK_EQUAL(seeds[3].token_code, 103u);
   BOOST_CHECK_EQUAL(seeds[3].reserve_code, 203u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_reserve_create_cancelled_seeds_only_reads_cancelled_effects) try {
   auto recipient = filled_pubkey(0x53);
   auto depositor = filled_pubkey(0x54);
   auto envelope  = envelope_with_entries({
      swap_remit_entry(110, 210, make_sol_addr(recipient)),
      reserve_create_cancelled_entry(111, 211),
      swap_revert_entry(112, 212, make_sol_addr(depositor)),
      reserve_create_cancelled_entry(111, 211),
      reserve_create_cancelled_entry(113, 213),
   });

   auto seeds = sysio::outpost_solana_client_detail::extract_inbound_reserve_create_cancelled_seeds(envelope);
   BOOST_REQUIRE_EQUAL(seeds.size(), 2u);
   BOOST_CHECK_EQUAL(seeds[0].token_code, 111u);
   BOOST_CHECK_EQUAL(seeds[0].reserve_code, 211u);
   BOOST_CHECK_EQUAL(seeds[1].token_code, 113u);
   BOOST_CHECK_EQUAL(seeds[1].reserve_code, 213u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(record_terminal_account_dedupes_and_merges_writable) try {
   std::vector<account_meta> metas;
   const solana_public_key readonly_key(filled_pubkey(0x55));
   const solana_public_key writable_key(filled_pubkey(0x56));

   sysio::outpost_solana_client_detail::record_terminal_account(metas, readonly_key, false);
   BOOST_REQUIRE_EQUAL(metas.size(), 1u);
   BOOST_CHECK(metas[0].key == readonly_key);
   BOOST_CHECK(!metas[0].is_signer);
   BOOST_CHECK(!metas[0].is_writable);

   sysio::outpost_solana_client_detail::record_terminal_account(metas, readonly_key, true);
   BOOST_REQUIRE_EQUAL(metas.size(), 1u);
   BOOST_CHECK(metas[0].key == readonly_key);
   BOOST_CHECK(!metas[0].is_signer);
   BOOST_CHECK(metas[0].is_writable);

   sysio::outpost_solana_client_detail::record_terminal_account(metas, writable_key, true);
   BOOST_REQUIRE_EQUAL(metas.size(), 2u);
   BOOST_CHECK(metas[1].key == writable_key);
   BOOST_CHECK(!metas[1].is_signer);
   BOOST_CHECK(metas[1].is_writable);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_swap_remit_spl_targets_uses_recipient_pubkey) try {
   auto recipient = filled_pubkey(0x61);
   auto envelope  = envelope_with_entries({
      swap_remit_entry(300, 400, make_sol_addr(recipient)),
   });

   auto targets = sysio::outpost_solana_client_detail::extract_inbound_swap_remit_spl_targets(envelope);
   BOOST_REQUIRE_EQUAL(targets.size(), 1u);
   BOOST_CHECK_EQUAL(targets[0].token_code, 300u);
   BOOST_CHECK_EQUAL(targets[0].reserve_code, 400u);
   BOOST_CHECK(targets[0].recipient.serialize() == recipient);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_swap_revert_spl_targets_uses_depositor_pubkey) try {
   auto depositor = filled_pubkey(0x62);
   auto envelope  = envelope_with_entries({
      swap_revert_entry(301, 401, make_sol_addr(depositor)),
   });

   auto targets = sysio::outpost_solana_client_detail::extract_inbound_swap_revert_spl_targets(envelope);
   BOOST_REQUIRE_EQUAL(targets.size(), 1u);
   BOOST_CHECK_EQUAL(targets[0].token_code, 301u);
   BOOST_CHECK_EQUAL(targets[0].reserve_code, 401u);
   BOOST_CHECK(targets[0].recipient.serialize() == depositor);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
