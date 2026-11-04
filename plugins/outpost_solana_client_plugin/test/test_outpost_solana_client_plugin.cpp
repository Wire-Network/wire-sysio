#include <boost/test/unit_test.hpp>

#include <fc-test/build_info.hpp>
#include <fc-test/capture_http_server.hpp>
#include <fc-test/crypto_utils.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/base64.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/io/json.hpp>
#include <fc/task/deadline.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_borsh.hpp>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>
#include <sysio/outpost_client/rpc_options.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <variant>

using namespace std::literals;
using namespace fc::network::solana;

namespace {

constexpr std::string_view counter_anchor_idl_fixture = "solana-idl-counter-anchor.json";
constexpr std::string_view opp_outpost_idl_fixture = "solana-idl-opp-outpost-stub.json";
constexpr std::string_view startup_test_rpc_url = "http://127.0.0.1:1";

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

/// Load an Anchor IDL fixture from the libfc test fixture directory.
idl::program load_idl_fixture(std::string_view filename) {
   auto path = fc::test::get_test_fixtures_path() / boost::filesystem::path(filename);
   return idl::parse_idl_file(path.generic_string());
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

/// Borsh-encode an `epoch_in` instruction payload with the production
/// argument order: `epoch_index, chunk_index, total_chunks, total_bytes,
/// chunk_data`. A zero-length chunk with `chunk_index == total_chunks` is the
/// terminal call.
std::vector<uint8_t> epoch_in_payload(const idl::instruction& instr, uint16_t chunk_index, uint16_t total_chunks,
                                      uint32_t total_bytes, size_t chunk_data_bytes) {
   std::vector<uint8_t> data;
   data.reserve(instr.discriminator.size() + 16 + chunk_data_bytes);
   data.insert(data.end(), instr.discriminator.begin(), instr.discriminator.end());
   write_u32_le(data, 1); // epoch_index
   write_u16_le(data, chunk_index);
   write_u16_le(data, total_chunks);
   write_u32_le(data, total_bytes);
   write_u32_le(data, static_cast<uint32_t>(chunk_data_bytes)); // chunk_data length prefix
   data.resize(data.size() + chunk_data_bytes);
   return data;
}

/// Build the IDL-declared static account metas used by production relay calls.
std::vector<account_meta> production_static_accounts(const idl::instruction& instr,
                                                     const solana_public_key& fee_payer) {
   std::vector<account_meta> accounts;
   accounts.reserve(instr.accounts.size());
   for (size_t i = 0; i < instr.accounts.size(); ++i) {
      const auto& acct = instr.accounts[i];
      solana_public_key key;
      if (acct.is_signer) {
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

/// Assert the ordered heap-frame then compute-unit-limit producer contract.
void check_compute_budget_preamble(const std::vector<instruction>& instructions, uint32_t heap_frame_bytes,
                                   uint32_t compute_unit_limit) {
   BOOST_REQUIRE_EQUAL(instructions.size(), 2u);
   const auto heap = system::compute_budget::request_heap_frame(heap_frame_bytes);
   const auto units = system::compute_budget::set_compute_unit_limit(compute_unit_limit);
   BOOST_CHECK(instructions[0].program_id == system::program_ids::COMPUTE_BUDGET_PROGRAM);
   BOOST_CHECK(instructions[0].accounts.empty());
   BOOST_CHECK(instructions[0].data == heap.data);
   BOOST_CHECK(instructions[1].program_id == system::program_ids::COMPUTE_BUDGET_PROGRAM);
   BOOST_CHECK(instructions[1].accounts.empty());
   BOOST_CHECK(instructions[1].data == units.data);
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

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(outpost_solana_client_plugin)

// ---------------------------------------------------------------------------
//  Startup configuration validation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(startup_accepts_matching_named_signer) {
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + std::string(startup_test_rpc_url),
   }));
}

BOOST_AUTO_TEST_CASE(authenticated_caller_address_returns_configured_signer_pubkey) {
   appbase::scoped_app test_application{};
   std::vector<std::string> arguments{"test_outpost_solana_client_plugin"};
   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) {
      argv.emplace_back(argument.data());
   }
   BOOST_REQUIRE(test_application->initialize<sysio::signature_provider_manager_plugin>(
      argv.size(), argv.data()));

   const auto fixture = fc::test::load_keygen_fixture("solana", 1);
   auto& manager = test_application->get_plugin<sysio::signature_provider_manager_plugin>();
   auto sig_provider = manager.create_provider(
      "signer-a",
      fc::crypto::chain_kind_solana,
      fc::crypto::chain_key_type_solana,
      fixture.public_key,
      fc::test::to_private_key_spec(fixture.private_key));

   auto sol_client = std::make_shared<solana_client>(
      sig_provider,
      std::variant<std::string, fc::url>{std::string(startup_test_rpc_url)});
   auto entry = std::make_shared<sysio::solana_client_entry_t>();
   entry->id = "client-a";
   entry->url = std::string(startup_test_rpc_url);
   entry->signature_provider = sig_provider;
   entry->client = sol_client;

   sysio::outpost_solana_client outpost(
      entry,
      measurement_pubkey(42),
      {load_idl_fixture(opp_outpost_idl_fixture)},
      1,
      1,
      sysio::solana_outpost_role::batch_operator);

   const auto expected_caller = sol_client->get_pubkey().serialize();
   const auto actual_caller = outpost.authenticated_caller_address();
   BOOST_CHECK_EQUAL_COLLECTIONS(
      expected_caller.begin(), expected_caller.end(),
      actual_caller.begin(), actual_caller.end());
}

BOOST_AUTO_TEST_CASE(startup_rejects_client_without_matching_named_signature_provider) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider("other-signer"),
      "--outpost-solana-client",
      "client-a,signer-a," + std::string(startup_test_rpc_url),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_anonymous_signature_provider_reference) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(""),
      "--outpost-solana-client",
      "client-a,key-0," + std::string(startup_test_rpc_url),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_named_signer_for_wrong_chain) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider("signer-a", fc::crypto::chain_kind_wire),
      "--outpost-solana-client",
      "client-a,signer-a," + std::string(startup_test_rpc_url),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_named_signer_with_wrong_key_type) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      solana_target_with_wire_key_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + std::string(startup_test_rpc_url),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_empty_client_id) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      ",signer-a," + std::string(startup_test_rpc_url),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_empty_signer_name) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,," + std::string(startup_test_rpc_url),
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_empty_rpc_url) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a,",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_four_field_client_spec) {
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_solana_signature_provider(),
      "--outpost-solana-client",
      "client-a,signer-a," + std::string(startup_test_rpc_url) + ",unexpected-fourth-field",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(authenticated_transport_options_are_registered) {
   sysio::outpost_solana_client_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   std::set<std::string> option_names;
   for (const auto& option : cfg.options())
      option_names.insert(option->long_name());

   BOOST_CHECK(option_names.contains("outpost-solana-additional-ca-file"));
   BOOST_CHECK(option_names.contains("outpost-solana-additional-ca-path"));
   BOOST_CHECK(option_names.contains("outpost-solana-proxy"));
}

/** Caller overrides do not replace JSON-RPC's retain-until-failure DNS policy. */
BOOST_AUTO_TEST_CASE(rpc_transport_overrides_preserve_json_rpc_dns_policy) {
   namespace bpo = boost::program_options;
   constexpr sysio::outbound_http::transport_option_names transport_names{
      .additional_ca_file = "outpost-solana-additional-ca-file",
      .additional_ca_path = "outpost-solana-additional-ca-path",
      .proxy = "outpost-solana-proxy",
   };

   bpo::options_description options;
   sysio::outbound_http::add_global_transport_program_options(options);
   sysio::outbound_http::add_transport_program_options(
      options,
      transport_names,
      "Solana RPC");
   std::array arguments{
      "test_outpost_solana_client_plugin",
      "--outbound-http-additional-ca-path",
      "/tmp/wire-global-ca",
      "--outpost-solana-proxy",
      "http://127.0.0.1:3128",
   };
   bpo::variables_map variables;
   bpo::store(
      bpo::parse_command_line(
         arguments.size(),
         const_cast<char**>(arguments.data()),
         options),
      variables);
   bpo::notify(variables);

   const auto rpc_options =
      sysio::outpost_rpc::rpc_options(
         variables,
         transport_names);
   BOOST_CHECK(!rpc_options.transport.dns_cache_timeout);
   BOOST_REQUIRE(rpc_options.transport.additional_ca_path);
   BOOST_CHECK_EQUAL(
      *rpc_options.transport.additional_ca_path,
      std::filesystem::path("/tmp/wire-global-ca"));
   BOOST_REQUIRE(rpc_options.transport.proxy);
   BOOST_CHECK_EQUAL(
      *rpc_options.transport.proxy,
      "http://127.0.0.1:3128");
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
namespace {

}

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
   // staging PDAs and records delivery on a zero-data terminal call where
   // chunk_index == total_chunks.
   //
   // ORDER IS LOAD-BEARING: args are encoded positionally, so this list must
   // match the program's #[instruction(...)] exactly.
   //
   // There is deliberately NO dispatch_limit here. The terminal call records
   // delivery and runs the consensus predicate and nothing else, so it carries
   // no effect accounts and no settlement window -- that is what stops a
   // dispatch concern from wedging consensus. Settlement is
   // `dispatch_attestations`, asserted below.
   BOOST_REQUIRE_EQUAL(epoch_in->args.size(), 5u);
   BOOST_CHECK_EQUAL(epoch_in->args[0].name, "epoch_index");
   BOOST_CHECK_EQUAL(epoch_in->args[1].name, "chunk_index");
   BOOST_CHECK_EQUAL(epoch_in->args[2].name, "total_chunks");
   BOOST_CHECK_EQUAL(epoch_in->args[3].name, "total_bytes");
   BOOST_CHECK_EQUAL(epoch_in->args[4].name, "chunk_data");

   // The settlement half of the split. Asserted HERE, beside epoch_in, because
   // the two signatures are one contract: what epoch_in stopped carrying,
   // dispatch_attestations must carry.
   const idl::instruction* dispatch = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "dispatch_attestations") { dispatch = &instr; break; }
   }
   BOOST_REQUIRE(dispatch != nullptr);
   BOOST_REQUIRE_EQUAL(dispatch->args.size(), 2u);
   BOOST_CHECK_EQUAL(dispatch->args[0].name, "epoch_index");
   BOOST_CHECK_EQUAL(dispatch->args[1].name, "dispatch_limit");
   // No inbound_envelopes: the crank settles effects, it does not append the
   // inbound audit record -- that happens once, on the consensus-tipping call.
   BOOST_CHECK_EQUAL(dispatch->accounts.size(), 9u);
   BOOST_CHECK(dispatch->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(dispatch->accounts[4].name, "chunk_buffer");

   // Accounts: operator (signer), config, operator_registry, epoch_deliveries,
   //           chunk_buffer, inbound_envelopes, system_program. No outbound-emit
   //           or effect accounts here -- epoch_in only stages/finalizes inbound
   //           chunks and records consensus; dispatch_attestations (asserted
   //           above) owns effect processing and the outbound emit.
   BOOST_CHECK_EQUAL(epoch_in->accounts.size(), 7u);
   BOOST_CHECK(epoch_in->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(epoch_in->accounts[4].name, "chunk_buffer");
   BOOST_CHECK_EQUAL(epoch_in->accounts[5].name, "inbound_envelopes");
   BOOST_CHECK_EQUAL(epoch_in->accounts[6].name, "system_program");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(opp_outpost_dispatch_attestations_present) try {
   // The relay's crank path requires the two-phase program: a deployment
   // whose IDL lacks `dispatch_attestations` must fail at BOOT (the
   // batch-operator constructor asserts has_idl), not at the first drain.
   // This pins the fixture the boot assert is validated against.
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   const idl::instruction* dispatch = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "dispatch_attestations") { dispatch = &instr; break; }
   }
   BOOST_REQUIRE(dispatch != nullptr);
   BOOST_REQUIRE_EQUAL(dispatch->args.size(), 2u);
   BOOST_CHECK_EQUAL(dispatch->args[0].name, "epoch_index");
   BOOST_CHECK_EQUAL(dispatch->args[1].name, "dispatch_limit");

   // Accounts: caller (permissionless signer), then the same static outpost
   // account list the relay overrides by name in send_dispatch_attestations.
   BOOST_REQUIRE_EQUAL(dispatch->accounts.size(), 9u);
   BOOST_CHECK_EQUAL(dispatch->accounts[0].name, "caller");
   BOOST_CHECK(dispatch->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(dispatch->accounts[3].name, "epoch_deliveries");
   BOOST_CHECK_EQUAL(dispatch->accounts[4].name, "chunk_buffer");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(opp_outpost_cleanup_envelope_chunks_present) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   const idl::instruction* cleanup = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == sysio::outpost_solana_client_detail::CLEANUP_ENVELOPE_CHUNKS_INSTRUCTION) {
         cleanup = &instr;
         break;
      }
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
BOOST_AUTO_TEST_CASE(epoch_in_full_data_chunk_fits_packet_limit) try {
   // A full data chunk must serialise under Solana's raw packet limit. Any
   // change to the `epoch_in` account list or argument tuple moves the fixed
   // overhead; adding `dispatch_limit` (+4 B) without lowering
   // SOLANA_MAX_CHUNK_BYTES pushed this to 1234 B and wedged every epoch whose
   // envelope needed a full chunk.
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   const idl::instruction* epoch_in = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "epoch_in") { epoch_in = &instr; break; }
   }
   BOOST_REQUIRE(epoch_in);

   const auto fee_payer = measurement_pubkey(1);
   auto instructions = sysio::outpost_solana_client_detail::epoch_in_pre_instructions(1, 2);
   BOOST_CHECK(instructions.empty());
   instructions.push_back(
      instruction{measurement_pubkey(3), production_static_accounts(*epoch_in, fee_payer),
                  epoch_in_payload(*epoch_in, 1, 2, static_cast<uint32_t>(2 * sysio::SOLANA_MAX_CHUNK_BYTES),
                                   sysio::SOLANA_MAX_CHUNK_BYTES)});

   const auto packet = build_measured_legacy_transaction(instructions, fee_payer).serialize();
   BOOST_TEST_MESSAGE("full epoch_in data chunk serialises to " << packet.size() << " bytes");
   BOOST_CHECK_LE(packet.size(), limits::PACKET_DATA_SIZE);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(epoch_in_terminal_budget_matches_producer_and_fits_packet_limit) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   const auto* epoch_in = prog.find_instruction("epoch_in");
   BOOST_REQUIRE(epoch_in);

   constexpr uint16_t total_chunks = 2;
   auto instructions = sysio::outpost_solana_client_detail::epoch_in_pre_instructions(total_chunks, total_chunks);
   check_compute_budget_preamble(instructions, sysio::SOLANA_RELAY_HEAP_FRAME_BYTES,
                                 sysio::SOLANA_RELAY_COMPUTE_UNIT_LIMIT);

   const auto fee_payer = measurement_pubkey(1);
   instructions.push_back(instruction{measurement_pubkey(3), production_static_accounts(*epoch_in, fee_payer),
                                      epoch_in_payload(*epoch_in, total_chunks, total_chunks,
                                                       static_cast<uint32_t>(2 * sysio::SOLANA_MAX_CHUNK_BYTES), 0)});

   const auto packet = build_measured_legacy_transaction(instructions, fee_payer).serialize();
   BOOST_TEST_MESSAGE("terminal epoch_in tx with heap and CU budgets serialises to " << packet.size() << " bytes");
   BOOST_CHECK_LE(packet.size(), limits::PACKET_DATA_SIZE);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(dispatch_attestations_full_manifest_fits_packet_limit) try {
   // MAX_TERMINAL_DYNAMIC_ACCOUNTS is a BUDGET, and a budget nobody measures is
   // a guess. It counts only the `remaining_accounts` extras -- not the
   // instruction's 9 IDL-declared accounts, not the fee payer, not the
   // compute-budget preamble the relay injects on this call. This test
   // is what makes the number honest: a dispatch_attestations tx carrying a
   // FULL manifest must still serialise inside Solana's packet limit.
   //
   // If this fails, lower MAX_TERMINAL_DYNAMIC_ACCOUNTS to the measured
   // maximum -- do not raise the limit, which is not ours to move.
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   const idl::instruction* dispatch = nullptr;
   for (auto& instr : prog.instructions) {
      if (instr.name == "dispatch_attestations") { dispatch = &instr; break; }
   }
   BOOST_REQUIRE(dispatch);

   const auto fee_payer = measurement_pubkey(1);
   auto accounts = production_static_accounts(*dispatch, fee_payer);

   // A full extras manifest: distinct writable accounts, the worst case for
   // packet size since each costs a fresh 32-byte key in the message.
   for (size_t i = 0; i < sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS; ++i) {
      accounts.push_back(
         account_meta::writable(measurement_pubkey(static_cast<uint32_t>(900 + i)), false));
   }

   std::vector<uint8_t> data;
   data.insert(data.end(), dispatch->discriminator.begin(), dispatch->discriminator.end());
   write_u32_le(data, 1);                                                    // epoch_index
   write_u32_le(data, static_cast<uint32_t>(sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS)); // dispatch_limit

   auto instructions = sysio::outpost_solana_client_detail::relay_compute_budget_pre_instructions();
   check_compute_budget_preamble(instructions, sysio::SOLANA_RELAY_HEAP_FRAME_BYTES,
                                 sysio::SOLANA_RELAY_COMPUTE_UNIT_LIMIT);
   instructions.push_back(instruction{measurement_pubkey(3), accounts, data});

   const auto packet = build_measured_legacy_transaction(instructions, fee_payer).serialize();
   BOOST_TEST_MESSAGE("dispatch_attestations tx with "
                      << sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS
                      << " extras serialises to " << packet.size() << " bytes");
   BOOST_CHECK_LE(packet.size(), limits::PACKET_DATA_SIZE);
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
   BOOST_CHECK_EQUAL(chunks_for(2526), 4u);   // 2526/668 = 3.78 -> 4
   // 32 KiB cap: ceil(32 768 / 668) = 50 chunks. Last chunk is 36 B
   // (32_768 mod 668 = 36), the first 49 are full at MAX_CHUNK_BYTES.
   BOOST_CHECK_EQUAL(chunks_for(sysio::SOLANA_MAX_ENVELOPE_BYTES), 50u);
   BOOST_CHECK_EQUAL(epoch_in_calls_for(sysio::SOLANA_MAX_ENVELOPE_BYTES), 51u);
   BOOST_CHECK_EQUAL(sysio::SOLANA_MAX_ENVELOPE_BYTES % sysio::SOLANA_MAX_CHUNK_BYTES, 36u);

   // Last-chunk size at the dev-026 reproduction: the loop fills the first
   // 3 chunks at MAX_CHUNK_BYTES (= 668) and the last with the remainder.
   const size_t last_chunk_size = 2526 - 3 * sysio::SOLANA_MAX_CHUNK_BYTES;
   BOOST_CHECK_EQUAL(last_chunk_size, 522u);   // 2526 - 2004 = 522
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
BOOST_AUTO_TEST_CASE(opp_outpost_excludes_retired_instruction_entrypoints) try {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);

   // `add_attestation` was retired from the client (attestations flow through
   // `commit_underwrite` / `epoch_in`); the fixture must not resurrect it.
   bool has_initialize = false, has_add = false, has_deposit = false;
   for (auto& instr : prog.instructions) {
      if (instr.name == "initialize")       has_initialize = true;
      if (instr.name == "add_attestation")  has_add        = true;
      if (instr.name == "deposit")          has_deposit    = true;
   }
   BOOST_CHECK(has_initialize);
   BOOST_CHECK(!has_add);
   BOOST_CHECK(!has_deposit);
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
// ── extract_inbound_effects: the single authoritative envelope decode.
//    Walks an inbound envelope's attestations in dispatch order and
//    surfaces one `inbound_effect` per account-needing attestation --
//    the manifests `drain_dispatch` builds `dispatch_attestations`
//    batches from. These tests pin the per-shape decode, the flat
//    dispatch-order indexing, and the malformed-input drops.

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

/// The depot request id `desyndicate_liq_entry` stamps unless a case names one.
constexpr uint64_t default_desyndication_request_id = 1;

/// Build a `DESYNDICATE_LIQ` entry releasing `token_code` to `user` under
/// `request_id`. The decoder reads `user` and `request_id` -- the pool mint comes
/// from `DistributionState`, not the payload -- so the other fields stay neutral.

sysio::opp::AttestationEntry desyndicate_liq_entry(uint64_t                               token_code,
                                                   const sysio::opp::types::ChainAddress& user,
                                                   uint64_t request_id = default_desyndication_request_id) {
   sysio::opp::attestations::DesyndicateLIQ dl;
   dl.set_chain_code(900);
   *dl.mutable_user() = user;
   dl.mutable_amount()->set_token_code(token_code);
   dl.mutable_amount()->set_amount(777);
   dl.set_request_id(request_id);
   std::string body;
   dl.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_DESYNDICATE_LIQ);
   entry.set_data(std::move(body));
   return entry;
}

/// Build an `OPERATOR_ACTION(WITHDRAW_REMIT)` entry for `op_addr`. The relay derives no Solana
/// effect from operator actions, so it must contribute no manifest entry.
sysio::opp::AttestationEntry remit_entry(uint64_t                               token_code,
                                         const sysio::opp::types::ChainAddress& op_addr) {
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT);
   oa.mutable_amount()->set_token_code(token_code);
   oa.mutable_amount()->set_amount(777);
   *oa.mutable_op_address() = op_addr;
   std::string body;
   oa.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION);
   entry.set_data(std::move(body));
   return entry;
}

/// Build an `OPERATOR_ACTION` entry whose action type settles no collateral
/// (a DEPOSIT_REQUEST is outbound-from-outpost) — must contribute no effect.
sysio::opp::AttestationEntry deposit_request_entry(
   uint64_t token_code, const sysio::opp::types::ChainAddress& op_addr) {
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(
      sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_DEPOSIT_REQUEST);
   oa.mutable_amount()->set_token_code(token_code);
   oa.mutable_amount()->set_amount(777);
   *oa.mutable_op_address() = op_addr;
   std::string body;
   oa.SerializeToString(&body);

   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION);
   entry.set_data(std::move(body));
   return entry;
}

/// Build an `OPERATORS` state-mirror entry — updates tables on-chain, needs
/// no effect accounts, but still occupies its dispatch-order index.
sysio::opp::AttestationEntry operators_mirror_entry() {
   sysio::opp::AttestationEntry entry;
   entry.set_type(sysio::opp::types::ATTESTATION_TYPE_OPERATORS);
   return entry;
}

