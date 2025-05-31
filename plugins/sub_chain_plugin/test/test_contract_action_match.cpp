#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/sub_chain_plugin/contract_action_match.hpp>
#include <sysio/chain/exceptions.hpp>

BOOST_AUTO_TEST_SUITE(contract_action_match_tests)

using namespace sysio;
using namespace sysio::chain;

BOOST_AUTO_TEST_CASE(exact_match) {
   fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::info);
   using namespace sysio;
   using namespace sysio::chain;
   // Test the suffix matcher
   contract_action_match matcher("s"_n, "test"_n, contract_action_match::match_type::exact);

   BOOST_CHECK(matcher.is_contract_match("test"_n));
   BOOST_CHECK_EQUAL("test."_n, "test"_n);                // any '.' at the end of a name is really a non-character
   BOOST_CHECK(matcher.is_contract_match("test."_n));     // which is why this also passes

   BOOST_CHECK(!matcher.is_contract_match(""_n));
   BOOST_CHECK(!matcher.is_contract_match("est"_n));
   BOOST_CHECK(!matcher.is_contract_match("tests"_n));
   BOOST_CHECK(!matcher.is_contract_match(".test"_n));
   BOOST_CHECK(!matcher.is_contract_match("fun.test"_n));
   BOOST_CHECK(!matcher.is_contract_match("test.fun"_n));
}

BOOST_AUTO_TEST_CASE(suffix_match) {
   fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::info);
   using namespace sysio;
   using namespace sysio::chain;
   // Test the suffix match matcher
   contract_action_match matcher("s"_n, "test"_n, contract_action_match::match_type::suffix);

   BOOST_CHECK(matcher.is_contract_match("test"_n));
   BOOST_CHECK(matcher.is_contract_match(".test"_n));
   BOOST_CHECK(matcher.is_contract_match("fun.test"_n));
   BOOST_CHECK(matcher.is_contract_match("fun.fun.test"_n));
   BOOST_CHECK(matcher.is_contract_match("fun...test"_n));
   BOOST_CHECK_EQUAL("test."_n, "test"_n);                // any '.' at the end of a name is really a non-character
   BOOST_CHECK(matcher.is_contract_match("test."_n));     // which is why this also passes

   BOOST_CHECK(!matcher.is_contract_match(""_n));
   BOOST_CHECK(!matcher.is_contract_match("est"_n));
   BOOST_CHECK(!matcher.is_contract_match("funtest"_n));
   BOOST_CHECK(!matcher.is_contract_match("testfun"_n));
}

BOOST_AUTO_TEST_CASE(prefix_match) {
   fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::info);
   // Test the suffix match matcher
   contract_action_match matcher("s"_n, "test"_n, contract_action_match::match_type::prefix);

   BOOST_CHECK(matcher.is_contract_match("test"_n));
   BOOST_CHECK(matcher.is_contract_match("test."_n));
   BOOST_CHECK(matcher.is_contract_match("test.fun"_n));
   BOOST_CHECK(matcher.is_contract_match("test...fun"_n));   //passes because "test.."_n is just "test"_n

   BOOST_CHECK(!matcher.is_contract_match(""_n));
   BOOST_CHECK(!matcher.is_contract_match("tes"_n));
   BOOST_CHECK(!matcher.is_contract_match("testfun"_n));
   BOOST_CHECK(!matcher.is_contract_match("test.fun.fun"_n));
   BOOST_CHECK(!matcher.is_contract_match("testfun"_n));

   contract_action_match matcher2("s"_n, "test.fun"_n, contract_action_match::match_type::prefix);
   BOOST_CHECK(matcher2.is_contract_match("test.fun.fun"_n));
   BOOST_CHECK(!matcher2.is_contract_match("test.fun"_n));
}

