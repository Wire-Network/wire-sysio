#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>          // to_variant(ChainKind) glue for the mvo below
#include <sysio/opp/types/types.pb.h>
#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using mvo = fc::mutable_variant_object;

namespace {

/// A `slug_name` renders in JSON/ABI as `{value: <uint64>}`.
inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return mvo()("value", fc::slug_name{s}.value);
}

// Well-formed sample addresses for the accept paths.
constexpr auto EVM_OPP      = "0x5FbDB2315678afecb367f032d93F642f64180aa3";  // OPP.sol
constexpr auto EVM_INBOUND  = "0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512";  // OPPInbound.sol
constexpr auto SVM_PROGRAM  = "So11111111111111111111111111111111111111112"; // 44-char base58

} // namespace

class sysio_chains_tester : public tester {
public:
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;

   sysio_chains_tester() {
      produce_blocks(2);
      create_accounts({CHAINS_ACCOUNT, EPOCH_ACCOUNT});
      produce_blocks(2);

      set_code(CHAINS_ACCOUNT, contracts::chains_wasm());
      set_abi(CHAINS_ACCOUNT, contracts::chains_abi().data());
      set_privileged(CHAINS_ACCOUNT);
      produce_blocks();

      const auto* accnt = control->find_account_metadata(CHAINS_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      chains_abi.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_chains(name action_name, const fc::variant_object& data) {
      try {
         const std::string action_type = chains_abi.get_action_type(action_name);
         action act;
         act.account = CHAINS_ACCOUNT;
         act.name    = action_name;
         act.data    = chains_abi.variant_to_binary(action_type, data,
                          abi_serializer::create_yield_function(abi_serializer_max_time));
         act.authorization = std::vector<permission_level>{{CHAINS_ACCOUNT, config::active_name}};

         signed_transaction trx;
         trx.actions.emplace_back(std::move(act));
         set_transaction_headers(trx);
         trx.sign(get_private_key(CHAINS_ACCOUNT, "active"), control->get_chain_id());
         push_transaction(trx);
         produce_block();
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result regchain(ChainKind kind, std::string_view code,
                          uint32_t external_chain_id,
                          std::string opp_addr, std::string opp_inbound_addr) {
      return push_chains("regchain"_n, mvo()
         ("kind",              kind)
         ("code",              codename_mvo(code))
         ("external_chain_id", external_chain_id)
         ("name",              std::string(code))
         ("description",       std::string{})
         ("opp_addr",          std::move(opp_addr))
         ("opp_inbound_addr",  std::move(opp_inbound_addr)));
   }

   action_result setoutpost(std::string_view code, std::string opp_addr, std::string opp_inbound_addr) {
      return push_chains("setoutpost"_n, mvo()
         ("code",             codename_mvo(code))
         ("opp_addr",         std::move(opp_addr))
         ("opp_inbound_addr", std::move(opp_inbound_addr)));
   }

   /// Read the stored `chain_row` for `code` (KV table `chains`, keyed by the
   /// slug_name uint64). Returns a null variant when the row is absent.
   fc::variant get_chain(std::string_view code) {
      auto data = get_row_by_id(CHAINS_ACCOUNT, CHAINS_ACCOUNT, "chains"_n, fc::slug_name{code}.value);
      return data.empty() ? fc::variant() : chains_abi.binary_to_variant(
         "chain_row", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_serializer chains_abi;
};

BOOST_AUTO_TEST_SUITE(sysio_chains_tests)

// ── EVM: valid addresses are accepted and stored verbatim ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_valid_addresses_stored, sysio_chains_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, EVM_OPP, EVM_INBOUND));
   auto row = get_chain("ETH");
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP),     row["opp_addr"].as_string());
   BOOST_REQUIRE_EQUAL(std::string(EVM_INBOUND), row["opp_inbound_addr"].as_string());
} FC_LOG_AND_RETHROW() }

// ── EVM: a malformed OPP / OPPInbound hex address is rejected ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_bad_hex_rejected, sysio_chains_tester) { try {
   // Wrong length.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, "0xdeadbeef", EVM_INBOUND)
                    .find("20-byte hex address") != std::string::npos);
   // Non-hex character in an otherwise 42-char string (trailing 'z').
   const std::string bad_hex = "0x5FbDB2315678afecb367f032d93F642f64180aaz";
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, EVM_OPP, bad_hex)
                    .find("non-hex character") != std::string::npos);
   BOOST_REQUIRE(get_chain("ETH").is_null());   // nothing registered on reject
} FC_LOG_AND_RETHROW() }

