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

class sysio_msgch_tester : public tester {
public:
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

   sysio_msgch_tester() {
      produce_blocks(2);

      create_accounts({
         MSGCH_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT,
         "batchop1"_n, "batchop2"_n, "batchop3"_n,
         "batchop4"_n, "batchop5"_n, "batchop6"_n,
         "batchop7"_n
      });
      produce_blocks(2);

      set_code(MSGCH_ACCOUNT, contracts::msgch_wasm());
      set_abi(MSGCH_ACCOUNT, contracts::msgch_abi().data());
      set_privileged(MSGCH_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(MSGCH_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_msgch_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);

      action act;
      act.account = MSGCH_ACCOUNT;
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

   action_result createreq(uint64_t outpost_id) {
      return push_msgch_action(MSGCH_ACCOUNT, "createreq"_n, mvo()
         ("outpost_id", outpost_id)
      );
   }

   action_result deliver(name op, uint64_t req_id,
                         fc::sha256 chain_hash, fc::sha256 merkle_root,
                         uint32_t msg_count, std::vector<char> raw = {}) {
      return push_msgch_action(op, "deliver"_n, mvo()
         ("operator_acct", op)
         ("req_id", req_id)
         ("chain_hash", chain_hash)
         ("merkle_root", merkle_root)
         ("msg_count", msg_count)
         ("raw_messages", raw)
      );
   }

   action_result evalcons(uint64_t req_id) {
      return push_msgch_action(MSGCH_ACCOUNT, "evalcons"_n, mvo()
         ("req_id", req_id)
      );
   }

   action_result queueout(uint64_t outpost_id, uint16_t attest_type, std::vector<char> data = {}) {
      return push_msgch_action(MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("outpost_id", outpost_id)
         ("attest_type", attest_type)
         ("data", data)
      );
   }

   action_result buildenv(uint64_t outpost_id) {
      return push_msgch_action(MSGCH_ACCOUNT, "buildenv"_n, mvo()
         ("outpost_id", outpost_id)
      );
   }

   fc::sha256 make_hash(const std::string& seed) {
      return fc::sha256::hash(seed);
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_msgch_tests)

BOOST_FIXTURE_TEST_CASE(createreq_basic, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), createreq(0));
   BOOST_REQUIRE_EQUAL(success(), createreq(1));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deliver_basic, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), createreq(0));

   auto hash = make_hash("test_chain_data");
   auto merkle = make_hash("test_merkle");
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop1"_n, 0, hash, merkle, 5));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deliver_duplicate_rejected, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), createreq(0));

   auto hash = make_hash("test_chain_data");
   auto merkle = make_hash("test_merkle");
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop1"_n, 0, hash, merkle, 5));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator already delivered for this request"),
      deliver("batchop1"_n, 0, hash, merkle, 5)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deliver_invalid_request, sysio_msgch_tester) { try {
   auto hash = make_hash("test");
   auto merkle = make_hash("merkle");
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: chain request not found"),
      deliver("batchop1"_n, 999, hash, merkle, 1)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(evalcons_consensus_ok_unanimous, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), createreq(0));

   auto hash = make_hash("identical_chain");
   auto merkle = make_hash("merkle");

   // All 7 operators deliver identical hash
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop1"_n, 0, hash, merkle, 10));
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop2"_n, 0, hash, merkle, 10));
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop3"_n, 0, hash, merkle, 10));
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop4"_n, 0, hash, merkle, 10));
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop5"_n, 0, hash, merkle, 10));
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop6"_n, 0, hash, merkle, 10));
   BOOST_REQUIRE_EQUAL(success(), deliver("batchop7"_n, 0, hash, merkle, 10));

   BOOST_REQUIRE_EQUAL(success(), evalcons(0));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(queueout_basic, sysio_msgch_tester) { try {
   // AttestationType: EPOCH_SYNC = 60940
   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(buildenv_basic, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), buildenv(0));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
