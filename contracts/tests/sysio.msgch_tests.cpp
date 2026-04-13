#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.pb.h>
#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include <sysio/chain/action.hpp>

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
         "batchop4"_n, "batchop5"_n, "batchop.a"_n,
         "batchop.b"_n
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
      return push_msgch_action(signer, action_name, vector<permission_level>{{signer, config::active_name}}, data);
   }
   action_result push_msgch_action(name signer, name action_name, std::vector<permission_level> auths, const variant_object& data) {
      base_tester::push_action(MSGCH_ACCOUNT, action_name, std::move(auths), data);
      return success();
   }

   action_result deliver(name op, uint64_t outpost_id,
                         std::vector<char> data = {}) {
      return push_msgch_action(op, "deliver"_n, mvo()
         ("batch_op_name", op)
         ("outpost_id", outpost_id)
         ("data", data)
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
      return push_msgch_action(MSGCH_ACCOUNT, "buildenv"_n, {{
         EPOCH_ACCOUNT, config::active_name
      }}, mvo()
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
BOOST_FIXTURE_TEST_CASE(deliver_invalid_request, sysio_msgch_tester) { try {
   opp::Envelope env;
   env.set_epoch_envelope_index(1);
   env.set_epoch_timestamp(1775612516983);

   std::vector<char> data(env.ByteSizeLong());
   env.SerializeToArray(data.data(), static_cast<int>(data.size()));
   BOOST_REQUIRE_EXCEPTION(
      deliver("batchop1"_n, 999, data),
      sysio_assert_message_exception,
      sysio_assert_message_is("epoch state not initialized")

   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(queueout_basic, sysio_msgch_tester) { try {
   // AttestationType: EPOCH_SYNC = 60940
   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(buildenv_basic, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), buildenv(0));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
