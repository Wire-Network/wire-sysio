/**
 * @file test_get_table_rows_page.cpp
 * @brief Unit tests for the extended `chain_apis::read_only::get_table_rows`.
 *
 * These tests pin down the behaviours folded onto `get_table_rows_params`
 * when the stall-at-epoch-25 bug was fixed:
 *
 * - `limit == 0` means "paginate through the entire scan" (a formerly-useless
 *   value; the fix repurposed it so the batch operator never re-creates a
 *   50-row clip bug).
 * - `unwrap_rows` strips the `{key, value, payer?}` wrapper.
 * - `filter` drops rows after unwrap.
 * - `reverse` + a small `limit` returns the latest N rows without skipping a
 *   row at every page boundary.
 *
 * Each case uses the `get_table_test` multi_index contract already wired up
 * for `get_table_tests.cpp`, so regressions surface as a failing assertion
 * rather than a live-cluster outage.
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
   /// Contract account that hosts the `numobjs` test table. Matches the
   /// pattern used in `get_table_tests.cpp::get_table_next_key_test`.
   constexpr auto TEST_ACCOUNT = "test";

   /// Build a read-only API instance against the tester's controller. Both
   /// timeouts are `maximum()` so we never fail a test case on RPC deadline.
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

   /// Populate the `numobjs` multi_index with `count` rows via the contract's
   /// `addnumobj` action. Each row gets auto-incremented primary key `i` and
   /// `sec64 == i` — matches what tests assert on. Produces a block every
   /// `ROWS_PER_BLOCK` actions so large populations don't exceed the block
   /// CPU budget.
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

   /// Build a starter params pointed at `test::numobjs`. Caller mutates the
   /// fields they care about.
   read_only::get_table_rows_params numobjs_params() {
      read_only::get_table_rows_params p;
      p.code  = "test"_n;
      p.scope = TEST_ACCOUNT;
      p.table = "numobjs";
      return p;
   }

   /// Run a deferred get_table_rows to completion and return the decoded
   /// result. Fails the test if the RPC produced an exception_ptr.
   read_only::get_table_rows_result
   run(read_only& ro, const read_only::get_table_rows_params& p) {
      auto variant = ro.get_table_rows(p, fc::time_point::maximum())();
      BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(variant));
      return std::get<read_only::get_table_rows_result>(std::move(variant));
   }
} // namespace

BOOST_AUTO_TEST_SUITE(table_reader_tests)

// ---------------------------------------------------------------------------
//  Baseline: the classic single-page path is unchanged. `limit=50` default
//  returns up to 50 rows with the HTTP-friendly `{key, value, payer?}` wrap.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(default_limit_is_single_page_wrapped, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 7);

   auto ro  = make_read_only(*this);
   auto res = run(ro, numobjs_params());

   BOOST_REQUIRE_EQUAL(res.rows.size(), 7u);
   // Rows are WRAPPED — each is a `{key, value, payer?}` object, callers read
   // `row["value"]["sec64"]` etc. (This is the HTTP surface behaviour; the
   // refactor must not accidentally break it.)
   BOOST_REQUIRE(res.rows[0].is_object());
   BOOST_CHECK(res.rows[0].get_object().contains("value"));
   BOOST_CHECK(res.rows[0].get_object().contains("key"));
   for (uint32_t i = 0; i < 7; ++i) {
      BOOST_CHECK_EQUAL(
         res.rows[i].get_object()["value"].get_object()["sec64"].as_uint64(), i);
   }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
//  `limit == 0` paginates through every row. Populate more rows than the
//  internal 100-row page size and assert every row comes back in order.
//  This is the behaviour whose absence caused the epoch-25 stall — a bare
//  50-row read dropped everything past id 49.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(limit_zero_paginates_all_rows, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 205); // crosses 2 internal pages + spills into a 3rd

   auto p = numobjs_params();
   p.limit = 0;

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

// ---------------------------------------------------------------------------
//  `unwrap_rows` strips the `{key, value, payer}` wrapper, leaving just the
//  contract-side value. Default stays wrapped (asserted above) so HTTP
//  clients are unaffected.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(unwrap_rows_returns_bare_values, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 3);

   auto p = numobjs_params();
   p.unwrap_rows = true;

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

// ---------------------------------------------------------------------------
//  Filter drops rows post-unwrap. Predicate sees the same shape the caller
//  will consume.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(filter_drops_unmatched_rows, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 10);

   auto p = numobjs_params();
   p.unwrap_rows = true;
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

// ---------------------------------------------------------------------------
//  `reverse` + a small `limit` returns the LATEST N rows. This is the exact
//  pattern used by `batch_operator_plugin::read_pending_outbound` after the
//  epoch-stall fix; a regression here implies the stall has returned.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(reverse_with_limit_returns_latest, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 12);

   auto p = numobjs_params();
   p.reverse     = true;
   p.limit       = 3;
   p.unwrap_rows = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
   BOOST_CHECK_EQUAL(res.rows[0].get_object()["key"].as_uint64(), 11u);
   BOOST_CHECK_EQUAL(res.rows[1].get_object()["key"].as_uint64(), 10u);
   BOOST_CHECK_EQUAL(res.rows[2].get_object()["key"].as_uint64(),  9u);
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
//  `reverse` + `limit == 0` walks the table in reverse from end to start.
//  The cursor advance has to use each page's LAST returned row as the next
//  `upper_bound` (not `next_key`, which is the first UNSEEN row below it —
//  setting `upper_bound = next_key` would skip a row per page boundary).
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(reverse_limit_zero_spans_pages_without_skips, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 205); // forces multi-page reverse walk

   auto p = numobjs_params();
   p.reverse     = true;
   p.limit       = 0;
   p.unwrap_rows = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 205u);
   // Strictly descending keys, no gaps. A page-boundary skip would show up
   // as two consecutive rows that differ by 2 (or more) instead of 1.
   for (size_t i = 0; i < res.rows.size(); ++i) {
      BOOST_CHECK_EQUAL(res.rows[i].get_object()["key"].as_uint64(), 204u - i);
   }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
//  Empty table + `limit == 0` returns an empty result with no error.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(empty_table_paginate_all_is_empty, validating_tester) try {
   deploy_contract(*this); // no populate_numobjs call

   auto p = numobjs_params();
   p.limit = 0;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);
   BOOST_CHECK_EQUAL(res.rows.size(), 0u);
   BOOST_CHECK(!res.more);
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
//  `find` + `index_name` is an exact-key lookup. With `limit == 0` the
//  helper must still return only the matching row — pagination past a
//  single-key match makes no sense.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(find_with_index_returns_single_match_even_when_paginate_all,
                        validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 5); // keys/sec64 = 0..4

   auto p = numobjs_params();
   p.find        = R"({"bysec1":3})";
   p.index_name  = "bysec1";
   p.limit       = 0; // should not loop past the single match
   p.unwrap_rows = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 1u);
   BOOST_CHECK_EQUAL(res.rows[0].get_object()["sec64"].as_uint64(), 3u);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
