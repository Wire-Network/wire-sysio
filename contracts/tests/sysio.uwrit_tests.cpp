#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;

using mvo = fc::mutable_variant_object;

class sysio_uwrit_tester : public tester {
public:
   static constexpr auto UWRIT_ACCOUNT = "sysio.uwrit"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

   sysio_uwrit_tester() {
      produce_blocks(2);

      create_accounts({
         UWRIT_ACCOUNT, CHALG_ACCOUNT,
         "sysio.epoch"_n, "sysio.msgch"_n,
         "underwriter1"_n, "underwriter2"_n
      });
      produce_blocks(2);

      set_code(UWRIT_ACCOUNT, contracts::uwrit_wasm());
      set_abi(UWRIT_ACCOUNT, contracts::uwrit_abi().data());
      set_privileged(UWRIT_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(UWRIT_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_uwrit_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);

      action act;
      act.account = UWRIT_ACCOUNT;
      act.name = action_name;
      act.data = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function(abi_serializer_max_time)
      );
      act.authorization = vector<permission_level>{{signer, config::active_name}};

      signed_transaction trx;
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      trx.sign(get_private_key(signer, "active"), control->get_chain_id());
      try {
         push_transaction(trx);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result setconfig(uint32_t fee_bps = 10, uint32_t lock_sec = 86400,
                           uint32_t uw_pct = 50, uint32_t other_pct = 25,
                           uint32_t batch_pct = 25) {
      return push_uwrit_action(UWRIT_ACCOUNT, "setconfig"_n, mvo()
         ("fee_bps", fee_bps)
         ("confirm_lock_sec", lock_sec)
         ("uw_fee_share_pct", uw_pct)
         ("other_uw_share_pct", other_pct)
         ("batch_op_share_pct", batch_pct)
      );
   }

   action_result updcltrl(name uw, uint8_t chain_kind, asset amount, bool increase) {
      return push_uwrit_action(UWRIT_ACCOUNT, "updcltrl"_n, mvo()
         ("underwriter", uw)
         ("chain_kind", chain_kind)
         ("amount", amount)
         ("is_increase", increase)
      );
   }

   action_result submituw(name uw, uint64_t msg_id) {
      auto sig = fc::sha256::hash(std::string("sig"));
      return push_uwrit_action(uw, "submituw"_n, mvo()
         ("underwriter", uw)
         ("msg_id", msg_id)
         ("source_sig", sig)
         ("target_sig", sig)
      );
   }

   action_result confirmuw(uint64_t entry_id) {
      return push_uwrit_action(UWRIT_ACCOUNT, "confirmuw"_n, mvo()
         ("uw_entry_id", entry_id)
      );
   }

   action_result distfee(uint64_t entry_id) {
      return push_uwrit_action(UWRIT_ACCOUNT, "distfee"_n, mvo()
         ("uw_entry_id", entry_id)
      );
   }

   action_result slash(name uw, std::string reason) {
      return push_uwrit_action(CHALG_ACCOUNT, "slash"_n, mvo()
         ("underwriter", uw)
         ("reason", reason)
      );
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_uwrit_tests)

BOOST_FIXTURE_TEST_CASE(setconfig_basic, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_validates_percentages, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: fee share percentages must sum to 100"),
      setconfig(10, 86400, 50, 25, 30)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(updcltrl_increase, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());

   // ChainKind: ETHEREUM=2
   auto amount = asset::from_string("100.0000 SYS");
   BOOST_REQUIRE_EQUAL(success(), updcltrl("underwriter1"_n, 2, amount, true));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(updcltrl_decrease_nonexistent, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());

   auto amount = asset::from_string("50.0000 SYS");
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: cannot decrease non-existent collateral"),
      updcltrl("underwriter1"_n, 2, amount, false)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(submituw_without_config, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: underwriting config not initialized"),
      submituw("underwriter1"_n, 42)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(submituw_basic, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), submituw("underwriter1"_n, 100));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(submituw_duplicate_message, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), submituw("underwriter1"_n, 100));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: message already has underwriting entry"),
      submituw("underwriter2"_n, 100)
   );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
