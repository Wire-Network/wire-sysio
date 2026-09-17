/// Address-validation coverage for `sysio.chains`. The registration SEMANTICS
/// (code/kind reservation, EVM external_chain_id uniqueness, SVM cardinality,
/// metadata bounds) are covered by the `regchain_*` cases in
/// `sysio.epoch_tests.cpp`; this suite covers only the remote outpost contract
/// identities that `regchain` and `setoutpost` carry.

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>          // to_variant(ChainKind) glue for the mvo below
#include <sysio/opp/types/types.pb.h>
#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using sysio_system::test_support::evm_outpost_mvo;
using sysio_system::test_support::no_outpost_mvo;
using sysio_system::test_support::svm_outpost_mvo;
using mvo = fc::mutable_variant_object;

namespace {

/// A `slug_name` renders in JSON/ABI as `{value: <uint64>}`.
inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return mvo()("value", fc::slug_name{s}.value);
}

// Well-formed sample addresses for the accept paths.
constexpr auto EVM_OPP      = "0x5FbDB2315678afecb367f032d93F642f64180aa3";  // OPP.sol
constexpr auto EVM_INBOUND  = "0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512";  // OPPInbound.sol
constexpr auto EVM_OPREG    = "0x9fE46736679d2D9a65F0992F2272dE9f3c7fa6e0";  // OperatorRegistry.sol
constexpr auto EVM_DEPOSIT  = "0xCf7Ed3AccA5a467e9e704C703E8D87F634fB0Fc9";  // SwapDeposit emitter
constexpr auto SVM_PROGRAM  = "So11111111111111111111111111111111111111112"; // 43-char base58

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

   action_result regchain(ChainKind kind, std::string_view code, uint32_t external_chain_id,
                          const fc::variant_object& outpost) {
      return push_chains("regchain"_n, mvo()
         ("kind",              kind)
         ("code",              codename_mvo(code))
         ("external_chain_id", external_chain_id)
         ("name",              std::string(code))
         ("description",       std::string{})
         ("outpost",           outpost));
   }

   action_result setoutpost(std::string_view code, const fc::variant_object& outpost) {
      return push_chains("setoutpost"_n, mvo()
         ("code",    codename_mvo(code))
         ("outpost", outpost));
   }

   /// Read the stored `chain_row` for `code` (KV table `chains`, keyed by the
   /// slug_name uint64). Returns a null variant when the row is absent.
   fc::variant get_chain(std::string_view code) {
      auto data = get_row_by_id(CHAINS_ACCOUNT, CHAINS_ACCOUNT, "chains"_n, fc::slug_name{code}.value);
      return data.empty() ? fc::variant() : chains_abi.binary_to_variant(
         "chain_row", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// One address field off a stored row's nested `outpost` struct.
   std::string stored_addr(std::string_view code, const char* field) {
      auto row = get_chain(code);
      BOOST_REQUIRE(!row.is_null());
      return row["outpost"][field].as_string();
   }

   abi_serializer chains_abi;
};

BOOST_AUTO_TEST_SUITE(sysio_chains_tests)

// ── EVM: all four role addresses are accepted and stored verbatim ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_addresses_stored, sysio_chains_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
      evm_outpost_mvo(EVM_OPP, EVM_INBOUND, EVM_OPREG, EVM_DEPOSIT)));
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP),     stored_addr("ETH", "opp_addr"));
   BOOST_REQUIRE_EQUAL(std::string(EVM_INBOUND), stored_addr("ETH", "opp_inbound_addr"));
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPREG),   stored_addr("ETH", "operator_registry_addr"));
   BOOST_REQUIRE_EQUAL(std::string(EVM_DEPOSIT), stored_addr("ETH", "source_deposit_addr"));
} FC_LOG_AND_RETHROW() }

// ── EVM: a malformed hex address is rejected in EVERY role, not just opp_addr ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_bad_hex_rejected, sysio_chains_tester) { try {
   // Wrong length.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
                    evm_outpost_mvo("0xdeadbeef", EVM_INBOUND, EVM_OPREG, EVM_DEPOSIT))
                    .find("20-byte hex address") != std::string::npos);
   // Non-hex character in an otherwise 42-char string (trailing 'z'), in each role.
   const std::string bad_hex = "0x5FbDB2315678afecb367f032d93F642f64180aaz";
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
                    evm_outpost_mvo(EVM_OPP, bad_hex, EVM_OPREG, EVM_DEPOSIT))
                    .find("opp_inbound_addr contains a non-hex character") != std::string::npos);
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
                    evm_outpost_mvo(EVM_OPP, EVM_INBOUND, bad_hex, EVM_DEPOSIT))
                    .find("operator_registry_addr contains a non-hex character") != std::string::npos);
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
                    evm_outpost_mvo(EVM_OPP, EVM_INBOUND, EVM_OPREG, bad_hex))
                    .find("source_deposit_addr contains a non-hex character") != std::string::npos);
   BOOST_REQUIRE(get_chain("ETH").is_null());   // nothing registered on reject
} FC_LOG_AND_RETHROW() }

// ── EVM: empty addresses are allowed (register now, deploy and configure later) ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_empty_addresses_allowed, sysio_chains_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, no_outpost_mvo()));
   BOOST_REQUIRE_EQUAL(std::string{}, stored_addr("ETH", "opp_addr"));
   BOOST_REQUIRE_EQUAL(std::string{}, stored_addr("ETH", "source_deposit_addr"));
} FC_LOG_AND_RETHROW() }

