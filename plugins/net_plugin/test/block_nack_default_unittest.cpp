#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/net_utils.hpp>

using namespace sysio::net_utils;

BOOST_AUTO_TEST_SUITE(block_nack_default)

// A node with no producer configured keeps the bandwidth optimization.
BOOST_AUTO_TEST_CASE(default_enabled_for_non_producer) {
   constexpr bool explicitly_set = false;
   constexpr bool configured_producer = false;
   BOOST_CHECK_EQUAL(resolve_disable_block_nack(false, explicitly_set, configured_producer), false);
}

// A configured producer must never receive a notice in place of a block.
BOOST_AUTO_TEST_CASE(default_disabled_for_producer) {
   constexpr bool explicitly_set = false;
   constexpr bool configured_producer = true;
   BOOST_CHECK_EQUAL(resolve_disable_block_nack(false, explicitly_set, configured_producer), true);
}

// An operator turning it off on a producer must win over the producer default.
BOOST_AUTO_TEST_CASE(explicit_false_overrides_producer_default) {
   constexpr bool explicitly_set = true;
   constexpr bool configured_producer = true;
   BOOST_CHECK_EQUAL(resolve_disable_block_nack(false, explicitly_set, configured_producer), false);
}

// An operator turning it on for a non-producer must win over the non-producer default.
BOOST_AUTO_TEST_CASE(explicit_true_overrides_non_producer_default) {
   constexpr bool explicitly_set = true;
   constexpr bool configured_producer = false;
   BOOST_CHECK_EQUAL(resolve_disable_block_nack(true, explicitly_set, configured_producer), true);
}

// An explicit setting that matches the default is still honoured as explicit.
BOOST_AUTO_TEST_CASE(explicit_value_is_used_when_it_matches_the_default) {
   constexpr bool explicitly_set = true;
   BOOST_CHECK_EQUAL(resolve_disable_block_nack(true, explicitly_set, true), true);
   BOOST_CHECK_EQUAL(resolve_disable_block_nack(false, explicitly_set, false), false);
}

BOOST_AUTO_TEST_SUITE_END()
