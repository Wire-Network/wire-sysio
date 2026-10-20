#include "../src/role_config.hpp"

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace sysio::batch_operator_detail;

BOOST_AUTO_TEST_SUITE(role_config_tests)

BOOST_AUTO_TEST_CASE(a_permission_level_defaults_to_active) {
   const auto level = parse_permission_level("uw.acct");
   BOOST_CHECK_EQUAL(std::string("uw.acct"), level.actor.to_string());
   BOOST_CHECK_EQUAL(std::string("active"), level.permission.to_string());

   const auto custom = parse_permission_level("uw.acct@underwrite");
   BOOST_CHECK_EQUAL(std::string("uw.acct"), custom.actor.to_string());
   BOOST_CHECK_EQUAL(std::string("underwrite"), custom.permission.to_string());
}

BOOST_AUTO_TEST_CASE(a_permission_level_needs_an_account_and_a_permission) {
   BOOST_CHECK_THROW(parse_permission_level(""), fc::exception);
   BOOST_CHECK_THROW(parse_permission_level("@active"), fc::exception);
   BOOST_CHECK_THROW(parse_permission_level("uw.acct@"), fc::exception);
   BOOST_CHECK_THROW(parse_permission_level("Uw.Acct"), fc::exception);           // not a name
   BOOST_CHECK_THROW(parse_permission_level("uw.acct@Active"), fc::exception);   // not a name
}

BOOST_AUTO_TEST_CASE(exposure_caps_are_keyed_by_symbol_code) {
   const auto caps = parse_exposure_caps({"100.000000000 LIQETH", "5.000000000 LIQSOL"});
   BOOST_REQUIRE_EQUAL(2u, caps.size());
   const auto liqeth = sysio::chain::asset::from_string("100.000000000 LIQETH");
   const auto found  = caps.find(liqeth.get_symbol().to_symbol_code());
   BOOST_REQUIRE(found != caps.end());
   BOOST_CHECK(liqeth == found->second);
   BOOST_CHECK(parse_exposure_caps({}).empty());
}

BOOST_AUTO_TEST_CASE(an_exposure_cap_must_be_positive_well_formed_and_named_once) {
   BOOST_CHECK_THROW(parse_exposure_caps({"0.000000000 LIQETH"}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_caps({"-1.000000000 LIQETH"}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_caps({"100 liqeth"}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_caps({"many LIQETH"}), fc::exception);
   // The same symbol twice, even at another precision.
   BOOST_CHECK_THROW(parse_exposure_caps({"1.000000000 LIQETH", "2.0 LIQETH"}), fc::exception);
}

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
