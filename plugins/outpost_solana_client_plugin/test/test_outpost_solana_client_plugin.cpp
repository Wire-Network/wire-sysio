#include <boost/test/unit_test.hpp>

#include <fc-test/build_info.hpp>
#include <fc-test/crypto_utils.hpp>
#include <fc/crypto/base64.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/io/json.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_borsh.hpp>

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

/// Borsh-encode a data-chunk `epoch_in` instruction payload carrying
/// `chunk_data_bytes` of chunk data. Mirrors the client's argument order:
/// `epoch_index, chunk_index, total_chunks, total_bytes, chunk_data,
/// dispatch_limit`.
std::vector<uint8_t> epoch_in_data_chunk_payload(const idl::instruction& instr,
                                                 size_t chunk_data_bytes) {
   std::vector<uint8_t> data;
   data.reserve(instr.discriminator.size() + 20 + chunk_data_bytes);
   data.insert(data.end(), instr.discriminator.begin(), instr.discriminator.end());
   write_u32_le(data, 1);                                          // epoch_index
   write_u16_le(data, 0);                                          // chunk_index
   write_u16_le(data, 2);                                          // total_chunks
   write_u32_le(data, static_cast<uint32_t>(chunk_data_bytes * 2)); // total_bytes
   write_u32_le(data, static_cast<uint32_t>(chunk_data_bytes));     // chunk_data length prefix
   data.resize(data.size() + chunk_data_bytes);
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
      sysio::solana_outpost_role::underwriter);

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