std::array<uint8_t, 32> filled_pubkey(uint8_t byte) {
   std::array<uint8_t, 32> arr{};
   arr.fill(byte);
   return arr;
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(extract_effects_empty_envelope_returns_empty) try {
   std::vector<char> envelope = envelope_with_entries({});
   auto effects = sysio::outpost_solana_client_detail::extract_inbound_effects(envelope);
   BOOST_CHECK(effects.empty());
} FC_LOG_AND_RETHROW();
/// The effect index MUST equal the attestation's flat position in dispatch
/// order, counting attestations that need no effect accounts.
///
/// This is what makes resumable dispatch safe: the program settles
/// `[dispatched_count, dispatched_count + dispatch_limit)` over that same
/// sequence, so the relay sizes each terminal call's `dispatch_limit` to the
/// attestations whose accounts it is carrying. If an index drifted, a batch
/// would claim attestations whose accounts are absent -- and a handler with a
/// missing account log-and-skips (a SUCCESSFUL no-op) while the cursor
/// advances past it, silently dropping a remit or a slash with no retry path.
BOOST_AUTO_TEST_CASE(extract_effects_indices_track_dispatch_order) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto remit_op = filled_pubkey(0x11);
   auto revert_op = filled_pubkey(0x22);

   // An OPERATORS state-mirror sits BETWEEN the two account-needing
   // attestations and contributes no effect entry -- but it still occupies
   // index 1 on chain, so the entries either side must report 0 and 2, not
   // 0 and 1.
   auto envelope = envelope_with_entries({
      desyndicate_liq_entry(601, make_sol_addr(remit_op)),
      operators_mirror_entry(),
      desyndicate_liq_entry(602, make_sol_addr(revert_op)),
   });

   BOOST_CHECK_EQUAL(detail::count_inbound_attestations(envelope), 3u);

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 2u);

   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::desyndicate_liq);
   BOOST_REQUIRE(effects[0].recipient.has_value());
   BOOST_CHECK(effects[0].recipient->serialize() == remit_op);

   BOOST_CHECK_EQUAL(effects[1].attestation_index, 2u);
   BOOST_CHECK(effects[1].shape == detail::effect_shape::desyndicate_liq);
   BOOST_REQUIRE(effects[1].recipient.has_value());
   BOOST_CHECK(effects[1].recipient->serialize() == revert_op);
} FC_LOG_AND_RETHROW();
/// An envelope the decoder cannot parse yields no effects AND a zero count,
/// so the relay submits a terminal call claiming nothing rather than one
/// claiming attestations whose accounts it never derived.
BOOST_AUTO_TEST_CASE(extract_effects_on_undecodable_envelope_is_empty) try {
   namespace detail = sysio::outpost_solana_client_detail;
   const std::vector<char> garbage(32, '\xAB');
   BOOST_CHECK_EQUAL(detail::count_inbound_attestations(garbage), 0u);
   BOOST_CHECK(detail::extract_inbound_effects(garbage).empty());
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(extract_effects_repeated_recipient_yields_one_entry_per_attestation) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto op_pk = filled_pubkey(0xCC);
   // Two identical DESYNDICATE_LIQs to the same recipient and token_code. The
   // walk is per-attestation with NO cross-attestation dedup -- the cursor
   // counts both, so collapsing them would misalign every later
   // dispatch_limit -- and the duplicate ACCOUNTS (recipient wallet and its
   // token account) merge later in `record_terminal_account` when a batch's
   // manifests union (pinned by
   // record_terminal_account_dedupes_and_merges_writable).
   auto envelope = envelope_with_entries({
      desyndicate_liq_entry(600, make_sol_addr(op_pk)),
      desyndicate_liq_entry(600, make_sol_addr(op_pk)),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 2u);
   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK_EQUAL(effects[1].attestation_index, 1u);
   BOOST_CHECK(effects[0].recipient->serialize() == op_pk);
   BOOST_CHECK(effects[1].recipient->serialize() == op_pk);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(extract_effects_desyndicate_liq_carries_the_user) try {
   namespace detail = sysio::outpost_solana_client_detail;
   // DESYNDICATE_LIQ MUST be surfaced: the handler resolves the pool's state
   // singletons, the transfer's accounts and the hook's accounts out of
   // remaining_accounts, and a manifest that omits them aborts the dispatch
   // call and pins the cursor on this attestation for every retry.
   constexpr uint64_t request_id = 0x0102030405060708;
   auto user     = filled_pubkey(0xAB);
   auto envelope = envelope_with_entries({
      desyndicate_liq_entry(700, make_sol_addr(user), request_id),
      // Not an SVM pubkey: dropped here and logged-and-skipped on chain, while
      // the flat index still advances past it.
      desyndicate_liq_entry(700, make_eth_addr_32(user)),
      remit_entry(504, make_sol_addr(filled_pubkey(0xEE))),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 1u);

   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::desyndicate_liq);
   BOOST_REQUIRE(effects[0].recipient.has_value());
   BOOST_CHECK(effects[0].recipient->serialize() == user);
   // The request id keys the pending-payout PDA a frozen outpost stores the payout at.
   BOOST_REQUIRE(effects[0].desyndication_request_id.has_value());
   BOOST_CHECK_EQUAL(*effects[0].desyndication_request_id, request_id);

} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(extract_effects_skips_non_settling_operator_actions) try {
   namespace detail = sysio::outpost_solana_client_detail;
   // A DEPOSIT_REQUEST is outbound-from-outpost — inbound it settles
   // nothing and must contribute no effect entry (it still occupies its
   // dispatch-order index).
   auto envelope = envelope_with_entries({
      deposit_request_entry(801, make_sol_addr(filled_pubkey(0x77))),
      remit_entry(802, make_sol_addr(filled_pubkey(0x78))),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_CHECK(effects.empty());
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(extract_effects_skips_non_solana_chain) try {
   // A DESYNDICATE_LIQ whose `user` carries kind=ETHEREUM is not
   // for this outpost and must not contribute a SOL effect entry.
   auto eth_bytes = filled_pubkey(0x01);
   auto envelope  = envelope_with_entries({
      desyndicate_liq_entry(800, make_eth_addr_32(eth_bytes)),
   });

   auto effects = sysio::outpost_solana_client_detail::extract_inbound_effects(envelope);
   BOOST_CHECK(effects.empty());
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(extract_effects_skips_malformed_address_length) try {
   // 20-byte address with kind=SOLANA — the bytes pass the chain check
   // but fail the length check; the decoder must drop the entry rather
   // than truncate or zero-extend.
   sysio::opp::types::ChainAddress malformed;
   malformed.set_kind(sysio::opp::types::CHAIN_KIND_SVM);
   std::vector<uint8_t> short_addr(20, 0xAB);
   malformed.set_address(short_addr.data(), short_addr.size());

   auto envelope = envelope_with_entries({desyndicate_liq_entry(801, malformed)});

   auto effects = sysio::outpost_solana_client_detail::extract_inbound_effects(envelope);
   BOOST_CHECK(effects.empty());
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
// ── build_dispatch_manifests: the per-attestation account manifests the
//    dispatch crank packs its windows from, driven through its transfer-hook
//    and DistributionState read seams. These pin the properties the manifest
//    build must hold before a single dispatch is sent: custody comes from the
//    exact account the on-chain handler branches on, the pool singleton is read
//    once per build, and an absent account degrades only itself.

namespace {

namespace manifest_detail = sysio::outpost_solana_client_detail;

/// Scripted stand-in for the transfer-hook and DistributionState reads a
/// manifest build performs. The seeded values are on-chain truth; absent
/// entries degrade as uninitialized accounts. Every call is counted so
/// memoization is assertable.
struct manifest_build_harness {
   solana_public_key program_id = measurement_pubkey(42);

   /// Per-mint transfer-hook configuration. Empty by default: every mint in
   /// the system today is hook-free, so the manifest is unchanged unless a
   /// case opts in via `put_hook`.
   std::map<solana_public_key, manifest_detail::mint_transfer_hook> hooks;

   void put_hook(const solana_public_key& mint, const solana_public_key& hook_program,
                 std::vector<manifest_detail::extra_account_meta> declared) {
      hooks.emplace(mint,
                    manifest_detail::mint_transfer_hook{hook_program, std::move(declared)});
   }

   /// Counts reader invocations so a case can assert the Token-2022 gate
   /// skipped the read entirely rather than merely finding no hook.
   mutable size_t hook_reads = 0;

   manifest_detail::transfer_hook_reader hook_reader() {
      return [this](const solana_public_key& mint)
                -> std::optional<manifest_detail::mint_transfer_hook> {
         ++hook_reads;
         auto it = hooks.find(mint);
         if (it == hooks.end()) return std::nullopt;
         return it->second;
      };
   }

   /// The syndicated liqSOL pool, as `DistributionState` would render it.
   /// Empty by default: a build with no DESYNDICATE_LIQ never reads it, and a
   /// case that needs one seeds the pool's mint via `put_liq_pool`.
   std::optional<manifest_detail::liq_pool_info> liq_pool;
   /// Counts reader invocations so a case can assert the singleton is read once
   /// per build, however many desyndications the envelope carries.
   mutable size_t liq_pool_reads = 0;

   void put_liq_pool(const solana_public_key& liqsol_mint) {
      liq_pool = manifest_detail::liq_pool_info{liqsol_mint};
   }

   manifest_detail::liq_pool_reader liq_pool_reader() {
      return [this]() -> std::optional<manifest_detail::liq_pool_info> {
         ++liq_pool_reads;
         return liq_pool;
      };
   }

   std::vector<std::vector<account_meta>> build(
      const std::vector<manifest_detail::inbound_effect>& effects,
      uint32_t                                            total_attestations,
      const std::function<void()>&                        deadline_probe = [] {}) {
      return manifest_detail::build_dispatch_manifests(
         program_id, effects, total_attestations, deadline_probe,
         hook_reader(), liq_pool_reader(), "test-relay");
   }
};

bool manifest_has(const std::vector<account_meta>& metas, const solana_public_key& key) {
   return std::any_of(metas.begin(), metas.end(),
                      [&](const account_meta& meta) { return meta.key == key; });
}

}

namespace {

/// One DESYNDICATE_LIQ effect at `index` releasing syndicated liqSOL to `user`
/// under the depot's `request_id`.
manifest_detail::inbound_effect desyndicate_liq_effect(size_t index, const solana_public_key& user,
                                                       uint64_t request_id) {
   return manifest_detail::inbound_effect{
      index, manifest_detail::effect_shape::desyndicate_liq, user, request_id};
}

/// Whether `key` rides `metas` as WRITABLE.
bool manifest_writes(const std::vector<account_meta>& metas, const solana_public_key& key) {
   auto it = std::find_if(metas.begin(), metas.end(),
                          [&](const account_meta& meta) { return meta.key == key; });
   return it != metas.end() && it->is_writable;
}

} // namespace

BOOST_AUTO_TEST_CASE(build_liq_manifests_honors_deadline_before_pool_read) try {
   manifest_build_harness harness;
   BOOST_CHECK_THROW(
      harness.build({desyndicate_liq_effect(0, measurement_pubkey(92), 7)}, 1,
                    [] { FC_THROW_EXCEPTION(fc::timeout_exception, "deadline exceeded"); }),
      fc::timeout_exception);
   BOOST_CHECK_EQUAL(harness.liq_pool_reads, 0u);
   BOOST_CHECK_EQUAL(harness.hook_reads, 0u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(build_liq_manifests_propagates_pool_rpc_failure) try {
   manifest_build_harness harness;
   BOOST_CHECK_THROW(
      manifest_detail::build_dispatch_manifests(
         harness.program_id, {desyndicate_liq_effect(0, measurement_pubkey(92), 7)}, 1,
         [] {}, harness.hook_reader(),
         []() -> std::optional<manifest_detail::liq_pool_info> {
            FC_THROW_EXCEPTION(fc::exception, "pool RPC unavailable");
         }, "test-relay"),
      fc::exception);
   BOOST_CHECK_EQUAL(harness.hook_reads, 0u);
} FC_LOG_AND_RETHROW();

// The DESYNDICATE_LIQ manifest is `handle_desyndicate_liq`'s
// `require_remaining_account` list, derived from the user's pubkey, the
// program's fixed pool PDAs and the mint on `DistributionState`: the two state
// singletons (writable), the pool authority, the pool and user Token-2022 ATAs
// with their `UserRecord`s (all writable), the bucket ATA, the mint, Token-2022,
// and the hook's accounts -- the bucket authority, the mint's extra-metas PDA,
// the hook program and liqsol-core -- plus the request's pending-payout PDA
// (writable, right after the singletons) a frozen outpost stores the payout at.
// One DistributionState read serves every desyndication in the envelope.
BOOST_AUTO_TEST_CASE(build_manifests_desyndicate_liq_derives_the_pool_accounts) try {
   const auto liqsol_mint  = measurement_pubkey(90);
   const auto hook_program = measurement_pubkey(91);
   const auto user_a       = measurement_pubkey(92);
   const auto user_b       = measurement_pubkey(93);

   manifest_build_harness harness;
   harness.put_liq_pool(liqsol_mint);
   harness.put_hook(liqsol_mint, hook_program, {});

   const std::array<uint64_t, 2> request_ids{7, 0x0102030405060708};
   const auto manifests = harness.build(
      {desyndicate_liq_effect(0, user_a, request_ids[0]), desyndicate_liq_effect(1, user_b, request_ids[1])}, 2);
   BOOST_REQUIRE_EQUAL(manifests.size(), 2u);
   BOOST_CHECK_EQUAL(harness.liq_pool_reads, 1u);

   const auto& program_id       = harness.program_id;
   const auto  global_state     = manifest_detail::derive_liqsol_global_state_pda(program_id);
   const auto  distribution     = manifest_detail::derive_liqsol_distribution_state_pda(program_id);
   const auto  pool_authority   = manifest_detail::derive_liqsol_pool_authority_pda(program_id);
   const auto  bucket_authority = manifest_detail::derive_liqsol_bucket_authority_pda(program_id);
   const auto& token_2022       = system::program_ids::TOKEN_2022_PROGRAM;
   const auto  pool_ata   = system::get_associated_token_address(pool_authority, liqsol_mint, token_2022);
   const auto  bucket_ata = system::get_associated_token_address(bucket_authority, liqsol_mint, token_2022);

   for (size_t i = 0; i < 2; ++i) {
      const auto& m        = manifests[i];
      const auto& user     = i == 0 ? user_a : user_b;
      const auto  user_ata = system::get_associated_token_address(user, liqsol_mint, token_2022);
      const auto  pending  = manifest_detail::derive_pending_desyndication_pda(program_id, request_ids[i]);
      BOOST_TEST_CONTEXT("attestation " << i) {
         BOOST_REQUIRE_EQUAL(m.size(), 15u);
         BOOST_CHECK(manifest_writes(m, global_state));
         BOOST_CHECK(manifest_writes(m, distribution));
         // The pending-payout PDA follows the two singletons, writable: the
         // handler's frozen deferral creates it before touching any pool account.
         BOOST_CHECK(m[0].key == global_state && m[1].key == distribution && m[2].key == pending);
         BOOST_CHECK(manifest_writes(m, pending));
         // Each request keys its own PDA.
         BOOST_CHECK(!manifest_has(m, manifest_detail::derive_pending_desyndication_pda(
                                         program_id, request_ids[1 - i])));
         BOOST_CHECK(manifest_has(m, pool_authority) && !manifest_writes(m, pool_authority));
         BOOST_CHECK(manifest_writes(m, pool_ata));
         BOOST_CHECK(manifest_writes(m, user_ata));
         BOOST_CHECK(manifest_writes(m, manifest_detail::derive_liqsol_user_record_pda(program_id, pool_ata)));
         BOOST_CHECK(manifest_writes(m, manifest_detail::derive_liqsol_user_record_pda(program_id, user_ata)));
         BOOST_CHECK(manifest_has(m, bucket_ata) && !manifest_writes(m, bucket_ata));
         BOOST_CHECK(manifest_has(m, liqsol_mint) && !manifest_writes(m, liqsol_mint));
         BOOST_CHECK(manifest_has(m, token_2022));
         BOOST_CHECK(manifest_has(m, bucket_authority) && !manifest_writes(m, bucket_authority));
         BOOST_CHECK(manifest_has(m, program_id));
         BOOST_CHECK(manifest_has(m, hook_program));
         BOOST_CHECK(manifest_has(m, manifest_detail::derive_extra_account_metas_pda(hook_program, liqsol_mint)));
         // The bare user wallet is never an account of this shape: the payout
         // lands in their ATA.
         BOOST_CHECK(!manifest_has(m, user));
      }
   }
} FC_LOG_AND_RETHROW();
// Without a readable `DistributionState` the manifest carries the two state
// singletons only: the handler loads them first and turns an uninitialized
// pool into a logged skip, so nothing behind them is derivable or required.
BOOST_AUTO_TEST_CASE(build_manifests_desyndicate_liq_degrades_to_the_state_singletons) try {
   constexpr uint64_t     request_id = 11;
   manifest_build_harness harness;   // no pool seeded
   const auto manifests = harness.build({desyndicate_liq_effect(0, measurement_pubkey(94), request_id)}, 1);
   BOOST_REQUIRE_EQUAL(manifests.size(), 1u);
   const auto& m = manifests[0];
   BOOST_CHECK_EQUAL(m.size(), 3u);
   BOOST_CHECK(manifest_writes(m, manifest_detail::derive_liqsol_global_state_pda(harness.program_id)));
   BOOST_CHECK(manifest_writes(m, manifest_detail::derive_liqsol_distribution_state_pda(harness.program_id)));
   // A frozen outpost defers before it reads the pool, so the pending-payout PDA
   // rides even when the pool read degrades.
   BOOST_CHECK(manifest_writes(m, manifest_detail::derive_pending_desyndication_pda(harness.program_id, request_id)));
   BOOST_CHECK_EQUAL(harness.hook_reads, 0u);
} FC_LOG_AND_RETHROW();
// Request id 0 is "no id": it cannot key a PendingPayout, and the handler records
// such a payout as unpaid before it asks for the pending account, so the manifest
// carries no pending-payout PDA for it -- and nothing else changes.
BOOST_AUTO_TEST_CASE(build_manifests_desyndicate_liq_omits_the_pending_pda_for_request_id_zero) try {
   const auto liqsol_mint  = measurement_pubkey(90);
   const auto hook_program = measurement_pubkey(91);
   const auto user         = measurement_pubkey(92);

   manifest_build_harness harness;
   harness.put_liq_pool(liqsol_mint);
   harness.put_hook(liqsol_mint, hook_program, {});

   const auto manifests = harness.build({desyndicate_liq_effect(0, user, 0),
                                         // An effect built without an id at all behaves the same.
                                         manifest_detail::inbound_effect{
                                            1, manifest_detail::effect_shape::desyndicate_liq, user,
                                            std::nullopt}},
                                        2);
   BOOST_REQUIRE_EQUAL(manifests.size(), 2u);
   for (size_t i = 0; i < manifests.size(); ++i) {
      BOOST_TEST_CONTEXT("attestation " << i) {
         BOOST_CHECK_EQUAL(manifests[i].size(), 14u);
         BOOST_CHECK(!manifest_has(manifests[i],
                                   manifest_detail::derive_pending_desyndication_pda(harness.program_id, 0)));
      }
   }

   manifest_build_harness degraded;   // no pool seeded
   const auto bare = degraded.build({desyndicate_liq_effect(0, user, 0)}, 1);
   BOOST_REQUIRE_EQUAL(bare.size(), 1u);
   BOOST_CHECK_EQUAL(bare[0].size(), 2u);
} FC_LOG_AND_RETHROW();
// The pending-payout PDA is derived exactly as wire-solana's
// `PendingPayout::find_address` does: `["pending_desyndication",
// request_id.to_le_bytes()]` under liqsol-core. The expected addresses were
// computed independently with `@solana/web3.js`'s `findProgramAddressSync`
// against liqsol-core's declared program id; a multi-byte id pins the
// little-endian encoding.
BOOST_AUTO_TEST_CASE(pending_desyndication_pda_matches_the_program_derivation) try {
   const auto liqsol_core = solana_public_key::from_base58_string("5nBtmutQLrRKBUxNfHJPDjiW5u8id6QM9Hhjg1D1g1XH");
   const std::vector<std::pair<uint64_t, std::string>> expected{
      {1, "6nDs1ohHKjjBjDp9KD8BDvjjqs2ZEYeR3eTHyF7ZNA8T"},
      {7, "BV578hrPb772NQkcMxVUS55QFsJpZw9Xq9hAQXiJioJd"},
      {0x0102030405060708, "5K37mePygNUJ3W9nwD4fkFH9ehUxMVKNyseipwmMrfED"},
   };
   for (const auto& [request_id, address] : expected) {
      BOOST_TEST_CONTEXT("request_id " << request_id) {
         BOOST_CHECK_EQUAL(manifest_detail::derive_pending_desyndication_pda(liqsol_core, request_id)
                              .to_string(fc::yield_function_t{}),
                           address);
      }
   }
   // The seed constant is the program's `PENDING_PAYOUT_SEED`, and the derivation
   // spells it through that constant.
   BOOST_CHECK_EQUAL(manifest_detail::PENDING_DESYNDICATION_SEED, "pending_desyndication");
   std::vector<uint8_t> id_le(sizeof(uint64_t));
   for (size_t i = 0; i < id_le.size(); ++i) id_le[i] = static_cast<uint8_t>(uint64_t{7} >> (8 * i));
   BOOST_CHECK(manifest_detail::derive_pending_desyndication_pda(liqsol_core, 7) ==
               system::find_program_address(
                  {std::vector<uint8_t>(manifest_detail::PENDING_DESYNDICATION_SEED.begin(),
                                        manifest_detail::PENDING_DESYNDICATION_SEED.end()),
                   id_le},
                  liqsol_core)
                  .first);
} FC_LOG_AND_RETHROW();
namespace {

/// Pack a seed list into an `ExtraAccountMeta::address_config`, exactly as
/// `Seed::pack_into_address_config` does on chain: each seed is a 1-byte tag
/// then its payload, and the remainder is left zero (the Uninitialized
/// terminator). Only the two forms the resolver implements are modelled.
struct seed_literal { std::string bytes; };
struct seed_account_key { uint8_t index; };
using packed_seed = std::variant<seed_literal, seed_account_key>;

std::array<uint8_t, 32> pack_seeds(const std::vector<packed_seed>& seeds) {
   std::array<uint8_t, 32> config{};
   size_t at = 0;
   for (const auto& seed : seeds) {
      if (const auto* lit = std::get_if<seed_literal>(&seed)) {
         config[at++] = 1;                                     // Seed::Literal
         config[at++] = static_cast<uint8_t>(lit->bytes.size());
         for (char c : lit->bytes) config[at++] = static_cast<uint8_t>(c);
      } else {
         const auto& acct = std::get<seed_account_key>(seed);
         config[at++] = 3;                                     // Seed::AccountKey
         config[at++] = acct.index;
      }
   }
   return config;
}

manifest_detail::extra_account_meta literal_meta(const solana_public_key& key, bool writable) {
   manifest_detail::extra_account_meta meta;
   meta.discriminator = 0;
   const auto bytes = key._data;
   std::copy(bytes.begin(), bytes.end(), meta.address_config.begin());
   meta.is_writable = writable;
   return meta;
}

/// A literal meta that demands a signature -- a legal `ExtraAccountMeta` shape
/// the dispatch transaction cannot satisfy.
manifest_detail::extra_account_meta signer_meta(const solana_public_key& key) {
   auto meta      = literal_meta(key, false);
   meta.is_signer = true;
   return meta;
}

manifest_detail::extra_account_meta external_pda_meta(uint8_t program_index,
                                                      const std::vector<packed_seed>& seeds,
                                                      bool writable) {
   manifest_detail::extra_account_meta meta;
   meta.discriminator  = static_cast<uint8_t>(program_index + 0x80);
   meta.address_config = pack_seeds(seeds);
   meta.is_writable    = writable;
   return meta;
}

} // anonymous namespace

/// A hook-bearing Token-2022 custody mint -- the case SOL-396 exists for, and
/// the one the ATA-derivation cases above do NOT cover.
///
/// `settle_desyndication` pays through
/// `spl_token_2022::onchain::invoke_transfer_checked`, which for such a mint
/// resolves the hook program, its ExtraAccountMetaList PDA, and every account
/// that PDA declares out of `remaining_accounts`. Omitting them aborts the CPI
/// before the transfer and re-packs the window from the unadvanced cursor.
///
/// The declared metas mirror the real liqSOL hook: a literal program id, then
/// per-participant `user_record` PDAs seeded on the SOURCE and DESTINATION
/// token accounts -- which is why the Execute account order (0 source, 1 mint,
/// 2 destination, 3 authority, 4 validation PDA) is load-bearing rather than
/// decorative.
BOOST_AUTO_TEST_CASE(build_manifests_carry_transfer_hook_accounts_for_a_hook_mint) try {
   const auto spl_mint     = measurement_pubkey(92);
   const auto recipient    = measurement_pubkey(93);
   const auto token_2022 = system::program_ids::TOKEN_2022_PROGRAM;
   const auto hook_program = measurement_pubkey(96);
   const auto hook_owned   = measurement_pubkey(97);   // a literal meta, e.g. liqsol_core

   manifest_build_harness harness;
   harness.put_liq_pool(spl_mint);
   harness.put_hook(spl_mint, hook_program,
                    {literal_meta(hook_owned, false),
                     // sender's user_record, seeded on the SOURCE token account (index 0)
                     external_pda_meta(5, {seed_literal{"user_record"}, seed_account_key{0}}, true),
                     // receiver's user_record, seeded on the DESTINATION (index 2)
                     external_pda_meta(5, {seed_literal{"user_record"}, seed_account_key{2}}, true)});

   const auto manifests =
      harness.build({desyndicate_liq_effect(0, recipient, 1)}, 1);
   BOOST_REQUIRE_EQUAL(manifests.size(), 1u);
   const auto& m = manifests[0];

   const auto vault = system::get_associated_token_address(
      manifest_detail::derive_liqsol_pool_authority_pda(harness.program_id), spl_mint, token_2022);
   const auto dest_ata =
      system::get_associated_token_address(recipient, spl_mint, token_2022);
   const auto validation_pda =
      manifest_detail::derive_extra_account_metas_pda(hook_program, spl_mint);

   // The hook program and its validation PDA.
   BOOST_CHECK(manifest_has(m, hook_program));
   BOOST_CHECK(manifest_has(m, validation_pda));
   // The literal meta.
   BOOST_CHECK(manifest_has(m, hook_owned));

   // The two per-participant PDAs, derived under the meta's NAMED program
   // (index 5 = the first resolved extra, i.e. `hook_owned`) and seeded on the
   // transfer's source and destination respectively.
   const auto source_record = system::find_program_address(
      {std::vector<uint8_t>{'u','s','e','r','_','r','e','c','o','r','d'},
       std::vector<uint8_t>(vault._data.begin(), vault._data.end())}, hook_owned).first;
   const auto dest_record = system::find_program_address(
      {std::vector<uint8_t>{'u','s','e','r','_','r','e','c','o','r','d'},
       std::vector<uint8_t>(dest_ata._data.begin(), dest_ata._data.end())}, hook_owned).first;
   BOOST_CHECK(manifest_has(m, source_record));
   BOOST_CHECK(manifest_has(m, dest_record));

   // Writability is carried from the declaration, not guessed.
   const auto writable_of = [&](const solana_public_key& key) {
      const auto it = std::find_if(m.begin(), m.end(),
                                   [&](const account_meta& a) { return a.key == key; });
      BOOST_REQUIRE(it != m.end());
      return it->is_writable;
   };
   BOOST_CHECK_EQUAL(writable_of(source_record), true);
   BOOST_CHECK_EQUAL(writable_of(dest_record), true);
   BOOST_CHECK_EQUAL(writable_of(hook_owned), false);
   BOOST_CHECK_EQUAL(writable_of(hook_program), false);
} FC_LOG_AND_RETHROW();
namespace {

/// Build a well-formed `ExtraAccountMetaList` account: the Execute TLV
/// discriminator, a u32 entry length, then a PodSlice (u32 count + 35-byte
/// records). `declared_count` is what the header CLAIMS, so a test can make it
/// disagree with the bytes actually present.
std::vector<uint8_t> validation_account(uint32_t declared_count, uint32_t present_records,
                                        std::optional<uint32_t> entry_len_override = std::nullopt) {
   const std::array<uint8_t, 8> execute_disc = {0x69, 0x25, 0x65, 0xc5, 0x4b, 0xfb, 0x66, 0x1a};
   const uint32_t entry_len = entry_len_override.value_or(4 + present_records * 35);

   std::vector<uint8_t> data(execute_disc.begin(), execute_disc.end());
   const auto push_u32 = [&](uint32_t v) {
      data.push_back(static_cast<uint8_t>(v & 0xff));
      data.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
      data.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
      data.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
   };
   push_u32(entry_len);
   push_u32(declared_count);
   for (uint32_t i = 0; i < present_records; ++i) data.insert(data.end(), 35, 0);
   return data;
}

} // anonymous namespace

/// The parser must THROW on every malformed shape rather than returning a short
/// list. A partial parse is the worst outcome available: the caller wraps it as
/// authoritative, the manifest carries the hook program and validation PDA while
/// silently omitting declared metas, and the CPI then fails with
/// `IncorrectAccount` -- window aborted, cursor pinned, nothing naming the cause.
BOOST_AUTO_TEST_CASE(extra_account_meta_parser_refuses_malformed_accounts) try {
   using manifest_detail::parse_extra_account_metas;

   // Sanity: the well-formed shape parses, so the rejections below are about
   // malformation and not about the builder being wrong.
   BOOST_CHECK_EQUAL(parse_extra_account_metas(validation_account(2, 2)).size(), 2u);

   // Too short to hold a TLV header at all.
   BOOST_CHECK_THROW(parse_extra_account_metas(std::vector<uint8_t>(11, 0)), fc::assert_exception);

   // Entry claims more bytes than the account holds.
   BOOST_CHECK_THROW(parse_extra_account_metas(validation_account(1, 1, /*entry_len*/ 4096)),
                     fc::assert_exception);

   // THE DANGEROUS ONE: a count that overruns its own entry. Bounding the walk
   // by the account size instead of the entry's extent would parse the FOLLOWING
   // TLV bytes as metas and push arbitrary pubkeys in as writable accounts.
   auto lying = validation_account(/*declared*/ 8, /*present*/ 1);
   lying.insert(lying.end(), 35 * 8, 0xAB);   // plenty of trailing bytes to walk into
   BOOST_CHECK_THROW(parse_extra_account_metas(lying), fc::assert_exception);

   // No Execute entry: the mint declares a hook whose accounts are undeterminable.
   auto wrong_disc = validation_account(1, 1);
   wrong_disc[0] ^= 0xff;
   BOOST_CHECK_THROW(parse_extra_account_metas(wrong_disc), fc::assert_exception);
} FC_LOG_AND_RETHROW();
/// The DESYNDICATE_LIQ manifest under the REAL liqSOL hook declaration, pending-
/// payout PDA included. Every meta the hook declares resolves to an account the
/// handler already requires (liqsol-core, the two `UserRecord`s,
/// `DistributionState`, the bucket ATA), so the hook adds only its program and
/// extra-metas PDA and one desyndication costs 15 of the 16 dynamic slots. The
/// packer therefore carries one desyndication per dispatch window (two would
/// need 18). Pinned exactly so the next program-side account spends the last
/// slot visibly.
BOOST_AUTO_TEST_CASE(desyndicate_liq_manifest_under_the_real_hook_fits_the_dispatch_budget) try {
   const auto liqsol_mint  = measurement_pubkey(110);
   const auto hook_program = measurement_pubkey(111);

   manifest_build_harness harness;
   const auto bucket_ata = system::get_associated_token_address(
      manifest_detail::derive_liqsol_bucket_authority_pda(harness.program_id), liqsol_mint,
      system::program_ids::TOKEN_2022_PROGRAM);
   harness.put_liq_pool(liqsol_mint);
   harness.put_hook(liqsol_mint, hook_program,
                    {literal_meta(harness.program_id, false),
                     external_pda_meta(5, {seed_literal{"user_record"}, seed_account_key{0}}, true),
                     external_pda_meta(5, {seed_literal{"user_record"}, seed_account_key{2}}, true),
                     external_pda_meta(5, {seed_literal{"distribution_state"}}, true),
                     literal_meta(bucket_ata, false)});

   const auto manifests = harness.build({desyndicate_liq_effect(0, measurement_pubkey(112), 9)}, 1);
   BOOST_REQUIRE_EQUAL(manifests.size(), 1u);
   BOOST_CHECK_LE(manifests[0].size(), sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS);
   BOOST_CHECK_EQUAL(manifests[0].size(), 15u);
   BOOST_CHECK(manifest_has(manifests[0],
                            manifest_detail::derive_pending_desyndication_pda(harness.program_id, 9)));
} FC_LOG_AND_RETHROW();
/// A `ExtraAccountMeta` may legally demand a signature. The dispatch
/// transaction is signed by the batch operator alone, so the resolver cannot
/// satisfy one -- and dropping the flag silently (keeping only key+writable)
/// would build a manifest that aborts inside the CPI as an unauthorized signer,
/// with the cursor pinned and no local diagnostic. Refuse at build time.
BOOST_AUTO_TEST_CASE(hook_manifest_refuses_a_meta_demanding_a_signature) try {
   const auto spl_mint     = measurement_pubkey(120);
   const auto recipient    = measurement_pubkey(122);
   const auto hook_program = measurement_pubkey(123);
   const auto needs_sig    = measurement_pubkey(124);

   manifest_build_harness harness;
   harness.put_liq_pool(spl_mint);

   // Sanity: the same declaration WITHOUT the signer flag builds fine, so the
   // rejection below is about the flag and not about the meta being unresolvable.
   harness.put_hook(spl_mint, hook_program, {literal_meta(needs_sig, false)});
   BOOST_CHECK_NO_THROW(
      harness.build({desyndicate_liq_effect(0, recipient, 1)}, 1));

   harness.hooks.clear();
   harness.put_hook(spl_mint, hook_program, {signer_meta(needs_sig)});
   BOOST_CHECK_THROW(
      harness.build({desyndicate_liq_effect(0, recipient, 1)}, 1),
      fc::assert_exception);
} FC_LOG_AND_RETHROW();
/// The mint TLV walk itself: what it reads, and what it refuses.
///
/// The refusals matter for the same reason `parse_extra_account_metas`'s do --
/// a malformed entry read as "no hook" produces a hook-free manifest for a mint
/// that may well declare one, which aborts in the CPI rather than here.
BOOST_AUTO_TEST_CASE(mint_transfer_hook_parser_reads_and_refuses) try {
   using manifest_detail::mint_transfer_hook_program;

   constexpr uint16_t transfer_hook_ext = 14;
   constexpr size_t   tlv_start         = 166;   // 165-byte base + the AccountType byte

   // A Token-2022 mint carrying one TLV entry of `declared_len` bytes, whose
   // value is `value` padded/truncated to that length.
   const auto mint_with_entry = [&](uint16_t ext_type, uint16_t declared_len,
                                    std::vector<uint8_t> value, bool truncate_account) {
      std::vector<uint8_t> data(tlv_start, 0);
      data.push_back(static_cast<uint8_t>(ext_type & 0xff));
      data.push_back(static_cast<uint8_t>(ext_type >> 8));
      data.push_back(static_cast<uint8_t>(declared_len & 0xff));
      data.push_back(static_cast<uint8_t>(declared_len >> 8));
      if (!truncate_account) value.resize(declared_len, 0);
      data.insert(data.end(), value.begin(), value.end());
      return data;
   };

   const auto hook_program = measurement_pubkey(140);
   std::vector<uint8_t> configured(32, 0xAA);   // authority
   configured.insert(configured.end(), hook_program._data.begin(), hook_program._data.end());

   // Reads the program id out of the second 32 bytes.
   const auto found = mint_transfer_hook_program(mint_with_entry(transfer_hook_ext, 64,
                                                                 configured, false));
   BOOST_REQUIRE(found.has_value());
   BOOST_CHECK(*found == hook_program);

   // A legacy SPL mint is 82 bytes and never reaches the TLV at all.
   BOOST_CHECK(!mint_transfer_hook_program(std::vector<uint8_t>(82, 0)).has_value());

   // Extension present, hook explicitly disabled by an all-zero program id.
   BOOST_CHECK(!mint_transfer_hook_program(
                   mint_with_entry(transfer_hook_ext, 64, std::vector<uint8_t>(64, 0), false))
                   .has_value());

   // A different extension is walked past, not mistaken for a hook.
   BOOST_CHECK(!mint_transfer_hook_program(
                   mint_with_entry(/*MintCloseAuthority*/ 3, 32, std::vector<uint8_t>(32, 0x11),
                                   false))
                   .has_value());

   // REFUSED: the entry claims more bytes than the account holds.
   BOOST_CHECK_THROW(mint_transfer_hook_program(
                        mint_with_entry(transfer_hook_ext, 4096, configured, true)),
                     fc::assert_exception);

   // REFUSED: a TransferHook entry too short to carry authority + program id.
   // Falling through to the next entry would report "no hook" for a mint that
   // demonstrably declares one.
   BOOST_CHECK_THROW(mint_transfer_hook_program(
                        mint_with_entry(transfer_hook_ext, 32, std::vector<uint8_t>(32, 0xAA),
                                        false)),
                     fc::assert_exception);
} FC_LOG_AND_RETHROW();
/// The result is sized to the envelope's attestation TOTAL and indexed by the
/// flat dispatch position, so an attestation needing no effect account keeps
/// an empty entry and the cursor stays aligned with the manifests.
BOOST_AUTO_TEST_CASE(build_manifests_index_by_flat_dispatch_position) try {
   manifest_build_harness harness;
   const auto payee = measurement_pubkey(93);
   const auto manifests = harness.build({desyndicate_liq_effect(2, payee, 7)}, 4);
   BOOST_REQUIRE_EQUAL(manifests.size(), 4u);
   BOOST_CHECK(manifests[0].empty());
   BOOST_CHECK(manifests[1].empty());
   BOOST_CHECK(manifests[3].empty());
   BOOST_REQUIRE_EQUAL(manifests[2].size(), 3u);
   BOOST_CHECK(manifest_has(manifests[2], manifest_detail::derive_pending_desyndication_pda(harness.program_id, 7)));
   BOOST_CHECK_EQUAL(harness.liq_pool_reads, 1u);
} FC_LOG_AND_RETHROW();
// ── drive_dispatch_rounds: the dispatch-crank state machine, driven through
//    its RPC seams (progress read + dispatch send) with a scripted on-chain
//    model. Both confirmed drain defects (the zero-attestation early return
//    and the dispatch throw fused into delivery) lived in this previously
//    untested region -- these tests pin the loop's packing, dispatch_limit
//    sizing, cursor resume, and every exit.

namespace {

namespace drive_detail = sysio::outpost_solana_client_detail;

/// Scripted stand-in for the on-chain `EpochDeliveries` + dispatch send.
/// `send` records each batch and, like the program, advances the cursor by
/// `limit` (clamped to the total) unless `advance_on_send` is cleared to model
/// a send whose effect this relay never observes (another caller's drain, a
/// dropped tx).
struct dispatch_drive_harness {
   explicit dispatch_drive_harness(uint32_t total_attestations,
                                   bool     consensus_reached = true,
                                   uint32_t initial_cursor    = 0)
      : total(total_attestations) {
      chain.consensus_reached = consensus_reached;
      chain.dispatched_count  = initial_cursor;
   }

   struct sent_batch {
      uint32_t                  limit = 0;
      std::vector<account_meta> accounts;
   };

   uint32_t                              total;
   drive_detail::epoch_dispatch_progress chain;
   bool                                  advance_on_send = true;
   std::vector<sent_batch>               sends;

   std::function<void()> no_deadline() {
      return [] {};
   }
   std::function<drive_detail::epoch_dispatch_progress()> read() {
      return [this] { return chain; };
   }
   std::function<std::string(uint32_t, std::vector<account_meta>)> send() {
      return [this](uint32_t limit, std::vector<account_meta> accounts) {
         sends.push_back(sent_batch{limit, std::move(accounts)});
         if (advance_on_send) {
            chain.dispatched_count = std::min(chain.dispatched_count + limit, total);
         }
         return "sig-" + std::to_string(sends.size() - 1);
      };
   }

   std::string drive(const std::vector<std::vector<account_meta>>& per_attestation,
                     const std::function<void()>&                  deadline_probe) {
      return drive_detail::drive_dispatch_rounds(
         7, per_attestation, deadline_probe, read(), send(), "test-relay");
   }
   std::string drive(const std::vector<std::vector<account_meta>>& per_attestation) {
      return drive(per_attestation, no_deadline());
   }
};

/// `count` manifests of `accounts_each` DISTINCT account metas apiece --
/// distinct across the whole fixture, so every attestation widens the batch
/// union by exactly `accounts_each`.
std::vector<std::vector<account_meta>> distinct_manifests(uint32_t count,
                                                          uint32_t accounts_each) {
   std::vector<std::vector<account_meta>> manifests(count);
   uint32_t seed = 1;
   for (auto& manifest : manifests) {
      for (uint32_t i = 0; i < accounts_each; ++i) {
         manifest.emplace_back(measurement_pubkey(seed++), false, true);
      }
   }
   return manifests;
}

} // namespace

BOOST_AUTO_TEST_CASE(drive_dispatch_zero_attestation_envelope_sends_one_close_crank) try {
   // A zero-attestation envelope still has to CLOSE its epoch: the program's
   // completion block runs on a crank whose window clamps to the empty
   // envelope, and `dispatch_attestations` is the only place
   // `next_epoch_index` advances. The old loop's `0 >= 0` early return meant
   // NO relay ever cranked, stranding the epoch with consensus tipped.
   dispatch_drive_harness harness(0);
   harness.advance_on_send = false;  // the program closes the epoch; the cursor stays 0
   const auto sig = harness.drive({});
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
   BOOST_CHECK_EQUAL(harness.sends[0].limit, 1u);
   BOOST_CHECK(harness.sends[0].accounts.empty());
   BOOST_CHECK_EQUAL(sig, "sig-0");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_zero_attestation_waits_for_consensus) try {
   dispatch_drive_harness harness(0, /*consensus_reached=*/false);
   const auto sig = harness.drive({});
   BOOST_CHECK(harness.sends.empty());
   BOOST_CHECK_EQUAL(sig, "");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_consensus_not_reached_sends_nothing) try {
   dispatch_drive_harness harness(3, /*consensus_reached=*/false);
   const auto sig = harness.drive(distinct_manifests(3, 2));
   BOOST_CHECK(harness.sends.empty());
   BOOST_CHECK_EQUAL(sig, "");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_already_drained_cursor_sends_nothing) try {
   dispatch_drive_harness harness(3, true, /*initial_cursor=*/3);
   const auto sig = harness.drive(distinct_manifests(3, 2));
   BOOST_CHECK(harness.sends.empty());
   BOOST_CHECK_EQUAL(sig, "");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_single_round_settles_small_manifests) try {
   // 3 attestations x 2 distinct accounts = a 6-account union, well inside
   // the 16-account budget -- one send covers the whole envelope.
   dispatch_drive_harness harness(3);
   const auto sig = harness.drive(distinct_manifests(3, 2));
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
   BOOST_CHECK_EQUAL(harness.sends[0].limit, 3u);
   BOOST_CHECK_EQUAL(harness.sends[0].accounts.size(), 6u);
   BOOST_CHECK_EQUAL(sig, "sig-0");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_packs_union_to_exact_account_budget) try {
   // 8 distinct accounts per attestation: two fill the 16-account budget
   // EXACTLY (16 is allowed; the pack breaks only past it), a third would
   // make 24 -- so 4 attestations settle as two 2-wide rounds.
   dispatch_drive_harness harness(4);
   harness.drive(distinct_manifests(4, 8));
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 2u);
   BOOST_CHECK_EQUAL(harness.sends[0].limit, 2u);
   BOOST_CHECK_EQUAL(harness.sends[0].accounts.size(), sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS);
   BOOST_CHECK_EQUAL(harness.sends[1].limit, 2u);
   BOOST_CHECK_EQUAL(harness.sends[1].accounts.size(), sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_shared_accounts_pack_as_a_union) try {
   // Every attestation references the SAME 10 accounts: the batch union stays
   // at 10, so all 3 fit one round even though 3 x 10 raw metas would not.
   dispatch_drive_harness harness(3);
   std::vector<account_meta> shared;
   for (uint32_t i = 0; i < 10; ++i) {
      shared.emplace_back(measurement_pubkey(1000 + i), false, true);
   }
   harness.drive({shared, shared, shared});
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
   BOOST_CHECK_EQUAL(harness.sends[0].limit, 3u);
   BOOST_CHECK_EQUAL(harness.sends[0].accounts.size(), 10u);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_resumes_from_nonzero_cursor) try {
   // Cursor already at 2 of 5 (a prior partial drain): the batch starts AT
   // the cursor and carries only the unsettled manifests' accounts.
   const auto manifests = distinct_manifests(5, 2);
   dispatch_drive_harness harness(5, true, /*initial_cursor=*/2);
   harness.drive(manifests);
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
   BOOST_CHECK_EQUAL(harness.sends[0].limit, 3u);
   BOOST_CHECK_EQUAL(harness.sends[0].accounts.size(), 6u);
   const auto& sent = harness.sends[0].accounts;
   auto sent_has = [&](const solana_public_key& key) {
      return std::any_of(sent.begin(), sent.end(),
                         [&](const account_meta& meta) { return meta.key == key; });
   };
   BOOST_CHECK(!sent_has(manifests[0][0].key));   // settled before the cursor
   BOOST_CHECK(!sent_has(manifests[1][0].key));
   BOOST_CHECK(sent_has(manifests[2][0].key));    // the resumed window
   BOOST_CHECK(sent_has(manifests[4][1].key));
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_oversized_single_manifest_still_takes_one) try {
   // One attestation whose own manifest exceeds the budget: the pack must
   // still take it (alone) so it can make progress -- an unsendable manifest
   // fails loudly at serialize, not by wedging the cursor.
   constexpr uint32_t oversized = sysio::MAX_TERMINAL_DYNAMIC_ACCOUNTS + 4;
   dispatch_drive_harness harness(1);
   harness.drive(distinct_manifests(1, oversized));
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
   BOOST_CHECK_EQUAL(harness.sends[0].limit, 1u);
   BOOST_CHECK_EQUAL(harness.sends[0].accounts.size(), oversized);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_exits_when_cursor_does_not_advance) try {
   // The send lands but the observed cursor does not move: another caller is
   // draining this envelope (or the read raced the write). One send, then a
   // clean exit -- never a spin.
   dispatch_drive_harness harness(2);
   harness.advance_on_send = false;
   const auto sig = harness.drive(distinct_manifests(2, 2));
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
   BOOST_CHECK_EQUAL(sig, "sig-0");
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_round_exhaustion_alarms_and_returns) try {
   // 9-account manifests force one attestation per round; two more
   // attestations than the round budget means the loop stops at the budget.
   // Under tick-driven cranking exhaustion is an ALARM (elog), not a caller
   // failure: the cursor persists on-chain and the next inbound tick resumes
   // from it, so the call returns the last signature it sent instead of
   // throwing away that bounded progress.
   const uint32_t total = sysio::MAX_DISPATCH_ROUNDS + 2;
   dispatch_drive_harness harness(total);
   const auto sig = harness.drive(distinct_manifests(total, 9));
   BOOST_CHECK_EQUAL(harness.sends.size(), sysio::MAX_DISPATCH_ROUNDS);
   BOOST_CHECK_EQUAL(sig, "sig-" + std::to_string(sysio::MAX_DISPATCH_ROUNDS - 1));
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(drive_dispatch_deadline_expiry_throws) try {
   // The deadline probe fires once per round, before any read or send. Let
   // round 0 through and expire on round 1: exactly one batch goes out and
   // the expiry propagates to the caller.
   dispatch_drive_harness harness(2);
   uint32_t probes = 0;
   BOOST_CHECK_EXCEPTION(
      harness.drive(distinct_manifests(2, 9),
                    [&] {
                       if (probes++ > 0) {
                          FC_THROW_EXCEPTION(fc::timeout_exception,
                                             "deadline exceeded (test probe)");
                       }
                    }),
      fc::timeout_exception,
      [](const fc::timeout_exception& e) {
         return e.to_detail_string().find("deadline exceeded") != std::string::npos;
      });
   BOOST_REQUIRE_EQUAL(harness.sends.size(), 1u);
} FC_LOG_AND_RETHROW();
// ── LatestOutboundEnvelope inbound-read path. The epoch=511 RCA was
//    hardcoded STANDALONE offsets (epoch@8) silently misreading the
//    INTEGRATED account (`bump`=0xFF at byte 8, `epoch_index`=1 at byte 9 ⇒
//    u32@8 = 0xFF | 1<<8 = 511), stalling the relay without an error. The
//    reader now decodes the whole account through libfc's IDL-driven
//    `decode_account_data` (declared field order + Anchor discriminator) and
//    validates the stored keccak256 checksum, so these tests drive the
//    complete post-fetch read path against synthesized accounts for BOTH
//    program layouts, plus every reject path.

namespace {

using sysio::outpost_solana_client_detail::assert_latest_envelope_shape;
using sysio::outpost_solana_client_detail::borsh_payload_bytes;
using sysio::outpost_solana_client_detail::decode_latest_envelope_account;
using sysio::outpost_solana_client_detail::select_program_idls_matching;

/// Shorthand for a primitive IDL type.
idl::idl_type prim(idl::primitive_type p) { return idl::idl_type::make_primitive(p); }

/// `[u8; 32]` — the checksum field's declared shape in both known layouts.
idl::idl_type u8_32_array() { return idl::idl_type::make_array(prim(idl::primitive_type::u8), 32); }

/// Field list of the standalone `opp_outpost` layout:
/// {epoch_index, checksum, data(bytes), bump}.
std::vector<idl::field> standalone_latest_fields() {
   return {{"epoch_index", prim(idl::primitive_type::u32)},
           {"checksum", u8_32_array()},
           {"data", prim(idl::primitive_type::bytes)},
           {"bump", prim(idl::primitive_type::u8)}};
}

/// Field list of the integrated `liqsol_core` layout:
/// {bump, epoch_index, checksum, data(Vec<u8>)}. Declaring `data` as
/// `Vec<u8>` (vs the standalone `bytes`) also exercises the second decoded
/// payload shape (integer array vs base64 string).
std::vector<idl::field> integrated_latest_fields() {
   return {{"bump", prim(idl::primitive_type::u8)},
           {"epoch_index", prim(idl::primitive_type::u32)},
           {"checksum", u8_32_array()},
           {"data", idl::idl_type::make_vec(prim(idl::primitive_type::u8))}};
}

/// Declare `LatestOutboundEnvelope` with `fields` on `prog` — inline on the
/// account (legacy IDL shape) or via the `types` section (Anchor IDL v2
/// keeps account struct fields there).
///
/// Any declaration `prog` already carries under that name is REMOVED first, so
/// the synthesized one is authoritative. `idl::program::find_account` and
/// `find_type` both return the FIRST match, so appending beside a fixture-borne
/// declaration would silently bind the decoder to the FIXTURE's field order and
/// make every layout test below decode a shape it did not ask for. The failure
/// is not loud: `decode_latest_envelope_catches_field_order_drift` asserts an
/// EMPTY result, and a wrong-order decode also yields empty, so it would keep
/// passing while testing nothing — and it is the epoch=511 RCA regression
/// guard. Erasing here keeps these tests independent of whatever
/// `solana-idl-opp-outpost-stub.json` happens to declare.
void declare_latest_envelope(idl::program& prog, std::vector<idl::field> fields,
                             bool fields_in_types_section) {
   constexpr std::string_view latest_outbound_envelope = "LatestOutboundEnvelope";
   std::erase_if(prog.accounts,
                 [&](const idl::account& a) { return a.name == latest_outbound_envelope; });
   std::erase_if(prog.types,
                 [&](const idl::type_def& t) { return t.name == latest_outbound_envelope; });

   idl::account account;
   account.name = std::string{latest_outbound_envelope};
   account.compute_discriminator();
   if (fields_in_types_section) {
      idl::type_def def;
      def.name          = account.name;
      def.struct_fields = std::move(fields);
      prog.types.push_back(std::move(def));
   } else {
      account.fields = std::move(fields);
   }
   prog.accounts.push_back(std::move(account));
}

/// Build a minimal synthetic program declaring only `LatestOutboundEnvelope`.
idl::program latest_envelope_program(std::vector<idl::field> fields, bool fields_in_types_section) {
   idl::program prog;
   prog.name = "layout_fixture";
   declare_latest_envelope(prog, std::move(fields), fields_in_types_section);
   return prog;
}

/// Build a minimal synthetic program declaring ONE account named `account_name`
/// with `fields`, in either IDL field home — the drift shapes a boot check must
/// refuse before a batch operator ever cranks. Shared by the `EpochDeliveries`
/// and `DistributionState` shape tests so both drive the same declaration machinery.
idl::program named_account_program(std::string account_name, std::vector<idl::field> fields,
                                   bool fields_in_types_section) {
   idl::program prog;
   prog.name = account_name + "_fixture";
   idl::account account;
   account.name = std::move(account_name);
   account.compute_discriminator();
   if (fields_in_types_section) {
      idl::type_def def;
      def.name          = account.name;
      def.struct_fields = std::move(fields);
      prog.types.push_back(std::move(def));
   } else {
      account.fields = std::move(fields);
   }
   prog.accounts.push_back(std::move(account));
   return prog;
}

/// Stub outpost program (real instruction set, so `opp_solana_outpost_client`
/// constructs) + a synthesized `LatestOutboundEnvelope` declaration.
idl::program outpost_program_with_latest(std::vector<idl::field> fields) {
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   declare_latest_envelope(prog, std::move(fields), /*fields_in_types_section=*/true);
   return prog;
}

/// Serialized protobuf Envelope carrying `epoch` — the payload the outpost
/// program stores in `LatestOutboundEnvelope.data`.
std::vector<uint8_t> envelope_payload_bytes(uint32_t epoch) {
   sysio::opp::Envelope env;
   env.set_epoch_index(epoch);
   std::string buf;
   env.SerializeToString(&buf);
   return std::vector<uint8_t>(buf.begin(), buf.end());
}

/// `keccak256(payload)` — the checksum both program versions store.
std::array<uint8_t, 32> keccak_checksum(const std::vector<uint8_t>& payload) {
   const auto hash =
      fc::crypto::keccak256::hash(std::span<const uint8_t>(payload.data(), payload.size()));
   std::array<uint8_t, 32> out{};
   std::memcpy(out.data(), hash.data(), out.size());
   return out;
}

/// Serialize a STANDALONE-layout account: disc + {epoch, checksum, data, bump}.
std::vector<uint8_t> standalone_latest_account(uint32_t epoch,
                                               const std::vector<uint8_t>& payload,
                                               const std::array<uint8_t, 32>& checksum) {
   borsh::encoder enc;
   const auto disc = idl::compute_account_discriminator("LatestOutboundEnvelope");
   enc.write_fixed_bytes(disc.data(), disc.size());
   enc.write_u32(epoch);
   enc.write_fixed_bytes(checksum.data(), checksum.size());
   enc.write_bytes(payload);
   enc.write_u8(0xFF); // bump
   return enc.data();
}

/// Serialize an INTEGRATED-layout account: disc + {bump, epoch, checksum, data}.
/// `bump`=0xFF reproduces the epoch=511 RCA input when misread as standalone.
std::vector<uint8_t> integrated_latest_account(uint32_t epoch,
                                               const std::vector<uint8_t>& payload,
                                               const std::array<uint8_t, 32>& checksum) {
   borsh::encoder enc;
   const auto disc = idl::compute_account_discriminator("LatestOutboundEnvelope");
   enc.write_fixed_bytes(disc.data(), disc.size());
   enc.write_u8(0xFF); // bump
   enc.write_u32(epoch);
   enc.write_fixed_bytes(checksum.data(), checksum.size());
   enc.write_bytes(payload);
   return enc.data();
}

/// Drive the complete post-fetch read path: construct the typed outpost
/// client around a stub program declaring `fields`, then decode
/// `account_bytes` asking for `requested_epoch`. No RPC endpoint is needed —
/// decode is pure and the null client is never dereferenced.
std::vector<char> decode_with_layout(std::vector<idl::field>     fields,
                                     const std::vector<uint8_t>& account_bytes,
                                     uint32_t                    requested_epoch) {
   const auto prog = outpost_program_with_latest(std::move(fields));
   sysio::opp_solana_outpost_client program_client(
      solana_client_ptr{}, measurement_pubkey(42), {prog});
   return decode_latest_envelope_account(program_client, account_bytes, requested_epoch, "test");
}

/// Compare a decoded envelope byte vector against the synthesized payload.
bool bytes_equal(const std::vector<char>& actual, const std::vector<uint8_t>& expected) {
   return actual.size() == expected.size() &&
          std::equal(actual.begin(), actual.end(), expected.begin(),
                     [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; });
}

/// Name-and-address-only IDL program for selection tests.
idl::program named_program(std::string address) {
   idl::program prog;
   prog.name    = "opp_outpost";
   prog.address = std::move(address);
   return prog;
}

/// Stub outpost program + the outbound `EnvelopeLog` as liqsol-core declares it: {bump, envelopes: Vec<
/// EnvelopeRecord{epoch_index, emitted_at, checksum}>}.
idl::program outpost_program_with_envelope_log() {
   constexpr std::string_view envelope_log    = "EnvelopeLog";
   constexpr std::string_view envelope_record = "EnvelopeRecord";
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   std::erase_if(prog.accounts, [&](const idl::account& a) { return a.name == envelope_log; });
   std::erase_if(prog.types,
                 [&](const idl::type_def& t) { return t.name == envelope_log || t.name == envelope_record; });

   idl::type_def record;
   record.name          = std::string{envelope_record};
   record.struct_fields = std::vector<idl::field>{{"epoch_index", prim(idl::primitive_type::u32)},
                                                  {"emitted_at", prim(idl::primitive_type::u64)},
                                                  {"checksum", u8_32_array()}};
   prog.types.push_back(std::move(record));
   idl::type_def log;
   log.name          = std::string{envelope_log};
   log.struct_fields = std::vector<idl::field>{
      {"bump", prim(idl::primitive_type::u8)},
      {"envelopes", idl::idl_type::make_vec(idl::idl_type::make_defined(std::string{envelope_record}))}};
   prog.types.push_back(std::move(log));
   idl::account account;
   account.name = std::string{envelope_log};
   account.compute_discriminator();
   prog.accounts.push_back(std::move(account));
   return prog;
}

/// Serialize an `EnvelopeLog` account holding one record per (epoch, checksum).
std::vector<uint8_t> envelope_log_account(const std::vector<std::pair<uint32_t, std::array<uint8_t, 32>>>& records) {
   borsh::encoder enc;
   const auto disc = idl::compute_account_discriminator("EnvelopeLog");
   enc.write_fixed_bytes(disc.data(), disc.size());
   enc.write_u8(0xFF); // bump
   enc.write_u32(static_cast<uint32_t>(records.size()));
   for (const auto& [epoch, checksum] : records) {
      enc.write_u32(epoch);
      enc.write_u64(1'700'000'000); // emitted_at
      enc.write_fixed_bytes(checksum.data(), checksum.size());
   }
   return enc.data();
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(latest_envelope_shape_accepts_known_layouts) try {
   // Field ORDER is unconstrained — the reader decodes through the IDL at
   // runtime — so both known layouts pass, in both IDL field homes.
   for (bool in_types : {false, true}) {
      BOOST_TEST_CONTEXT("fields_in_types_section=" << in_types) {
         BOOST_CHECK_NO_THROW(
            assert_latest_envelope_shape(latest_envelope_program(standalone_latest_fields(), in_types)));
         BOOST_CHECK_NO_THROW(
            assert_latest_envelope_shape(latest_envelope_program(integrated_latest_fields(), in_types)));
         // The IDL-driven decoder follows the declared field order at runtime,
         // so a variable-length field ahead of the payload or an epoch-after-
         // data order still decodes: the shape check validates field TYPES,
         // never field ORDER, and must accept these.
         BOOST_CHECK_NO_THROW(assert_latest_envelope_shape(latest_envelope_program(
            {{"note", prim(idl::primitive_type::string)},
             {"data", prim(idl::primitive_type::bytes)},
             {"epoch_index", prim(idl::primitive_type::u32)}},
            in_types)));
      }
   }
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(latest_envelope_shape_rejects_misdeclared_idls) try {
   struct reject_case {
      const char*             name;
      std::vector<idl::field> fields;
   };

   std::vector<reject_case> cases;
   cases.push_back({"missing epoch_index field",
                    {{"bump", prim(idl::primitive_type::u8)},
                     {"checksum", u8_32_array()},
                     {"data", prim(idl::primitive_type::bytes)}}});
   cases.push_back({"missing data field",
                    {{"epoch_index", prim(idl::primitive_type::u32)},
                     {"checksum", u8_32_array()},
                     {"bump", prim(idl::primitive_type::u8)}}});
   // A wrong declared width means the IDL disagrees with the on-chain u32.
   cases.push_back({"epoch_index declared u16",
                    {{"epoch_index", prim(idl::primitive_type::u16)},
                     {"data", prim(idl::primitive_type::bytes)}}});
   cases.push_back({"epoch_index declared u64",
                    {{"epoch_index", prim(idl::primitive_type::u64)},
                     {"data", prim(idl::primitive_type::bytes)}}});
   // A fixed `[u8; N]` payload cannot represent the variable-length envelope.
   cases.push_back({"data declared fixed [u8; 64] array",
                    {{"epoch_index", prim(idl::primitive_type::u32)},
                     {"data", idl::idl_type::make_array(prim(idl::primitive_type::u8), 64)}}});
   // A malformed type object (primitive kind without a primitive value) must
   // hit the per-field diagnostic, not crash formatting it.
   cases.push_back({"data declared with malformed type object",
                    {{"epoch_index", prim(idl::primitive_type::u32)},
                     {"data", idl::idl_type{}}}});

   for (const auto& c : cases) {
      BOOST_TEST_CONTEXT(c.name) {
         for (bool in_types : {false, true}) {
            BOOST_CHECK_THROW(
               assert_latest_envelope_shape(latest_envelope_program(c.fields, in_types)),
               fc::assert_exception);
         }
      }
   }

   // No `LatestOutboundEnvelope` account declared at all.
   BOOST_CHECK_THROW(assert_latest_envelope_shape(idl::program{}), fc::assert_exception);
   // Account declared but with no field definition anywhere (inline or types).
   BOOST_CHECK_THROW(assert_latest_envelope_shape(latest_envelope_program({}, false)),
                     fc::assert_exception);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(epoch_deliveries_shape_accepts_the_deployed_declaration) try {
   using sysio::outpost_solana_client_detail::assert_epoch_deliveries_shape;

   // The stub fixture carries the program's own `EpochDeliveries` struct, so
   // this is the boot check running against the shape a batch operator will
   // actually meet. It is what makes the cursor read safe: both fields are
   // guaranteed present before the first drain.
   BOOST_CHECK_NO_THROW(assert_epoch_deliveries_shape(load_idl_fixture(opp_outpost_idl_fixture)));
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(epoch_deliveries_shape_rejects_drifted_declarations) try {
   using sysio::outpost_solana_client_detail::assert_epoch_deliveries_shape;

   // A PRESENT but drifted EpochDeliveries is the dangerous case: the account
   // decodes, the row simply lacks the field, `consensus_reached` reads false
   // forever, and the drain no-ops every tick behind a dlog with the epoch
   // never closing. Each of these must fail at BOOT instead.
   struct reject_case {
      const char*             name;
      std::vector<idl::field> fields;
   };
   const auto bool_t = prim(idl::primitive_type::bool_t);
   const auto u32_t  = prim(idl::primitive_type::u32);

   std::vector<reject_case> cases;
   cases.push_back({"consensus_reached renamed away",
                    {{"consensus", bool_t}, {"dispatched_count", u32_t}}});
   cases.push_back({"dispatched_count dropped",
                    {{"consensus_reached", bool_t}, {"bump", prim(idl::primitive_type::u8)}}});
   cases.push_back({"consensus_reached declared u8",
                    {{"consensus_reached", prim(idl::primitive_type::u8)},
                     {"dispatched_count", u32_t}}});
   cases.push_back({"dispatched_count declared u64",
                    {{"consensus_reached", bool_t},
                     {"dispatched_count", prim(idl::primitive_type::u64)}}});

   for (const auto& c : cases) {
      BOOST_TEST_CONTEXT(c.name) {
         for (bool in_types : {false, true}) {
            BOOST_CHECK_THROW(
               assert_epoch_deliveries_shape(named_account_program("EpochDeliveries", c.fields, in_types)),
               fc::assert_exception);
         }
      }
   }

   // No `EpochDeliveries` account declared at all, and one declared with no
   // field definition in either home.
   BOOST_CHECK_THROW(assert_epoch_deliveries_shape(idl::program{}), fc::assert_exception);
   BOOST_CHECK_THROW(assert_epoch_deliveries_shape(named_account_program("EpochDeliveries", {}, false)),
                     fc::assert_exception);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(distribution_state_shape_accepts_a_pubkey_mint_and_rejects_drift) try {
   using sysio::outpost_solana_client_detail::assert_distribution_state_shape;

   // The stub fixture carries the integrated program's `DistributionState`, so
   // this is the boot check running against the declaration a batch operator
   // meets. A standalone outpost program declares none at all; the boot check
   // skips the assert (and logs it) there, and the assert itself refuses it.
   BOOST_CHECK_NO_THROW(assert_distribution_state_shape(load_idl_fixture(opp_outpost_idl_fixture)));
   BOOST_CHECK_THROW(assert_distribution_state_shape(idl::program{}), fc::assert_exception);

   for (bool in_types : {false, true}) {
      BOOST_TEST_CONTEXT("fields in types section: " << in_types) {
         BOOST_CHECK_NO_THROW(assert_distribution_state_shape(named_account_program(
            "DistributionState",
            {{"bump", prim(idl::primitive_type::u8)}, {"liqsol_mint", prim(idl::primitive_type::pubkey)}},
            in_types)));
         // The mint declared as anything but a pubkey, or not declared at all.
         BOOST_CHECK_THROW(assert_distribution_state_shape(named_account_program(
                              "DistributionState", {{"liqsol_mint", prim(idl::primitive_type::u64)}}, in_types)),
                           fc::assert_exception);
         BOOST_CHECK_THROW(assert_distribution_state_shape(named_account_program(
                              "DistributionState", {{"bump", prim(idl::primitive_type::u8)}}, in_types)),
                           fc::assert_exception);
      }
   }
} FC_LOG_AND_RETHROW();
namespace {

/// A synthetic program declaring `GlobalState` with `wire_state` of enum type
/// `WireState` carrying `variants`, and optional `frozen` / `paused` fields.
idl::program global_state_program(std::vector<std::string> variants, bool fields_in_types_section,
                                  std::optional<idl::idl_type> frozen_type = std::nullopt,
                                  std::optional<idl::idl_type> paused_type = prim(idl::primitive_type::bool_t)) {
   std::vector<idl::field> fields;
   if (frozen_type) fields.push_back({"frozen", *frozen_type});
   if (paused_type) fields.push_back({"paused", *paused_type});
   fields.push_back({"wire_state", idl::idl_type::make_defined("WireState")});
   auto prog = named_account_program("GlobalState", std::move(fields), fields_in_types_section);
   idl::type_def wire_state;
   wire_state.name          = "WireState";
   wire_state.enum_variants = std::vector<idl::enum_variant>{};
   for (auto& name : variants) {
      idl::enum_variant variant;
      variant.name = std::move(name);
      wire_state.enum_variants->push_back(std::move(variant));
   }
   prog.types.push_back(std::move(wire_state));
   return prog;
}

/// Current liqsol-core `GlobalState` byte-layout facts pinned by Solana commit
/// c9c42ff: 8 discriminator bytes plus 115 Borsh body bytes.
constexpr size_t current_global_state_account_bytes = 123;
constexpr size_t current_global_state_frozen_offset = 97;
constexpr size_t current_global_state_wire_state_offset = 121;
constexpr uint64_t current_layout_version = 0x8000'0000'0000'0002;
constexpr uint64_t reportable_yield_accumulated = 90;
constexpr uint64_t reportable_yield_watermark = 40;
constexpr uint64_t liq_sequence = 7;
constexpr size_t current_global_state_reserved_bytes = 23;
constexpr size_t zeroed_accounting_fields_before_yield = 6;
constexpr uint8_t current_global_state_bump = 0xFE;
constexpr uint8_t invalid_borsh_bool = 2;
constexpr uint8_t invalid_wire_state_variant = std::numeric_limits<uint8_t>::max();

/// liqsol-core WireState; the stored Borsh u8 is the declaration index.
enum class current_wire_state : uint8_t {
   pre_launch,
   post_launch,
   refund,
   launching,
};

/// Borsh-encode the current 123-byte liqsol-core `GlobalState` in its exact
/// declaration order. Distinct accounting values catch decoding at wrong offsets.
std::vector<uint8_t> current_global_state_account(current_wire_state wire_state, bool frozen,
                                                  bool paused = false) {
   borsh::encoder enc;
   const auto discriminator = idl::compute_account_discriminator("GlobalState");
   enc.write_fixed_bytes(discriminator.data(), discriminator.size());
   enc.write_u64(current_layout_version);
   enc.write_u64(0);       // yield_index_remainder
   enc.write_bool(paused);
   for (size_t i = 0; i < zeroed_accounting_fields_before_yield; ++i) enc.write_u64(0);
   enc.write_u64(reportable_yield_accumulated);
   enc.write_u64(liq_sequence);
   enc.write_u64(reportable_yield_watermark);
   enc.write_bool(frozen);
   const std::array<uint8_t, current_global_state_reserved_bytes> reserved{};
   enc.write_fixed_bytes(reserved.data(), reserved.size());
   enc.write_u8(magic_enum::enum_integer(wire_state));
   enc.write_u8(current_global_state_bump);
   return enc.data();
}

} // namespace

BOOST_AUTO_TEST_CASE(global_state_shape_requires_a_post_launch_variant) try {
   using sysio::outpost_solana_client_detail::assert_global_state_shape;

   // The stub fixture carries the integrated program's `GlobalState`.
   BOOST_CHECK_NO_THROW(assert_global_state_shape(load_idl_fixture(opp_outpost_idl_fixture)));
   BOOST_CHECK_THROW(assert_global_state_shape(idl::program{}), fc::assert_exception);

   for (bool in_types : {false, true}) {
      BOOST_TEST_CONTEXT("fields in types section: " << in_types) {
         BOOST_CHECK_NO_THROW(assert_global_state_shape(
            global_state_program({"PreLaunch", "PostLaunch", "Refund", "Launching"}, in_types,
                                 prim(idl::primitive_type::bool_t))));
         // The variant the crank gates on renamed away, or the field not an enum.
         BOOST_CHECK_THROW(assert_global_state_shape(global_state_program(
                              {"PreLaunch", "Live"}, in_types, prim(idl::primitive_type::bool_t))),
                           fc::assert_exception);
         BOOST_CHECK_THROW(assert_global_state_shape(named_account_program(
                              "GlobalState",
                              {{"frozen", prim(idl::primitive_type::bool_t)},
                               {"paused", prim(idl::primitive_type::bool_t)},
                               {"wire_state", prim(idl::primitive_type::u8)}},
                              in_types)),
                           fc::assert_exception);
         // The reporting layout must declare both bool gates.
         BOOST_CHECK_NO_THROW(assert_global_state_shape(
            global_state_program({"PostLaunch"}, in_types, prim(idl::primitive_type::bool_t))));
         BOOST_CHECK_THROW(assert_global_state_shape(global_state_program({"PostLaunch"}, in_types)),
                           fc::assert_exception);
         BOOST_CHECK_THROW(assert_global_state_shape(
                              global_state_program({"PostLaunch"}, in_types, prim(idl::primitive_type::u8))),
                           fc::assert_exception);
         BOOST_CHECK_THROW(assert_global_state_shape(global_state_program(
                              {"PostLaunch"}, in_types, prim(idl::primitive_type::bool_t), std::nullopt)),
                           fc::assert_exception);
         BOOST_CHECK_THROW(assert_global_state_shape(global_state_program(
                              {"PostLaunch"}, in_types, prim(idl::primitive_type::bool_t),
                              prim(idl::primitive_type::u8))), fc::assert_exception);
      }
   }
} FC_LOG_AND_RETHROW();
// The crank gates on the decoder's rendering of `GlobalState.wire_state` -- an
// enum comes back as `{"variant": <name>}` -- and only PostLaunch is due.
BOOST_AUTO_TEST_CASE(liq_yield_report_is_due_only_post_launch) try {
   using sysio::outpost_solana_client_detail::liq_yield_report_due;
   auto state = [](const char* variant) {
      return fc::mutable_variant_object()
         ("wire_state", fc::mutable_variant_object()("variant", variant))
         ("frozen", false)("paused", false);
   };
   BOOST_CHECK(liq_yield_report_due(state("PostLaunch")));
   BOOST_CHECK(!liq_yield_report_due(state("PreLaunch")));
   BOOST_CHECK(!liq_yield_report_due(state("Launching")));
   BOOST_CHECK(!liq_yield_report_due(fc::mutable_variant_object()("paused", false)("frozen", false)));
   // A bare string is not the decoder's shape: never due rather than guessed.
   BOOST_CHECK(!liq_yield_report_due(fc::mutable_variant_object()
      ("wire_state", "PostLaunch")("frozen", false)("paused", false)));
   BOOST_CHECK(!liq_yield_report_due(fc::mutable_variant_object()
      ("wire_state", fc::mutable_variant_object())("frozen", false)("paused", false)));
   BOOST_CHECK(!liq_yield_report_due(fc::mutable_variant_object()
      ("wire_state", fc::mutable_variant_object()("variant", uint64_t{1}))
      ("frozen", false)("paused", false)));
} FC_LOG_AND_RETHROW();
// The emergency stop: while `GlobalState.frozen` is set the program refuses
// `report_liq_yield`, so the crank does not fire. Missing or misshaped gating
// fields fail closed and are never due.
BOOST_AUTO_TEST_CASE(liq_yield_report_is_not_due_while_the_outpost_is_frozen) try {
   using sysio::outpost_solana_client_detail::liq_outpost_frozen;
   using sysio::outpost_solana_client_detail::liq_yield_report_due;
   auto state = [](const char* variant, std::optional<fc::variant> frozen) {
      fc::mutable_variant_object row;
      row("wire_state", fc::mutable_variant_object()("variant", variant))("paused", false);
      if (frozen) row("frozen", *frozen);
      return fc::variant_object(row);
   };
   BOOST_CHECK(!liq_outpost_frozen(state("PostLaunch", fc::variant(false))));
   BOOST_CHECK(liq_yield_report_due(state("PostLaunch", fc::variant(false))));

   BOOST_CHECK(liq_outpost_frozen(state("PostLaunch", fc::variant(true))));
   BOOST_CHECK(!liq_yield_report_due(state("PostLaunch", fc::variant(true))));
   BOOST_CHECK(!liq_yield_report_due(state("PreLaunch", fc::variant(true))));

   BOOST_CHECK(liq_outpost_frozen(state("PostLaunch", std::nullopt)));
   BOOST_CHECK(!liq_yield_report_due(state("PostLaunch", std::nullopt)));

   BOOST_CHECK(liq_outpost_frozen(state("PostLaunch", fc::variant(uint64_t{0}))));
   BOOST_CHECK(!liq_yield_report_due(state("PostLaunch", fc::variant("false"))));
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(liq_yield_report_is_not_due_while_the_outpost_is_paused) try {
   using sysio::outpost_solana_client_detail::liq_outpost_paused;
   using sysio::outpost_solana_client_detail::liq_yield_report_due;
   auto state = [](std::optional<fc::variant> paused) {
      fc::mutable_variant_object row;
      row("wire_state", fc::mutable_variant_object()("variant", "PostLaunch"))("frozen", false);
      if (paused) row("paused", *paused);
      return fc::variant_object(row);
   };
   BOOST_CHECK(!liq_outpost_paused(state(fc::variant(false))));
   BOOST_CHECK(liq_yield_report_due(state(fc::variant(false))));
   for (const auto& paused : {std::optional<fc::variant>{}, std::optional{fc::variant(true)},
                            std::optional{fc::variant(uint64_t{0})}, std::optional{fc::variant("false")}}) {
      BOOST_CHECK(liq_outpost_paused(state(paused)));
      BOOST_CHECK(!liq_yield_report_due(state(paused)));
   }
} FC_LOG_AND_RETHROW();

// Exercise the automatic crank's fetch/decode/decision/submission path with the
// deployed 123-byte account. A size cutoff inside that path prevents submission
// and fails this regression. No live RPC or manually supplied report is involved.
BOOST_AUTO_TEST_CASE(current_global_state_layout_is_eligible_for_automatic_yield_reporting) try {
   using sysio::outpost_solana_client_detail::drive_liq_yield_report;
   using state = sysio::outpost_solana_client_detail::liq_yield_report_state;
   const auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   sysio::opp_solana_outpost_client program_client(
      solana_client_ptr{}, measurement_pubkey(42), {prog});

   const auto account = current_global_state_account(current_wire_state::post_launch, false);
   BOOST_REQUIRE_EQUAL(account.size(), current_global_state_account_bytes);
   const auto decoded = program_client.decode_account_info_data("GlobalState", account);
   const auto& row = decoded.get_object();
   BOOST_CHECK_EQUAL(row["yield_accumulated_liqsol"].as_uint64(), reportable_yield_accumulated);
   BOOST_CHECK_EQUAL(row["liq_sequence"].as_uint64(), liq_sequence);
   BOOST_CHECK_EQUAL(row["liq_yield_reported"].as_uint64(), reportable_yield_watermark);
   BOOST_CHECK_GT(row["yield_accumulated_liqsol"].as_uint64(), row["liq_yield_reported"].as_uint64());
   std::optional<account_info> fetched{account_info{}};
   fetched->data = account;
   unsigned reads = 0, decodes = 0, submissions = 0;
   const auto run = [&] {
      return drive_liq_yield_report(
         [&] { ++reads; return fetched; },
         [&](const std::vector<uint8_t>& data) {
            ++decodes;
            return program_client.decode_account_info_data("GlobalState", data);
         },
         [&](const std::vector<instruction>& pre_instructions) {
            ++submissions;
            // Inspect the actual production preamble, not a copy used only by tests.
            BOOST_REQUIRE_EQUAL(pre_instructions.size(), 1u);
            const auto& budget = pre_instructions.front();
            BOOST_CHECK(budget.program_id == system::program_ids::COMPUTE_BUDGET_PROGRAM);
            BOOST_CHECK(budget.accounts.empty());
            const auto expected = system::compute_budget::set_compute_unit_limit(800'000);
            BOOST_CHECK(budget.data == expected.data);

            const auto* report = prog.find_instruction("report_liq_yield");
            BOOST_REQUIRE(report);
            const auto payer = measurement_pubkey(1);
            auto metas = production_static_accounts(*report, payer);
            auto instructions = pre_instructions;
            instructions.push_back(instruction{measurement_pubkey(42), metas,
               {report->discriminator.begin(), report->discriminator.end()}});
            const auto packet = build_measured_legacy_transaction(instructions, payer).serialize();
            BOOST_TEST_MESSAGE("report_liq_yield tx with compute budget serialises to " << packet.size() << " bytes");
            BOOST_CHECK_LE(packet.size(), limits::PACKET_DATA_SIZE);
         });
   };
   BOOST_CHECK(run() == state::due);
   BOOST_CHECK_EQUAL(reads, 1u);
   BOOST_CHECK_EQUAL(decodes, 1u);
   BOOST_CHECK_EQUAL(submissions, 1u);
   fetched->data = current_global_state_account(current_wire_state::post_launch, true);
   BOOST_CHECK(run() == state::frozen);
   fetched->data = current_global_state_account(current_wire_state::post_launch, false, true);
   BOOST_CHECK(run() == state::paused);
   for (const auto wire_state : {current_wire_state::pre_launch, current_wire_state::refund,
                                 current_wire_state::launching}) {
      fetched->data = current_global_state_account(wire_state, false);
      BOOST_CHECK(run() == state::not_due);
   }
   BOOST_CHECK_EQUAL(submissions, 1u);
   const auto decodes_before_absent = decodes;
   fetched->data.clear();
   BOOST_CHECK(run() == state::no_account);
   fetched.reset();
   BOOST_CHECK(run() == state::no_account);
   BOOST_CHECK_EQUAL(decodes, decodes_before_absent);
   BOOST_CHECK_EQUAL(submissions, 1u);
} FC_LOG_AND_RETHROW();

// The crank path propagates truncated data, a wrong discriminator and invalid
// bool / enum encodings before its report callback can resolve or submit a tx.
BOOST_AUTO_TEST_CASE(current_global_state_layout_rejects_malformed_accounts) try {
   const auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   sysio::opp_solana_outpost_client program_client(
      solana_client_ptr{}, measurement_pubkey(42), {prog});

   const auto rejects = [&](const std::vector<uint8_t>& data) {
      using sysio::outpost_solana_client_detail::drive_liq_yield_report;
      bool submitted = false;
      BOOST_CHECK_THROW(drive_liq_yield_report(
         [&] { account_info info; info.data = data; return std::optional{info}; },
         [&](const auto& bytes) { return program_client.decode_account_info_data("GlobalState", bytes); },
         [&](const auto&) { submitted = true; }), fc::assert_exception);
      BOOST_CHECK(!submitted);
   };
   auto truncated = current_global_state_account(current_wire_state::post_launch, false);
   truncated.pop_back();
   rejects(truncated);

   auto wrong_discriminator = current_global_state_account(current_wire_state::post_launch, false);
   wrong_discriminator.front() ^= 0x01;
   rejects(wrong_discriminator);

   auto invalid_frozen = current_global_state_account(current_wire_state::post_launch, false);
   invalid_frozen[current_global_state_frozen_offset] = invalid_borsh_bool;
   rejects(invalid_frozen);

   auto invalid_wire_state = current_global_state_account(current_wire_state::post_launch, false);
   invalid_wire_state[current_global_state_wire_state_offset] = invalid_wire_state_variant;
   rejects(invalid_wire_state);
} FC_LOG_AND_RETHROW();
// The overrides cover every non-signer account `report_liq_yield` declares, and
// derive each the way the program's `#[derive(Accounts)]` constraints do: the
// Token-2022 ATAs of the pool and bucket authorities, the `UserRecord`s seeded
// on those ATAs, the hook's extra-metas PDA, and Token-2022 itself as the token
// program (the well-known table would have resolved legacy SPL Token).
BOOST_AUTO_TEST_CASE(report_liq_yield_overrides_resolve_every_declared_account) try {
   namespace accounts = manifest_detail::report_liq_yield_accounts;
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   const idl::instruction* instr = prog.find_instruction("report_liq_yield");
   BOOST_REQUIRE(instr != nullptr);

   const auto program_id = measurement_pubkey(42);
   const auto mint       = measurement_pubkey(90);
   const auto hook       = measurement_pubkey(91);
   const auto config     = measurement_pubkey(95);
   const auto buffer     = measurement_pubkey(96);
   const auto overrides  = manifest_detail::report_liq_yield_overrides(program_id, mint, hook, config, buffer);

   size_t signers = 0;
   for (const auto& acct : instr->accounts) {
      if (acct.is_signer) { ++signers; continue; }   // the cranker is the client's own key
      BOOST_CHECK_MESSAGE(overrides.contains(acct.name), "no override for '" << acct.name << "'");
   }
   BOOST_CHECK_EQUAL(signers, 1u);
   BOOST_CHECK_EQUAL(overrides.size(), instr->accounts.size() - signers);

   const auto& token_2022       = system::program_ids::TOKEN_2022_PROGRAM;
   const auto  pool_authority   = manifest_detail::derive_liqsol_pool_authority_pda(program_id);
   const auto  bucket_authority = manifest_detail::derive_liqsol_bucket_authority_pda(program_id);
   const auto  pool_ata         = system::get_associated_token_address(pool_authority, mint, token_2022);
   const auto  bucket_ata       = system::get_associated_token_address(bucket_authority, mint, token_2022);
   BOOST_CHECK(overrides.at(accounts::liqsol_mint) == mint);
   BOOST_CHECK(overrides.at(accounts::global_state) == manifest_detail::derive_liqsol_global_state_pda(program_id));
   BOOST_CHECK(overrides.at(accounts::distribution_state) ==
               manifest_detail::derive_liqsol_distribution_state_pda(program_id));
   BOOST_CHECK(overrides.at(accounts::pool_authority) == pool_authority);
   BOOST_CHECK(overrides.at(accounts::bucket_authority) == bucket_authority);
   BOOST_CHECK(overrides.at(accounts::liqsol_pool_ata) == pool_ata);
   BOOST_CHECK(overrides.at(accounts::bucket_token_account) == bucket_ata);
   BOOST_CHECK(overrides.at(accounts::pool_user_record) ==
               manifest_detail::derive_liqsol_user_record_pda(program_id, pool_ata));
   BOOST_CHECK(overrides.at(accounts::bucket_user_record) ==
               manifest_detail::derive_liqsol_user_record_pda(program_id, bucket_ata));
   BOOST_CHECK(overrides.at(accounts::extra_account_meta_list) ==
               manifest_detail::derive_extra_account_metas_pda(hook, mint));
   BOOST_CHECK(overrides.at(accounts::liqsol_core_program) == program_id);
   BOOST_CHECK(overrides.at(accounts::transfer_hook_program) == hook);
   BOOST_CHECK(overrides.at(accounts::config) == config);
   BOOST_CHECK(overrides.at(accounts::outbound_message_buffer) == buffer);
   BOOST_CHECK(overrides.at(accounts::token_program) == token_2022);
   BOOST_CHECK(overrides.at(accounts::associated_token_program) == system::program_ids::ASSOCIATED_TOKEN_PROGRAM);
   BOOST_CHECK(overrides.at(accounts::system_program) == system::program_ids::SYSTEM_PROGRAM);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(decode_latest_envelope_reads_both_known_layouts) try {
   constexpr uint32_t epoch = 7;
   const auto payload  = envelope_payload_bytes(epoch);
   const auto checksum = keccak_checksum(payload);

   // Standalone layout (data declared `bytes` → base64-string decode path).
   const auto standalone = decode_with_layout(
      standalone_latest_fields(), standalone_latest_account(epoch, payload, checksum), epoch);
   BOOST_CHECK(bytes_equal(standalone, payload));

   // Integrated layout (data declared `Vec<u8>` → integer-array decode path).
   // Byte-identical result from a DIFFERENT serialized field order is the
   // property the epoch=511 RCA violated.
   const auto integrated = decode_with_layout(
      integrated_latest_fields(), integrated_latest_account(epoch, payload, checksum), epoch);
   BOOST_CHECK(bytes_equal(integrated, payload));
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(decode_latest_envelope_catches_field_order_drift) try {
   // The epoch=511 RCA input: an INTEGRATED account (bump=0xFF first) read
   // by a client whose loaded IDL declares the STANDALONE field order. The
   // decode either trips a Borsh bound (vec length read from misaligned
   // bytes) or yields stored_epoch=511 ≠ requested — both must reject.
   constexpr uint32_t epoch = 1;
   const auto payload  = envelope_payload_bytes(epoch);
   const auto checksum = keccak_checksum(payload);
   const auto integrated_account = integrated_latest_account(epoch, payload, checksum);

   BOOST_CHECK(decode_with_layout(standalone_latest_fields(), integrated_account, epoch).empty());
   // Even a caller asking for exactly the misread value (511 = 0xFF | 1<<8)
   // must not be handed drift-garbage bytes.
   BOOST_CHECK(decode_with_layout(standalone_latest_fields(), integrated_account, 511).empty());
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(decode_latest_envelope_rejects_invalid_accounts) try {
   constexpr uint32_t epoch = 7;
   const auto payload  = envelope_payload_bytes(epoch);
   const auto checksum = keccak_checksum(payload);
   const auto valid    = integrated_latest_account(epoch, payload, checksum);

   // Wrong Anchor discriminator (foreign or re-homed account bytes).
   {
      auto corrupted = valid;
      corrupted[0] ^= 0x01;
      BOOST_CHECK(decode_with_layout(integrated_latest_fields(), corrupted, epoch).empty());
   }
   // Truncated account.
   {
      auto truncated = valid;
      truncated.resize(truncated.size() / 2);
      BOOST_CHECK(decode_with_layout(integrated_latest_fields(), truncated, epoch).empty());
   }
   // Unwritten sentinel (epoch=0) and stale/ahead epochs.
   BOOST_CHECK(decode_with_layout(integrated_latest_fields(),
                                  integrated_latest_account(0, payload, checksum), 0).empty());
   BOOST_CHECK(decode_with_layout(integrated_latest_fields(), valid, epoch + 1).empty());
   BOOST_CHECK(decode_with_layout(integrated_latest_fields(), valid, epoch - 1).empty());
   // Stored checksum disagreeing with keccak256(data) — the drift tripwire.
   {
      auto bad_checksum = checksum;
      bad_checksum[0] ^= 0x01;
      BOOST_CHECK(decode_with_layout(integrated_latest_fields(),
                                     integrated_latest_account(epoch, payload, bad_checksum),
                                     epoch).empty());
   }
   // Payload that is not a protobuf Envelope (checksum matches the garbage,
   // so this exercises the protobuf gate, not the checksum gate).
   {
      const std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF};
      BOOST_CHECK(decode_with_layout(integrated_latest_fields(),
                                     integrated_latest_account(epoch, garbage, keccak_checksum(garbage)),
                                     epoch).empty());
   }
   // Envelope whose INNER epoch disagrees with the stored account epoch.
   {
      const auto inner_mismatch = envelope_payload_bytes(epoch + 1);
      BOOST_CHECK(decode_with_layout(integrated_latest_fields(),
                                     integrated_latest_account(epoch, inner_mismatch,
                                                               keccak_checksum(inner_mismatch)),
                                     epoch).empty());
   }
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(borsh_payload_bytes_accepts_both_decoded_shapes) try {
   const std::vector<uint8_t> payload = {0x12, 0x00, 0xFF, 0x42};

   // `bytes` decodes to a base64 string variant.
   const auto b64 = fc::base64_encode(reinterpret_cast<const char*>(payload.data()),
                                      static_cast<unsigned int>(payload.size()));
   BOOST_CHECK(bytes_equal(borsh_payload_bytes(fc::variant(b64)), payload));

   // `Vec<u8>` decodes to an integer array variant.
   fc::variants arr;
   for (auto b : payload) arr.push_back(fc::variant(static_cast<uint64_t>(b)));
   BOOST_CHECK(bytes_equal(borsh_payload_bytes(fc::variant(arr)), payload));

   // Out-of-byte-range element and non-payload shapes must throw.
   fc::variants oversized = {fc::variant(static_cast<uint64_t>(256))};
   BOOST_CHECK_THROW(borsh_payload_bytes(fc::variant(oversized)), fc::assert_exception);
   BOOST_CHECK_THROW(borsh_payload_bytes(fc::variant(static_cast<uint64_t>(7))), fc::assert_exception);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(libfc_decode_renders_bytes_base64_and_vec_u8_as_array) try {
   // `borsh_payload_bytes` depends on HOW libfc's IDL decoder renders the
   // payload field into a variant: `bytes` as a base64 STRING, `Vec<u8>` as
   // an integer ARRAY. That rendering is an internal libfc convention, not a
   // documented contract - this test pins it against `decode_account_info_data`
   // itself so a future libfc rendering change fails HERE at test time
   // instead of silently breaking the envelope reader at runtime.
   constexpr uint32_t epoch = 7;
   const auto payload  = envelope_payload_bytes(epoch);
   const auto checksum = keccak_checksum(payload);

   // `bytes` spelling (standalone layout) -> base64 string variant.
   {
      const auto prog = outpost_program_with_latest(standalone_latest_fields());
      sysio::opp_solana_outpost_client program_client(
         solana_client_ptr{}, measurement_pubkey(42), {prog});
      const auto decoded = program_client.decode_account_info_data(
         "LatestOutboundEnvelope", standalone_latest_account(epoch, payload, checksum));
      const auto& obj = decoded.get_object();
      BOOST_CHECK_EQUAL(obj["epoch_index"].as_uint64(), epoch);
      BOOST_REQUIRE(obj["data"].is_string());
      BOOST_CHECK_EQUAL(obj["data"].as_string(),
                        fc::base64_encode(reinterpret_cast<const char*>(payload.data()),
                                          static_cast<unsigned int>(payload.size())));
   }
   // `Vec<u8>` spelling (integrated layout) -> per-element integer array.
   {
      const auto prog = outpost_program_with_latest(integrated_latest_fields());
      sysio::opp_solana_outpost_client program_client(
         solana_client_ptr{}, measurement_pubkey(42), {prog});
      const auto decoded = program_client.decode_account_info_data(
         "LatestOutboundEnvelope", integrated_latest_account(epoch, payload, checksum));
      const auto& obj = decoded.get_object();
      BOOST_CHECK_EQUAL(obj["epoch_index"].as_uint64(), epoch);
      BOOST_REQUIRE(obj["data"].is_array());
      const auto& arr = obj["data"].get_array();
      BOOST_REQUIRE_EQUAL(arr.size(), payload.size());
      for (size_t i = 0; i < arr.size(); ++i) {
         BOOST_CHECK_EQUAL(arr[i].as_uint64(), static_cast<uint64_t>(payload[i]));
      }
   }
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(select_program_idls_prefers_declared_address_match) try {
   const auto deployed = measurement_pubkey(90);
   const auto other    = measurement_pubkey(91);
   const auto deployed_b58 = deployed.to_string(fc::yield_function_t{});
   const auto other_b58    = other.to_string(fc::yield_function_t{});

   // Two same-named IDL versions loaded; the SECOND matches the deployed
   // program id. File order must not decide — only the match survives.
   {
      const auto selected =
         select_program_idls_matching({named_program(other_b58), named_program(deployed_b58)}, deployed);
      BOOST_REQUIRE_EQUAL(selected.size(), 1u);
      BOOST_CHECK_EQUAL(selected.front().address, deployed_b58);
   }
   // An unparseable declared address is skipped, not fatal, when another
   // candidate matches.
   {
      const auto selected =
         select_program_idls_matching({named_program("not-base58!"), named_program(deployed_b58)}, deployed);
      BOOST_REQUIRE_EQUAL(selected.size(), 1u);
      BOOST_CHECK_EQUAL(selected.front().address, deployed_b58);
   }
   // A single candidate stays usable without a declared address (stub/dev
   // IDLs) and — with a warning — with a mismatched one.
   BOOST_CHECK_EQUAL(select_program_idls_matching({named_program("")}, deployed).size(), 1u);
   BOOST_CHECK_EQUAL(select_program_idls_matching({named_program(other_b58)}, deployed).size(), 1u);
   // Multiple candidates with no address match would make the pick
   // order-dependent — exactly the silent-misread risk — so it throws.
   BOOST_CHECK_THROW(
      select_program_idls_matching({named_program(other_b58), named_program("")}, deployed),
      fc::assert_exception);
   BOOST_CHECK_THROW(
      select_program_idls_matching({named_program(""), named_program("")}, deployed),
      fc::assert_exception);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_CASE(envelope_log_shape_is_checked_for_the_underwriter) try {
   using sysio::outpost_solana_client_detail::assert_envelope_log_shape;
   auto prog = outpost_program_with_envelope_log();
   BOOST_CHECK_NO_THROW(assert_envelope_log_shape(prog));

   // A checksum that is not [u8; 32] cannot carry a keccak digest.
   auto drifted = prog;
   for (auto& type : drifted.types) {
      if (type.name != "EnvelopeRecord") continue;
      for (auto& field : *type.struct_fields) {
         if (field.name == "checksum") field.type = idl::idl_type::make_array(prim(idl::primitive_type::u8), 20);
      }
   }
   BOOST_CHECK_THROW(assert_envelope_log_shape(drifted), fc::exception);

   // No EnvelopeLog at all.
   auto missing = prog;
   std::erase_if(missing.accounts, [](const idl::account& a) { return a.name == "EnvelopeLog"; });
   std::erase_if(missing.types, [](const idl::type_def& t) { return t.name == "EnvelopeLog"; });
   BOOST_CHECK_THROW(assert_envelope_log_shape(missing), fc::exception);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(decode_envelope_log_digest_finds_the_epochs_record) try {
   using sysio::outpost_solana_client_detail::decode_envelope_log_digest;
   const auto prog = outpost_program_with_envelope_log();
   sysio::opp_solana_outpost_client program_client(solana_client_ptr{}, measurement_pubkey(42), {prog});

   const auto c7      = keccak_checksum(envelope_payload_bytes(7));
   const auto c8      = keccak_checksum(envelope_payload_bytes(8));
   const auto account = envelope_log_account({{7, c7}, {8, c8}});
   const auto d8      = decode_envelope_log_digest(program_client, account, 8);
   BOOST_REQUIRE(d8.has_value());
   BOOST_CHECK(std::memcmp(d8->data(), c8.data(), c8.size()) == 0);

   // No record for the epoch: not emitted yet, or past the program's retention.
   BOOST_CHECK(!decode_envelope_log_digest(program_client, account, 9).has_value());
   BOOST_CHECK(!decode_envelope_log_digest(program_client, envelope_log_account({}), 7).has_value());

   // Another account's bytes are a failed read, not an answer.
   BOOST_CHECK_THROW(decode_envelope_log_digest(program_client,
                                                standalone_latest_account(7, envelope_payload_bytes(7), c7), 7),
                     fc::exception);
} FC_LOG_AND_RETHROW();

// ── EnvelopeChunks upload resume. A partial upload used to restart at chunk 0
//    every tick; the program no-ops already-staged chunks, but each one still
//    costs a send + confirm, so an envelope larger than one delivery deadline's
//    worth of chunks never completed. These pin the header read the resume
//    depends on against the program's bytemuck layout.

namespace {

constexpr std::string_view envelope_chunks_name = "EnvelopeChunks";
/// IDL field names whose byte positions discovery assumes.
constexpr auto envelope_chunks_epoch_field = "epoch_index";
constexpr auto envelope_chunks_operator_field = "operator";
/// Arbitrary non-zero `bump`, so a decode that slips one byte cannot read it as a zero field.
constexpr uint8_t envelope_chunks_bump = 0xFE;
/// Fill byte of the staged payload tail: non-zero, so a decode that reads into the tail is visible.
constexpr uint8_t staged_payload_fill = 0xAB;
/// `_padding: [u8; 3]` after `bump`, keeping the `repr(C)` header's `operator` 4-byte aligned.
constexpr size_t envelope_chunks_padding_bytes = 3;
/// Program id the resume tests' outpost program client is built for, and so the owner a staged buffer must carry.
const solana_public_key resume_program_id = measurement_pubkey(42);

/// The stub outpost program, unmodified. Its `EnvelopeChunks` declaration is copied verbatim from `liqsol_core`'s
/// generated IDL and kept there by `solana_idl_stub_consistency_test`, so these tests exercise the IDL the relay
/// actually loads rather than a hand-built copy of it.
sysio::opp_solana_outpost_client resume_program_client(idl::program prog = load_idl_fixture(opp_outpost_idl_fixture)) {
   return sysio::opp_solana_outpost_client(solana_client_ptr{}, resume_program_id, {std::move(prog)});
}

/// An `EnvelopeChunks` account as the program lays it out: discriminator, the
/// little-endian `repr(C)` header, then `staged_bytes` of raw payload tail.
/// Parameters follow `decode_first_unstaged_chunk`'s (epoch, total_chunks,
/// total_bytes) order, so the two read the same at a call site.
std::vector<uint8_t> envelope_chunks_data(uint32_t epoch, uint16_t total_chunks, uint32_t total_bytes,
                                          uint16_t received, const solana_public_key& uploader, size_t staged_bytes) {
   borsh::encoder enc;
   const auto disc = idl::compute_account_discriminator(std::string{envelope_chunks_name});
   enc.write_fixed_bytes(disc.data(), disc.size());
   enc.write_u32(epoch);
   enc.write_u32(total_bytes);
   enc.write_u16(total_chunks);
   enc.write_u16(received);
   enc.write_u8(envelope_chunks_bump);
   const std::array<uint8_t, envelope_chunks_padding_bytes> padding{};
   enc.write_fixed_bytes(padding.data(), padding.size());
   enc.write_pubkey(uploader);
   auto bytes = enc.data();
   bytes.resize(bytes.size() + staged_bytes, staged_payload_fill);
   return bytes;
}

/// `data` as the RPC returns an account the outpost program owns.
std::optional<account_info> program_owned(std::vector<uint8_t> data) {
   account_info info;
   info.owner = resume_program_id;
   info.data  = std::move(data);
   return info;
}

/// A program-owned `EnvelopeChunks` account; see `envelope_chunks_data`.
std::optional<account_info> envelope_chunks_account(uint32_t epoch, uint16_t total_chunks, uint32_t total_bytes,
                                                    uint16_t received, const solana_public_key& uploader,
                                                    size_t staged_bytes) {
   return program_owned(envelope_chunks_data(epoch, total_chunks, total_bytes, received, uploader, staged_bytes));
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(first_unstaged_chunk_resumes_the_same_upload) try {
   using sysio::outpost_solana_client_detail::decode_first_unstaged_chunk;
   auto           program_client = resume_program_client();
   const auto     me             = measurement_pubkey(7);
   constexpr auto total_chunks   = uint16_t{50};
   constexpr auto total_bytes    = uint32_t{32'768};

   // Partially staged: resume at the first chunk the buffer has not received.
   // The staged payload tail past the header must not disturb the read.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(
                        program_client,
                        envelope_chunks_account(9, total_chunks, total_bytes, 20, me,
                                                20 * sysio::SOLANA_MAX_CHUNK_BYTES),
                        9, total_chunks, total_bytes, me),
                     20u);
   // Fully staged: every data chunk is skipped and only the terminal call remains.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, total_chunks, total_bytes, total_chunks,
                                                                         me, total_bytes),
                                                 9, total_chunks, total_bytes, me),
                     total_chunks);
   // A received count past the envelope's chunk count is clamped, never skipping the terminal call's range. The
   // account holds two chunks more than the envelope, so the length cap alone would allow 52 and only the
   // `total_chunks` clamp can produce 50.
   const auto oversized_bytes = total_bytes + 2 * sysio::SOLANA_MAX_CHUNK_BYTES;
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, total_chunks, total_bytes, 52, me,
                                                                         oversized_bytes),
                                                 9, total_chunks, total_bytes, me),
                     total_chunks);
   // A header claiming more chunks than the account's length can hold (a misread from a drifted IDL) is capped by
   // the length, which the program sets to exactly the header plus the staged bytes. Resuming past it would start
   // every tick at a chunk the program rejects as out of order.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, total_chunks, total_bytes, 20, me,
                                                                         5 * sysio::SOLANA_MAX_CHUNK_BYTES),
                                                 9, total_chunks, total_bytes, me),
                     5u);
   // Freshly created for chunk 0 (one chunk's room, nothing received): nothing to skip.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, total_chunks, total_bytes, 0, me,
                                                                         sysio::SOLANA_MAX_CHUNK_BYTES),
                                                 9, total_chunks, total_bytes, me),
                     0u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(first_unstaged_chunk_resumes_before_a_short_last_chunk) try {
   using sysio::outpost_solana_client_detail::decode_first_unstaged_chunk;
   auto       program_client = resume_program_client();
   const auto me             = measurement_pubkey(7);
   // Three chunks, the last one short: 2 full chunks plus a 100-byte tail.
   constexpr auto last_chunk_bytes = size_t{100};
   constexpr auto total_chunks     = uint16_t{3};
   const auto     total_bytes      = static_cast<uint32_t>(2 * sysio::SOLANA_MAX_CHUNK_BYTES + last_chunk_bytes);

   // Every chunk but the short last one staged: resume at the last chunk.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, total_chunks, total_bytes, 2, me,
                                                                         2 * sysio::SOLANA_MAX_CHUNK_BYTES),
                                                 9, total_chunks, total_bytes, me),
                     2u);
   // The short last chunk staged too: the length rounds up to all three chunks, leaving only the terminal call.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, total_chunks, total_bytes, total_chunks,
                                                                         me, total_bytes),
                                                 9, total_chunks, total_bytes, me),
                     total_chunks);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(first_unstaged_chunk_starts_over_for_another_upload) try {
   using sysio::outpost_solana_client_detail::decode_first_unstaged_chunk;
   auto       program_client = resume_program_client();
   const auto me             = measurement_pubkey(7);
   const auto staged         = 2 * sysio::SOLANA_MAX_CHUNK_BYTES; // room for the 2 chunks every header claims

   // Any header field that differs from this upload means the buffer is not
   // ours to resume: start at 0. A different epoch or operator at this PDA can
   // only be IDL drift (the seeds bind both) and is logged; a different
   // total_bytes / total_chunks is a real shape change the program reports.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, envelope_chunks_account(8, 3, 2000, 2, me, staged), 9,
                                                 3, 2000, me),
                     0u);
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, envelope_chunks_account(9, 3, 1999, 2, me, staged), 9,
                                                 3, 2000, me),
                     0u);
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, envelope_chunks_account(9, 4, 2000, 2, me, staged), 9,
                                                 3, 2000, me),
                     0u);
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client,
                                                 envelope_chunks_account(9, 3, 2000, 2, measurement_pubkey(8), staged),
                                                 9, 3, 2000, me),
                     0u);
   // Sanity: the same fixture with every field matching does resume.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, envelope_chunks_account(9, 3, 2000, 2, me, staged), 9,
                                                 3, 2000, me),
                     2u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(first_unstaged_chunk_is_zero_when_nothing_is_staged) try {
   using sysio::outpost_solana_client_detail::decode_first_unstaged_chunk;
   auto       program_client = resume_program_client();
   const auto me             = measurement_pubkey(7);
   const auto staged         = 2 * sysio::SOLANA_MAX_CHUNK_BYTES;

   // No account at the buffer's address yet.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, std::nullopt, 9, 3, 2000, me), 0u);
   // An account with no data.
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, program_owned({}), 9, 3, 2000, me), 0u);
   // A header that would resume, at an address the outpost program does not own (system-owned before its first
   // chunk created it): never decoded, never trusted.
   auto foreign = envelope_chunks_account(9, 3, 2000, 2, me, staged);
   foreign->owner = measurement_pubkey(43);
   BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, foreign, 9, 3, 2000, me), 0u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(first_unstaged_chunk_without_the_idl_declaration_or_with_foreign_bytes) try {
   using sysio::outpost_solana_client_detail::decode_first_unstaged_chunk;
   const auto me      = measurement_pubkey(7);
   const auto account = envelope_chunks_account(9, 3, 2000, 2, me, 0);

   // An IDL that does not declare EnvelopeChunks keeps the pre-resume behaviour.
   {
      auto prog = load_idl_fixture(opp_outpost_idl_fixture);
      std::erase_if(prog.accounts, [](const idl::account& a) { return a.name == envelope_chunks_name; });
      auto program_client = resume_program_client(std::move(prog));
      BOOST_CHECK_EQUAL(decode_first_unstaged_chunk(program_client, account, 9, 3, 2000, me), 0u);
   }
   auto program_client = resume_program_client();
   // Another account's bytes at the buffer's address are a failed read, not an answer.
   BOOST_CHECK_THROW(
      decode_first_unstaged_chunk(program_client, program_owned(envelope_log_account({})), 9, 3, 2000, me),
      fc::exception);
   // A header cut short of `DATA_OFFSET` cannot be decoded either.
   auto truncated = envelope_chunks_data(9, 3, 2000, 2, me, 0);
   truncated.resize(truncated.size() - 1);
   BOOST_CHECK_THROW(decode_first_unstaged_chunk(program_client, program_owned(std::move(truncated)), 9, 3, 2000, me),
                     fc::exception);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(first_unstaged_chunk_names_a_field_the_idl_dropped) try {
   using sysio::outpost_solana_client_detail::decode_first_unstaged_chunk;
   const auto me = measurement_pubkey(7);
   // An IDL that renamed `operator` decodes the same bytes, but without the field the resume compares. That must be
   // a failure naming the field -- the caller turns it into a resume from 0 and logs this message, the only trace
   // that the IDL disabled the resume.
   auto prog = load_idl_fixture(opp_outpost_idl_fixture);
   auto type = std::ranges::find_if(prog.types, [](const idl::type_def& t) { return t.name == envelope_chunks_name; });
   BOOST_REQUIRE(type != prog.types.end());
   BOOST_REQUIRE(type->struct_fields.has_value());
   auto field = std::ranges::find_if(*type->struct_fields, [](const idl::field& f) { return f.name == "operator"; });
   BOOST_REQUIRE(field != type->struct_fields->end());
   field->name = "uploader";
   auto program_client = resume_program_client(std::move(prog));

   BOOST_CHECK_EXCEPTION(
      decode_first_unstaged_chunk(program_client,
                                  envelope_chunks_account(9, 3, 2000, 2, me, 2 * sysio::SOLANA_MAX_CHUNK_BYTES), 9, 3,
                                  2000, me),
      fc::exception,
      [](const fc::exception& e) { return e.to_detail_string().find("'operator'") != std::string::npos; });
} FC_LOG_AND_RETHROW();

// ── Chunk-buffer reaper: `cleanup_envelope_chunks` returns the rent of every
//    buffer this relay staged once its epoch has settled. Driven through its
//    seams (the gate read, the discovery read, the close send) so the whole
//    sweep runs without a Solana endpoint.

namespace {

namespace reap_detail = sysio::outpost_solana_client_detail;

/// Stable buffer identity and settlement cursor for reconciliation scenarios.
namespace reap_scenario {
   constexpr uint32_t uploader_seed = 7;
   constexpr uint32_t buffer_epoch = 2;
   constexpr uint32_t next_epoch = 9;
}

/// Rent a scripted buffer carries; distinct per epoch so the sum pins which buffers closed.
uint64_t buffer_rent(uint32_t epoch) { return 1'000'000 + epoch; }

/// The program's `EnvelopeChunks` PDA, derived here from the program's seed declaration
/// (`["envelope_chunks", epoch_index.to_le_bytes(), uploader]`) rather than through the relay's own helper.
solana_public_key program_chunk_buffer_pda(const solana_public_key& program_id, uint32_t epoch,
                                           const solana_public_key& uploader) {
   const std::string_view seed  = "envelope_chunks";
   const auto             owner = uploader.serialize();
   return system::find_program_address({std::vector<uint8_t>(seed.begin(), seed.end()),
                                        {static_cast<uint8_t>(epoch), static_cast<uint8_t>(epoch >> 8),
                                         static_cast<uint8_t>(epoch >> 16), static_cast<uint8_t>(epoch >> 24)},
                                        std::vector<uint8_t>(owner.begin(), owner.end())},
                                       program_id)
      .first;
}

reap_detail::envelope_chunk_buffer scripted_buffer(uint32_t epoch, const solana_public_key& uploader) {
   return {program_chunk_buffer_pda(resume_program_id, epoch, uploader), epoch, buffer_rent(epoch)};
}

/// One `getProgramAccounts` entry as the RPC returns it under the reaper's header-only `dataSlice`.
fc::variant program_accounts_entry(const solana_public_key& address, const std::vector<uint8_t>& sliced_data,
                                   uint64_t lamports) {
   const std::string b64 = fc::base64_encode(sliced_data.data(), sliced_data.size());
   return fc::mutable_variant_object()("pubkey", address.to_string(fc::yield_function_t{}))(
      "account", fc::mutable_variant_object()("lamports", lamports)("owner",
                                                                     resume_program_id.to_string(fc::yield_function_t{}))(
                    "data", fc::variants{fc::variant(b64), fc::variant("base64")})("executable", false));
}

/// An `EnvelopeChunks` buffer's discriminator + header: exactly what the header slice returns.
std::vector<uint8_t> sliced_header(uint32_t epoch, const solana_public_key& operator_key) {
   return envelope_chunks_data(epoch, 3, 2000, 3, operator_key, 0);
}

/// Scripted close sender: records each epoch it was asked to close, and throws for the epochs listed in `failing`.
struct reap_harness {
   std::vector<uint32_t> sent;
   std::set<uint32_t>    failing;

   std::function<std::string(uint32_t)> send() {
      return [this](uint32_t epoch) -> std::string {
         sent.push_back(epoch);
         if (failing.contains(epoch)) FC_THROW("custom program error: 0x1776 for epoch {}", epoch);
         return "sig-" + std::to_string(epoch);
      };
   }
};

/// The walk's deadline predicate for a sweep with time to spare.
bool deadline_never_passes() { return false; }

} // anonymous namespace

BOOST_AUTO_TEST_CASE(envelope_chunks_reapable_mirrors_the_program_gates) try {
   using reap_detail::envelope_chunks_reapable;
   // Settled: the draining round moved next_epoch_index past it and emitted outbound for it.
   BOOST_CHECK(envelope_chunks_reapable({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 7));
   BOOST_CHECK(envelope_chunks_reapable({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 3));
   // The live epoch: consensus has not drained yet.
   BOOST_CHECK(!envelope_chunks_reapable({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 8));
   // A buffer staged ahead of the cursor (a relay racing the next epoch).
   BOOST_CHECK(!envelope_chunks_reapable({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 9));
   // next_epoch_index alone is not enough: the program also requires the outbound emit to have reached the epoch.
   BOOST_CHECK(!envelope_chunks_reapable({.next_epoch_index = 8, .latest_outbound_epoch = 6}, 7));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_reap_closes_every_settled_buffer_oldest_first) try {
   const auto     me    = measurement_pubkey(7);
   reap_harness   harness;
   // Discovery supplies oldest-first order. Epoch 12 is the live epoch, still settling.
   const std::vector buffers = {scripted_buffer(4, me), scripted_buffer(9, me), scripted_buffer(11, me),
                                scripted_buffer(12, me)};

   const auto result = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = 12, .latest_outbound_epoch = 11}, buffers, deadline_never_passes, harness.send(),
      "test-relay", {});

   BOOST_CHECK(harness.sent == (std::vector<uint32_t>{4, 9, 11}));
   BOOST_CHECK_EQUAL(result.reaped, 3u);
   BOOST_CHECK_EQUAL(result.not_yet_eligible, 1u);
   BOOST_CHECK_EQUAL(result.failed, 0u);
   BOOST_CHECK_EQUAL(result.deferred, 0u);
   BOOST_CHECK_EQUAL(result.lamports_reaped, buffer_rent(4) + buffer_rent(9) + buffer_rent(11));
   // The live epoch's buffer is not a reason to sweep again: it becomes reapable only after the cursor moves.
   BOOST_CHECK(result.complete());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_reap_continues_past_a_failed_close) try {
   const auto   me = measurement_pubkey(7);
   reap_harness harness;
   harness.failing = {5};

   const auto result = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = 9, .latest_outbound_epoch = 8},
      {scripted_buffer(5, me), scripted_buffer(6, me), scripted_buffer(8, me)}, deadline_never_passes, harness.send(),
      "test-relay", {});

   // One stuck buffer must not strand the rest.
   BOOST_CHECK(harness.sent == (std::vector<uint32_t>{5, 6, 8}));
   BOOST_CHECK_EQUAL(result.reaped, 2u);
   BOOST_CHECK_EQUAL(result.failed, 1u);
   BOOST_CHECK_EQUAL(result.lamports_reaped, buffer_rent(6) + buffer_rent(8));
   // ...and the failure keeps the sweep incomplete, so the caller retries it.
   BOOST_CHECK(!result.complete());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_reap_bounds_one_sweep_and_defers_the_newest) try {
   const auto me      = measurement_pubkey(7);
   const auto backlog = sysio::outpost_solana_client_detail::MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP + 3;
   std::vector<reap_detail::envelope_chunk_buffer> buffers;
   for (uint32_t epoch = 1; epoch <= backlog; ++epoch) buffers.push_back(scripted_buffer(epoch, me));
   reap_harness harness;

   const auto result = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = backlog + 1, .latest_outbound_epoch = backlog}, buffers, deadline_never_passes, harness.send(),
      "test-relay", {});

   BOOST_CHECK_EQUAL(result.reaped, sysio::outpost_solana_client_detail::MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP);
   BOOST_CHECK_EQUAL(result.deferred, 3u);
   BOOST_CHECK(!result.complete());
   // The oldest epochs went first; the newest are the ones left for the next sweep.
   BOOST_REQUIRE(!harness.sent.empty());
   BOOST_CHECK_EQUAL(harness.sent.front(), 1u);
   BOOST_CHECK_EQUAL(harness.sent.back(), sysio::outpost_solana_client_detail::MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_reap_stops_at_the_deadline) try {
   const auto   me = measurement_pubkey(7);
   reap_harness harness;
   // The deadline passes once the first close has gone out: the walk stops there and defers the rest -- it neither
   // throws away the count of what it closed nor counts the remaining buffers as failed closes.
   const auto deadline_passed = [&] { return !harness.sent.empty(); };

   const auto result = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = 9, .latest_outbound_epoch = 8},
      {scripted_buffer(2, me), scripted_buffer(3, me), scripted_buffer(4, me), scripted_buffer(9, me)},
      deadline_passed, harness.send(), "test-relay", {});

   BOOST_CHECK(harness.sent == (std::vector<uint32_t>{2}));
   BOOST_CHECK_EQUAL(result.reaped, 1u);
   BOOST_CHECK_EQUAL(result.lamports_reaped, buffer_rent(2));
   BOOST_CHECK_EQUAL(result.deferred, 2u);
   BOOST_CHECK_EQUAL(result.failed, 0u);
   BOOST_CHECK_EQUAL(result.not_yet_eligible, 1u);
   BOOST_CHECK(result.deadline_reached);
   BOOST_CHECK(!result.complete());

   // Recorded, a deadline stop is not a failure: no back-off, and the same cursor stays due so the next call resumes.
   reap_detail::envelope_chunk_sweep_schedule schedule;
   const auto                                 now = fc::time_point::now();
   schedule.record(9, result, now);
   BOOST_CHECK(!schedule.backing_off(now));
   BOOST_CHECK(schedule.due(9, now));
} FC_LOG_AND_RETHROW();

