#include <sysio/snapshot_api_plugin/snapshot_catalog.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <map>

namespace snapshot_attest = sysio::protocol::snapshot_attestation;

BOOST_AUTO_TEST_SUITE(snapshot_api_plugin_tests)

/** A newer manual snapshot cannot hide the newest scheduled snapshot from discovery. */
BOOST_AUTO_TEST_CASE(latest_scheduled_snapshot_skips_newer_manual_entries) {
   constexpr uint32_t first_scheduled_block = snapshot_attest::block_spacing;
   constexpr uint32_t latest_scheduled_block = snapshot_attest::block_spacing * 2;
   constexpr uint32_t newer_manual_block = latest_scheduled_block + 1;

   std::map<uint32_t, uint32_t> catalog{
      {first_scheduled_block, first_scheduled_block},
      {latest_scheduled_block, latest_scheduled_block},
      {newer_manual_block, newer_manual_block},
   };

   auto latest = sysio::snapshot_api::find_latest_scheduled_snapshot(catalog);
   BOOST_REQUIRE(latest != catalog.rend());
   BOOST_CHECK_EQUAL(latest->first, latest_scheduled_block);

   catalog.erase(first_scheduled_block);
   catalog.erase(latest_scheduled_block);
   latest = sysio::snapshot_api::find_latest_scheduled_snapshot(catalog);
   BOOST_CHECK(latest == catalog.rend());
}

BOOST_AUTO_TEST_SUITE_END()