/// A decoded `Reserve` account object as libfc's IDL decoder renders it:
/// pubkeys as base58 strings, `custody_decimals` as an integer.
fc::variant_object reserve_row(const solana_public_key& creator,
                               const solana_public_key& custody_mint,
                               unsigned                 custody_decimals) {
   return fc::mutable_variant_object("creator", creator.to_string(fc::yield_function_t{}))(
      "custody_mint", custody_mint.to_string(fc::yield_function_t{}))(
      "custody_decimals", custody_decimals);
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(reserve_info_reads_custody_from_the_reserve_account) try {
   using sysio::outpost_solana_client_detail::reserve_info_from_account;

   const auto creator       = measurement_pubkey(76);
   const auto spl_mint      = measurement_pubkey(77);
   const auto native_marker = system::program_ids::SYSTEM_PROGRAM;

   // SPL custody: the mint and decimals PINNED on the reserve at creation --
   // the same two values `handle_swap_remit_spl` / `handle_swap_revert_spl`
   // are handed off `reserve.custody_mint` / `reserve.custody_decimals`.
   const auto spl = reserve_info_from_account(reserve_row(creator, spl_mint, 6));
   BOOST_CHECK(spl.creator == creator);
   BOOST_CHECK(spl.custody_mint == spl_mint);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(spl.custody_decimals), 6u);

   // Native custody is the all-zero `NATIVE_TOKEN_MARKER`, which is exactly
   // what the handler compares against to take the lamport branch.
   const auto native = reserve_info_from_account(reserve_row(creator, native_marker, 9));
   BOOST_CHECK(native.custody_mint == native_marker);
   BOOST_CHECK_EQUAL(static_cast<unsigned>(native.custody_decimals), 9u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(reserve_info_requires_every_custody_field) try {
   using sysio::outpost_solana_client_detail::reserve_info_from_account;

   const auto creator  = measurement_pubkey(78);
   const auto spl_mint = measurement_pubkey(79);

   // A record missing ANY of the three is not one this relay can build an
   // account-consistent manifest from. Defaulting custody_mint would silently
   // mean "native" -- the divergence that makes the on-chain call abort.
   BOOST_CHECK_THROW(
      reserve_info_from_account(fc::mutable_variant_object(
         "custody_mint", spl_mint.to_string(fc::yield_function_t{}))("custody_decimals", 6)),
      fc::assert_exception);
   BOOST_CHECK_THROW(
      reserve_info_from_account(fc::mutable_variant_object(
         "creator", creator.to_string(fc::yield_function_t{}))("custody_decimals", 6)),
      fc::assert_exception);
   BOOST_CHECK_THROW(
      reserve_info_from_account(fc::mutable_variant_object(
         "creator", creator.to_string(fc::yield_function_t{}))(
         "custody_mint", spl_mint.to_string(fc::yield_function_t{}))),
      fc::assert_exception);
   BOOST_CHECK_THROW(reserve_info_from_account(fc::variant_object{fc::mutable_variant_object{}}),
                     fc::assert_exception);
   // Out-of-byte-range decimals: a decoded row disagreeing with the on-chain
   // u8 must be refused, not truncated.
   BOOST_CHECK_THROW(reserve_info_from_account(reserve_row(creator, spl_mint, 256)),
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
   BOOST_CHECK_EQUAL(dispatch->accounts.size(), 11u);
   BOOST_CHECK(dispatch->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(dispatch->accounts[4].name, "chunk_buffer");

   // Accounts: operator (signer), config, operator_registry, epoch_deliveries,
   //           chunk_buffer, inbound_envelopes, system_program. No outbound-emit
   //           accounts here -- epoch_in only stages/finalizes inbound chunks and
   //           processes the enclosed attestations inline on consensus reach;
   //           dispatch_attestations (asserted above) owns the outbound emit.
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
   BOOST_REQUIRE_EQUAL(dispatch->accounts.size(), 11u);
   BOOST_CHECK_EQUAL(dispatch->accounts[0].name, "caller");
   BOOST_CHECK(dispatch->accounts[0].is_signer);
   BOOST_CHECK_EQUAL(dispatch->accounts[3].name, "epoch_deliveries");
   BOOST_CHECK_EQUAL(dispatch->accounts[4].name, "chunk_buffer");
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
   std::vector<instruction> instructions = {
      instruction{measurement_pubkey(3),
                  terminal_static_accounts(*epoch_in, fee_payer),
                  epoch_in_data_chunk_payload(*epoch_in, sysio::SOLANA_MAX_CHUNK_BYTES)},
   };

   const auto packet = build_measured_legacy_transaction(instructions, fee_payer).serialize();
   BOOST_CHECK_LE(packet.size(), limits::PACKET_DATA_SIZE);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(dispatch_attestations_full_manifest_fits_packet_limit) try {
   // MAX_TERMINAL_DYNAMIC_ACCOUNTS is a BUDGET, and a budget nobody measures is
   // a guess. It counts only the `remaining_accounts` extras -- not the
   // instruction's ~11 IDL-declared accounts, not the fee payer, not the
   // compute-budget pre-instruction the relay injects on this call. This test
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
   auto accounts = terminal_static_accounts(*dispatch, fee_payer);

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

   // The relay injects a heap-frame request on this call; it occupies packet
   // budget exactly like any other instruction, so the measurement includes it.
   std::vector<instruction> instructions = {
      system::compute_budget::request_heap_frame(sysio::SOLANA_DISPATCH_HEAP_FRAME_BYTES),
      instruction{measurement_pubkey(3), accounts, data},
   };

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

BOOST_AUTO_TEST_CASE(opp_outpost_has_initialize_and_deposit) try {
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

/// Build an `OPERATOR_ACTION(WITHDRAW_REMIT)` entry pointing at
/// `op_addr`, remitting `token_code`. The decoder reads `op_address`,
/// `action_type` and `amount.token_code` (SOL-379/380 keys the
/// CollateralPosition PDA on it); other proto fields stay neutral.
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

/// Same as `remit_entry` but for SLASH — surfaced with its own shape so the
/// manifest can declare the slashed operator's CollateralPosition PDA (and
/// the reserve_aggregate ATA under SPL custody).
sysio::opp::AttestationEntry slash_entry(uint64_t                               token_code,
                                         const sysio::opp::types::ChainAddress& op_addr) {
   sysio::opp::attestations::OperatorAction oa;
   oa.set_action_type(sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_SLASH);
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

/// Build a `DEPOSIT_REVERT` entry pointing at `depositor_addr`, refunding
/// `token_code` (keys the CollateralPosition PDA).
sysio::opp::AttestationEntry revert_entry(uint64_t                               token_code,
                                          const sysio::opp::types::ChainAddress& depositor_addr) {
   sysio::opp::attestations::DepositRevert dr;
   dr.mutable_refund_amount()->set_token_code(token_code);
   dr.mutable_refund_amount()->set_amount(888);
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

BOOST_AUTO_TEST_CASE(extract_effects_empty_envelope_returns_empty) try {
   std::vector<char> envelope = envelope_with_entries({});
   auto effects = sysio::outpost_solana_client_detail::extract_inbound_effects(envelope);
   BOOST_CHECK(effects.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_single_withdraw_remit) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto op_pk = filled_pubkey(0xAA);
   auto envelope = envelope_with_entries({remit_entry(501, make_sol_addr(op_pk))});

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 1u);
   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::withdraw_remit);
   BOOST_REQUIRE(effects[0].recipient.has_value());
   BOOST_CHECK(effects[0].recipient->serialize() == op_pk);
   BOOST_CHECK(!effects[0].reserve.has_value());
   // SOL-379: the remit amount's token_code keys the CollateralPosition PDA.
   BOOST_REQUIRE(effects[0].collateral_token_code.has_value());
   BOOST_CHECK_EQUAL(*effects[0].collateral_token_code, 501u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_deposit_revert) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto depositor_pk = filled_pubkey(0xBB);
   auto envelope = envelope_with_entries({revert_entry(503, make_sol_addr(depositor_pk))});

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 1u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::deposit_revert);
   BOOST_REQUIRE(effects[0].recipient.has_value());
   BOOST_CHECK(effects[0].recipient->serialize() == depositor_pk);
   // SOL-379: the refund amount's token_code keys the CollateralPosition PDA.
   BOOST_REQUIRE(effects[0].collateral_token_code.has_value());
   BOOST_CHECK_EQUAL(*effects[0].collateral_token_code, 503u);
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
      remit_entry(601, make_sol_addr(remit_op)),
      operators_mirror_entry(),
      revert_entry(602, make_sol_addr(revert_op)),
   });

   BOOST_CHECK_EQUAL(detail::count_inbound_attestations(envelope), 3u);

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 2u);

   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::withdraw_remit);
   BOOST_REQUIRE(effects[0].recipient.has_value());
   BOOST_CHECK(effects[0].recipient->serialize() == remit_op);

   BOOST_CHECK_EQUAL(effects[1].attestation_index, 2u);
   BOOST_CHECK(effects[1].shape == detail::effect_shape::deposit_revert);
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
   // Two identical WITHDRAW_REMITs to the same operator and token_code. The
   // walk is per-attestation with NO cross-attestation dedup -- the cursor
   // counts both, so collapsing them would misalign every later
   // dispatch_limit -- and the duplicate ACCOUNTS (operator wallet,
   // CollateralPosition PDA) merge later in `record_terminal_account` when a
   // batch's manifests union (pinned by
   // record_terminal_account_dedupes_and_merges_writable).
   auto envelope = envelope_with_entries({
      remit_entry(600, make_sol_addr(op_pk)),
      remit_entry(600, make_sol_addr(op_pk)),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 2u);
   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK_EQUAL(effects[1].attestation_index, 1u);
   BOOST_CHECK(effects[0].recipient->serialize() == op_pk);
   BOOST_CHECK(effects[1].recipient->serialize() == op_pk);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_slash_carries_collateral_position_key) try {
   namespace detail = sysio::outpost_solana_client_detail;
   // SLASH MUST be surfaced (SOL-379/380): the handler resolves the slashed
   // operator's per-(operator, token_code) CollateralPosition PDA out of
   // remaining_accounts, and under SPL custody additionally the collateral
   // vault + reserve_aggregate ATA. An omitted manifest entry would abort
   // the dispatch call on-chain and stall the cursor on this attestation.
   auto slash_op = filled_pubkey(0xDD);
   auto remit_op = filled_pubkey(0xEE);
   auto envelope = envelope_with_entries({
      slash_entry(502, make_sol_addr(slash_op)),
      remit_entry(504, make_sol_addr(remit_op)),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 2u);

   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::slash);
   BOOST_REQUIRE(effects[0].recipient.has_value());
   BOOST_CHECK(effects[0].recipient->serialize() == slash_op);
   BOOST_REQUIRE(effects[0].collateral_token_code.has_value());
   BOOST_CHECK_EQUAL(*effects[0].collateral_token_code, 502u);

   BOOST_CHECK_EQUAL(effects[1].attestation_index, 1u);
   BOOST_CHECK(effects[1].shape == detail::effect_shape::withdraw_remit);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_keeps_distinct_collateral_token_codes) try {
   namespace detail = sysio::outpost_solana_client_detail;
   // One operator withdrawing two different token_codes resolves TWO
   // distinct CollateralPosition PDAs — each effect carries its own key.
   auto op_pk    = filled_pubkey(0x75);
   auto envelope = envelope_with_entries({
      remit_entry(700, make_sol_addr(op_pk)),
      remit_entry(701, make_sol_addr(op_pk)),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 2u);
   BOOST_CHECK_EQUAL(*effects[0].collateral_token_code, 700u);
   BOOST_CHECK_EQUAL(*effects[1].collateral_token_code, 701u);
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
   BOOST_REQUIRE_EQUAL(effects.size(), 1u);
   BOOST_CHECK_EQUAL(effects[0].attestation_index, 1u);
   BOOST_CHECK(effects[0].shape == detail::effect_shape::withdraw_remit);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_skips_non_solana_chain) try {
   // A WITHDRAW_REMIT whose `op_address` carries kind=ETHEREUM is not
   // for this outpost and must not contribute a SOL effect entry.
   auto eth_bytes = filled_pubkey(0x01);
   auto envelope  = envelope_with_entries({
      remit_entry(800, make_eth_addr_32(eth_bytes)),
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

   auto envelope = envelope_with_entries({remit_entry(801, malformed)});

   auto effects = sysio::outpost_solana_client_detail::extract_inbound_effects(envelope);
   BOOST_CHECK(effects.empty());
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_mixed_remit_and_revert_preserved_order) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto op_a       = filled_pubkey(0x10);
   auto depositor  = filled_pubkey(0x20);
   auto op_b       = filled_pubkey(0x30);
   auto envelope   = envelope_with_entries({
      remit_entry(901, make_sol_addr(op_a)),
      revert_entry(902, make_sol_addr(depositor)),
      remit_entry(903, make_sol_addr(op_b)),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 3u);
   BOOST_CHECK(effects[0].recipient->serialize() == op_a);
   BOOST_CHECK(effects[1].recipient->serialize() == depositor);
   BOOST_CHECK(effects[2].recipient->serialize() == op_b);
   BOOST_CHECK_EQUAL(effects[0].attestation_index, 0u);
   BOOST_CHECK_EQUAL(effects[1].attestation_index, 1u);
   BOOST_CHECK_EQUAL(effects[2].attestation_index, 2u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_swap_shapes_carry_wallets_and_reserve_seeds) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto swap_recipient = filled_pubkey(0x41);
   auto swap_depositor = filled_pubkey(0x42);
   auto withdraw_op    = filled_pubkey(0x43);
   auto envelope       = envelope_with_entries({
      swap_remit_entry(10, 20, make_sol_addr(swap_recipient)),
      swap_revert_entry(11, 21, make_sol_addr(swap_depositor)),
      remit_entry(12, make_sol_addr(withdraw_op)),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 3u);

   // SWAP_REMIT: recipient wallet + (token, reserve) seeds for the
   // Reserve/vault PDA derivations and the recipient ATA. Reserve-backed
   // shapes carry no collateral key — custody rides the Reserve record.
   BOOST_CHECK(effects[0].shape == detail::effect_shape::swap_remit);
   BOOST_CHECK(effects[0].recipient->serialize() == swap_recipient);
   BOOST_REQUIRE(effects[0].reserve.has_value());
   BOOST_CHECK_EQUAL(effects[0].reserve->token_code, 10u);
   BOOST_CHECK_EQUAL(effects[0].reserve->reserve_code, 20u);
   BOOST_CHECK(!effects[0].collateral_token_code.has_value());

   // SWAP_REVERT: the wallet is the DEPOSITOR (the refund target).
   BOOST_CHECK(effects[1].shape == detail::effect_shape::swap_revert);
   BOOST_CHECK(effects[1].recipient->serialize() == swap_depositor);
   BOOST_REQUIRE(effects[1].reserve.has_value());
   BOOST_CHECK_EQUAL(effects[1].reserve->token_code, 11u);
   BOOST_CHECK_EQUAL(effects[1].reserve->reserve_code, 21u);
   BOOST_CHECK(!effects[1].collateral_token_code.has_value());

   BOOST_CHECK(effects[2].shape == detail::effect_shape::withdraw_remit);
   BOOST_CHECK(effects[2].recipient->serialize() == withdraw_op);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_reserve_shapes_carry_seeds_per_attestation) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto recipient = filled_pubkey(0x51);
   auto depositor = filled_pubkey(0x52);
   // The repeated RESERVE_READY pair appears TWICE: the walk is
   // per-attestation (the cursor counts both); the duplicate Reserve PDA
   // merges later in `record_terminal_account`.
   auto envelope  = envelope_with_entries({
      swap_remit_entry(100, 200, make_sol_addr(recipient)),
      swap_revert_entry(101, 201, make_sol_addr(depositor)),
      reserve_ready_entry(102, 202),
      reserve_create_cancelled_entry(103, 203),
      reserve_ready_entry(102, 202),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 5u);
   const std::array<std::pair<uint64_t, uint64_t>, 5> expected_seeds{{
      {100u, 200u}, {101u, 201u}, {102u, 202u}, {103u, 203u}, {102u, 202u}}};
   for (size_t i = 0; i < expected_seeds.size(); ++i) {
      BOOST_REQUIRE(effects[i].reserve.has_value());
      BOOST_CHECK_EQUAL(effects[i].reserve->token_code, expected_seeds[i].first);
      BOOST_CHECK_EQUAL(effects[i].reserve->reserve_code, expected_seeds[i].second);
   }
   BOOST_CHECK(effects[2].shape == detail::effect_shape::reserve_ready);
   BOOST_CHECK(!effects[2].recipient.has_value());
   BOOST_CHECK(effects[3].shape == detail::effect_shape::reserve_create_cancelled);
   BOOST_CHECK(!effects[3].recipient.has_value());
   BOOST_CHECK(effects[4].shape == detail::effect_shape::reserve_ready);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(extract_effects_reserve_create_cancelled_shape_selects_cancelled_only) try {
   namespace detail = sysio::outpost_solana_client_detail;
   auto recipient = filled_pubkey(0x53);
   auto depositor = filled_pubkey(0x54);
   auto envelope  = envelope_with_entries({
      swap_remit_entry(110, 210, make_sol_addr(recipient)),
      reserve_create_cancelled_entry(111, 211),
      swap_revert_entry(112, 212, make_sol_addr(depositor)),
      reserve_create_cancelled_entry(111, 211),
      reserve_create_cancelled_entry(113, 213),
   });

   const auto effects = detail::extract_inbound_effects(envelope);
   BOOST_REQUIRE_EQUAL(effects.size(), 5u);
   std::vector<std::pair<uint64_t, uint64_t>> cancelled;
   for (const auto& effect : effects) {
      if (effect.shape != detail::effect_shape::reserve_create_cancelled) continue;
      BOOST_REQUIRE(effect.reserve.has_value());
      cancelled.emplace_back(effect.reserve->token_code, effect.reserve->reserve_code);
   }
   const std::vector<std::pair<uint64_t, uint64_t>> expected{
      {111u, 211u}, {111u, 211u}, {113u, 213u}};
   BOOST_CHECK(cancelled == expected);
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
//    dispatch crank packs its windows from, driven through its Reserve and
//    CollateralPosition read seams. These pin the properties the manifest
//    build must hold before a single dispatch is sent: custody comes from the
//    exact account the on-chain handler branches on, reads are memoised by the
//    account's full PDA seed tuple, and an absent account degrades only itself.

namespace {

namespace manifest_detail = sysio::outpost_solana_client_detail;

/// Scripted stand-in for the Reserve and CollateralPosition reads a manifest
/// build performs. The record maps are on-chain truth; absent entries degrade
/// as uninitialized accounts. Every call is recorded so memoization is
/// assertable.
struct manifest_build_harness {
   using seeds            = std::pair<uint64_t, uint64_t>;
   /// One collateral position's full `(operator, token_code)` PDA seed tuple.
   using collateral_seeds = std::pair<solana_public_key, uint64_t>;

   solana_public_key program_id = measurement_pubkey(42);
   /// The named `reserve_aggregate` account — SPL slash seizures settle into
   /// its canonical ATA.
   solana_public_key reserve_aggregate = measurement_pubkey(43);
   std::map<seeds, manifest_detail::reserve_terminal_info>             records;
   std::vector<seeds>                                                   reads;
   std::map<collateral_seeds, manifest_detail::token_custody_info> collateral_records;
   std::vector<collateral_seeds>                                      collateral_reads;

   void put(uint64_t token_code, uint64_t reserve_code,
            const solana_public_key& creator, const solana_public_key& custody_mint,
            uint8_t custody_decimals) {
      records.emplace(seeds{token_code, reserve_code},
                      manifest_detail::reserve_terminal_info{creator, custody_mint,
                                                             custody_decimals});
   }

   /// Seed one position's pinned custody for the collateral reader.
   void put_collateral(const solana_public_key& operator_key, uint64_t token_code,
                       const solana_public_key& custody_mint) {
      collateral_records.emplace(collateral_seeds{operator_key, token_code},
                                 manifest_detail::token_custody_info{custody_mint});
   }

   manifest_detail::reserve_info_reader reader() {
      return [this](uint64_t token_code,
                    uint64_t reserve_code) -> std::optional<manifest_detail::reserve_terminal_info> {
         reads.emplace_back(token_code, reserve_code);
         auto it = records.find(seeds{token_code, reserve_code});
         if (it == records.end()) return std::nullopt;
         return it->second;
      };
   }

   /// Scripted stand-in for `collateral_position_custody`: anything absent
   /// from `collateral_records` reads as an absent-position degrade.
   manifest_detail::collateral_custody_reader collateral_reader() {
      return [this](const solana_public_key& operator_key,
                    uint64_t token_code) -> std::optional<manifest_detail::token_custody_info> {
         const auto key = collateral_seeds{operator_key, token_code};
         collateral_reads.emplace_back(key);
         auto it = collateral_records.find(key);
         if (it == collateral_records.end()) return std::nullopt;
         return it->second;
      };
   }

   std::vector<std::vector<account_meta>> build(
      const std::vector<manifest_detail::inbound_effect>& effects,
      uint32_t                                            total_attestations,
      const std::function<void()>&                        deadline_probe = [] {}) {
      return manifest_detail::build_dispatch_manifests(
         program_id, effects, total_attestations, deadline_probe, reader(),
         collateral_reader(), reserve_aggregate, "test-relay");
   }
};

/// One SWAP_REMIT effect at `index` paying `recipient` out of `(token, reserve)`.
manifest_detail::inbound_effect swap_remit_effect(size_t index, uint64_t token_code,
                                                  uint64_t reserve_code,
                                                  const solana_public_key& recipient) {
   return manifest_detail::inbound_effect{
      index, manifest_detail::effect_shape::swap_remit, recipient,
      manifest_detail::reserve_pda_seeds{token_code, reserve_code}};
}

/// One WITHDRAW_REMIT effect at `index` paying `recipient` out of their
/// `token_code`-keyed `CollateralPosition`.
manifest_detail::inbound_effect withdraw_remit_effect(size_t index, uint64_t token_code,
                                                      const solana_public_key& recipient) {
   return manifest_detail::inbound_effect{
      index, manifest_detail::effect_shape::withdraw_remit, recipient, std::nullopt,
      token_code};
}

/// One SLASH effect at `index` seizing `operator_key`'s `token_code`-keyed
/// `CollateralPosition`.
manifest_detail::inbound_effect slash_effect(size_t index, uint64_t token_code,
                                             const solana_public_key& operator_key) {
   return manifest_detail::inbound_effect{
      index, manifest_detail::effect_shape::slash, operator_key, std::nullopt, token_code};
}

/// One DEPOSIT_REVERT effect at `index` refunding `depositor`'s
/// `token_code`-keyed `CollateralPosition`.
manifest_detail::inbound_effect deposit_revert_effect(size_t index, uint64_t token_code,
                                                      const solana_public_key& depositor) {
   return manifest_detail::inbound_effect{
      index, manifest_detail::effect_shape::deposit_revert, depositor, std::nullopt,
      token_code};
}

bool manifest_has(const std::vector<account_meta>& metas, const solana_public_key& key) {
   return std::any_of(metas.begin(), metas.end(),
                      [&](const account_meta& meta) { return meta.key == key; });
}

} // anonymous namespace

/// Custody MUST come from the Reserve account, never from a token-code-keyed
/// config map. The program's `handle_swap_remit` branches on
/// `reserve.custody_mint`; if the relay resolved custody from the mutable
/// `OutpostConfig.token_addresses_by_code` instead, an admin re-pointing that
/// token address after the reserve was created would send the relay down the
/// native branch while the program took the SPL one -- and the missing vault
/// and recipient-ATA accounts abort the call permanently, wedging the epoch.
///
/// The divergence is modelled directly: ONE token_code backs two reserves
/// whose pinned custody disagrees. A config-keyed lookup could only produce
/// one answer for both; following the Reserve produces the right answer twice.
BOOST_AUTO_TEST_CASE(build_manifests_follows_reserve_custody_per_reserve) try {
   constexpr uint64_t token_code = 111;
   const auto spl_mint     = measurement_pubkey(77);
   const auto spl_recipient    = measurement_pubkey(80);
   const auto native_recipient = measurement_pubkey(81);
   const auto creator          = measurement_pubkey(82);

   manifest_build_harness harness;
   harness.put(token_code, 200, creator, spl_mint, 6);                              // SPL custody
   harness.put(token_code, 201, creator, system::program_ids::SYSTEM_PROGRAM, 9);   // native custody

   const auto manifests = harness.build(
      {swap_remit_effect(0, token_code, 200, spl_recipient),
       swap_remit_effect(1, token_code, 201, native_recipient)},
      2);
   BOOST_REQUIRE_EQUAL(manifests.size(), 2u);

   // SPL branch: Reserve PDA + vault + the recipient's ATA FOR THE RESERVE'S
   // OWN MINT + the token program.
   const auto& spl = manifests[0];
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_reserve_pda(
                                    harness.program_id, token_code, 200)));
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_reserve_vault_pda(
                                    harness.program_id, token_code, 200)));
   BOOST_CHECK(manifest_has(spl, system::get_associated_token_address(spl_recipient, spl_mint)));
   BOOST_CHECK(manifest_has(spl, system::program_ids::TOKEN_PROGRAM));
   // The bare recipient wallet is the NATIVE branch's account and must not
   // appear on the SPL one.
   BOOST_CHECK(!manifest_has(spl, spl_recipient));

   // Native branch, same token_code: Reserve PDA + the recipient wallet, and
   // NO vault / ATA / token program.
   const auto& native = manifests[1];
   BOOST_CHECK(manifest_has(native, manifest_detail::derive_reserve_pda(
                                       harness.program_id, token_code, 201)));
   BOOST_CHECK(manifest_has(native, native_recipient));
   BOOST_CHECK(!manifest_has(native, manifest_detail::derive_reserve_vault_pda(
                                        harness.program_id, token_code, 201)));
   BOOST_CHECK(!manifest_has(native, system::program_ids::TOKEN_PROGRAM));
} FC_LOG_AND_RETHROW();

/// Collateral custody MUST be resolved per position, never per token code.
/// The program pins `custody_mint` on each `(operator, token_code)`
/// `CollateralPosition`, so two operators using the same token code may take
/// different native/SPL settlement branches. A token-code-only cache would
/// make one manifest disagree with the program and permanently wedge the
/// dispatch cursor on `EffectAccountMissing`.
BOOST_AUTO_TEST_CASE(build_manifests_follows_collateral_custody_per_position) try {
   constexpr uint64_t token_code = 700;
   const auto native_operator = measurement_pubkey(96);
   const auto spl_operator    = measurement_pubkey(97);
   const auto spl_mint        = measurement_pubkey(98);

   manifest_build_harness harness;
   harness.put_collateral(native_operator, token_code,
                          system::program_ids::SYSTEM_PROGRAM);
   harness.put_collateral(spl_operator, token_code, spl_mint);

   const auto manifests = harness.build(
      {withdraw_remit_effect(0, token_code, native_operator),
       withdraw_remit_effect(1, token_code, spl_operator)},
      2);
   BOOST_REQUIRE_EQUAL(manifests.size(), 2u);

   // Native position: operator + position only, with no SPL extras.
   const auto& native = manifests[0];
   BOOST_REQUIRE_EQUAL(native.size(), 2u);
   BOOST_CHECK(manifest_has(native, native_operator));
   BOOST_CHECK(manifest_has(native, manifest_detail::derive_collateral_position_pda(
                                      harness.program_id, native_operator, token_code)));
   BOOST_CHECK(!manifest_has(native, manifest_detail::derive_collateral_vault_pda(
                                       harness.program_id, token_code, spl_mint)));
   BOOST_CHECK(!manifest_has(native, system::program_ids::TOKEN_PROGRAM));

   // SPL position, same token code: position + collateral vault + operator's
   // ATA for THIS POSITION'S mint + token program.
   const auto& spl = manifests[1];
   BOOST_REQUIRE_EQUAL(spl.size(), 5u);
   BOOST_CHECK(manifest_has(spl, spl_operator));
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_collateral_position_pda(
                                   harness.program_id, spl_operator, token_code)));
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_collateral_vault_pda(
                                   harness.program_id, token_code, spl_mint)));
   BOOST_CHECK(manifest_has(spl, system::get_associated_token_address(spl_operator, spl_mint)));
   BOOST_CHECK(manifest_has(spl, system::program_ids::TOKEN_PROGRAM));

   BOOST_REQUIRE_EQUAL(harness.collateral_reads.size(), 2u);
   BOOST_CHECK((harness.collateral_reads[0] ==
                manifest_build_harness::collateral_seeds{native_operator, token_code}));
   BOOST_CHECK((harness.collateral_reads[1] ==
                manifest_build_harness::collateral_seeds{spl_operator, token_code}));
} FC_LOG_AND_RETHROW();

/// SLASH is the one collateral shape whose SPL destination is NOT the
/// recipient: the seizure settles into the `reserve_aggregate`'s canonical
/// ATA, and the operator wallet is deliberately absent (SLASH pays nobody
/// directly). A wrong ATA owner here is the silently-stuck-seizure failure
/// mode — the handler's uninitialised-destination check degrades to
/// "status flipped, funds stuck" rather than aborting — so the owner swap is
/// pinned at the manifest level, native and SPL.
BOOST_AUTO_TEST_CASE(build_manifests_slash_settles_into_reserve_aggregate_ata) try {
   constexpr uint64_t token_code = 700;
   const auto native_operator = measurement_pubkey(90);
   const auto spl_operator    = measurement_pubkey(91);
   const auto spl_mint        = measurement_pubkey(92);

   manifest_build_harness harness;
   harness.put_collateral(native_operator, token_code,
                          system::program_ids::SYSTEM_PROGRAM);
   harness.put_collateral(spl_operator, token_code, spl_mint);

   const auto manifests = harness.build(
      {slash_effect(0, token_code, native_operator),
       slash_effect(1, token_code, spl_operator)},
      2);
   BOOST_REQUIRE_EQUAL(manifests.size(), 2u);

   // Native seizure: position ONLY — the lamports land in the named
   // `reserve_aggregate` account, and the operator wallet is never declared.
   const auto& native = manifests[0];
   BOOST_REQUIRE_EQUAL(native.size(), 1u);
   BOOST_CHECK(manifest_has(native, manifest_detail::derive_collateral_position_pda(
                                      harness.program_id, native_operator, token_code)));
   BOOST_CHECK(!manifest_has(native, native_operator));

   // SPL seizure: position + collateral vault + the RESERVE_AGGREGATE's ATA
   // (not the operator's) + token program; still no operator wallet.
   const auto& spl = manifests[1];
   BOOST_REQUIRE_EQUAL(spl.size(), 4u);
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_collateral_position_pda(
                                   harness.program_id, spl_operator, token_code)));
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_collateral_vault_pda(
                                   harness.program_id, token_code, spl_mint)));
   BOOST_CHECK(manifest_has(
      spl, system::get_associated_token_address(harness.reserve_aggregate, spl_mint)));
   BOOST_CHECK(!manifest_has(spl, system::get_associated_token_address(spl_operator, spl_mint)));
   BOOST_CHECK(manifest_has(spl, system::program_ids::TOKEN_PROGRAM));
   BOOST_CHECK(!manifest_has(spl, spl_operator));
} FC_LOG_AND_RETHROW();

