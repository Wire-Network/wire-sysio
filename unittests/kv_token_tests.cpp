#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <test_contracts.hpp>
#include <sysio/chain/asset.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(kv_token_tests)

struct kv_token_tester : validating_tester {
   kv_token_tester() {
      create_accounts({"sysio.token"_n, "alice"_n, "bob"_n, "carol"_n});
      produce_block();

      set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
      set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
      produce_block();
   }

   void create_token(name issuer, asset max_supply) {
      push_action("sysio.token"_n, "create"_n, "sysio.token"_n,
         fc::mutable_variant_object()("issuer", issuer)("maximum_supply", max_supply));
   }

   void issue(name to, asset quantity, std::string memo = "") {
      push_action("sysio.token"_n, "issue"_n, to,
         fc::mutable_variant_object()("to", to)("quantity", quantity)("memo", memo));
   }

   void transfer(name from, name to, asset quantity, std::string memo = "") {
      push_action("sysio.token"_n, "transfer"_n, from,
         fc::mutable_variant_object()("from", from)("to", to)("quantity", quantity)("memo", memo));
   }

   void retire(name issuer, asset quantity, std::string memo = "") {
      push_action("sysio.token"_n, "retire"_n, issuer,
         fc::mutable_variant_object()("quantity", quantity)("memo", memo));
   }

   void open(name owner, symbol sym, name payer) {
      push_action("sysio.token"_n, "open"_n, payer,
         fc::mutable_variant_object()("owner", owner)("symbol", sym)("ram_payer", payer));
   }

   void close(name owner, symbol sym) {
      push_action("sysio.token"_n, "close"_n, owner,
         fc::mutable_variant_object()("owner", owner)("symbol", sym));
   }
};

BOOST_FIXTURE_TEST_CASE(kv_token_create_issue_transfer, kv_token_tester) {
   auto sym = symbol(SY(4, WIRE));

   // Create token
   create_token("alice"_n, asset(1000000000, sym));
   produce_block();

   // Issue to alice
   issue("alice"_n, asset(10000000, sym), "issue");
   produce_block();

   // Transfer alice → bob
   transfer("alice"_n, "bob"_n, asset(1000000, sym), "hello");
   produce_block();

   // Transfer bob → carol
   transfer("bob"_n, "carol"_n, asset(500000, sym), "fwd");
   produce_block();

   // Verify we can do multiple transfers
   for (int i = 0; i < 10; ++i) {
      transfer("alice"_n, "bob"_n, asset(1000, sym), "batch" + std::to_string(i));
   }
   produce_block();

   BOOST_TEST_MESSAGE("All token operations succeeded with KV-backed sysio.token");
}

BOOST_FIXTURE_TEST_CASE(kv_token_retire, kv_token_tester) {
   auto sym = symbol(SY(4, WIRE));

   create_token("alice"_n, asset(1000000000, sym));
   produce_block();

   issue("alice"_n, asset(10000000, sym));
   produce_block();

   // Retire some tokens
   retire("alice"_n, asset(5000000, sym), "burn");
   produce_block();

   BOOST_TEST_MESSAGE("Token retire succeeded with KV-backed sysio.token");
}

BOOST_FIXTURE_TEST_CASE(kv_token_open_close, kv_token_tester) {
   auto sym = symbol(SY(4, WIRE));

   create_token("alice"_n, asset(1000000000, sym));
   produce_block();

   // Open account for bob (alice pays)
   open("bob"_n, sym, "alice"_n);
   produce_block();

   // Close bob's zero-balance account
   close("bob"_n, sym);
   produce_block();

   BOOST_TEST_MESSAGE("Token open/close succeeded with KV-backed sysio.token");
}

BOOST_FIXTURE_TEST_CASE(kv_token_overdrawn_check, kv_token_tester) {
   auto sym = symbol(SY(4, WIRE));

   create_token("alice"_n, asset(1000000000, sym));
   issue("alice"_n, asset(10000, sym));
   produce_block();

   // Try to overdraw — should fail
   BOOST_CHECK_THROW(
      transfer("alice"_n, "bob"_n, asset(20000, sym)),
      sysio_assert_message_exception
   );
}

BOOST_FIXTURE_TEST_CASE(kv_token_duplicate_create, kv_token_tester) {
   auto sym = symbol(SY(4, WIRE));

   create_token("alice"_n, asset(1000000000, sym));
   produce_block();

   // Duplicate create — should fail
   BOOST_CHECK_THROW(
      create_token("alice"_n, asset(1000000000, sym)),
      sysio_assert_message_exception
   );
}

BOOST_AUTO_TEST_SUITE_END()
