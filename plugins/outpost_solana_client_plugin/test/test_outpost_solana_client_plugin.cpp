#include <boost/test/unit_test.hpp>

#include <fc-test/build_info.hpp>
#include <fc/io/json.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_borsh.hpp>

#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

using namespace std::literals;
using namespace fc::network::solana;

namespace {

constexpr std::string_view counter_anchor_idl_fixture = "solana-idl-counter-anchor.json";
constexpr std::string_view opp_outpost_idl_fixture = "solana-idl-opp-outpost-stub.json";

idl::program load_idl_fixture(std::string_view filename) {
   auto path = fc::test::get_test_fixtures_path() / boost::filesystem::path(filename);
   return idl::parse_idl_file(path.generic_string());
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(outpost_solana_client_plugin)

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
   BOOST_CHECK_EQUAL(chunks_for(2526), 4u);   // 2526/672 = 3.76 → 4
   // 64 KiB cap: ceil(65 536 / 672) = 98 chunks. Last chunk is 352 B
   // (65_536 mod 672 = 352), the first 97 are full at MAX_CHUNK_BYTES.
   BOOST_CHECK_EQUAL(chunks_for(sysio::SOLANA_MAX_ENVELOPE_BYTES), 98u);
   BOOST_CHECK_EQUAL(epoch_in_calls_for(sysio::SOLANA_MAX_ENVELOPE_BYTES), 99u);
   BOOST_CHECK_EQUAL(sysio::SOLANA_MAX_ENVELOPE_BYTES % sysio::SOLANA_MAX_CHUNK_BYTES, 352u);

   // Last-chunk size at the dev-026 reproduction: the loop fills the first
   // 3 chunks at MAX_CHUNK_BYTES (= 672) and the last with the remainder.
   const size_t last_chunk_size = 2526 - 3 * sysio::SOLANA_MAX_CHUNK_BYTES;
   BOOST_CHECK_EQUAL(last_chunk_size, 510u);   // 2526 − 2016 = 510
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