/// A deadline-cut close defers without issuing an absence RPC after the budget has expired.
BOOST_AUTO_TEST_CASE(chunk_reap_defers_a_close_the_deadline_cut_short) try {
   const auto   me = measurement_pubkey(7);
   reap_harness harness;
   bool         deadline_passed = false;
   unsigned     absence_reads = 0;
   // The close for epoch 3 runs into the deadline: its confirmation polling throws because the scoped deadline ran
   // out, not because the program refused it. It may have landed, so it is deferred, never counted as a failed close.
   const auto send = [&](uint32_t epoch) -> std::string {
      if (epoch == 3) {
         deadline_passed = true;
         FC_THROW_EXCEPTION(fc::timeout_exception, "solana:send_transaction_and_confirm: deadline exceeded");
      }
      return harness.send()(epoch);
   };

   const auto result = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = 9, .latest_outbound_epoch = 8},
      {scripted_buffer(2, me), scripted_buffer(3, me), scripted_buffer(4, me)}, [&] { return deadline_passed; }, send,
      "test-relay", [&](const reap_detail::envelope_chunk_buffer&) {
         ++absence_reads;
         return true;
      });

   BOOST_CHECK_EQUAL(absence_reads, 0u);
   BOOST_CHECK_EQUAL(result.already_closed, 0u);
   BOOST_CHECK(harness.sent == (std::vector<uint32_t>{2}));
   BOOST_CHECK_EQUAL(result.reaped, 1u);
   BOOST_CHECK_EQUAL(result.failed, 0u);
   BOOST_CHECK_EQUAL(result.deferred, 2u);
   BOOST_CHECK(result.deadline_reached);

   // The same timeout BEFORE the deadline (an RPC's own read timeout, a dead endpoint) is a failed close.
   reap_harness before_deadline;
   const auto   own_timeout = [&](uint32_t epoch) -> std::string {
      before_deadline.sent.push_back(epoch);
      FC_THROW_EXCEPTION(fc::timeout_exception, "Outbound HTTP read timeout for epoch {}", epoch);
   };
   const auto failed = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = 9, .latest_outbound_epoch = 8}, {scripted_buffer(2, me), scripted_buffer(3, me)},
      deadline_never_passes, own_timeout, "test-relay", {});
   BOOST_CHECK(before_deadline.sent == (std::vector<uint32_t>{2, 3}));
   BOOST_CHECK_EQUAL(failed.failed, 2u);
   BOOST_CHECK_EQUAL(failed.deferred, 0u);
   BOOST_CHECK(!failed.deadline_reached);
} FC_LOG_AND_RETHROW();