BOOST_AUTO_TEST_CASE(malformed_contract_names) {
   fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::info);

   // Test no empty names
   BOOST_CHECK_EXCEPTION(contract_action_match("s"_n, ""_n, contract_action_match::match_type::prefix), chain::producer_exception,
      [&](const fc::exception &e) {
         return testing::expect_assert_message(e, "contract_match_name cannot be empty");
      });
   BOOST_CHECK_EXCEPTION(contract_action_match("s"_n, ""_n, contract_action_match::match_type::exact), chain::producer_exception,
      [&](const fc::exception &e) {
         return testing::expect_assert_message(e, "contract_match_name cannot be empty");
      });
   BOOST_CHECK_EXCEPTION(contract_action_match("s"_n, ""_n, contract_action_match::match_type::suffix), chain::producer_exception,
      [&](const fc::exception &e) {
         return testing::expect_assert_message(e, "contract_match_name cannot be empty");
      });

   // test suffix cannot have '.'
   BOOST_CHECK_EXCEPTION(contract_action_match("s"_n, ".test"_n, contract_action_match::match_type::suffix), chain::producer_exception,
      [&](const fc::exception &e) {
         return testing::expect_assert_message(e, "contract_match_name must be the desired suffix and should not contain any '.'.");
      });

   // test doesn't support type of any
   BOOST_CHECK_EXCEPTION(contract_action_match("s"_n, "test"_n, contract_action_match::match_type::any), chain::producer_exception,
      [&](const fc::exception &e) {
         return testing::expect_assert_message(e, "contract_action_match does not support the given type: 3");
      });
   }

   BOOST_AUTO_TEST_CASE(action_match) {
      fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::info);
      // Test the suffix match matcher
      contract_action_match matcher("s"_n, "test"_n, contract_action_match::match_type::prefix);
      matcher.add_action("add.match"_n, contract_action_match::match_type::exact);

      BOOST_CHECK(matcher.is_contract_match("test"_n));

      BOOST_CHECK(matcher.is_action_match("add.match"_n));
      BOOST_CHECK(!matcher.is_action_match("add"_n));
      BOOST_CHECK(!matcher.is_action_match("match"_n));

      BOOST_CHECK(!matcher.is_action_match("big"_n));
      BOOST_CHECK(!matcher.is_action_match("blue"_n));

      matcher.add_action("big"_n, contract_action_match::match_type::prefix);

      BOOST_CHECK(matcher.is_action_match("add.match"_n));
      BOOST_CHECK(matcher.is_action_match("big"_n));

      BOOST_CHECK(!matcher.is_action_match("blue"_n));
      BOOST_CHECK(!matcher.is_action_match("big.blue"_n));
      BOOST_CHECK(!matcher.is_action_match("bi"_n));

      BOOST_CHECK(!matcher.is_action_match("red"_n));
      BOOST_CHECK(!matcher.is_action_match("kite"_n));

      matcher.add_action("kite"_n, contract_action_match::match_type::suffix);

      BOOST_CHECK(matcher.is_action_match("add.match"_n));
      BOOST_CHECK(matcher.is_action_match("kite"_n));
      BOOST_CHECK(matcher.is_action_match("big"_n));

      BOOST_CHECK(!matcher.is_action_match("red"_n));

      BOOST_CHECK(!matcher.is_action_match(""_n));

      // any type means that all of the actions will match for the given contract
      matcher.add_action(""_n, contract_action_match::match_type::any);
      BOOST_CHECK(matcher.is_action_match("add.match"_n));
      BOOST_CHECK(matcher.is_action_match("kite"_n));
      BOOST_CHECK(matcher.is_action_match("big"_n));
      BOOST_CHECK(matcher.is_action_match("blue"_n));
      BOOST_CHECK(matcher.is_action_match("big.blue"_n));
      BOOST_CHECK(matcher.is_action_match("ig"_n));
      BOOST_CHECK(matcher.is_action_match("blu"_n));
      BOOST_CHECK(matcher.is_action_match("aaaaaaaaaaaaa"_n));
   }

BOOST_AUTO_TEST_SUITE_END()
