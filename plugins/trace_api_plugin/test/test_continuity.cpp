#include <boost/test/unit_test.hpp>

#include <sysio/trace_api/chain_extraction.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

struct continuity_mock_store {
   // first == nullopt means no data at all
   continuity_mock_store(std::optional<uint32_t> first_block, std::optional<uint32_t> last_block,
                         std::optional<std::pair<uint32_t,uint32_t>> gap = std::nullopt)
   : _first_block(first_block), _last_block(last_block), _gap(gap) {}

   template <typename BlockTrace>
   void append(const BlockTrace&) {}
   void append_lib(uint32_t) {}
   void append_trx_ids(block_trxs_entry) {}
   void rollback_abis(uint32_t) {}

   std::optional<std::pair<uint32_t,uint32_t>> first_and_last_recorded_blocks() const {
      if (!_first_block) return std::nullopt;
      return std::make_pair(*_first_block, _last_block.value_or(*_first_block));
   }

   std::optional<std::pair<uint32_t,uint32_t>> find_index_slice_gap() const {
      return _gap;
   }

   std::optional<uint32_t> _first_block;
   std::optional<uint32_t> _last_block;
   std::optional<std::pair<uint32_t,uint32_t>> _gap;
};

struct continuity_fixture {
   // Convenience: data [first, last] exists, or nullopt for empty store.
   // Each try_block_start() constructs a fresh extractor instance -- models
   // an independent node startup.  For tests that need to assert the check
   // fires only once across multiple block_start signals on the SAME
   // extractor, do not use this fixture; build the extractor inline (see
   // check_only_on_first_block_start below).
   continuity_fixture(std::optional<uint32_t> first_block, std::optional<uint32_t> last_block,
                      std::optional<std::pair<uint32_t,uint32_t>> gap = std::nullopt)
   : store_first(first_block), store_last(last_block), store_gap(gap)
   {}

   bool try_block_start(uint32_t block_num) {
      bool threw = false;
      auto except = exception_handler{[&threw](const exception_with_context&) {
         threw = true;
         throw yield_exception("continuity error");
      }};
      chain_extraction_impl_type<continuity_mock_store> impl(
         continuity_mock_store{store_first, store_last, store_gap}, std::move(except));
      try {
         impl.signal_block_start(block_num);
      } catch (const yield_exception&) {
         // expected on continuity error
      }
      return !threw;
   }

   std::optional<uint32_t> store_first;
   std::optional<uint32_t> store_last;
   std::optional<std::pair<uint32_t,uint32_t>> store_gap;
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

   // Internal gap: middle slices missing, must error even though the chain head
   // lines up with the recorded first/last range.
   BOOST_AUTO_TEST_CASE(internal_gap_detected) {
      continuity_fixture f(1, 50'000, std::make_pair(20'000u, 29'999u));
      BOOST_CHECK(!f.try_block_start(50'001)); // exact continuation, but a hole inside
   }

   BOOST_AUTO_TEST_CASE(internal_gap_detected_on_overlap_start) {
      continuity_fixture f(1, 50'000, std::make_pair(20'000u, 29'999u));
      BOOST_CHECK(!f.try_block_start(40'000)); // overlap start, hole still fatal
   }

   BOOST_AUTO_TEST_CASE(no_internal_gap_passes) {
      continuity_fixture f(1, 50'000, std::nullopt);
      BOOST_CHECK(f.try_block_start(50'001));
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

   // The continuity check flips its "already checked" flag BEFORE running, so
   // subsequent block_start signals skip the check regardless of what the
   // except_handler did on the first one.  Verify via a non-throwing handler
   // (which would otherwise re-enter the check if the flag weren't set early).
   BOOST_AUTO_TEST_CASE(check_not_rerun_after_non_throwing_except_handler) {
      int handler_calls = 0;
      auto except = exception_handler{[&handler_calls](const exception_with_context&) {
         ++handler_calls;
         // deliberately do NOT throw -- simulates a handler that just logs
      }};

      // Prior data ending at 100; first block_start at 200 is a forward gap.
      chain_extraction_impl_type<continuity_mock_store> impl(
         continuity_mock_store{1, 100}, std::move(except));

      impl.signal_block_start(200); // gap detected; handler called once
      BOOST_CHECK_EQUAL(handler_calls, 1);

      impl.signal_block_start(300); // second call must NOT re-invoke the check
      BOOST_CHECK_EQUAL(handler_calls, 1);
   }

BOOST_AUTO_TEST_SUITE_END()