/// Reconcile only a null exact-buffer read; preserve real failures and deadline semantics on read errors.
BOOST_AUTO_TEST_CASE(chunk_reap_recognizes_already_closed_without_claiming_rent) try {
   const auto me = measurement_pubkey(7);
   const auto buffer = scripted_buffer(2, me);
   /// Distinguish authoritative absence from presence, transport failure, and deadline expiry.
   enum class read_result { absent, present, rpc_error, deadline };
   for (const auto mode : {read_result::absent, read_result::present, read_result::rpc_error, read_result::deadline}) {
      bool expired = false;
      unsigned reads = 0;
      const auto result = reap_detail::drive_envelope_chunk_reap(
         {.next_epoch_index = 9, .latest_outbound_epoch = 8}, {buffer}, [&] { return expired; },
         [](uint32_t) -> std::string { FC_THROW("Transaction failed: custom program error"); }, "test-relay",
         [&](const reap_detail::envelope_chunk_buffer& queried) {
            ++reads;
            BOOST_CHECK(queried.address == buffer.address);
            if (mode == read_result::rpc_error) FC_THROW("account read failed");
            if (mode == read_result::deadline) {
               expired = true;
               throw std::runtime_error("account read deadline");
            }
            return mode == read_result::absent;
         });
      BOOST_CHECK_EQUAL(reads, 1u);
      BOOST_CHECK_EQUAL(result.already_closed, mode == read_result::absent ? 1u : 0u);
      BOOST_CHECK_EQUAL(result.reaped, 0u);
      BOOST_CHECK_EQUAL(result.lamports_reaped, 0u);
      BOOST_CHECK_EQUAL(result.failed, mode == read_result::present || mode == read_result::rpc_error ? 1u : 0u);
      BOOST_CHECK_EQUAL(result.deferred, mode == read_result::deadline ? 1u : 0u);
      reap_detail::envelope_chunk_sweep_schedule schedule;
      const auto now = fc::time_point::now();
      schedule.record(9, result, now);
      BOOST_CHECK_EQUAL(schedule.backing_off(now), mode == read_result::present || mode == read_result::rpc_error);
      if (mode == read_result::absent) BOOST_CHECK(!schedule.due(9, now));
   }
} FC_LOG_AND_RETHROW();

