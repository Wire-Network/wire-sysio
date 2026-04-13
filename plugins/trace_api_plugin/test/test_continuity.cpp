#include <boost/test/unit_test.hpp>

#include <sysio/trace_api/chain_extraction.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

struct continuity_mock_store {
   // first == nullopt means no data at all
   continuity_mock_store(std::optional<uint32_t> first_block, std::optional<uint32_t> last_block)
   : _first_block(first_block), _last_block(last_block) {}

   template <typename BlockTrace>
   void append(const BlockTrace&) {}
   void append_lib(uint32_t) {}
   void append_trx_ids(block_trxs_entry) {}

   std::optional<uint32_t> first_recorded_block() const { return _first_block; }
   std::optional<uint32_t> last_recorded_block()  const { return _last_block;  }

   std::optional<uint32_t> _first_block;
   std::optional<uint32_t> _last_block;
};

struct continuity_fixture {
   // Convenience: data [first, last] exists, or nullopt for empty store
   continuity_fixture(std::optional<uint32_t> first_block, std::optional<uint32_t> last_block)
   : store_first(first_block), store_last(last_block)
   {}

   bool try_block_start(uint32_t block_num) {
      bool threw = false;
      auto except = exception_handler{[&threw](const exception_with_context&) {
         threw = true;
         throw yield_exception("continuity error");
      }};
      chain_extraction_impl_type<continuity_mock_store> impl(
         continuity_mock_store{store_first, store_last}, std::move(except));
      try {
         impl.signal_block_start(block_num);
      } catch (const yield_exception&) {
         // expected on continuity error
      }
      return !threw;
   }

   std::optional<uint32_t> store_first;
   std::optional<uint32_t> store_last;
};

} // namespace

BOOST_AUTO_TEST_SUITE(continuity_tests)

   // Empty slice dir: any starting block is a fresh start, always succeeds
   BOOST_AUTO_TEST_CASE(fresh_start_block_1) {
      continuity_fixture f(std::nullopt, std::nullopt);
      BOOST_CHECK(f.try_block_start(1));
   }

   BOOST_AUTO_TEST_CASE(fresh_start_high_block) {
      continuity_fixture f(std::nullopt, std::nullopt);
      BOOST_CHECK(f.try_block_start(50'000'000));
   }

   // Exact continuation: chain head == last_recorded + 1
   BOOST_AUTO_TEST_CASE(exact_continuation_from_block_1) {
      continuity_fixture f(1, 1);
      BOOST_CHECK(f.try_block_start(2));
   }

   BOOST_AUTO_TEST_CASE(exact_continuation_mid_chain) {
      continuity_fixture f(1, 50'000'000);
      BOOST_CHECK(f.try_block_start(50'000'001));
   }

   // Overlap: chain head is within existing data range (snapshot older than trace end)
   // Should succeed — re-applied blocks will overwrite.
   BOOST_AUTO_TEST_CASE(overlap_at_first_recorded) {
      continuity_fixture f(500, 600);
      BOOST_CHECK(f.try_block_start(500)); // chain head == first recorded
   }

   BOOST_AUTO_TEST_CASE(overlap_mid_range) {
      continuity_fixture f(500, 600);
      BOOST_CHECK(f.try_block_start(550)); // chain head in middle of existing data
   }

   BOOST_AUTO_TEST_CASE(overlap_at_last_recorded) {
      continuity_fixture f(500, 600);
      BOOST_CHECK(f.try_block_start(600)); // chain head == last recorded (last block replay)
   }

   // Gap forward: chain head > last_recorded + 1, must error
   BOOST_AUTO_TEST_CASE(gap_forward_small) {
      continuity_fixture f(1, 100);
      BOOST_CHECK(!f.try_block_start(105));
   }

   BOOST_AUTO_TEST_CASE(gap_forward_large) {
      continuity_fixture f(1, 50'000'000);
      BOOST_CHECK(!f.try_block_start(60'000'000));
   }

   // Snapshot before trace data begins: chain head < first_recorded, must error
   BOOST_AUTO_TEST_CASE(snapshot_before_data_start) {
      continuity_fixture f(500, 600);
      BOOST_CHECK(!f.try_block_start(400)); // chain head < first recorded
   }

   BOOST_AUTO_TEST_CASE(snapshot_well_before_data_start) {
      continuity_fixture f(50'000'000, 60'000'000);
      BOOST_CHECK(!f.try_block_start(1));
   }

   // check_continuity called only once: subsequent block_start calls do not re-check
   BOOST_AUTO_TEST_CASE(check_only_on_first_block_start) {
      bool threw = false;
      auto except = exception_handler{[&threw](const exception_with_context&) {
         threw = true;
         throw yield_exception("continuity error");
      }};

      // fresh start, no prior data
      chain_extraction_impl_type<continuity_mock_store> impl(
         continuity_mock_store{std::nullopt, std::nullopt}, std::move(except));

      impl.signal_block_start(100); // first call: fresh start, succeeds
      BOOST_CHECK(!threw);

      impl.signal_block_start(200); // second call: no re-check, also succeeds
      BOOST_CHECK(!threw);
   }

BOOST_AUTO_TEST_SUITE_END()
