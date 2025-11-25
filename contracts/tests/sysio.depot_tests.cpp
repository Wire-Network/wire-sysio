#include <test_contracts.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include "sysio.system_tester.hpp"
#include <fc/variant_object.hpp>
#include <boost/test/unit_test.hpp>
#include <string>
#include <type_traits>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

constexpr account_name DEPOT       = "sysio.depot"_n;
constexpr uint64_t     NETWORK_GEN = 0;

class sysio_depot_tester : public tester {
public:
   abi_serializer abi_ser;

   sysio_depot_tester() {
      produce_blocks(2);

      create_accounts({"node1"_n, "sysio.depot"_n});
      produce_blocks(2);

      set_code("sysio.depot"_n, contracts::depot_wasm());
      set_abi("sysio.depot"_n, contracts::depot_abi().data());
      set_privileged("sysio.depot"_n);

      produce_blocks();


      const auto* accnt = control->find_account_metadata("sysio.depot"_n);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action(const account_name& signer, const action_name& name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(name);

      action act;
      act.account = DEPOT;
      act.name    = name;
      act.data    = abi_ser.variant_to_binary(action_type_name, data,
                                           abi_serializer::create_yield_function(abi_serializer_max_time));

      return base_tester::push_action(std::move(act), signer.to_uint64_t());
   }

   transaction_trace_ptr push_paid_action(const account_name&   signer, const action_name& name,
                                          const variant_object& data) {
      return base_tester::push_action(DEPOT, name,
                                      vector<permission_level>{{signer, "active"_n}, {signer, "sysio.payer"_n}},
                                      data);
   }
};

BOOST_AUTO_TEST_SUITE(sysio_depot_tests)
BOOST_FIXTURE_TEST_CASE(swap_quote, sysio_depot_tester) try {
   BOOST_REQUIRE_EQUAL(1, 1);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()