/// Already-closed buffers spend the same per-sweep slot as a successful or failed close.
BOOST_AUTO_TEST_CASE(chunk_reap_already_closed_consumes_the_sweep_bound) try {
   constexpr auto bound = reap_detail::MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP;
   constexpr uint32_t extra_buffers = 2;
   const auto me = measurement_pubkey(reap_scenario::uploader_seed);
   std::vector<reap_detail::envelope_chunk_buffer> buffers;
   reap_harness harness;
   for (uint32_t epoch = 1; epoch <= bound + extra_buffers; ++epoch) {
      buffers.push_back(scripted_buffer(epoch, me));
      harness.failing.insert(epoch);
   }
   unsigned absence_reads = 0;
   const auto result = reap_detail::drive_envelope_chunk_reap(
      {.next_epoch_index = bound + extra_buffers + 1, .latest_outbound_epoch = bound + extra_buffers},
      buffers, deadline_never_passes, harness.send(), "test-relay",
      [&](const reap_detail::envelope_chunk_buffer& buffer) {
         BOOST_CHECK(buffer.address == buffers.at(absence_reads).address);
         ++absence_reads;
         return true;
      });
   BOOST_CHECK_EQUAL(harness.sent.size(), bound);
   BOOST_CHECK_EQUAL(absence_reads, bound);
   BOOST_CHECK_EQUAL(result.already_closed, bound);
   BOOST_CHECK_EQUAL(result.reaped, 0u);
   BOOST_CHECK_EQUAL(result.lamports_reaped, 0u);
   BOOST_CHECK_EQUAL(result.failed, 0u);
   BOOST_CHECK_EQUAL(result.deferred, extra_buffers);
   BOOST_CHECK(!result.deadline_reached);
} FC_LOG_AND_RETHROW();