/// DEPOSIT_REVERT settles SPL for real on the companion program
/// (`handle_deposit_revert`'s non-native branch drains the collateral vault
/// into the DEPOSITOR's canonical ATA via `resolve_collateral_vault_transfer`,
/// which `require_remaining_account`s the vault, the destination ATA, and the
/// token program). A manifest that omits them aborts the dispatch round and
/// re-packs the identical window from the same cursor on every retry — the
/// regression this test pins is exactly the earlier "SPL deposit-revert is
/// refused on-chain" early-return, which held the cursor once the companion
/// implemented settlement.
BOOST_AUTO_TEST_CASE(build_manifests_deposit_revert_declares_spl_refund_accounts) try {
   constexpr uint64_t token_code = 700;
   const auto native_depositor = measurement_pubkey(93);
   const auto spl_depositor    = measurement_pubkey(94);
   const auto spl_mint         = measurement_pubkey(95);

   manifest_build_harness harness;
   harness.put_collateral(native_depositor, token_code,
                          system::program_ids::SYSTEM_PROGRAM);
   harness.put_collateral(spl_depositor, token_code, spl_mint);

   const auto manifests = harness.build(
      {deposit_revert_effect(0, token_code, native_depositor),
       deposit_revert_effect(1, token_code, spl_depositor)},
      2);
   BOOST_REQUIRE_EQUAL(manifests.size(), 2u);

   // Native refund: depositor + position only, with no SPL extras.
   const auto& native = manifests[0];
   BOOST_REQUIRE_EQUAL(native.size(), 2u);
   BOOST_CHECK(manifest_has(native, native_depositor));
   BOOST_CHECK(manifest_has(native, manifest_detail::derive_collateral_position_pda(
                                      harness.program_id, native_depositor, token_code)));
   BOOST_CHECK(!manifest_has(native, system::program_ids::TOKEN_PROGRAM));

   // SPL refund: depositor wallet (the full-drain close's rent recipient) +
   // position + collateral vault + the DEPOSITOR's ATA + token program.
   const auto& spl = manifests[1];
   BOOST_REQUIRE_EQUAL(spl.size(), 5u);
   BOOST_CHECK(manifest_has(spl, spl_depositor));
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_collateral_position_pda(
                                   harness.program_id, spl_depositor, token_code)));
   BOOST_CHECK(manifest_has(spl, manifest_detail::derive_collateral_vault_pda(
                                   harness.program_id, token_code, spl_mint)));
   BOOST_CHECK(manifest_has(spl, system::get_associated_token_address(spl_depositor, spl_mint)));
   BOOST_CHECK(manifest_has(spl, system::program_ids::TOKEN_PROGRAM));
} FC_LOG_AND_RETHROW();

