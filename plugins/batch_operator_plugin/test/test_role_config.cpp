#include "../src/role_config.hpp"

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace sysio::batch_operator_detail;

BOOST_AUTO_TEST_SUITE(role_config_tests)

BOOST_AUTO_TEST_CASE(a_signer_is_chosen_only_when_exactly_one_provider_qualifies) {
   const std::vector<int> providers{1, 2, 3, 4};

   const auto none = choose_signer(providers, [](int p) { return p > 4; });
   BOOST_CHECK(!none.chosen.has_value());
   BOOST_CHECK_EQUAL(0u, none.matches);

   const auto one = choose_signer(providers, [](int p) { return p == 3; });
   BOOST_REQUIRE(one.chosen.has_value());
   BOOST_CHECK_EQUAL(3, *one.chosen);
   BOOST_CHECK_EQUAL(1u, one.matches);

   // Two would leave the choice to provider order.
   const auto two = choose_signer(providers, [](int p) { return p % 2 == 0; });
   BOOST_CHECK(!two.chosen.has_value());
   BOOST_CHECK_EQUAL(2u, two.matches);
}

BOOST_AUTO_TEST_SUITE_END()