/// Recognized prior closes reset the no-progress streak, including after an expired back-off.
BOOST_AUTO_TEST_CASE(chunk_sweep_already_closed_resets_deadline_stops) try {
   reap_detail::envelope_chunk_sweep_schedule schedule;
   auto now = fc::time_point::now();
   const reap_detail::envelope_chunk_reap_result progress{
      .already_closed = 1, .deferred = 1, .deadline_reached = true};
   for (uint32_t i = 1; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i)
      BOOST_CHECK(!schedule.record_deadline_stop(now));
   BOOST_CHECK(!schedule.record(reap_scenario::next_epoch, progress, now));
   BOOST_CHECK(!schedule.backing_off(now));
   BOOST_CHECK(schedule.due(reap_scenario::next_epoch, now));
   for (uint32_t i = 1; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i)
      BOOST_CHECK(!schedule.record_deadline_stop(now));
   BOOST_CHECK(schedule.record_deadline_stop(now));
   BOOST_CHECK(schedule.backing_off(now));
   now += reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
   for (uint32_t i = 1; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i)
      BOOST_CHECK(!schedule.record_deadline_stop(now));
   BOOST_CHECK(!schedule.record(reap_scenario::next_epoch, progress, now));
   BOOST_CHECK(!schedule.backing_off(now));
   BOOST_CHECK(schedule.due(reap_scenario::next_epoch, now));
   for (uint32_t i = 1; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i)
      BOOST_CHECK(!schedule.record_deadline_stop(now));
   BOOST_CHECK(schedule.record_deadline_stop(now));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(outpost_epoch_settled_reads_only_the_dispatch_cursor) try {
   using reap_detail::outpost_epoch_settled;
   // next_epoch_index > epoch: the draining round moved past it, so a delivery for it has nothing left to do.
   BOOST_CHECK(outpost_epoch_settled({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 7));
   BOOST_CHECK(outpost_epoch_settled({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 3));
   // next_epoch_index == epoch: the live epoch, still to be delivered.
   BOOST_CHECK(!outpost_epoch_settled({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 8));
   // next_epoch_index < epoch: a delivery ahead of the outpost.
   BOOST_CHECK(!outpost_epoch_settled({.next_epoch_index = 8, .latest_outbound_epoch = 7}, 9));
   // Unlike reapability, settlement does not wait for the outbound emit cursor.
   BOOST_CHECK(outpost_epoch_settled({.next_epoch_index = 8, .latest_outbound_epoch = 6}, 7));
} FC_LOG_AND_RETHROW();

namespace {

/// One second, the margin the schedule tests put on either side of the back-off's end.
constexpr fc::microseconds SWEEP_SCHEDULE_TEST_MARGIN = fc::seconds(1);

} // anonymous namespace

BOOST_AUTO_TEST_CASE(chunk_sweep_schedule_reruns_only_after_the_cursor_moves) try {
   reap_detail::envelope_chunk_sweep_schedule schedule;
   const auto                                 now = fc::time_point::now();
   // The first sweep after start-up is always due: it recovers buffers a previous run left behind.
   BOOST_CHECK(!schedule.backing_off(now));
   BOOST_CHECK(schedule.due(8, now));

   schedule.record(8, reap_detail::envelope_chunk_reap_result{.reaped = 2}, now);
   // A complete sweep at the same cursor has nothing new to find, however much time passes.
   BOOST_CHECK(!schedule.backing_off(now));
   BOOST_CHECK(!schedule.due(8, now));
   BOOST_CHECK(!schedule.due(8, now + reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF + SWEEP_SCHEDULE_TEST_MARGIN));
   // Once the cursor moves an epoch has settled, so its buffer may be reapable.
   BOOST_CHECK(schedule.due(9, now));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_schedule_backs_off_after_a_failed_close) try {
   reap_detail::envelope_chunk_sweep_schedule schedule;
   const auto                                 now = fc::time_point::now();
   schedule.record(7, reap_detail::envelope_chunk_reap_result{}, now);
   schedule.record(8, reap_detail::envelope_chunk_reap_result{.reaped = 1, .failed = 1}, now);

   const auto backoff_end = now + reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
   BOOST_CHECK(schedule.backing_off(now));
   // Not due before the back-off ends, even though the cursor moved.
   BOOST_CHECK(!schedule.due(9, now));
   BOOST_CHECK(!schedule.due(9, backoff_end - SWEEP_SCHEDULE_TEST_MARGIN));
   BOOST_CHECK(schedule.due(9, backoff_end));
   // The failed sweep left the watermark at 7, so the cursor it failed at is retried too.
   BOOST_CHECK(!schedule.backing_off(backoff_end));
   BOOST_CHECK(schedule.due(8, backoff_end));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_schedule_stays_due_while_only_deferred) try {
   reap_detail::envelope_chunk_sweep_schedule schedule;
   const auto                                 now = fc::time_point::now();
   schedule.record(
      8,
      reap_detail::envelope_chunk_reap_result{
         .reaped = sysio::outpost_solana_client_detail::MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP, .deferred = 3},
      now);
   // The per-sweep bound cut the walk short: nothing failed, so no back-off, and the same cursor is still due.
   BOOST_CHECK(!schedule.backing_off(now));
   BOOST_CHECK(schedule.due(8, now));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_schedule_backs_off_after_a_thrown_sweep) try {
   reap_detail::envelope_chunk_sweep_schedule schedule;
   const auto                                 now = fc::time_point::now();
   schedule.record(8, reap_detail::envelope_chunk_reap_result{}, now);
   schedule.record_failure(now);

   const auto backoff_end = now + reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
   BOOST_CHECK(schedule.backing_off(now));
   BOOST_CHECK(!schedule.due(9, now));
   BOOST_CHECK(!schedule.due(9, backoff_end - SWEEP_SCHEDULE_TEST_MARGIN));
   BOOST_CHECK(schedule.due(9, backoff_end));
   // A thrown sweep leaves the watermark where the last complete sweep put it.
   BOOST_CHECK(!schedule.due(8, backoff_end));
} FC_LOG_AND_RETHROW();

// ── The sweep itself (`sweep_envelope_chunks`), driven through scripted touchpoints: a manual clock, counted reads,
//    and hooks that advance the clock or throw inside a read or after a close.

namespace {

using reap_detail::envelope_chunk_sweep_outcome;

/// The sweep deadline the scripted sweeps start with, relative to the scripted clock.
constexpr fc::microseconds SWEEP_TEST_DEADLINE = fc::seconds(30);
/// Maximum scheduling overhead allowed between entering the member and installing its deadline.
constexpr fc::microseconds SWEEP_ENTRY_TOLERANCE = fc::seconds(1);

/// A scripted sweep. Each read is counted before its hook runs, so a hook that throws still counts as a read.
struct sweep_harness {
   fc::time_point                                  clock = fc::time_point::now();
   reap_detail::envelope_chunk_reap_gates          gates{.next_epoch_index = 9, .latest_outbound_epoch = 8};
   std::vector<reap_detail::envelope_chunk_buffer> buffers;
   reap_harness                                    closes;
   uint32_t                                        gate_reads   = 0;
   uint32_t                                        buffer_reads = 0;
   /// Runs inside the cursor read, after it is counted.
   std::function<void()>                           on_read_gates;
   /// Runs inside the discovery read, after it is counted.
   std::function<void()>                           on_read_buffers;
   /// Runs after a close returned.
   std::function<void(uint32_t)>                   after_close;

   /// Reconcile a failed close against its exact buffer when supplied by the test.
   std::function<bool(const reap_detail::envelope_chunk_buffer&)> buffer_absent;

   /// Bind all production sweep touchpoints to the scripted clock and hooks.
   reap_detail::envelope_chunk_sweep_touchpoints touchpoints() {
      return {
         .now        = [this] { return clock; },
         .read_gates =
            [this] {
               ++gate_reads;
               if (on_read_gates) on_read_gates();
               return gates;
            },
         .read_buffers =
            [this] {
               ++buffer_reads;
               if (on_read_buffers) on_read_buffers();
               return buffers;
            },
         .send_cleanup =
            [this](uint32_t epoch) {
               auto sig = closes.send()(epoch);
               if (after_close) after_close(epoch);
               return sig;
            },
         .buffer_absent = buffer_absent,
      };
   }

   /// Capture the effective window before any scripted RPC advances the clock.
   envelope_chunk_sweep_outcome sweep(reap_detail::envelope_chunk_sweep_schedule& schedule, fc::time_point deadline_abs,
                                      const std::optional<reap_detail::envelope_chunk_reap_gates>& known = std::nullopt) {
      return reap_detail::sweep_envelope_chunks(schedule, clock, deadline_abs, known, touchpoints(), "test-relay");
   }
};

/// Capture warnings while preserving the process-wide logger on every test exit path.
struct chunk_sweep_warning_capture {
   std::ostringstream output;
   fc::log_level saved_level = fc::logger::default_logger().get_log_level();
   std::unique_ptr<spdlog::logger> saved_agent;

   /// Install a warning-only sink so per-stop informational logs cannot satisfy warning assertions.
   chunk_sweep_warning_capture() {
      auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
      sink->set_pattern("%v");
      auto agent = std::make_unique<spdlog::logger>("chunk-sweep-test", sink);
      agent->set_level(spdlog::level::warn);
      auto& logger = fc::logger::default_logger();
      saved_agent = std::move(logger.get_agent_logger());
      logger.update_agent_logger(std::move(agent));
      logger.set_log_level(fc::log_level::warn);
   }

   /// Restore both the logger and its severity after assertions or exceptions.
   ~chunk_sweep_warning_capture() {
      auto& logger = fc::logger::default_logger();
      logger.update_agent_logger(std::move(saved_agent));
      logger.set_log_level(saved_level);
   }
};

} // anonymous namespace

/// Landed closes cut off during confirmation drain across sweeps without a false no-progress back-off.
BOOST_AUTO_TEST_CASE(chunk_sweep_cut_off_landed_closes_count_as_progress) try {
   for (const bool landed : {false, true}) {
      sweep_harness harness;
      reap_detail::envelope_chunk_sweep_schedule schedule;
      constexpr uint32_t backlog = reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT + 1;
      const auto me = measurement_pubkey(reap_scenario::uploader_seed);
      for (uint32_t epoch = 1; epoch <= backlog; ++epoch)
         harness.buffers.push_back(scripted_buffer(epoch, me));
      for (uint32_t attempt = 0; attempt < backlog; ++attempt) {
         const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
         harness.after_close = [&, deadline](uint32_t epoch) {
            if (landed) std::erase_if(harness.buffers, [&](const auto& buffer) {
               return buffer.epoch_index == epoch;
            });
            harness.clock = deadline;
            FC_THROW_EXCEPTION(fc::timeout_exception, "confirmation cut off");
         };
         BOOST_CHECK(harness.sweep(schedule, deadline) ==
                     (!landed && attempt >= reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT
                         ? envelope_chunk_sweep_outcome::backing_off : envelope_chunk_sweep_outcome::swept));
         BOOST_CHECK_EQUAL(schedule.backing_off(harness.clock),
                           !landed && attempt + 1 >= reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT);
      }
      if (landed) {
         BOOST_CHECK(harness.buffers.empty());
         BOOST_CHECK_EQUAL(harness.closes.sent.size(), backlog);
         harness.after_close = nullptr;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                     envelope_chunk_sweep_outcome::swept);
         BOOST_CHECK(!schedule.due(harness.gates.next_epoch_index, harness.clock));
      }
   }
} FC_LOG_AND_RETHROW();

/// Gate and discovery exhaustion warn on the first stop, for thrown and returned reads.
BOOST_AUTO_TEST_CASE(chunk_sweep_discovery_exhaustion_logs_warning) try {
   for (const bool gate : {false, true}) {
      for (const bool throws : {false, true}) {
         chunk_sweep_warning_capture capture;
         sweep_harness harness;
         reap_detail::envelope_chunk_sweep_schedule schedule;
         harness.buffers = {scripted_buffer(reap_scenario::buffer_epoch,
                                            measurement_pubkey(reap_scenario::uploader_seed))};
         const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
         auto& read = gate ? harness.on_read_gates : harness.on_read_buffers;
         read = [&, deadline] {
            harness.clock = deadline;
            if (throws) FC_THROW_EXCEPTION(fc::timeout_exception, "read cut off");
         };
         harness.sweep(schedule, deadline);
         const auto warning = capture.output.str();
         BOOST_CHECK(warning.find("outpost_solana_client[test-relay]:") != std::string::npos);
         BOOST_CHECK(warning.find("gate/discovery reads exhausted the sweep budget") != std::string::npos);
         BOOST_CHECK(warning.find("before any close") != std::string::npos);
         BOOST_CHECK(harness.closes.sent.empty());
         BOOST_CHECK_EQUAL(harness.buffer_reads, gate ? 0u : 1u);
         BOOST_CHECK_EQUAL(std::count(warning.begin(), warning.end(), '\n'), 1);
         BOOST_CHECK(!schedule.backing_off(harness.clock));
      }
   }
} FC_LOG_AND_RETHROW();

/// A caller-limited window must not blame endpoint speed for gate or discovery exhaustion.
BOOST_AUTO_TEST_CASE(chunk_sweep_short_window_does_not_warn) try {
   constexpr auto caller_window = fc::milliseconds(150);
   for (const bool gate : {false, true}) {
      for (const bool throws : {false, true}) {
         chunk_sweep_warning_capture capture;
         sweep_harness harness;
         reap_detail::envelope_chunk_sweep_schedule schedule;
         const auto deadline = harness.clock + caller_window;
         auto& read = gate ? harness.on_read_gates : harness.on_read_buffers;
         read = [&, deadline] {
            harness.clock = deadline;
            if (throws) FC_THROW_EXCEPTION(fc::timeout_exception, "read cut off");
         };
         harness.sweep(schedule, deadline);
         BOOST_CHECK(capture.output.str().empty());
         BOOST_CHECK_EQUAL(harness.buffer_reads, gate ? 0u : 1u);
         BOOST_CHECK(harness.closes.sent.empty());
      }
   }
} FC_LOG_AND_RETHROW();

/// Timely discovery followed by a cut-off close must not emit a gate/discovery warning.
BOOST_AUTO_TEST_CASE(chunk_sweep_timely_discovery_cut_off_close_does_not_warn) try {
   for (const bool throws : {false, true}) {
      chunk_sweep_warning_capture capture;
      sweep_harness harness;
      reap_detail::envelope_chunk_sweep_schedule schedule;
      harness.buffers = {scripted_buffer(reap_scenario::buffer_epoch,
                                         measurement_pubkey(reap_scenario::uploader_seed))};
      const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
      harness.after_close = [&, deadline](uint32_t) {
         harness.clock = deadline;
         if (throws) FC_THROW_EXCEPTION(fc::timeout_exception, "confirmation cut off");
      };
      BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::swept);
      BOOST_CHECK_EQUAL(harness.buffer_reads, 1u);
      BOOST_CHECK_EQUAL(harness.closes.sent.size(), 1u);
      BOOST_CHECK(capture.output.str().empty());
   }
} FC_LOG_AND_RETHROW();

/// The sweep must forward its absence callback so a stale discovery does not create failure back-off.
BOOST_AUTO_TEST_CASE(chunk_sweep_wires_already_closed_reconciliation) try {
   const auto me = measurement_pubkey(reap_scenario::uploader_seed);
   sweep_harness harness;
   harness.buffers = {scripted_buffer(reap_scenario::buffer_epoch, me)};
   harness.closes.failing.insert(harness.buffers.front().epoch_index);
   unsigned absence_reads = 0;
   harness.buffer_absent = [&](const reap_detail::envelope_chunk_buffer& buffer) {
      ++absence_reads;
      BOOST_CHECK(buffer.address == harness.buffers.front().address);
      return true;
   };
   reap_detail::envelope_chunk_sweep_schedule schedule;
   const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
   BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK_EQUAL(absence_reads, 1u);
   BOOST_CHECK_EQUAL(harness.closes.sent.size(), 1u);
   BOOST_CHECK(!schedule.backing_off(harness.clock));
   BOOST_CHECK(!schedule.due(harness.gates.next_epoch_index, harness.clock));
   BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::not_due);
   BOOST_CHECK_EQUAL(harness.buffer_reads, 1u);
} FC_LOG_AND_RETHROW();

/// A positive remaining budget permits a close without a minimum-latency start guard.
BOOST_AUTO_TEST_CASE(chunk_sweep_starts_cleanup_with_a_tight_budget) try {
   constexpr auto remaining = fc::microseconds(1);
   sweep_harness harness;
   harness.buffers = {scripted_buffer(reap_scenario::buffer_epoch, measurement_pubkey(reap_scenario::uploader_seed))};
   reap_detail::envelope_chunk_sweep_schedule schedule;
   BOOST_CHECK(harness.sweep(schedule, harness.clock + remaining) == envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK_EQUAL(harness.closes.sent.size(), 1u);
   BOOST_CHECK(!schedule.due(harness.gates.next_epoch_index, harness.clock));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_reads_nothing_while_backing_off) try {
   const auto    me = measurement_pubkey(7);
   sweep_harness harness;
   harness.buffers = {scripted_buffer(2, me)};
   reap_detail::envelope_chunk_sweep_schedule schedule;
   schedule.record_failure(harness.clock);

   // Backing off: not even the cursor read goes out.
   BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) == envelope_chunk_sweep_outcome::backing_off);
   BOOST_CHECK_EQUAL(harness.gate_reads, 0u);
   BOOST_CHECK_EQUAL(harness.buffer_reads, 0u);
   BOOST_CHECK(harness.closes.sent.empty());

   // Once the back-off has run out the same call sweeps.
   harness.clock = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
   BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) == envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK_EQUAL(harness.gate_reads, 1u);
   BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_reads_nothing_without_deadline) try {
   const auto    me = measurement_pubkey(7);
   sweep_harness harness;
   harness.buffers = {scripted_buffer(2, me)};
   reap_detail::envelope_chunk_sweep_schedule schedule;

   BOOST_CHECK(harness.sweep(schedule, harness.clock) == envelope_chunk_sweep_outcome::deadline_spent);
   BOOST_CHECK_EQUAL(harness.gate_reads, 0u);
   BOOST_CHECK_EQUAL(harness.buffer_reads, 0u);
   // Starting with no time left is not a failure.
   BOOST_CHECK(!schedule.backing_off(harness.clock));
   BOOST_CHECK(schedule.due(harness.gates.next_epoch_index, harness.clock));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_reuses_given_gates_and_reruns_only_when_due) try {
   const auto    me = measurement_pubkey(7);
   sweep_harness harness;
   harness.buffers = {scripted_buffer(2, me), scripted_buffer(9, me)};
   reap_detail::envelope_chunk_sweep_schedule schedule;

   // Cursors the caller just read are used as they are.
   BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE, harness.gates) ==
               envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK_EQUAL(harness.gate_reads, 0u);
   BOOST_CHECK_EQUAL(harness.buffer_reads, 1u);
   BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2}));

   // The cursor has not moved: the next call reads it and stops before the discovery read.
   BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) == envelope_chunk_sweep_outcome::not_due);
   BOOST_CHECK_EQUAL(harness.gate_reads, 1u);
   BOOST_CHECK_EQUAL(harness.buffer_reads, 1u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_backs_off_when_a_read_fails_before_the_deadline) try {
   const auto me = measurement_pubkey(7);
   {
      sweep_harness harness;
      harness.buffers         = {scripted_buffer(2, me)};
      harness.on_read_buffers = [] { FC_THROW("getProgramAccounts is disabled on this endpoint"); };
      reap_detail::envelope_chunk_sweep_schedule schedule;

      BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                  envelope_chunk_sweep_outcome::failed);
      BOOST_CHECK(harness.closes.sent.empty());
      BOOST_CHECK(schedule.backing_off(harness.clock));
      BOOST_CHECK(!schedule.due(harness.gates.next_epoch_index, harness.clock));
   }
   {
      // A non-fc exception out of the cursor read backs off the same way.
      sweep_harness harness;
      harness.on_read_gates = [] { throw std::runtime_error("account data shorter than its declared layout"); };
      reap_detail::envelope_chunk_sweep_schedule schedule;

      BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                  envelope_chunk_sweep_outcome::failed);
      BOOST_CHECK_EQUAL(harness.buffer_reads, 0u);
      BOOST_CHECK(schedule.backing_off(harness.clock));
   }
   {
      // A timeout that is the endpoint's own (connect / read), before the sweep's deadline, is a failure too.
      sweep_harness harness;
      harness.on_read_buffers = [] { FC_THROW_EXCEPTION(fc::timeout_exception, "Outbound HTTP connect timeout"); };
      reap_detail::envelope_chunk_sweep_schedule schedule;

      BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                  envelope_chunk_sweep_outcome::failed);
      BOOST_CHECK(schedule.backing_off(harness.clock));
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_first_deadline_stop_stays_due) try {
   const auto me = measurement_pubkey(7);
   for (const bool in_gate_read : {true, false}) {
      BOOST_TEST_CONTEXT("deadline runs out in the " << (in_gate_read ? "cursor" : "discovery") << " read") {
         sweep_harness harness;
         harness.buffers         = {scripted_buffer(2, me)};
         const auto deadline_abs = harness.clock + SWEEP_TEST_DEADLINE;
         // The scoped deadline ends the RPC: the transport throws once the clock reaches it.
         const auto run_out = [&] {
            harness.clock = deadline_abs;
            FC_THROW_EXCEPTION(fc::timeout_exception, "Outbound HTTP total request deadline expired");
         };
         (in_gate_read ? harness.on_read_gates : harness.on_read_buffers) = run_out;
         reap_detail::envelope_chunk_sweep_schedule schedule;

         BOOST_CHECK(harness.sweep(schedule, deadline_abs) == envelope_chunk_sweep_outcome::cut_short);
         BOOST_CHECK(harness.closes.sent.empty());
         // The first deadline stop does not back off: the next call with its own deadline sweeps right away.
         BOOST_CHECK(!schedule.backing_off(harness.clock));
         BOOST_CHECK(schedule.due(harness.gates.next_epoch_index, harness.clock));

         harness.on_read_gates   = nullptr;
         harness.on_read_buffers = nullptr;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                     envelope_chunk_sweep_outcome::swept);
         BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2}));
      }
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_resumes_a_walk_the_deadline_stopped) try {
   const auto    me = measurement_pubkey(7);
   sweep_harness harness;
   harness.buffers         = {scripted_buffer(2, me), scripted_buffer(3, me), scripted_buffer(4, me)};
   const auto deadline_abs = harness.clock + SWEEP_TEST_DEADLINE;
   // The first close uses up the rest of the deadline.
   harness.after_close = [&](uint32_t) { harness.clock = deadline_abs; };
   reap_detail::envelope_chunk_sweep_schedule schedule;

   BOOST_CHECK(harness.sweep(schedule, deadline_abs) == envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2}));
   BOOST_CHECK(!schedule.backing_off(harness.clock));
   BOOST_CHECK(schedule.due(harness.gates.next_epoch_index, harness.clock));

   // The next call carries on with what is left on chain, at the same cursor.
   harness.after_close = nullptr;
   harness.buffers     = {scripted_buffer(3, me), scripted_buffer(4, me)};
   BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) == envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2, 3, 4}));
   // Complete now: the same cursor is no longer due.
   BOOST_CHECK(!schedule.due(harness.gates.next_epoch_index, harness.clock));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_backs_off_after_a_failed_close) try {
   const auto    me = measurement_pubkey(7);
   sweep_harness harness;
   harness.buffers        = {scripted_buffer(2, me), scripted_buffer(3, me)};
   harness.closes.failing = {2};
   reap_detail::envelope_chunk_sweep_schedule schedule;

   BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) == envelope_chunk_sweep_outcome::swept);
   BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2, 3}));
   BOOST_CHECK(schedule.backing_off(harness.clock));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(settled_epoch_delivery_is_skipped_and_swept) try {
   const reap_detail::envelope_chunk_reap_gates      gates{.next_epoch_index = 8, .latest_outbound_epoch = 7};
   std::vector<reap_detail::envelope_chunk_reap_gates> swept;
   const auto sweep      = [&](const reap_detail::envelope_chunk_reap_gates& read) { swept.push_back(read); };
   const auto read_gates = [&] { return gates; };

   // Settled: nothing is delivered, and the sweep runs with the cursors just read.
   BOOST_CHECK(reap_detail::skip_settled_epoch_delivery(7, read_gates, sweep, "test-relay"));
   BOOST_REQUIRE_EQUAL(swept.size(), 1u);
   BOOST_CHECK_EQUAL(swept[0].next_epoch_index, gates.next_epoch_index);
   BOOST_CHECK_EQUAL(swept[0].latest_outbound_epoch, gates.latest_outbound_epoch);

   // The live epoch and one ahead of the outpost are delivered as usual, with no sweep in front of them.
   BOOST_CHECK(!reap_detail::skip_settled_epoch_delivery(8, read_gates, sweep, "test-relay"));
   BOOST_CHECK(!reap_detail::skip_settled_epoch_delivery(9, read_gates, sweep, "test-relay"));
   BOOST_CHECK_EQUAL(swept.size(), 1u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(settled_epoch_check_failure_delivers_normally) try {
   uint32_t   sweeps = 0;
   const auto sweep  = [&](const reap_detail::envelope_chunk_reap_gates&) { ++sweeps; };

   // An unread cursor is "not known settled": deliver, and do not sweep.
   BOOST_CHECK(!reap_detail::skip_settled_epoch_delivery(
      7,
      []() -> reap_detail::envelope_chunk_reap_gates {
         FC_THROW_EXCEPTION(fc::timeout_exception, "getMultipleAccounts timed out");
      },
      sweep, "test-relay"));
   BOOST_CHECK(!reap_detail::skip_settled_epoch_delivery(
      7, []() -> reap_detail::envelope_chunk_reap_gates { throw std::runtime_error("OutpostConfig is absent"); },
      sweep, "test-relay"));
   BOOST_CHECK_EQUAL(sweeps, 0u);
} FC_LOG_AND_RETHROW();

/// Idle reads forward the delivery deadline; the sweep member owns the single short-budget cap.
BOOST_AUTO_TEST_CASE(inbound_read_sweeps_only_when_idle_and_forwards_deadline) try {
   const auto                  start = fc::time_point::now();
   fc::time_point              clock = start;
   std::vector<fc::time_point> sweep_deadlines;
   const auto sweep = [&](fc::time_point deadline_abs) { sweep_deadlines.push_back(deadline_abs); };
   const auto read  = [&](fc::microseconds took, std::vector<char> envelope) {
      return [&clock, start, took, envelope] {
         clock = start + took;
         return envelope;
      };
   };

   // An envelope to deliver: returned untouched, with no sweep in front of the depot delivery.
   const std::vector<char> found = {'e', 'n', 'v'};
   BOOST_CHECK(reap_detail::read_envelope_then_sweep_when_idle(read(fc::seconds(1), found), sweep,
                                                               start + fc::seconds(60)) == found);
   BOOST_CHECK(sweep_deadlines.empty());

   // Idle reads forward the caller deadline; the real sweep member applies the short cap once.
   BOOST_CHECK(
      reap_detail::read_envelope_then_sweep_when_idle(read(fc::seconds(2), {}), sweep, start + fc::seconds(60))
         .empty());
   BOOST_REQUIRE_EQUAL(sweep_deadlines.size(), 1u);
   BOOST_CHECK(sweep_deadlines[0] == start + fc::seconds(60));

   // Idle with less than the budget left: the caller's deadline still bounds it.
   const auto tight = start + fc::seconds(2) + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET - fc::seconds(1);
   BOOST_CHECK(reap_detail::read_envelope_then_sweep_when_idle(read(fc::seconds(2), {}), sweep, tight).empty());
   BOOST_REQUIRE_EQUAL(sweep_deadlines.size(), 2u);
   BOOST_CHECK(sweep_deadlines[1] == tight);
} FC_LOG_AND_RETHROW();

namespace {

/// Fixture account and field names used by the production gate decoder.
namespace reap_gate_fixture {
   constexpr auto config_name = "OutpostConfig";
   constexpr auto latest_name = "LatestOutboundEnvelope";
   constexpr auto next_field = "next_epoch_index";
   constexpr auto latest_field = "epoch_index";
   constexpr auto renamed_field = "unrecognized_cursor";
   constexpr uint32_t next_epoch = 23;
   constexpr uint32_t latest_epoch = 17;
   constexpr uint8_t bump = 0xFE;
   constexpr uint8_t config_version = 1;
   constexpr uint64_t chain_code = 0x12345678;
   constexpr size_t hash_bytes = 32;
   constexpr uint32_t empty_u32 = 0;
   constexpr uint64_t empty_u64 = 0;
   constexpr int64_t empty_i64 = 0;
   constexpr uint32_t foreign_program_seed = 43;
   constexpr size_t account_count = 2;
   /// Shared RPC slice covers config cursor bytes 18..22 and latest cursor bytes 9..13.
   constexpr size_t header_bytes = 22;
   constexpr std::array<const char*, account_count> account_names = {config_name, latest_name};
   constexpr std::array<const char*, account_count> cursor_fields = {next_field, latest_field};
}

/// Serialize both gate accounts in the fixture's full integrated Borsh layout, including fields after the cursors.
/// Distinct cursors and nonzero prefix fields detect a swapped account/field or an incorrect byte offset.
std::vector<std::optional<account_info>> reap_gate_accounts() {
   using namespace reap_gate_fixture;
   const std::array<uint8_t, hash_bytes> hash{};
   borsh::encoder config;
   const auto disc = idl::compute_account_discriminator(config_name);
   config.write_fixed_bytes(disc.data(), disc.size());
   config.write_u8(bump);
   config.write_u8(config_version);
   config.write_u64(chain_code);
   config.write_u32(next_epoch);
   config.write_fixed_bytes(hash.data(), hash.size()); // previous_epoch_hash
   config.write_fixed_bytes(hash.data(), hash.size()); // previous_outbound_epoch_hash
   config.write_u32(empty_u32); // epoch_duration_sec
   config.write_i64(empty_i64); // current_epoch_started_at
   config.write_bool(false); // registry_initialized
   config.write_fixed_bytes(hash.data(), hash.size()); // last_message_id
   config.write_u64(empty_u64); // last_message_timestamp
   config.write_u32(empty_u32); // envelope_retention_epochs
   config.write_u32(empty_u32); // token_addresses_by_code length
   config.write_u32(empty_u32); // precision_by_token_code length
   return {program_owned(config.data()), program_owned(integrated_latest_account(latest_epoch, {}, hash))};
}

/// Require an actionable diagnostic naming the specific invalid account, and optionally its missing field.
bool gate_error_names(const fc::exception& e, const char* account, const char* field = nullptr) {
   const auto message = e.to_detail_string();
   return message.find(account) != std::string::npos &&
          (field == nullptr || message.find(field) != std::string::npos);
}

} // anonymous namespace

/// Decode the real fixture layouts; the two cursors must retain their separate meanings.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_decode_distinct_cursors) try {
   auto client = resume_program_client();
   const auto gates = reap_detail::decode_envelope_chunk_reap_gates(client, reap_gate_accounts());
   BOOST_CHECK_EQUAL(gates.next_epoch_index, reap_gate_fixture::next_epoch);
   BOOST_CHECK_EQUAL(gates.latest_outbound_epoch, reap_gate_fixture::latest_epoch);
} FC_LOG_AND_RETHROW();

/// Neither missing singleton can supply a trustworthy settlement cursor.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_missing_accounts) try {
   auto client = resume_program_client();
   for (size_t i = 0; i < reap_gate_fixture::account_names.size(); ++i) {
      auto accounts = reap_gate_accounts();
      accounts[i] = std::nullopt;
      BOOST_CHECK_EXCEPTION(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception,
                            [i](const fc::exception& e) {
                               return gate_error_names(e, reap_gate_fixture::account_names[i]);
                            });
   }
} FC_LOG_AND_RETHROW();