/// The build reads each DISTINCT reserve exactly once, however many
/// attestations reference it -- the property that keeps a 500-remit envelope
/// from costing 500 sequential round-trips per drain. There is no
/// `OutpostConfig` read on this path at all: custody rides the Reserve.
BOOST_AUTO_TEST_CASE(build_manifests_reads_each_reserve_once) try {
   const auto recipient = measurement_pubkey(83);
   const auto creator   = measurement_pubkey(84);
   const auto mint      = measurement_pubkey(85);

   manifest_build_harness harness;
   harness.put(10, 20, creator, mint, 6);
   harness.put(11, 21, creator, mint, 6);

   // Six attestations across TWO distinct reserves.
   harness.build({swap_remit_effect(0, 10, 20, recipient),
                  swap_remit_effect(1, 11, 21, recipient),
                  swap_remit_effect(2, 10, 20, recipient),
                  swap_remit_effect(3, 10, 20, recipient),
                  swap_remit_effect(4, 11, 21, recipient),
                  swap_remit_effect(5, 10, 20, recipient)},
                 6);

   BOOST_CHECK_EQUAL(harness.reads.size(), 2u);
   const std::vector<std::pair<uint64_t, uint64_t>> expected{{10u, 20u}, {11u, 21u}};
   BOOST_CHECK(harness.reads == expected);
} FC_LOG_AND_RETHROW();