// ── EVM: an oversized string cannot reach sysio-billed state ──
BOOST_FIXTURE_TEST_CASE(regchain_evm_oversized_addr_rejected, sysio_chains_tester) { try {
   const std::string too_long(4096, 'a');
   BOOST_REQUIRE(!regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
                    evm_outpost_mvo(too_long, EVM_INBOUND, EVM_OPREG, EVM_DEPOSIT)).empty());
   BOOST_REQUIRE(get_chain("ETH").is_null());
} FC_LOG_AND_RETHROW() }

// ── SVM: base58 program id in opp_addr; the single program serves every role ──
BOOST_FIXTURE_TEST_CASE(regchain_svm_program_id_accepted, sysio_chains_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 900,
                                           svm_outpost_mvo(SVM_PROGRAM)));
   BOOST_REQUIRE_EQUAL(std::string(SVM_PROGRAM), stored_addr("SOL", "opp_addr"));
   BOOST_REQUIRE_EQUAL(std::string{},            stored_addr("SOL", "opp_inbound_addr"));
} FC_LOG_AND_RETHROW() }

// ── SVM: every role other than opp_addr must be left empty ──
BOOST_FIXTURE_TEST_CASE(regchain_svm_other_roles_must_be_empty, sysio_chains_tester) { try {
   const auto with = [&](const char* inbound, const char* opreg, const char* deposit) {
      return regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 900,
                      evm_outpost_mvo(SVM_PROGRAM, inbound, opreg, deposit));
   };
   BOOST_REQUIRE(with(SVM_PROGRAM, "", "").find("opp_inbound_addr must be empty") != std::string::npos);
   BOOST_REQUIRE(with("", SVM_PROGRAM, "").find("operator_registry_addr must be empty") != std::string::npos);
   BOOST_REQUIRE(with("", "", SVM_PROGRAM).find("source_deposit_addr must be empty") != std::string::npos);
   BOOST_REQUIRE(get_chain("SOL").is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regchain_svm_bad_base58_rejected, sysio_chains_tester) { try {
   // '0', 'O', 'I', 'l' are not in the base58 alphabet.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 900,
                    svm_outpost_mvo("So1111111111111111111111111111111111111111O"))
                    .find("non-base58 character") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── WIRE: the depot self-row has no remote deployment ──
BOOST_FIXTURE_TEST_CASE(regchain_wire_rejects_addresses, sysio_chains_tester) { try {
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0,
                    evm_outpost_mvo(EVM_OPP, "", "", ""))
                    .find("no remote deployment") != std::string::npos);
   // WIRE with every field empty is fine.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0, no_outpost_mvo()));
} FC_LOG_AND_RETHROW() }

// ── setoutpost: updates a registered row, validates, guards the WIRE row ──
BOOST_FIXTURE_TEST_CASE(setoutpost_updates_and_guards, sysio_chains_tester) { try {
   // Register EVM with empty addresses, then fill them in — the redeploy path.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, no_outpost_mvo()));
   BOOST_REQUIRE_EQUAL(success(),
      setoutpost("ETH", evm_outpost_mvo(EVM_OPP, EVM_INBOUND, EVM_OPREG, EVM_DEPOSIT)));
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP),   stored_addr("ETH", "opp_addr"));
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPREG), stored_addr("ETH", "operator_registry_addr"));

   // Same validation as regchain: a bad address is rejected and the row is unchanged.
   BOOST_REQUIRE(setoutpost("ETH", evm_outpost_mvo("0xnothex", EVM_INBOUND, EVM_OPREG, EVM_DEPOSIT))
                    .find("20-byte hex address") != std::string::npos);
   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP), stored_addr("ETH", "opp_addr"));

   // The whole set is replaced, so an omitted field is cleared rather than kept.
   BOOST_REQUIRE_EQUAL(success(), setoutpost("ETH", evm_outpost_mvo(EVM_OPP, EVM_INBOUND, "", "")));
   BOOST_REQUIRE_EQUAL(std::string{}, stored_addr("ETH", "operator_registry_addr"));

   // Unregistered code is rejected.
   BOOST_REQUIRE(setoutpost("NOPE", no_outpost_mvo()).find("not registered") != std::string::npos);

   // The WIRE depot self-row has no remote deployment.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0, no_outpost_mvo()));
   BOOST_REQUIRE(setoutpost("WIRE", no_outpost_mvo()).find("no remote deployment") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── Two same-kind EVM outposts each keep their own distinct deployment ──
// (the registry half of WSA-075: both operator daemons read these per row.)
BOOST_FIXTURE_TEST_CASE(two_evm_outposts_keep_distinct_bindings, sysio_chains_tester) { try {
   constexpr auto BASE_OPP     = "0x1111111111111111111111111111111111111111";
   constexpr auto BASE_INBOUND = "0x2222222222222222222222222222222222222222";
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1,
      evm_outpost_mvo(EVM_OPP, EVM_INBOUND, EVM_OPREG, EVM_DEPOSIT)));
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "BASE", 8453,
      evm_outpost_mvo(BASE_OPP, BASE_INBOUND, EVM_OPREG, EVM_DEPOSIT)));

   BOOST_REQUIRE_EQUAL(std::string(EVM_OPP),  stored_addr("ETH",  "opp_addr"));
   BOOST_REQUIRE_EQUAL(std::string(BASE_OPP), stored_addr("BASE", "opp_addr"));
   // Distinct external_chain_id is what the batch operator matches its RPC client on.
   BOOST_REQUIRE_EQUAL(1u,    get_chain("ETH")["external_chain_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(8453u, get_chain("BASE")["external_chain_id"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