/// An existing account without data must fail just like an absent account.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_empty_accounts) try {
   auto client = resume_program_client();
   for (size_t i = 0; i < reap_gate_fixture::account_names.size(); ++i) {
      auto accounts = reap_gate_accounts();
      accounts[i]->data.clear();
      BOOST_CHECK_EXCEPTION(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception,
                            [i](const fc::exception& e) {
                               return gate_error_names(e, reap_gate_fixture::account_names[i]);
                            });
   }
} FC_LOG_AND_RETHROW();

/// Valid bytes belonging to another program cannot authorize either cursor.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_foreign_owners) try {
   auto client = resume_program_client();
   for (size_t i = 0; i < reap_gate_fixture::account_names.size(); ++i) {
      auto accounts = reap_gate_accounts();
      accounts[i]->owner = measurement_pubkey(reap_gate_fixture::foreign_program_seed);
      BOOST_CHECK_EXCEPTION(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception,
                            [i](const fc::exception& e) {
                               return gate_error_names(e, reap_gate_fixture::account_names[i]);
                            });
   }
} FC_LOG_AND_RETHROW();

/// Both cursor accounts must match their declared Anchor discriminator before decoding.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_wrong_discriminator) try {
   auto client = resume_program_client();
   for (size_t i = 0; i < reap_gate_fixture::account_names.size(); ++i) {
      auto accounts = reap_gate_accounts();
      accounts[i]->data.front() ^= 1;
      BOOST_CHECK_EXCEPTION(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception,
                            [&](const fc::exception& e) {
                               return gate_error_names(e, reap_gate_fixture::account_names[i]);
                            });
   }
} FC_LOG_AND_RETHROW();

/// A header truncated inside either cursor must name the broken singleton.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_short_data) try {
   auto client = resume_program_client();
   for (size_t i = 0; i < reap_gate_fixture::account_names.size(); ++i) {
      auto accounts = reap_gate_accounts();
      constexpr std::array<size_t, reap_gate_fixture::account_count> cursor_ends = {22, 13};
      accounts[i]->data.resize(cursor_ends[i] - 1);
      BOOST_CHECK_EXCEPTION(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception,
                            [i](const fc::exception& e) {
                               return gate_error_names(e, reap_gate_fixture::account_names[i]);
                            });
   }
} FC_LOG_AND_RETHROW();

/// A renamed cursor must fail the boot probe with the account and required field in the diagnostic.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_missing_idl_fields) try {
   for (size_t i = 0; i < reap_gate_fixture::account_names.size(); ++i) {
      auto prog = load_idl_fixture(opp_outpost_idl_fixture);
      auto type = std::ranges::find_if(prog.types, [i](const idl::type_def& t) {
         return t.name == reap_gate_fixture::account_names[i];
      });
      BOOST_REQUIRE(type != prog.types.end());
      BOOST_REQUIRE(type->struct_fields.has_value());
      auto field = std::ranges::find_if(*type->struct_fields, [i](const idl::field& f) {
         return f.name == reap_gate_fixture::cursor_fields[i];
      });
      BOOST_REQUIRE(field != type->struct_fields->end());
      field->name = reap_gate_fixture::renamed_field;
      auto client = resume_program_client(std::move(prog));
      BOOST_CHECK_EXCEPTION(reap_detail::assert_envelope_chunk_gate_shapes(client), fc::exception,
                            [i](const fc::exception& e) {
                               return gate_error_names(e, reap_gate_fixture::account_names[i],
                                                       reap_gate_fixture::cursor_fields[i]);
                            });
   }
} FC_LOG_AND_RETHROW();

/// A short or oversized RPC result is not the ordered pair the decoder requires.
BOOST_AUTO_TEST_CASE(chunk_reap_gates_reject_wrong_account_count) try {
   auto client = resume_program_client();
   auto accounts = reap_gate_accounts();
   accounts.pop_back();
   BOOST_CHECK_THROW(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception);
   accounts = reap_gate_accounts();
   accounts.push_back(std::nullopt);
   BOOST_CHECK_THROW(reap_detail::decode_envelope_chunk_reap_gates(client, accounts), fc::exception);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_buffer_discovery_filters_on_discriminator_and_operator) try {
   const auto me    = measurement_pubkey(7);
   const auto disc  = idl::compute_account_discriminator(std::string{envelope_chunks_name});
   const auto filts = reap_detail::envelope_chunk_buffer_filters(disc, me);
   BOOST_REQUIRE_EQUAL(filts.size(), 2u);

   const auto& by_disc = filts[0].get_object()["memcmp"].get_object();
   BOOST_CHECK_EQUAL(by_disc["offset"].as_uint64(), 0u);
   const auto disc_bytes = fc::from_base58(by_disc["bytes"].as_string());
   BOOST_CHECK(std::ranges::equal(disc_bytes, disc, [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; }));

   // The operator filter's offset is checked against a real buffer's bytes, not against the relay's constant.
   const auto& by_operator = filts[1].get_object()["memcmp"].get_object();
   const auto  offset      = by_operator["offset"].as_uint64();
   BOOST_CHECK_EQUAL(by_operator["bytes"].as_string(), me.to_string(fc::yield_function_t{}));
   const auto header = sliced_header(4, me);
   const auto key    = me.serialize();
   BOOST_REQUIRE_LE(offset + key.size(), header.size());
   BOOST_CHECK(std::equal(key.begin(), key.end(), header.begin() + static_cast<std::ptrdiff_t>(offset)));

   // The slice carries the whole header and nothing of the staged payload.
   const auto slice = reap_detail::envelope_chunk_buffer_header_slice();
   BOOST_CHECK_EQUAL(slice.offset, 0u);
   BOOST_CHECK_EQUAL(slice.length, header.size());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_buffer_discovery_decodes_own_buffers_and_skips_drift) try {
   auto       program_client = resume_program_client();
   const auto me             = measurement_pubkey(7);
   const auto other          = measurement_pubkey(8);

   const fc::variants rpc_result = {
      program_accounts_entry(program_chunk_buffer_pda(resume_program_id, 6, me), sliced_header(6, me),
                             buffer_rent(6)),
      program_accounts_entry(program_chunk_buffer_pda(resume_program_id, 2, me), sliced_header(2, me),
                             buffer_rent(2)),
      // Decodes to another operator: a drifted decode the reaper must leave alone.
      program_accounts_entry(program_chunk_buffer_pda(resume_program_id, 3, other), sliced_header(3, other),
                             buffer_rent(3)),
      // Decodes to epoch 5 at the address of epoch 4: a close built from it would fail its seeds check.
      program_accounts_entry(program_chunk_buffer_pda(resume_program_id, 4, me), sliced_header(5, me),
                             buffer_rent(4)),
   };

   const auto buffers = reap_detail::decode_envelope_chunk_buffers(program_client, fc::variant(rpc_result), me);
   BOOST_REQUIRE_EQUAL(buffers.size(), 2u);
   BOOST_CHECK_EQUAL(buffers[0].epoch_index, 2u);
   BOOST_CHECK(buffers[0].address == program_chunk_buffer_pda(resume_program_id, 2, me));
   BOOST_CHECK_EQUAL(buffers[0].lamports, buffer_rent(2));
   BOOST_CHECK_EQUAL(buffers[1].epoch_index, 6u);

   // A result that is not the RPC's array shape is an error, not "no buffers".
   BOOST_CHECK_THROW(reap_detail::decode_envelope_chunk_buffers(program_client, fc::variant("unexpected"), me),
                     fc::exception);
} FC_LOG_AND_RETHROW();

namespace {

/// Own appbase and a real fixture signer for member tests; only account reads are stubbed.
struct chunk_member_fixture {
   appbase::scoped_app application{};
   sysio::solana_client_entry_ptr entry = std::make_shared<sysio::solana_client_entry_t>();

   /// Initialize the signer exactly as the plugin does, without connecting to an endpoint.
   chunk_member_fixture() {
      std::vector<std::string> arguments{"test_outpost_solana_client_plugin"};
      std::vector<char*> argv;
      for (auto& argument : arguments) argv.push_back(argument.data());
      BOOST_REQUIRE(application->initialize<sysio::signature_provider_manager_plugin>(argv.size(), argv.data()));
      const auto key = fc::test::load_keygen_fixture("solana", 1);
      entry->signature_provider = application->get_plugin<sysio::signature_provider_manager_plugin>().create_provider(
         "signer-a", fc::crypto::chain_kind_solana, fc::crypto::chain_key_type_solana, key.public_key,
         fc::test::to_private_key_spec(key.private_key));
      entry->client = std::make_shared<solana_client>(
         entry->signature_provider, std::variant<std::string, fc::url>{std::string(startup_test_rpc_url)});
   }
};

} // anonymous namespace

BOOST_FIXTURE_TEST_CASE(cleanup_envelope_chunks_names_the_signer_as_uploader, chunk_member_fixture) try {
   // The binding used to leave `uploader` to the resolver, which cannot fill a non-signer account with no IDL
   // seeds: every call threw before a transaction was built. Pin the resolved list against the IDL.
   sysio::opp_solana_outpost_client program_client(entry->client, resume_program_id,
                                                   {load_idl_fixture(opp_outpost_idl_fixture)});
   const auto signer   = entry->client->get_pubkey();
   const auto accounts = program_client.cleanup_envelope_chunks_accounts(9);

   BOOST_REQUIRE_EQUAL(accounts.size(), 5u);
   BOOST_CHECK(accounts[0].key == signer);   // reaper
   BOOST_CHECK(accounts[0].is_signer);
   BOOST_CHECK(accounts[1].key == program_client.config_pda);
   BOOST_CHECK(accounts[2].key == program_client.latest_outbound_envelope_pda);
   BOOST_CHECK(accounts[3].key == program_chunk_buffer_pda(resume_program_id, 9, signer));
   BOOST_CHECK(accounts[3].is_writable);
   BOOST_CHECK(accounts[4].key == signer);   // uploader: the rent goes back to the relay that paid it
   BOOST_CHECK(accounts[4].is_writable);
} FC_LOG_AND_RETHROW();

namespace {

/// Keep startup declarations valid but reject the cleanup binding's epoch argument before any RPC.
idl::program cleanup_argument_failure_idl() {
   constexpr auto cleanup_instruction = reap_detail::CLEANUP_ENVELOPE_CHUNKS_INSTRUCTION;
   auto program = load_idl_fixture(opp_outpost_idl_fixture);
   auto instruction = std::ranges::find_if(program.instructions, [](const auto& candidate) {
      return candidate.name == cleanup_instruction;
   });
   BOOST_REQUIRE(instruction != program.instructions.end());
   instruction->args.clear();
   return program;
}

/// JSON-RPC request fields and scripted transaction results used by the member regressions.
namespace chunk_rpc_fixture {
   constexpr auto account_key = "account";
   constexpr auto rent_epoch_key = "rentEpoch";
   constexpr auto context_key = "context";
   constexpr auto slot_key = "slot";
   constexpr auto value_key = "value";
   constexpr auto commitment_key = "commitment";
   constexpr auto finalized = "finalized";
   constexpr auto account_method = "getAccountInfo";
   constexpr auto multiple_accounts_method = "getMultipleAccounts";
   constexpr auto method_key = "method";
   constexpr auto params_key = "params";
   constexpr auto memcmp_key = "memcmp";
   constexpr auto offset_key = "offset";
   constexpr auto bytes_key = "bytes";
   constexpr auto preflight_key = "preflightCommitment";
   constexpr auto skip_preflight_key = "skipPreflight";
   constexpr auto send_method = "sendTransaction";
   constexpr auto confirmed = "confirmed";
   constexpr auto signature = "cleanup-signature";
   constexpr auto timeout_error = "settled check scoped deadline expired";
   constexpr char payload = 'e';
   constexpr size_t filters_count = 2;
   constexpr size_t requests_per_transaction = 3;
}

/// Drive the actual member glue with scripted account reads and no network traffic.
struct chunk_member_client : sysio::outpost_solana_client {
   /// Construct the real program client and run its batch-operator IDL checks, with an optional test IDL.
   explicit chunk_member_client(sysio::solana_client_entry_ptr entry,
                                idl::program program = load_idl_fixture(opp_outpost_idl_fixture))
      : outpost_solana_client(std::move(entry), resume_program_id, {std::move(program)},
                              1, 1, sysio::solana_outpost_role::batch_operator) {}

   using outpost_solana_client::reap_settled_envelope_chunks;
   /// Attempted gate reads.
   uint32_t gate_reads = 0;
   /// Attempted discovery reads.
   uint32_t discovery_reads = 0;
   /// Attempted upload resume reads.
   uint32_t resume_reads = 0;
   /// Active deadline observed by discovery.
   std::optional<fc::time_point> discovery_deadline;
   /// Use the real finalized and confirmed header RPC reads.
   bool use_rpc_reads = false;
   /// Scripted finalized envelope bytes.
   std::vector<char> inbound;
   /// Scripted discovery buffers.
   std::vector<reap_detail::envelope_chunk_buffer> buffers;
   /// Commitments passed to exact-buffer reads.
   std::vector<commitment_t> absence_commitments;
   /// Addresses passed to exact-buffer reads.
   std::vector<solana_public_key> absence_addresses;
   /// Active deadline observed by the absence read.
   std::optional<fc::time_point> absence_deadline;
   /// Scripted exact-buffer response.
   std::optional<account_info> buffer_account;

   /// Observe the exact account, commitment and scoped deadline used to reconcile cleanup errors.
   std::optional<account_info> read_envelope_chunk_buffer(
      const solana_public_key& address, commitment_t commitment) override {
      absence_addresses.push_back(address);
      absence_commitments.push_back(commitment);
      absence_deadline = fc::task::current_deadline();
      return buffer_account;
   }

   /// Expire the first optional settled check using the real scoped deadline.
   bool gate_timeout = false;
   /// Permit upload after the optional settled check fails.
   bool allow_upload = false;

   /// Exercise the real gate reader and decoder, asserting the ordered RPC arguments.
   std::vector<std::optional<account_info>> read_chunk_reap_accounts(
      const std::vector<solana_public_key>& accounts, commitment_t commitment,
      const std::optional<data_slice>& slice) override {
      ++gate_reads;
      BOOST_REQUIRE(slice);
      BOOST_CHECK_EQUAL(slice->offset, 0u);
      BOOST_CHECK_EQUAL(slice->length, reap_gate_fixture::header_bytes);
      if (use_rpc_reads) return outpost_solana_client::read_chunk_reap_accounts(accounts, commitment, slice);
      const auto expected = resume_program_client();
      BOOST_REQUIRE_EQUAL(accounts.size(), reap_gate_fixture::account_count);
      BOOST_CHECK(accounts[0] == expected.config_pda);
      BOOST_CHECK(accounts[1] == expected.latest_outbound_envelope_pda);
      BOOST_CHECK(commitment == commitment_t::confirmed);
      if (gate_timeout && gate_reads == 1) {
         const auto deadline = fc::task::current_deadline();
         BOOST_REQUIRE(deadline);
         BOOST_CHECK(*deadline <= fc::time_point::now() + reap_detail::ENVELOPE_CHUNK_SETTLED_CHECK_BUDGET);
         std::this_thread::sleep_for(std::chrono::microseconds(
            std::max(int64_t{0}, (*deadline - fc::time_point::now()).count())));
         FC_THROW_EXCEPTION(fc::timeout_exception, chunk_rpc_fixture::timeout_error);
      }
      return reap_gate_accounts();
   }
   /// Exercise the real discovery reader and decoder, asserting every RPC option before returning headers.
   fc::variant read_chunk_program_accounts(
      const solana_public_key& program, const std::vector<fc::variant>& filters,
      commitment_t commitment, const std::optional<data_slice>& slice) override {
      ++discovery_reads;
      discovery_deadline = fc::task::current_deadline();
      BOOST_CHECK(program == resume_program_id);
      BOOST_CHECK(commitment == commitment_t::confirmed);
      BOOST_REQUIRE_EQUAL(filters.size(), chunk_rpc_fixture::filters_count);
      const auto& disc = filters[0].get_object()[chunk_rpc_fixture::memcmp_key].get_object();
      BOOST_CHECK_EQUAL(disc[chunk_rpc_fixture::offset_key].as_uint64(), 0u);
      const auto discriminator = idl::compute_account_discriminator(std::string{envelope_chunks_name});
      BOOST_CHECK_EQUAL(disc[chunk_rpc_fixture::bytes_key].as_string(), fc::to_base58(
         reinterpret_cast<const char*>(discriminator.data()), discriminator.size(), fc::yield_function_t{}));
      const auto& owner = filters[1].get_object()[chunk_rpc_fixture::memcmp_key].get_object();
      const auto signer = entry()->client->get_pubkey();
      BOOST_CHECK_EQUAL(owner[chunk_rpc_fixture::bytes_key].as_string(), signer.to_string(fc::yield_function_t{}));
      const auto header = sliced_header(reap_scenario::buffer_epoch, signer);
      const auto offset = owner[chunk_rpc_fixture::offset_key].as_uint64();
      const auto signer_bytes = signer.serialize();
      BOOST_REQUIRE(offset + signer_bytes.size() <= header.size());
      BOOST_CHECK(std::equal(signer_bytes.begin(), signer_bytes.end(), header.begin() + offset));
      BOOST_REQUIRE(slice);
      BOOST_CHECK_EQUAL(slice->offset, 0u);
      BOOST_CHECK_EQUAL(slice->length, header.size());
      fc::variants result;
      for (const auto& buffer : buffers)
         result.push_back(program_accounts_entry(buffer.address, sliced_header(buffer.epoch_index, signer),
                                                  buffer.lamports));
      return fc::variant(result);
   }
   /// Supply scripted bytes, or exercise the real finalized read for the idle timing regression.
   std::vector<char> read_latest_outbound_envelope(uint32_t epoch, fc::time_point deadline) override {
      if (use_rpc_reads) return outpost_solana_client::read_latest_outbound_envelope(epoch, deadline);
      return inbound;
   }
   /// Fail before any upload if settled delivery accidentally continues into resume.
   uint16_t read_first_unstaged_chunk(uint32_t, uint16_t, uint32_t) override {
      ++resume_reads;
      if (allow_upload) return 0;
      throw std::runtime_error("settled delivery must not resume an upload");
   }
};

/// Script one JSON-RPC result, preserving the client's sequential request IDs.
fc::test::capture_http_server::scripted_response chunk_rpc_reply(std::string_view result, uint32_t id) {
   fc::test::capture_http_server::scripted_response reply;
   reply.body = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":)" + std::string(result) + "}";
   return reply;
}

/// Three replies for a real signed transaction: blockhash, send and confirmed status.
std::vector<fc::test::capture_http_server::scripted_response> chunk_transaction_replies(uint32_t first_id) {
   constexpr std::string_view blockhash =
      R"({"context":{"slot":1},"value":{"blockhash":"11111111111111111111111111111111","lastValidBlockHeight":100}})";
   const auto signature = fc::json::to_string(fc::variant(chunk_rpc_fixture::signature), fc::json::yield_function_t{});
   constexpr std::string_view status =
      R"({"context":{"slot":1},"value":[{"slot":1,"confirmations":1,"err":null,"confirmationStatus":"confirmed"}]})";
   return {chunk_rpc_reply(blockhash, first_id), chunk_rpc_reply(signature, first_id + 1),
           chunk_rpc_reply(status, first_id + 2)};
}

} // anonymous namespace

/// Discovery offsets must round-trip through the real fixture, and drift must fail during construction.
BOOST_FIXTURE_TEST_CASE(chunk_boot_checks_discovery_offsets, chunk_member_fixture) try {
   BOOST_CHECK_NO_THROW(chunk_member_client{entry});
   for (const auto* moved_field : {envelope_chunks_epoch_field, envelope_chunks_operator_field}) {
      auto program = load_idl_fixture(opp_outpost_idl_fixture);
      auto type = std::ranges::find(program.types, std::string{envelope_chunks_name}, &idl::type_def::name);
      BOOST_REQUIRE(type != program.types.end());
      BOOST_REQUIRE(type->struct_fields);
      auto& fields = *type->struct_fields;
      const auto field = std::ranges::find(fields, std::string{moved_field}, &idl::field::name);
      BOOST_REQUIRE(field != fields.end());
      const auto neighbor = field == fields.begin() ? std::next(field) : std::prev(field);
      std::iter_swap(field, neighbor);
      BOOST_CHECK_EXCEPTION((chunk_member_client{entry, std::move(program)}), fc::exception,
         [](const fc::exception& e) {
            constexpr auto diagnostic = "EnvelopeChunks IDL epoch/operator offsets disagree with chunk discovery";
            return e.to_detail_string().find(diagnostic) != std::string::npos;
         });
   }
} FC_LOG_AND_RETHROW();

/// A field inserted before operator must name the discovery account and retain the decoder's failure.
BOOST_FIXTURE_TEST_CASE(chunk_boot_names_oversized_discovery_layout, chunk_member_fixture) try {
   constexpr auto inserted_field = "extra_header_byte";
   constexpr auto diagnostic = "EnvelopeChunks IDL probe cannot be decoded for chunk discovery filter";
   constexpr auto cause = "Borsh decoder: not enough data";
   auto program = load_idl_fixture(opp_outpost_idl_fixture);
   auto type = std::ranges::find(program.types, std::string{envelope_chunks_name}, &idl::type_def::name);
   BOOST_REQUIRE(type != program.types.end());
   BOOST_REQUIRE(type->struct_fields);
   auto& fields = *type->struct_fields;
   const auto field = std::ranges::find(fields, std::string{envelope_chunks_operator_field}, &idl::field::name);
   BOOST_REQUIRE(field != fields.end());
   fields.insert(field, {inserted_field, prim(idl::primitive_type::u8)});
   BOOST_CHECK_EXCEPTION((chunk_member_client{entry, std::move(program)}), fc::exception,
      [](const fc::exception& e) {
         const auto message = e.to_detail_string();
         return message.find(diagnostic) != std::string::npos && message.find(cause) != std::string::npos;
      });
} FC_LOG_AND_RETHROW();

/// Both cursor constants must agree with the real IDL, with an explicit diagnostic on layout drift.
BOOST_FIXTURE_TEST_CASE(chunk_boot_checks_gate_offsets, chunk_member_fixture) try {
   const chunk_member_client valid_client{entry};
   for (size_t i = 0; i < reap_gate_fixture::account_count; ++i) {
      auto program = load_idl_fixture(opp_outpost_idl_fixture);
      auto type = std::ranges::find(program.types, std::string{reap_gate_fixture::account_names[i]},
                                    &idl::type_def::name);
      BOOST_REQUIRE(type != program.types.end());
      BOOST_REQUIRE(type->struct_fields);
      auto& fields = *type->struct_fields;
      const auto field = std::ranges::find(fields, std::string{reap_gate_fixture::cursor_fields[i]}, &idl::field::name);
      BOOST_REQUIRE(field != fields.end());
      std::iter_swap(field, std::prev(field));
      BOOST_CHECK_EXCEPTION((chunk_member_client{entry, std::move(program)}), fc::exception,
         [i](const fc::exception& e) {
            constexpr auto diagnostic = "IDL cursor offset disagrees with chunk reap gates";
            return gate_error_names(e, reap_gate_fixture::account_names[i]) &&
                   e.to_detail_string().find(diagnostic) != std::string::npos;
         });
   }
} FC_LOG_AND_RETHROW();