/// An ABSENT reserve costs THAT attestation its custody-dependent accounts and
/// nothing else: every other attestation's manifest is still built, so the
/// envelope's healthy prefix still dispatches. This degrade is safe precisely
/// because the program skips an uninitialized reserve
/// (`load_reserve_from_remaining` -> `Ok(None)` -> logged skip), so none of the
/// omitted accounts is ever reached. The opposite case — a reserve that exists
/// but this relay cannot read — must NOT come through here; it is pinned by
/// `build_manifests_propagate_an_unreadable_reserve` below.
BOOST_AUTO_TEST_CASE(build_manifests_degrade_an_absent_reserve) try {
   const auto recipient = measurement_pubkey(86);
   const auto payee     = measurement_pubkey(87);
   const auto creator   = measurement_pubkey(88);
   const auto mint      = measurement_pubkey(89);

   manifest_build_harness harness;
   harness.put(10, 20, creator, mint, 6);
   // (12, 22) deliberately absent -> the read degrades.
   // Native custody for the collateral token: the zero-mint marker.
   harness.put_collateral(payee, 700, system::program_ids::SYSTEM_PROGRAM);

   const auto manifests = harness.build({swap_remit_effect(0, 10, 20, recipient),
                                         swap_remit_effect(1, 12, 22, recipient),
                                         withdraw_remit_effect(2, 700, payee)},
                                        3);
   BOOST_REQUIRE_EQUAL(manifests.size(), 3u);

   // The healthy SPL attestation is complete.
   BOOST_CHECK(manifest_has(manifests[0], system::get_associated_token_address(recipient, mint)));
   // The degraded one still carries everything derivable WITHOUT the record
   // (Reserve PDA, vault, recipient, token program) -- just not the ATA, which
   // needs the custody mint.
   BOOST_CHECK(manifest_has(manifests[1], manifest_detail::derive_reserve_pda(
                                             harness.program_id, 12, 22)));
   BOOST_CHECK(manifest_has(manifests[1], manifest_detail::derive_reserve_vault_pda(
                                             harness.program_id, 12, 22)));
   BOOST_CHECK(manifest_has(manifests[1], recipient));
   // The attestation AFTER the degraded one is untouched by it: a native
   // withdraw remit declares exactly the payee and their CollateralPosition.
   BOOST_REQUIRE_EQUAL(manifests[2].size(), 2u);
   BOOST_CHECK(manifest_has(manifests[2], payee));
   BOOST_CHECK(manifest_has(manifests[2], manifest_detail::derive_collateral_position_pda(
                                             harness.program_id, payee, 700)));

   // A permanently unusable reserve costs ONE read for the whole build, not
   // one per attestation referencing it.
   const auto degraded_reads = std::count(harness.reads.begin(), harness.reads.end(),
                                          std::pair<uint64_t, uint64_t>{12u, 22u});
   BOOST_CHECK_EQUAL(degraded_reads, 1);
} FC_LOG_AND_RETHROW();

