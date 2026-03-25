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

class sysio_chalg_tester : public tester {
public:
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto UWRIT_ACCOUNT = "sysio.uwrit"_n;

   sysio_chalg_tester() {
      produce_blocks(2);

      create_accounts({
         CHALG_ACCOUNT, EPOCH_ACCOUNT, MSGCH_ACCOUNT, UWRIT_ACCOUNT,
         "sysio.msig"_n,
         "badop1"_n, "badop2"_n, "goodop1"_n,
         "resolver1"_n
      });
      produce_blocks(2);

      set_code(CHALG_ACCOUNT, contracts::chalg_wasm());
      set_abi(CHALG_ACCOUNT, contracts::chalg_abi().data());
      set_privileged(CHALG_ACCOUNT);

      // Deploy epoch and uwrit for inline action targets
      set_code(EPOCH_ACCOUNT, contracts::epoch_wasm());
      set_abi(EPOCH_ACCOUNT, contracts::epoch_abi().data());
      set_privileged(EPOCH_ACCOUNT);

      set_code(UWRIT_ACCOUNT, contracts::uwrit_wasm());
      set_abi(UWRIT_ACCOUNT, contracts::uwrit_abi().data());
      set_privileged(UWRIT_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(CHALG_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_chalg_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);

      action act;
      act.account = CHALG_ACCOUNT;
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

   action_result initchal(uint64_t chain_req_id) {
      return push_chalg_action(MSGCH_ACCOUNT, "initchal"_n, mvo()
         ("chain_req_id", chain_req_id)
      );
   }

   action_result submitresp(uint64_t challenge_id, fc::sha256 response_hash,
                            std::vector<name> correct, std::vector<name> faulty) {
      return push_chalg_action(CHALG_ACCOUNT, "submitresp"_n, mvo()
         ("challenge_id", challenge_id)
         ("response_hash", response_hash)
         ("correct_ops", correct)
         ("faulty_ops", faulty)
      );
   }

   action_result escalate(uint64_t challenge_id) {
      return push_chalg_action(CHALG_ACCOUNT, "escalate"_n, mvo()
         ("challenge_id", challenge_id)
      );
   }

   action_result submitres(name submitter, uint64_t challenge_id,
                           fc::sha256 orig, fc::sha256 r1, fc::sha256 r2) {
      return push_chalg_action(submitter, "submitres"_n, mvo()
         ("submitter", submitter)
         ("challenge_id", challenge_id)
         ("orig_hash", orig)
         ("r1_hash", r1)
         ("r2_hash", r2)
      );
   }

   action_result enforce(uint64_t resolution_id) {
      return push_chalg_action(CHALG_ACCOUNT, "enforce"_n, mvo()
         ("resolution_id", resolution_id)
      );
   }

   action_result slashop(name op, std::string reason) {
      return push_chalg_action(CHALG_ACCOUNT, "slashop"_n, mvo()
         ("operator_acct", op)
         ("reason", reason)
      );
   }

   fc::sha256 make_hash(const std::string& seed) {
      return fc::sha256::hash(seed);
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_chalg_tests)

BOOST_FIXTURE_TEST_CASE(initchal_basic, sysio_chalg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), initchal(0));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(initchal_requires_msgch_auth, sysio_chalg_tester) { try {
   // Direct call from non-msgch should fail
   BOOST_REQUIRE_EQUAL(
      error("missing authority of sysio.msgch"),
      push_chalg_action(CHALG_ACCOUNT, "initchal"_n, mvo()("chain_req_id", 0))
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(escalate_not_in_response_state, sysio_chalg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), initchal(0));

   // Challenge is in CHALLENGE_SENT, not RESPONSE_RECEIVED
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: challenge must be in RESPONSE_RECEIVED state to escalate"),
      escalate(0)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(submitres_not_escalated, sysio_chalg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), initchal(0));

   auto h = make_hash("test");
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: challenge must be escalated for manual resolution"),
      submitres("resolver1"_n, 0, h, h, h)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(enforce_not_found, sysio_chalg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: resolution not found"),
      enforce(999)
   );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
