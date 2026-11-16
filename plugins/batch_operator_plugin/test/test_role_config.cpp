#include "../src/role_config.hpp"

#include <boost/test/unit_test.hpp>

#include <fc/crypto/private_key.hpp>

#include <array>
#include <string>
#include <string_view>
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

BOOST_AUTO_TEST_CASE(exposure_policies_are_keyed_by_symbol_code) {
   const auto policies = parse_exposure_policies({"100.000000000 LIQETH", "5.000000000 LIQSOL"}, {});
   BOOST_REQUIRE_EQUAL(2u, policies.size());
   const auto liqeth = sysio::chain::asset::from_string("100.000000000 LIQETH");
   const auto found  = policies.find(liqeth.get_symbol().to_symbol_code());
   BOOST_REQUIRE(found != policies.end());
   BOOST_CHECK(liqeth == std::get<sysio::chain::asset>(found->second));
   BOOST_CHECK(parse_exposure_policies({}, {}).empty());
}

BOOST_AUTO_TEST_CASE(an_exposure_cap_must_be_positive_well_formed_and_named_once) {
   BOOST_CHECK_THROW(parse_exposure_policies({"0.000000000 LIQETH"}, {}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_policies({"-1.000000000 LIQETH"}, {}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_policies({"100 liqeth"}, {}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_policies({"many LIQETH"}, {}), fc::exception);
   // The same symbol twice, even at another precision.
   BOOST_CHECK_THROW(parse_exposure_policies({"1.000000000 LIQETH", "2.0 LIQETH"}, {}), fc::exception);
}

/// Unlimited is explicit per symbol and may coexist with a finite cap for a different symbol.
BOOST_AUTO_TEST_CASE(unlimited_exposure_is_distinct_from_a_cap_and_an_absent_token) {
   const auto policies = parse_exposure_policies({"100.000000000 LIQETH"}, {"LIQSOL", "LIQBTC"});
   BOOST_REQUIRE_EQUAL(3u, policies.size());
   BOOST_CHECK(std::holds_alternative<sysio::chain::asset>(
      policies.at(sysio::chain::symbol(9, "LIQETH").to_symbol_code())));
   BOOST_CHECK(std::holds_alternative<unlimited_exposure>(
      policies.at(sysio::chain::symbol(9, "LIQSOL").to_symbol_code())));
   BOOST_CHECK(std::holds_alternative<unlimited_exposure>(
      policies.at(sysio::chain::symbol(9, "LIQBTC").to_symbol_code())));
   BOOST_CHECK(!policies.contains(sysio::chain::symbol(9, "OTHER").to_symbol_code()));
   BOOST_CHECK(parse_exposure_policies({}, {}).empty());
}

/// Reject ambiguous policies and malformed symbols before the node starts, even without an underwriter account.
BOOST_AUTO_TEST_CASE(unlimited_exposure_requires_unique_valid_symbols_without_finite_caps) {
   BOOST_CHECK_THROW(parse_exposure_policies({"100.000000000 LIQETH"}, {"LIQETH"}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_policies({"1.0 LIQETH"}, {"LIQETH"}), fc::exception);
   BOOST_CHECK_THROW(parse_exposure_policies({}, {"LIQETH", "LIQETH"}), fc::exception);
   for (const std::string spec : {"", "liqeth", "LIQ1", "LIQETH ", " LIQETH", "9,LIQETH", "1 LIQETH", "ABCDEFGH"}) {
      BOOST_CHECK_THROW(parse_exposure_policies({}, {spec}), fc::exception);
   }
   BOOST_CHECK_EQUAL(2u, parse_exposure_policies({}, {"A", "ABCDEFG"}).size());
}

/// Registry resolution preserves unlimited policies across precisions and refuses mismatched finite caps.
BOOST_AUTO_TEST_CASE(exposure_policies_resolve_to_shadow_token_codes) {
   const auto configured = parse_exposure_policies({"10.000 LIQSOL", "3.000 LIQBTC"}, {"LIQETH"});
   const fc::slug_name liqeth("ETHLIQ"), liqsol("SOLLIQ"), liqbtc("BTCLIQ");
   for (const uint8_t precision : {uint8_t{2}, uint8_t{9}, uint8_t{18}}) {
      unsigned mismatches = 0;
      const auto resolved = resolve_exposure_policies(
         configured, {{liqeth, sysio::chain::symbol(precision, "LIQETH")},
                      {liqsol, sysio::chain::symbol(9, "LIQSOL")}, {liqbtc, sysio::chain::symbol(3, "LIQBTC")}},
         [&](const sysio::chain::asset& cap, const sysio::chain::symbol& shadow) {
            ++mismatches;
            BOOST_CHECK_EQUAL(cap.to_string(), "10.000 LIQSOL");
            BOOST_CHECK_EQUAL(shadow.to_string(), "9,LIQSOL");
         },
         [](const char*, const std::string&) { BOOST_FAIL("all configured symbols name shadows"); });
      BOOST_REQUIRE_EQUAL(resolved.size(), 2u);
      BOOST_CHECK(std::holds_alternative<unlimited_exposure>(resolved.at(liqeth)));
      BOOST_CHECK_EQUAL(std::get<sysio::chain::asset>(resolved.at(liqbtc)).to_string(), "3.000 LIQBTC");
      BOOST_CHECK(!resolved.contains(liqsol));
      BOOST_CHECK_EQUAL(mismatches, 1u);
   }
}

/// Unknown finite and unlimited symbols report the option and value that the operator must correct.
BOOST_AUTO_TEST_CASE(unknown_exposure_symbols_report_their_own_option) {
   const auto configured = parse_exposure_policies({"10.000 LIQSOL"}, {"LIQETH"});
   std::map<std::string, std::string> diagnostics;
   const auto resolved = resolve_exposure_policies(
      configured, {},
      [](const sysio::chain::asset&, const sysio::chain::symbol&) { BOOST_FAIL("there are no shadows"); },
      [&](const char* option, const std::string& value) { BOOST_CHECK(diagnostics.emplace(option, value).second); });
   BOOST_CHECK(resolved.empty());
   BOOST_REQUIRE_EQUAL(diagnostics.size(), 2u);
   BOOST_CHECK_EQUAL(diagnostics.at("batch-underwriter-max-exposure"), "10.000 LIQSOL");
   BOOST_CHECK_EQUAL(diagnostics.at("batch-underwriter-unlimited-exposure"), "LIQETH");
}

/// A symbol-code name is precision-free and is also the canonical variant representation.
BOOST_AUTO_TEST_CASE(symbol_code_names_match_variant_serialization) {
   for (const std::string name : {"A", "LIQETH", "ABCDEFG"}) {
      const auto code = sysio::chain::symbol(9, name).to_symbol_code();
      BOOST_CHECK_EQUAL(code.name(), name);
      fc::variant serialized;
      fc::to_variant(code, serialized);
      BOOST_CHECK_EQUAL(serialized.as_string(), code.name());
      sysio::chain::symbol_code decoded;
      fc::from_variant(serialized, decoded);
      BOOST_CHECK_EQUAL(decoded.value, code.value);
   }
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

BOOST_AUTO_TEST_CASE(a_signer_passes_its_check_when_it_signs_for_its_key_or_is_briefly_unavailable) {
   const fc::crypto::private_key key   = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::k1);
   const fc::crypto::private_key other = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::k1);
   const fc::sha256              probe = fc::sha256::hash(std::string{"probe"});
   const auto provider = [&](fc::crypto::sign_fn sign) {
      return fc::crypto::signature_provider_t{fc::crypto::chain_kind_wire, fc::crypto::chain_key_type_wire, "wire-01",
                                              key.get_public_key(), std::nullopt, std::move(sign)};
   };

   BOOST_CHECK(signs_for_its_key("the relay", provider(fc::crypto::make_local_sign_fn(key)), probe));
   BOOST_CHECK(!signs_for_its_key("the relay", provider(fc::crypto::make_local_sign_fn(other)), probe));

   // A remote signer briefly unavailable says nothing about its key.
   BOOST_CHECK(signs_for_its_key("the relay", provider([](const fc::sha256&) -> fc::crypto::signature {
      FC_THROW_EXCEPTION(sysio::chain::signing_transient_exception, "throttled");
   }), probe));
   // A permanent failure does.
   BOOST_CHECK(!signs_for_its_key("the relay", provider([](const fc::sha256&) -> fc::crypto::signature {
      FC_THROW_EXCEPTION(sysio::chain::plugin_config_exception, "access denied");
   }), probe));
}

/// Review of #675: the relay required its permission to authorize `sysio.kicker::kick` even with
/// `--batch-kick-crank=false`, so an unsatisfied kick link stopped a node that would never push it.
BOOST_AUTO_TEST_CASE(a_disabled_crank_action_is_not_required_of_the_permission) {
   constexpr std::array always{pushed_action{"sysio.msgch", "deliver"}, pushed_action{"sysio.msgch", "chkcons"}};
   constexpr pushed_action kick{"sysio.kicker", "kick"};
   // The relay's permission may declare every action except the kick.
   const auto linked = [](const pushed_action& a) { return std::string_view(a.action) != "kick"; };

   const auto disabled = required_actions(always, std::array{optional_action{kick, false}});
   BOOST_CHECK_EQUAL(always.size(), disabled.size());
   BOOST_CHECK(unlinked_actions(disabled, linked).empty());   // startup resolves the relay's signer

   const auto enabled = required_actions(always, std::array{optional_action{kick, true}});
   BOOST_REQUIRE_EQUAL(always.size() + 1, enabled.size());
   const auto unlinked = unlinked_actions(enabled, linked);
   BOOST_REQUIRE_EQUAL(1u, unlinked.size());   // startup refuses, naming the action to link
   BOOST_CHECK_EQUAL(std::string("sysio.kicker"), unlinked.front().contract);
   BOOST_CHECK_EQUAL(std::string("kick"), unlinked.front().action);
}

BOOST_AUTO_TEST_SUITE_END()
