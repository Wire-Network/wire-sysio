/**
 * @file test_get_table_rows_page.cpp
 * @brief Unit tests for `chain_apis::read_only::get_table_rows` behaviours layered over `get_table_rows_page`.
 *
 * These cases pin down the wrapper-level fields on `get_table_rows_params`:
 *
 * - `all_rows` walks the entire scan in a single call, ignoring `limit`, `time_limit_ms`, and the caller deadline.
 *   C++-only (not FC_REFLECT'd) so HTTP cannot trigger unbounded server work.
 * - `values_only` strips the `{key, value, payer?}` wrapper, returning just the value side of each row.
 * - `filter` drops rows post-`values_only`; the predicate sees the same shape the caller will consume.
 * - `reverse` + a small `limit` returns the latest N rows without skipping a row at every page boundary (the
 *   reverse-off-by-one fix in `get_table_rows_page`).
 *
 * Each case uses the `get_table_test` multi_index contract already wired up for `get_table_tests.cpp`.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/testing/tester.hpp>

#include <test_contracts.hpp>

#include <fc/variant_object.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::chain_apis;
using namespace sysio::testing;

namespace {
   /// Contract account that hosts the `numobjs` test table. Matches the pattern used in
   /// `get_table_tests.cpp::get_table_next_key_test`.
   constexpr auto TEST_ACCOUNT = "test";

   /// Build a read-only API instance against the tester's controller. Both timeouts are `maximum()` so we never
   /// fail a test case on RPC deadline.
   read_only make_read_only(validating_tester& t) {
      std::optional<sysio::chain_apis::tracked_votes> tv;
      return read_only(*(t.control), {}, {}, tv,
                       fc::microseconds::maximum(),
                       fc::microseconds::maximum(),
                       {});
   }

   /// Deploy the `get_table_test` contract to `test` and produce a block.
   void deploy_contract(validating_tester& t) {
      t.create_account("test"_n);
      t.set_code("test"_n, test_contracts::get_table_test_wasm());
      t.set_abi("test"_n,  test_contracts::get_table_test_abi());
      t.produce_block();
   }

   /// Populate the `numobjs` multi_index with `count` rows via the contract's `addnumobj` action. Each row gets
   /// auto-incremented primary key `i` and `sec64 == i` -- matches what tests assert on. Produces a block every
   /// `ROWS_PER_BLOCK` actions so large populations don't exceed the block CPU budget.
   void populate_numobjs(validating_tester& t, uint32_t count) {
      constexpr uint32_t ROWS_PER_BLOCK = 50;
      for (uint32_t i = 0; i < count; ++i) {
         t.push_action("test"_n, "addnumobj"_n, "test"_n,
                       fc::mutable_variant_object()("input", i));
         if (((i + 1) % ROWS_PER_BLOCK) == 0) {
            t.produce_block();
         }
      }
      t.produce_block();
   }

   /// Build a starter params pointed at `test::numobjs`. Caller mutates the fields they care about.
   read_only::get_table_rows_params numobjs_params() {
      read_only::get_table_rows_params p;
      p.code  = "test"_n;
      p.scope = TEST_ACCOUNT;
      p.table = "numobjs";
      return p;
   }

   /// Run a deferred get_table_rows to completion and return the decoded result. Fails the test if the RPC
   /// produced an exception_ptr.
   read_only::get_table_rows_result
   run(read_only& ro, const read_only::get_table_rows_params& p) {
      auto variant = ro.get_table_rows(p, fc::time_point::maximum())();
      BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(variant));
      return std::get<read_only::get_table_rows_result>(std::move(variant));
   }
} // namespace

BOOST_AUTO_TEST_SUITE(table_reader_tests)

// Baseline: the classic single-page path is unchanged. `limit=50` default returns up to 50 rows with the
// HTTP-friendly `{key, value, payer?}` wrap.
BOOST_FIXTURE_TEST_CASE(default_limit_is_single_page_wrapped, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 7);

   auto ro  = make_read_only(*this);
   auto res = run(ro, numobjs_params());

   BOOST_REQUIRE_EQUAL(res.rows.size(), 7u);
   // Rows are WRAPPED -- each is a `{key, value, payer?}` object; callers read `row["value"]["sec64"]` etc.
   BOOST_REQUIRE(res.rows[0].is_object());
   BOOST_CHECK(res.rows[0].get_object().contains("value"));
   BOOST_CHECK(res.rows[0].get_object().contains("key"));
   for (uint32_t i = 0; i < 7; ++i) {
      BOOST_CHECK_EQUAL(
         res.rows[i].get_object()["value"].get_object()["sec64"].as_uint64(), i);
   }
} FC_LOG_AND_RETHROW()

// `all_rows = true` walks every row in one call regardless of `limit`. Populate a row count well over the default
// `limit` of 50 to confirm the cap is ignored when `all_rows` is set.
BOOST_FIXTURE_TEST_CASE(all_rows_walks_every_row, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 205);

   auto p = numobjs_params();
   p.all_rows = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 205u);
   BOOST_CHECK(!res.more);
   BOOST_CHECK(res.next_key.empty());
   for (uint32_t i = 0; i < 205; ++i) {
      BOOST_CHECK_EQUAL(
         res.rows[i].get_object()["value"].get_object()["key"].as_uint64(), i);
   }
} FC_LOG_AND_RETHROW()

// `values_only` strips the `{key, value, payer}` wrapper, leaving just the contract-side value. Default stays
// wrapped (asserted above) so HTTP clients are unaffected.
BOOST_FIXTURE_TEST_CASE(values_only_strips_wrapper, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 3);

   auto p = numobjs_params();
   p.values_only = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
   for (size_t i = 0; i < res.rows.size(); ++i) {
      BOOST_REQUIRE(res.rows[i].is_object());
      // Unwrapped: value fields are directly addressable.
      BOOST_CHECK(res.rows[i].get_object().contains("sec64"));
      BOOST_CHECK(!res.rows[i].get_object().contains("value"));
      BOOST_CHECK_EQUAL(res.rows[i].get_object()["sec64"].as_uint64(), i);
   }
} FC_LOG_AND_RETHROW()

// Filter drops rows post-`values_only`. The predicate sees the same shape the caller will consume.
BOOST_FIXTURE_TEST_CASE(filter_drops_unmatched_rows, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 10);

   auto p = numobjs_params();
   p.values_only = true;
   p.filter = [](const fc::variant& row) {
      return (row.get_object()["sec64"].as_uint64() % 2) == 0;
   };

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 5u); // 0, 2, 4, 6, 8
   for (const auto& row : res.rows) {
      BOOST_CHECK_EQUAL(row.get_object()["sec64"].as_uint64() % 2, 0u);
   }
} FC_LOG_AND_RETHROW()

// `reverse` + a small `limit` returns the LATEST N rows. This is the exact pattern used by
// `batch_operator_plugin::read_pending_outbound`; a regression implies the epoch stall has returned.
BOOST_FIXTURE_TEST_CASE(reverse_with_limit_returns_latest, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 12);

   auto p = numobjs_params();
   p.reverse     = true;
   p.limit       = 3;
   p.values_only = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
   BOOST_CHECK_EQUAL(res.rows[0].get_object()["key"].as_uint64(), 11u);
   BOOST_CHECK_EQUAL(res.rows[1].get_object()["key"].as_uint64(), 10u);
   BOOST_CHECK_EQUAL(res.rows[2].get_object()["key"].as_uint64(),  9u);
} FC_LOG_AND_RETHROW()

// `reverse` + `all_rows` walks the table from highest to lowest key with no skips. The page-impl's reverse branch
// is what guarantees this -- a regression would show up as two consecutive rows differing by more than 1.
BOOST_FIXTURE_TEST_CASE(reverse_all_rows_no_skips, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 205);

   auto p = numobjs_params();
   p.reverse     = true;
   p.all_rows    = true;
   p.values_only = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 205u);
   for (size_t i = 0; i < res.rows.size(); ++i) {
      BOOST_CHECK_EQUAL(res.rows[i].get_object()["key"].as_uint64(), 204u - i);
   }
} FC_LOG_AND_RETHROW()

// Empty table + `all_rows` returns an empty result with no error.
BOOST_FIXTURE_TEST_CASE(empty_table_all_rows_is_empty, validating_tester) try {
   deploy_contract(*this); // no populate_numobjs call

   auto p = numobjs_params();
   p.all_rows = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);
   BOOST_CHECK_EQUAL(res.rows.size(), 0u);
   BOOST_CHECK(!res.more);
   BOOST_CHECK(res.next_key.empty());
} FC_LOG_AND_RETHROW()

// `find` + `index_name` is an exact-key lookup. With `all_rows` set the result must still be a single row --
// paginating past a single-key match makes no sense and the page-impl already returns `limit=1` worth naturally.
BOOST_FIXTURE_TEST_CASE(find_with_index_returns_single_match_even_when_all_rows,
                        validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 5); // keys/sec64 = 0..4

   auto p = numobjs_params();
   p.find        = R"({"bysec1":3})";
   p.index_name  = "bysec1";
   p.all_rows    = true;
   p.values_only = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 1u);
   BOOST_CHECK_EQUAL(res.rows[0].get_object()["sec64"].as_uint64(), 3u);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