/// A reserve that EXISTS but cannot be read by this relay must abort the build,
/// never degrade into a manifest. The program decodes that account fine, takes
/// its real branch, and requires the effect accounts that branch needs — the
/// custody ATA for an SPL remit/revert, the `creator` for a cancel with an
/// escrow to refund — so a degraded manifest is GUARANTEED to hit
/// `require_remaining_account` and abort.
///
/// That is the difference between a delayed epoch and a wedged one:
/// `drive_dispatch_rounds` repacks every window from the on-chain cursor, so an
/// aborting attestation heads every future window and the cursor never moves.
/// Letting the exception out fails the tick with the cursor untouched, which is
/// recoverable; shipping the manifest is not.
BOOST_AUTO_TEST_CASE(build_manifests_propagate_an_unreadable_reserve) try {
   const auto recipient = measurement_pubkey(94);
   const auto later     = measurement_pubkey(95);

   manifest_build_harness harness;
   // The reader models `reserve_info_for_codes` meeting a present-but-
   // undecodable Reserve: it throws rather than returning empty.
   auto throwing_reader = [&](uint64_t token_code, uint64_t reserve_code)
      -> std::optional<manifest_detail::reserve_terminal_info> {
      harness.reads.emplace_back(token_code, reserve_code);
      FC_THROW_EXCEPTION(fc::exception, "Reserve exists but is undecodable (test probe)");
   };

   // BOTH effects are reserve-backed, on DIFFERENT reserves, so the read count
   // is load-bearing: had the build swallowed the throw and carried on, the
   // second effect would have paid its own read and `reads` would be 2.
   BOOST_CHECK_EXCEPTION(
      manifest_detail::build_dispatch_manifests(
         harness.program_id,
         {swap_remit_effect(0, 10, 20, recipient), swap_remit_effect(1, 11, 21, later)},
         2, [] {}, throwing_reader, harness.collateral_reader(), harness.reserve_aggregate,
         "test-relay"),
      fc::exception,
      [](const fc::exception& e) {
         return e.to_detail_string().find("undecodable") != std::string::npos;
      });
   // It failed AT the first bad reserve; the second was never reached.
   BOOST_REQUIRE_EQUAL(harness.reads.size(), 1u);
   BOOST_CHECK((harness.reads[0] == std::pair<uint64_t, uint64_t>{10u, 20u}));
} FC_LOG_AND_RETHROW();