// ── EVM: empty addresses are allowed (deploy-then-configure) ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_empty_addresses_allowed, sysio_chains_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, "", ""));
   auto row = get_chain("ETH");
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(std::string{}, row["opp_addr"].as_string());
   BOOST_REQUIRE_EQUAL(std::string{}, row["opp_inbound_addr"].as_string());
} FC_LOG_AND_RETHROW() }

// ── SVM: base58 program id in opp_addr, inbound MUST be empty ──
BOOST_FIXTURE_TEST_CASE(regchain_svm_program_id_accepted, sysio_chains_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 900, SVM_PROGRAM, ""));
   BOOST_REQUIRE_EQUAL(std::string(SVM_PROGRAM), get_chain("SOL")["opp_addr"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regchain_svm_inbound_must_be_empty, sysio_chains_tester) { try {
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 900, SVM_PROGRAM, SVM_PROGRAM)
                    .find("opp_inbound_addr must be empty") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regchain_svm_bad_base58_rejected, sysio_chains_tester) { try {
   // '0', 'O', 'I', 'l' are not in the base58 alphabet.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 900, "So1111111111111111111111111111111111111111O", "")
                    .find("non-base58 character") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── WIRE: the depot self-row has no remote endpoint ──
BOOST_FIXTURE_TEST_CASE(regchain_wire_rejects_addresses, sysio_chains_tester) { try {
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0, EVM_OPP, "")
                    .find("no remote endpoint") != std::string::npos);
   // WIRE with both empty is fine.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0, "", ""));
} FC_LOG_AND_RETHROW() }

// ── setoutpost: updates a registered row, validates, guards the WIRE row ──
BOOST_FIXTURE_TEST_CASE(setoutpost_updates_and_guards, sysio_chains_tester) { try {
   // Register EVM with empty addresses, then fill them in.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, "", ""));
   BOOST_REQUIRE_EQUAL(success(), setoutpost("ETH", EVM_OPP, EVM_INBOUND));
   auto row = get_chain("ETH");
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP),     row["opp_addr"].as_string());
   BOOST_REQUIRE_EQUAL(std::string(EVM_INBOUND), row["opp_inbound_addr"].as_string());

   // Same validation as regchain: a bad address is rejected and the row is unchanged.
   BOOST_REQUIRE(setoutpost("ETH", "0xnothex", EVM_INBOUND).find("20-byte hex address") != std::string::npos);
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP), get_chain("ETH")["opp_addr"].as_string());

   // Unregistered code is rejected.
   BOOST_REQUIRE(setoutpost("NOPE", EVM_OPP, EVM_INBOUND).find("not registered") != std::string::npos);

   // The WIRE depot self-row has no remote endpoint.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0, "", ""));
   BOOST_REQUIRE(setoutpost("WIRE", EVM_OPP, EVM_INBOUND).find("no remote endpoint") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── Two same-kind EVM outposts each keep their own distinct binding ──
// (the registry half of WSA-075: the batch operator reads these per-row.)
BOOST_FIXTURE_TEST_CASE(two_evm_outposts_keep_distinct_bindings, sysio_chains_tester) { try {
   constexpr auto ETH2_OPP     = "0x1111111111111111111111111111111111111111";
   constexpr auto ETH2_INBOUND = "0x2222222222222222222222222222222222222222";
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH",  1,   EVM_OPP,  EVM_INBOUND));
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "BASE", 8453, ETH2_OPP, ETH2_INBOUND));

   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP),  get_chain("ETH")["opp_addr"].as_string());
   BOOST_REQUIRE_EQUAL(std::string(ETH2_OPP), get_chain("BASE")["opp_addr"].as_string());
   // Distinct external_chain_id is what the batch operator matches its RPC client on.
   BOOST_REQUIRE_EQUAL(1u,    get_chain("ETH")["external_chain_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(8453u, get_chain("BASE")["external_chain_id"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
