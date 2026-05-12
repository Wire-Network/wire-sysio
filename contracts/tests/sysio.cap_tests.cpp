#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

/// Test fixture for sysio.cap. Creates the contract account, deploys WASM /
/// ABI, and loads an ABI serializer. Reusable across `sysio.cap_tests` cases
/// that need the contract live but don't yet exercise inbound handlers
/// (those land once Q1 / Q5 resolutions arrive and the StakeUpdate /
/// StakingReward inbound paths are implemented).
class sysio_cap_tester : public tester {
public:
   static constexpr auto CAP_ACCOUNT    = "sysio.cap"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;

   sysio_cap_tester() {
      produce_blocks(2);

      create_accounts({
         CAP_ACCOUNT, EPOCH_ACCOUNT, TOKEN_ACCOUNT,
         "alice"_n, "bob"_n,
      });
      produce_blocks(2);

      set_code(CAP_ACCOUNT, contracts::cap_wasm());
      set_abi(CAP_ACCOUNT, contracts::cap_abi().data());
      set_privileged(CAP_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(CAP_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      cap_abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_cap_action(name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(CAP_ACCOUNT, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   abi_serializer cap_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_cap_tests)

BOOST_FIXTURE_TEST_CASE(setconfig_initializes_singleton, sysio_cap_tester) { try {
   const auto result = push_cap_action(CAP_ACCOUNT, "setconfig"_n, mvo{});
   BOOST_REQUIRE_EQUAL(result, success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(claim_rejects_empty_ledger, sysio_cap_tester) { try {
   const auto result = push_cap_action("alice"_n, "claim"_n, mvo()("wire_account", "alice"));
   BOOST_REQUIRE_NE(result, success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_accepts_credit_batch, sysio_cap_tester) { try {
   std::vector<char> addr(20, char(0xAB));
   const auto result = push_cap_action(CAP_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
      ("credits", fc::variants{
         mvo()("native_address", addr)("wire_atomic", 982953049502)
      })
   );
   BOOST_REQUIRE_EQUAL(result, success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_rejects_negative_atomic, sysio_cap_tester) { try {
   std::vector<char> addr(20, char(0xCD));
   const auto result = push_cap_action(CAP_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
      ("credits", fc::variants{
         mvo()("native_address", addr)("wire_atomic", -1)
      })
   );
   BOOST_REQUIRE_NE(result, success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importdone_locks_subsequent_importseed, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(
      push_cap_action(CAP_ACCOUNT, "importdone"_n, mvo{}),
      success());
   std::vector<char> addr(20, char(0xEF));
   const auto result = push_cap_action(CAP_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
      ("credits", fc::variants{
         mvo()("native_address", addr)("wire_atomic", 1)
      })
   );
   BOOST_REQUIRE_NE(result, success());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