/// The manifest build is where a large envelope burns its tick: the deadline
/// is probed per effect, BEFORE that effect's reserve read, so an over-deadline
/// build fails at the loop instead of deep in the RPC layer with the work
/// already lost and zero dispatches sent.
BOOST_AUTO_TEST_CASE(build_manifests_probes_deadline_before_each_reserve_read) try {
   const auto recipient = measurement_pubkey(90);
   const auto creator   = measurement_pubkey(91);
   const auto mint      = measurement_pubkey(92);

   manifest_build_harness harness;
   harness.put(10, 20, creator, mint, 6);
   harness.put(11, 21, creator, mint, 6);

   uint32_t probes = 0;
   BOOST_CHECK_EXCEPTION(
      harness.build({swap_remit_effect(0, 10, 20, recipient),
                     swap_remit_effect(1, 11, 21, recipient)},
                    2,
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
   // Probe fired before the SECOND effect's read, so only the first reserve
   // was ever fetched.
   BOOST_REQUIRE_EQUAL(harness.reads.size(), 1u);
   BOOST_CHECK((harness.reads[0] == std::pair<uint64_t, uint64_t>{10u, 20u}));
} FC_LOG_AND_RETHROW();

/// The result is sized to the envelope's attestation TOTAL and indexed by the
/// flat dispatch position, so an attestation needing no effect account keeps
/// an empty entry and the cursor stays aligned with the manifests.
BOOST_AUTO_TEST_CASE(build_manifests_index_by_flat_dispatch_position) try {
   const auto payee = measurement_pubkey(93);

   manifest_build_harness harness;
   // Native custody for the collateral token: the zero-mint marker.
   harness.put_collateral(payee, 700, system::program_ids::SYSTEM_PROGRAM);
   // Attestations 0, 1 and 3 contribute no effect at all.
   const auto manifests = harness.build({withdraw_remit_effect(2, 700, payee)}, 4);

   BOOST_REQUIRE_EQUAL(manifests.size(), 4u);
   BOOST_CHECK(manifests[0].empty());
   BOOST_CHECK(manifests[1].empty());
   BOOST_REQUIRE_EQUAL(manifests[2].size(), 2u);
   BOOST_CHECK(manifest_has(manifests[2], payee));
   BOOST_CHECK(manifest_has(manifests[2], manifest_detail::derive_collateral_position_pda(
                                             harness.program_id, payee, 700)));
   BOOST_CHECK(manifests[3].empty());
   // A collateral settlement needs no Reserve record — only its one custody
   // lookup is paid.
   BOOST_CHECK(harness.reads.empty());
   BOOST_REQUIRE_EQUAL(harness.collateral_reads.size(), 1u);
   BOOST_CHECK((harness.collateral_reads[0] ==
                manifest_build_harness::collateral_seeds{payee, 700u}));
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
/// and `Reserve` shape tests so both drive the same declaration machinery.
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

BOOST_AUTO_TEST_CASE(reserve_shape_accepts_the_deployed_declaration) try {
   using sysio::outpost_solana_client_detail::assert_reserve_shape;

   // The stub fixture carries the program's own `Reserve` struct, so this is
   // the boot check running against the declaration a batch operator will meet.
   // It is what guarantees the manifest builder can always resolve custody and
   // the cancel-refund creator from a reserve the program can read.
   BOOST_CHECK_NO_THROW(assert_reserve_shape(load_idl_fixture(opp_outpost_idl_fixture)));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(reserve_shape_rejects_drifted_declarations) try {
   using sysio::outpost_solana_client_detail::assert_reserve_shape;

   // IDL drift here is the realistic way a LIVE, program-readable Reserve
   // becomes unreadable to this relay — and in flight that is unrecoverable:
   // the effect account the program's branch requires cannot be derived, the
   // window aborts, and every later window repacks from the same cursor. Each
   // shape below must therefore fail at BOOT, while the IDL is still fixable.
   struct reject_case {
      const char*             name;
      std::vector<idl::field> fields;
   };
   const auto pubkey_t = prim(idl::primitive_type::pubkey);
   const auto u8_t     = prim(idl::primitive_type::u8);

   std::vector<reject_case> cases;
   cases.push_back({"creator dropped",
                    {{"custody_mint", pubkey_t}, {"custody_decimals", u8_t}}});
   cases.push_back({"custody_mint dropped",
                    {{"creator", pubkey_t}, {"custody_decimals", u8_t}}});
   cases.push_back({"custody_decimals dropped",
                    {{"creator", pubkey_t}, {"custody_mint", pubkey_t}}});
   cases.push_back({"custody_mint declared as a byte array",
                    {{"creator", pubkey_t},
                     {"custody_mint", u8_32_array()},
                     {"custody_decimals", u8_t}}});
   cases.push_back({"custody_decimals declared u32",
                    {{"creator", pubkey_t},
                     {"custody_mint", pubkey_t},
                     {"custody_decimals", prim(idl::primitive_type::u32)}}});

   for (const auto& c : cases) {
      BOOST_TEST_CONTEXT(c.name) {
         for (bool in_types : {false, true}) {
            BOOST_CHECK_THROW(assert_reserve_shape(named_account_program("Reserve", c.fields, in_types)),
                              fc::assert_exception);
         }
      }
   }

   // No `Reserve` account declared at all, and one declared with no field
   // definition in either home.
   BOOST_CHECK_THROW(assert_reserve_shape(idl::program{}), fc::assert_exception);
   BOOST_CHECK_THROW(assert_reserve_shape(named_account_program("Reserve", {}, false)),
                     fc::assert_exception);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(collateral_position_shape_accepts_the_expected_declaration) try {
   using sysio::outpost_solana_client_detail::assert_collateral_position_shape;

   const std::vector<idl::field> fields{
      {"bump", prim(idl::primitive_type::u8)},
      {"operator", prim(idl::primitive_type::pubkey)},
      {"token_code", prim(idl::primitive_type::u64)},
      {"custody_mint", prim(idl::primitive_type::pubkey)},
      {"amount", prim(idl::primitive_type::u64)}};
   for (bool in_types : {false, true}) {
      BOOST_CHECK_NO_THROW(assert_collateral_position_shape(
         named_account_program("CollateralPosition", fields, in_types)));
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(collateral_position_shape_rejects_drifted_declarations) try {
   using sysio::outpost_solana_client_detail::assert_collateral_position_shape;

   struct reject_case {
      const char*             name;
      std::vector<idl::field> fields;
   };
   const auto pubkey_t = prim(idl::primitive_type::pubkey);
   const auto u64_t    = prim(idl::primitive_type::u64);

   std::vector<reject_case> cases;
   cases.push_back({"operator dropped",
                    {{"token_code", u64_t}, {"custody_mint", pubkey_t}, {"amount", u64_t}}});
   cases.push_back({"token_code dropped",
                    {{"operator", pubkey_t}, {"custody_mint", pubkey_t}, {"amount", u64_t}}});
   cases.push_back({"custody_mint dropped",
                    {{"operator", pubkey_t}, {"token_code", u64_t}, {"amount", u64_t}}});
   cases.push_back({"amount dropped",
                    {{"operator", pubkey_t}, {"token_code", u64_t}, {"custody_mint", pubkey_t}}});
   cases.push_back({"operator declared as a byte array",
                    {{"operator", u8_32_array()},
                     {"token_code", u64_t},
                     {"custody_mint", pubkey_t},
                     {"amount", u64_t}}});
   cases.push_back({"custody_mint declared as a byte array",
                    {{"operator", pubkey_t},
                     {"token_code", u64_t},
                     {"custody_mint", u8_32_array()},
                     {"amount", u64_t}}});
   cases.push_back({"token_code declared u32",
                    {{"operator", pubkey_t},
                     {"token_code", prim(idl::primitive_type::u32)},
                     {"custody_mint", pubkey_t},
                     {"amount", u64_t}}});
   cases.push_back({"amount declared u32",
                    {{"operator", pubkey_t},
                     {"token_code", u64_t},
                     {"custody_mint", pubkey_t},
                     {"amount", prim(idl::primitive_type::u32)}}});

   for (const auto& c : cases) {
      BOOST_TEST_CONTEXT(c.name) {
         for (bool in_types : {false, true}) {
            BOOST_CHECK_THROW(assert_collateral_position_shape(
                                 named_account_program("CollateralPosition", c.fields, in_types)),
                              fc::assert_exception);
         }
      }
   }

   BOOST_CHECK_THROW(assert_collateral_position_shape(idl::program{}), fc::assert_exception);
   BOOST_CHECK_THROW(
      assert_collateral_position_shape(named_account_program("CollateralPosition", {}, false)),
      fc::assert_exception);
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

BOOST_AUTO_TEST_SUITE_END()