/// Fixed layouts larger than the boot probe must name the gate account and retain the decoder's failure.
BOOST_FIXTURE_TEST_CASE(chunk_boot_names_oversized_gate_layouts, chunk_member_fixture) try {
   /// Exceed the 1024-byte probe even before accounting for the existing fixed fields.
   constexpr size_t oversized_fixed_bytes = 1025;
   constexpr auto oversized_field = "oversized_fixed_tail";
   constexpr auto diagnostic = "IDL probe cannot be decoded for chunk reap gates";
   constexpr auto cause = "Borsh decoder: not enough data";
   for (size_t i = 0; i < reap_gate_fixture::account_count; ++i) {
      auto program = load_idl_fixture(opp_outpost_idl_fixture);
      auto type = std::ranges::find(program.types, std::string{reap_gate_fixture::account_names[i]},
                                    &idl::type_def::name);
      BOOST_REQUIRE(type != program.types.end());
      BOOST_REQUIRE(type->struct_fields);
      type->struct_fields->push_back(
         {oversized_field, idl::idl_type::make_array(prim(idl::primitive_type::u8), oversized_fixed_bytes)});
      BOOST_CHECK_EXCEPTION((chunk_member_client{entry, std::move(program)}), fc::exception,
         [i](const fc::exception& e) {
            const auto message = e.to_detail_string();
            return gate_error_names(e, reap_gate_fixture::account_names[i]) &&
                   message.find(diagnostic) != std::string::npos && message.find(cause) != std::string::npos;
         });
   }
} FC_LOG_AND_RETHROW();

/// Missing discovery fields fail boot with a useful diagnostic instead of an opaque key lookup failure.
BOOST_FIXTURE_TEST_CASE(chunk_boot_checks_missing_discovery_fields, chunk_member_fixture) try {
   for (const auto* missing : {envelope_chunks_epoch_field, envelope_chunks_operator_field}) {
      auto program = load_idl_fixture(opp_outpost_idl_fixture);
      auto type = std::ranges::find(program.types, std::string{envelope_chunks_name}, &idl::type_def::name);
      BOOST_REQUIRE(type != program.types.end());
      BOOST_REQUIRE(type->struct_fields);
      auto field = std::ranges::find(*type->struct_fields, std::string{missing}, &idl::field::name);
      BOOST_REQUIRE(field != type->struct_fields->end());
      field->name = reap_gate_fixture::renamed_field;
      BOOST_CHECK_EXCEPTION((chunk_member_client{entry, std::move(program)}), fc::exception,
         [](const fc::exception& e) {
            constexpr auto diagnostic = "EnvelopeChunks IDL missing epoch_index/operator fields for chunk discovery";
            return e.to_detail_string().find(diagnostic) != std::string::npos;
         });
   }
} FC_LOG_AND_RETHROW();

/// Finalized latest=E-1 must not delay closing E when one confirmed snapshot shows E settled and emitted.
BOOST_FIXTURE_TEST_CASE(chunk_member_idle_reaps_before_finalized_catches_up, chunk_member_fixture) try {
   using namespace chunk_rpc_fixture;
   constexpr uint32_t epoch = reap_gate_fixture::next_epoch - 1;
   constexpr auto dataslice_key = "dataSlice";
   constexpr auto length_key = "length";
   auto gates = reap_gate_accounts();
   const std::array<uint8_t, reap_gate_fixture::hash_bytes> hash{};
   gates[1] = program_owned(integrated_latest_account(epoch, {}, hash));
   const auto lagging = program_owned(integrated_latest_account(epoch - 1, {}, hash));
   const auto account_json = [&](const account_info& info) {
      fc::mutable_variant_object account(program_accounts_entry(
         resume_program_id, info.data, info.lamports).get_object()[account_key].get_object());
      account(rent_epoch_key, 0);
      return fc::variant(account);
   };
   const auto latest_result = fc::json::to_string(
      fc::mutable_variant_object()(context_key, fc::mutable_variant_object()(slot_key, 1))(
         value_key, account_json(*lagging)), fc::json::yield_function_t{});
   for (auto& gate : gates) gate->data.resize(std::min(gate->data.size(), reap_gate_fixture::header_bytes));
   const auto gate_result = fc::json::to_string(
      fc::mutable_variant_object()(context_key, fc::mutable_variant_object()(slot_key, 2))(
         value_key, fc::variants{account_json(*gates[0]), account_json(*gates[1])}), fc::json::yield_function_t{});
   auto replies = std::vector{chunk_rpc_reply(latest_result, 1), chunk_rpc_reply(gate_result, 2)};
   const auto cleanup = chunk_transaction_replies(3);
   replies.insert(replies.end(), cleanup.begin(), cleanup.end());
   fc::test::capture_http_server server(std::move(replies));
   entry->client = std::make_shared<solana_client>(entry->signature_provider, server.url());
   chunk_member_client client(entry);
   client.use_rpc_reads = true;
   client.buffers = {scripted_buffer(epoch, entry->client->get_pubkey())};
   BOOST_CHECK(client.read_inbound_envelope(epoch, SWEEP_TEST_DEADLINE).empty());
   BOOST_CHECK_EQUAL(client.discovery_reads, 1u);
   BOOST_REQUIRE_EQUAL(server.request_count(), 2u + requests_per_transaction);
   const auto latest = fc::json::from_string(server.request(0).body).get_object();
   const auto gate = fc::json::from_string(server.request(1).body).get_object();
   BOOST_CHECK_EQUAL(latest[method_key].as_string(), account_method);
   BOOST_CHECK_EQUAL(latest[params_key].get_array()[1][commitment_key].as_string(), finalized);
   BOOST_CHECK_EQUAL(gate[method_key].as_string(), multiple_accounts_method);
   const auto& params = gate[params_key].get_array();
   BOOST_REQUIRE_EQUAL(params[0].get_array().size(), reap_gate_fixture::account_count);
   const auto program = resume_program_client();
   BOOST_CHECK_EQUAL(params[0].get_array()[0].as_string(), program.config_pda.to_string(fc::yield_function_t{}));
   BOOST_CHECK_EQUAL(params[0].get_array()[1].as_string(),
                     program.latest_outbound_envelope_pda.to_string(fc::yield_function_t{}));
   BOOST_CHECK_EQUAL(params[1][commitment_key].as_string(), confirmed);
   const auto& slice = params[1][dataslice_key].get_object();
   BOOST_CHECK_EQUAL(slice[offset_key].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(slice[length_key].as_uint64(), reap_gate_fixture::header_bytes);
   const auto sent = fc::json::from_string(server.request(3).body).get_object();
   BOOST_CHECK_EQUAL(sent[method_key].as_string(), send_method);
   // The complete sweep watermarks this confirmed cursor; another idle tick needs no discovery or close.
   client.use_rpc_reads = false;
   client.reap_settled_envelope_chunks(fc::time_point::now() + SWEEP_TEST_DEADLINE);
   BOOST_CHECK_EQUAL(client.gate_reads, 2u);
   BOOST_CHECK_EQUAL(client.discovery_reads, 1u);
} FC_LOG_AND_RETHROW();

/// Observe the commitment reaching the real send, so removing options at the binding fails this test.
BOOST_FIXTURE_TEST_CASE(cleanup_binding_sends_confirmed_preflight, chunk_member_fixture) try {
   const auto& retry = reap_detail::ENVELOPE_CHUNK_CLEANUP_CONFIRM_OPTIONS.retry;
   const auto& expected = fc::task::retry_option_defaults;
   BOOST_CHECK(retry.initial_backoff == expected.initial_backoff);
   BOOST_CHECK(retry.max_backoff == expected.max_backoff);
   BOOST_CHECK(retry.total_timeout == expected.total_timeout);
   BOOST_CHECK_EQUAL(retry.growth_factor, expected.growth_factor);
   fc::test::capture_http_server server(chunk_transaction_replies(1));
   entry->client = std::make_shared<solana_client>(entry->signature_provider, server.url());
   sysio::opp_solana_outpost_client program(entry->client, resume_program_id,
                                             {load_idl_fixture(opp_outpost_idl_fixture)});
   BOOST_CHECK_EQUAL(program.cleanup_envelope_chunks(reap_scenario::buffer_epoch), chunk_rpc_fixture::signature);
   BOOST_REQUIRE_EQUAL(server.request_count(), chunk_rpc_fixture::requests_per_transaction);
   const auto request = fc::json::from_string(server.request(1).body).get_object();
   BOOST_CHECK_EQUAL(request[chunk_rpc_fixture::method_key].as_string(), chunk_rpc_fixture::send_method);
   const auto& config = request[chunk_rpc_fixture::params_key].get_array()[1].get_object();
   BOOST_CHECK_EQUAL(config[chunk_rpc_fixture::preflight_key].as_string(), chunk_rpc_fixture::confirmed);
   BOOST_CHECK(!config[chunk_rpc_fixture::skip_preflight_key].as_bool());
} FC_LOG_AND_RETHROW();

/// A timed-out optional gate read restores the delivery scope and still sends the data and terminal transactions.
BOOST_FIXTURE_TEST_CASE(chunk_member_settled_check_timeout_still_uploads, chunk_member_fixture) try {
   auto replies = chunk_transaction_replies(1);
   const auto terminal = chunk_transaction_replies(1 + chunk_rpc_fixture::requests_per_transaction);
   replies.insert(replies.end(), terminal.begin(), terminal.end());
   fc::test::capture_http_server server(std::move(replies));
   entry->client = std::make_shared<solana_client>(entry->signature_provider, server.url());
   chunk_member_client client(entry);
   client.gate_timeout = true;
   client.allow_upload = true;
   constexpr auto delivery_budget = fc::seconds(8);
   BOOST_CHECK_EQUAL(client.deliver_outbound_envelope(reap_scenario::buffer_epoch,
                                                       {chunk_rpc_fixture::payload}, delivery_budget),
                     chunk_rpc_fixture::signature);
   BOOST_CHECK_EQUAL(client.resume_reads, 1u);
   BOOST_CHECK_EQUAL(client.gate_reads, 2u);
   BOOST_CHECK_EQUAL(client.discovery_reads, 1u);
   BOOST_CHECK_EQUAL(server.request_count(), 2 * chunk_rpc_fixture::requests_per_transaction);
   BOOST_CHECK(!fc::task::current_deadline());
} FC_LOG_AND_RETHROW();

/// Both direct post-drain glue and settled delivery cap cleanup despite a much longer caller deadline.
BOOST_FIXTURE_TEST_CASE(chunk_member_delivery_sweeps_use_short_budget, chunk_member_fixture) try {
   for (const bool settled_delivery : {false, true}) {
      chunk_member_client client(entry);
      const auto start = fc::time_point::now();
      if (settled_delivery)
         client.deliver_outbound_envelope(reap_scenario::buffer_epoch,
                                           {chunk_rpc_fixture::payload}, SWEEP_TEST_DEADLINE);
      else
         client.reap_settled_envelope_chunks(start + SWEEP_TEST_DEADLINE);
      BOOST_REQUIRE(client.discovery_deadline);
      BOOST_CHECK(*client.discovery_deadline >= start + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET);
      BOOST_CHECK(*client.discovery_deadline <=
                  start + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET + SWEEP_ENTRY_TOLERANCE);
   }
} FC_LOG_AND_RETHROW();

/// Unsorted discovery results must become oldest-first before the production walk sees them.
BOOST_FIXTURE_TEST_CASE(chunk_member_discovery_orders_buffers, chunk_member_fixture) try {
   /// Expose production readers while retaining the RPC-only overrides.
   struct discovery_client : chunk_member_client {
      using chunk_member_client::chunk_member_client;
      using sysio::outpost_solana_client::read_own_envelope_chunk_buffers;
      using sysio::outpost_solana_client::read_envelope_chunk_reap_gates;
   } client(entry);
   constexpr uint32_t older_epoch = 2;
   constexpr uint32_t newer_epoch = 6;
   const auto signer = entry->client->get_pubkey();
   client.buffers = {scripted_buffer(newer_epoch, signer), scripted_buffer(older_epoch, signer)};
   const auto result = client.read_own_envelope_chunk_buffers();
   BOOST_REQUIRE_EQUAL(result.size(), 2u);
   BOOST_CHECK_EQUAL(result[0].epoch_index, older_epoch);
   BOOST_CHECK_EQUAL(result[1].epoch_index, newer_epoch);
   const auto gates = client.read_envelope_chunk_reap_gates();
   BOOST_CHECK_EQUAL(gates.next_epoch_index, reap_gate_fixture::next_epoch);
   BOOST_CHECK_EQUAL(gates.latest_outbound_epoch, reap_gate_fixture::latest_epoch);
} FC_LOG_AND_RETHROW();

/// The production callback chooses processed and reads only the exact failed buffer within the RPC deadline.
BOOST_FIXTURE_TEST_CASE(chunk_member_absence_read_uses_processed, chunk_member_fixture) try {
   for (const bool present : {false, true}) {
      chunk_member_client client(entry, cleanup_argument_failure_idl());
      client.buffers = {scripted_buffer(reap_scenario::buffer_epoch, entry->client->get_pubkey())};
      if (present) client.buffer_account = account_info{}; // Empty data is still a present account.
      const auto deadline = fc::time_point::now() + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
      client.reap_settled_envelope_chunks(deadline);
      BOOST_REQUIRE_EQUAL(client.absence_commitments.size(), 1u);
      BOOST_CHECK(client.absence_commitments.front() == commitment_t::processed);
      BOOST_REQUIRE_EQUAL(client.absence_addresses.size(), 1u);
      BOOST_CHECK(client.absence_addresses.front() == client.buffers.front().address);
      BOOST_REQUIRE(client.absence_deadline);
      BOOST_CHECK(*client.absence_deadline <= deadline);
      BOOST_CHECK(!fc::task::current_deadline());
      client.reap_settled_envelope_chunks(deadline);
      // Absence completed the cursor; presence backed off. Neither repeats discovery at the same cursor.
      BOOST_CHECK_EQUAL(client.discovery_reads, 1u);
      BOOST_CHECK_EQUAL(client.gate_reads, present ? 1u : 2u);
   }
} FC_LOG_AND_RETHROW();

BOOST_FIXTURE_TEST_CASE(chunk_member_reaper_installs_rpc_deadline, chunk_member_fixture) try {
   chunk_member_client client(entry);
   const auto deadline = fc::time_point::now() + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
   BOOST_CHECK(!fc::task::current_deadline());
   client.reap_settled_envelope_chunks(deadline);
   BOOST_REQUIRE_EQUAL(client.discovery_reads, 1u);
   BOOST_REQUIRE(client.discovery_deadline);
   BOOST_CHECK(*client.discovery_deadline <= deadline);
   BOOST_CHECK(!fc::task::current_deadline());
} FC_LOG_AND_RETHROW();

BOOST_FIXTURE_TEST_CASE(chunk_member_reaper_clamps_to_outer_deadline, chunk_member_fixture) try {
   chunk_member_client client(entry);
   const auto later = fc::time_point::now() + SWEEP_TEST_DEADLINE;
   {
      // Already spent outer budget: even a later explicit argument must cause no RPC.
      fc::task::deadline_scope outer(fc::time_point::now() - fc::seconds(1));
      client.reap_settled_envelope_chunks(later);
      BOOST_CHECK_EQUAL(client.gate_reads, 0u);
      BOOST_CHECK_EQUAL(client.discovery_reads, 0u);
   }
   // A single spent call must leave the next call due, with the outer scope restored.
   client.reap_settled_envelope_chunks(later);
   BOOST_CHECK_EQUAL(client.discovery_reads, 1u);
} FC_LOG_AND_RETHROW();

/// Idle reads receive the same full sweep budget as delivery paths.
BOOST_FIXTURE_TEST_CASE(chunk_member_idle_read_uses_short_budget, chunk_member_fixture) try {
   chunk_member_client client(entry);
   const auto start = fc::time_point::now();
   BOOST_CHECK(client.read_inbound_envelope(7, SWEEP_TEST_DEADLINE).empty());
   BOOST_REQUIRE_EQUAL(client.discovery_reads, 1u);
   BOOST_REQUIRE(client.discovery_deadline);
   BOOST_CHECK(*client.discovery_deadline >= start + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET);
   BOOST_CHECK(*client.discovery_deadline <= start + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET + SWEEP_ENTRY_TOLERANCE);
   BOOST_CHECK(!fc::task::current_deadline());

   // The real member must return a found envelope without another sweep.
   client.inbound = {'e'};
   BOOST_CHECK(client.read_inbound_envelope(7, SWEEP_TEST_DEADLINE) == client.inbound);
   BOOST_CHECK_EQUAL(client.gate_reads, 1u);
} FC_LOG_AND_RETHROW();

BOOST_FIXTURE_TEST_CASE(chunk_member_settled_delivery_returns_before_upload, chunk_member_fixture) try {
   chunk_member_client client(entry);
   BOOST_CHECK(client.deliver_outbound_envelope(7, {'e'}, SWEEP_TEST_DEADLINE).empty());
   BOOST_CHECK_EQUAL(client.gate_reads, 1u);
   BOOST_CHECK_EQUAL(client.discovery_reads, 1u);
   BOOST_CHECK_EQUAL(client.resume_reads, 0u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_repeated_deadline_stops_back_off) try {
   for (const bool throws : {true, false}) {
      BOOST_TEST_CONTEXT("discovery throws=" << throws) {
         sweep_harness harness;
         harness.buffers = {scripted_buffer(2, measurement_pubkey(7))};
         reap_detail::envelope_chunk_sweep_schedule schedule;
         for (uint32_t i = 0; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i) {
            BOOST_TEST_CONTEXT("deadline stop=" << i) {
               const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
               harness.on_read_buffers = [&, deadline] {
                  harness.clock = deadline;
                  if (throws) FC_THROW_EXCEPTION(fc::timeout_exception, "discovery deadline expired");
               };
               BOOST_CHECK(harness.sweep(schedule, deadline) ==
                           (throws ? envelope_chunk_sweep_outcome::cut_short : envelope_chunk_sweep_outcome::swept));
               BOOST_CHECK_EQUAL(schedule.backing_off(harness.clock),
                                 i + 1 == reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT);
            }
         }
         const auto reads = harness.buffer_reads;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET) ==
                     envelope_chunk_sweep_outcome::backing_off);
         BOOST_CHECK_EQUAL(harness.buffer_reads, reads);
         BOOST_CHECK(harness.closes.sent.empty());
         harness.clock = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
         const auto retry_deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
         harness.on_read_buffers = [&, retry_deadline] {
            harness.clock = retry_deadline;
            if (throws) FC_THROW_EXCEPTION(fc::timeout_exception, "retry discovery deadline expired");
         };
         // The expired back-off starts a fresh streak, so its first idle stop cannot trip again.
         BOOST_CHECK(harness.sweep(schedule, retry_deadline) ==
                     (throws ? envelope_chunk_sweep_outcome::cut_short : envelope_chunk_sweep_outcome::swept));
         BOOST_CHECK(!schedule.backing_off(harness.clock));
         BOOST_CHECK(schedule.due(harness.gates.next_epoch_index, harness.clock));
         harness.on_read_buffers = nullptr;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET) ==
                     envelope_chunk_sweep_outcome::swept);
         BOOST_CHECK(harness.closes.sent == (std::vector<uint32_t>{2}));
      }
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(chunk_sweep_success_resets_deadline_stop_streak) try {
   for (const bool partial_progress : {false, true}) {
      BOOST_TEST_CONTEXT("partial progress=" << partial_progress) {
         sweep_harness harness;
         reap_detail::envelope_chunk_sweep_schedule schedule;
         for (uint32_t cycle = 0; cycle < 2; ++cycle) {
            BOOST_TEST_CONTEXT("cycle=" << cycle) {
               for (uint32_t i = 1; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i) {
                  BOOST_TEST_CONTEXT("deadline stop=" << i) {
                     const auto deadline = harness.clock + SWEEP_TEST_DEADLINE;
                     harness.on_read_buffers = [&, deadline] {
                        harness.clock = deadline;
                        FC_THROW_EXCEPTION(fc::timeout_exception, "discovery deadline expired");
                     };
                     BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::cut_short);
                     BOOST_CHECK(!schedule.backing_off(harness.clock));
                  }
               }
               harness.on_read_buffers = nullptr;
               const auto deadline = harness.clock + SWEEP_TEST_DEADLINE;
               if (partial_progress) {
                  // A successful close resets the streak even with a buffer still deferred at the deadline.
                  harness.buffers = {scripted_buffer(2, measurement_pubkey(7)),
                                     scripted_buffer(3, measurement_pubkey(7))};
                  harness.after_close = [&, deadline](uint32_t) { harness.clock = deadline; };
               }
               BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::swept);
               BOOST_CHECK(!schedule.backing_off(harness.clock));
               ++harness.gates.next_epoch_index;
            }
         }
      }
   }
} FC_LOG_AND_RETHROW();

/// Delivery sweeps observe the same deadline-stop back-off, including with caller-supplied cursors.
BOOST_AUTO_TEST_CASE(chunk_sweep_delivery_observes_deadline_stop_backoff) try {
   for (const bool known_gates : {false, true}) {
      BOOST_TEST_CONTEXT("caller supplied gates=" << known_gates) {
         sweep_harness harness;
         harness.buffers = {scripted_buffer(2, measurement_pubkey(7))};
         reap_detail::envelope_chunk_sweep_schedule schedule;
         for (uint32_t i = 0; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i) {
            BOOST_TEST_CONTEXT("idle stop=" << i) {
               const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
               harness.on_read_buffers = [&, deadline] {
                  harness.clock = deadline;
                  FC_THROW_EXCEPTION(fc::timeout_exception, "discovery deadline expired");
               };
               BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::cut_short);
            }
         }
         harness.on_read_buffers = nullptr;
         const auto reads = harness.gate_reads;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET) ==
                     envelope_chunk_sweep_outcome::backing_off);
         BOOST_CHECK_EQUAL(harness.gate_reads, reads);
         const auto gates = known_gates ? std::optional{harness.gates} : std::nullopt;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE, gates) ==
                     envelope_chunk_sweep_outcome::backing_off);
         BOOST_CHECK(harness.closes.sent.empty());
         BOOST_CHECK_EQUAL(harness.gate_reads, reads);
         // Genuine failure back-off also applies to every delivery sweep.
         schedule.record_failure(harness.clock);
         BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE, gates) ==
                     envelope_chunk_sweep_outcome::backing_off);
      }
   }
} FC_LOG_AND_RETHROW();

/// Once back-off expires, completion or a close with work deferred clears the deadline-stop state.
BOOST_AUTO_TEST_CASE(chunk_sweep_progress_resets_streak_after_backoff_expires) try {
   for (const bool partial_progress : {false, true}) {
      BOOST_TEST_CONTEXT("partial progress=" << partial_progress) {
         sweep_harness harness;
         reap_detail::envelope_chunk_sweep_schedule schedule;
         for (uint32_t i = 0; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i)
            schedule.record_deadline_stop(harness.clock);
         BOOST_REQUIRE(schedule.backing_off(harness.clock));
         harness.clock += reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
         const auto deadline = harness.clock + SWEEP_TEST_DEADLINE;
         if (partial_progress) {
            harness.buffers = {scripted_buffer(2, measurement_pubkey(7)), scripted_buffer(3, measurement_pubkey(7))};
            harness.after_close = [&, deadline](uint32_t) { harness.clock = deadline; };
         }
         BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::swept);
         BOOST_CHECK_EQUAL(harness.closes.sent.size(), partial_progress ? 1u : 0u);
         BOOST_CHECK(!schedule.backing_off(harness.clock));
         BOOST_CHECK_EQUAL(schedule.due(harness.gates.next_epoch_index, harness.clock), partial_progress);
         // A fresh idle call resumes deferred work or observes the completed walk's watermark immediately.
         harness.after_close = nullptr;
         BOOST_CHECK(harness.sweep(schedule, harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET) ==
                     (partial_progress ? envelope_chunk_sweep_outcome::swept : envelope_chunk_sweep_outcome::not_due));
      }
   }
} FC_LOG_AND_RETHROW();

namespace {

/// Calls between deadline stops without completing a discovery walk or closing a buffer.
enum class chunk_sweep_interruption { not_due, deadline_spent, failure };

} // anonymous namespace

/// Not-due gates and failures reset the streak; a spent deadline neither resets nor increments it.
BOOST_AUTO_TEST_CASE(chunk_sweep_interruptions_update_deadline_stop_streak) try {
   for (const auto interruption : {chunk_sweep_interruption::not_due, chunk_sweep_interruption::deadline_spent,
                                   chunk_sweep_interruption::failure}) {
      BOOST_TEST_CONTEXT("interruption=" << magic_enum::enum_name(interruption)) {
         sweep_harness harness;
         reap_detail::envelope_chunk_sweep_schedule schedule;
         schedule.record(harness.gates.next_epoch_index, {}, harness.clock);
         for (uint32_t i = 1; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i) {
            BOOST_TEST_CONTEXT("gate stop=" << i) {
               const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
               harness.on_read_gates = [&, deadline] {
                  harness.clock = deadline;
                  FC_THROW_EXCEPTION(fc::timeout_exception, "gate deadline expired");
               };
               BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::cut_short);
               BOOST_CHECK(!schedule.backing_off(harness.clock));
            }
         }
         harness.on_read_gates = nullptr;
         if (interruption == chunk_sweep_interruption::not_due) {
            BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                        envelope_chunk_sweep_outcome::not_due);
         } else if (interruption == chunk_sweep_interruption::deadline_spent) {
            BOOST_CHECK(harness.sweep(schedule, harness.clock) == envelope_chunk_sweep_outcome::deadline_spent);
         } else {
            harness.on_read_gates = [] { FC_THROW("gate unavailable before deadline"); };
            BOOST_CHECK(harness.sweep(schedule, harness.clock + SWEEP_TEST_DEADLINE) ==
                        envelope_chunk_sweep_outcome::failed);
            harness.clock += reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
         }
         BOOST_CHECK(!schedule.backing_off(harness.clock));
         ++harness.gates.next_epoch_index;
         const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
         harness.on_read_gates = [&, deadline] {
            harness.clock = deadline;
            FC_THROW_EXCEPTION(fc::timeout_exception, "later gate deadline expired");
         };
         BOOST_CHECK(harness.sweep(schedule, deadline) == envelope_chunk_sweep_outcome::cut_short);
         BOOST_CHECK_EQUAL(schedule.backing_off(harness.clock),
                           interruption == chunk_sweep_interruption::deadline_spent);
         BOOST_CHECK_EQUAL(harness.buffer_reads, 0u);
      }
   }
} FC_LOG_AND_RETHROW();

/// Both thrown discovery reads and returned-but-expired walks warn exactly when deadline-stop back-off begins.
BOOST_AUTO_TEST_CASE(chunk_sweep_deadline_stop_backoff_logs_warning) try {
   for (const bool throws : {false, true}) {
      BOOST_TEST_CONTEXT("discovery throws=" << throws) {
         chunk_sweep_warning_capture capture;
         sweep_harness harness;
         harness.buffers = {scripted_buffer(2, measurement_pubkey(7))};
         reap_detail::envelope_chunk_sweep_schedule schedule;
         for (uint32_t i = 0; i < reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT; ++i) {
            BOOST_TEST_CONTEXT("deadline stop=" << i) {
               capture.output.str("");
               const auto deadline = harness.clock + reap_detail::ENVELOPE_CHUNK_SWEEP_BUDGET;
               harness.on_read_buffers = [&, deadline] {
                  harness.clock = deadline;
                  if (throws) FC_THROW_EXCEPTION(fc::timeout_exception, "discovery deadline expired");
               };
               BOOST_CHECK(harness.sweep(schedule, deadline) ==
                           (throws ? envelope_chunk_sweep_outcome::cut_short : envelope_chunk_sweep_outcome::swept));
            }
         }
         const auto warning = capture.output.str();
         BOOST_CHECK(warning.find("outpost_solana_client[test-relay]:") != std::string::npos);
         BOOST_CHECK(warning.find(fmt::format("{} consecutive deadline stops",
                                             reap_detail::ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT)) !=
                     std::string::npos);
         BOOST_CHECK(warning.find("sweeps retry after") != std::string::npos);
         BOOST_CHECK(warning.find(fmt::format("retry after {} s",
                                             reap_detail::ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF.to_seconds())) !=
                     std::string::npos);
         BOOST_CHECK_EQUAL(std::count(warning.begin(), warning.end(), '\n'), 2);
      }
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
