/**
 * @file test_get_table_rows_page.cpp
 * @brief Unit tests for `chain_apis::read_only::get_table_rows` covering the wrapper-level fields on
 *        `get_table_rows_params` (the suite is named `table_reader_tests`).
 *
 * These cases pin down:
 *
 * - `all_rows` walks the entire scan in a single call, ignoring `limit`, `time_limit_ms`, and the caller deadline.
 *   C++-only (not FC_REFLECT'd) so HTTP cannot trigger unbounded server work.
 * - `values_only` strips the `{key, value, payer?}` wrapper, returning just the value side of each row.
 * - `filter` drops rows post-`values_only`; the predicate sees the same shape the caller will consume.
 * - `reverse` + a small `limit` returns the latest N rows without skipping a row at every page boundary (pins the
 *   reverse-off-by-one fix).
 *
 * Each case uses the `get_table_test` multi_index contract already wired up for `get_table_tests.cpp`.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/testing/tester.hpp>

#include <test_contracts.hpp>

#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
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

// A finite deadline clamps the caller's `limit` to `api_base::max_return_items`, matching get_table_by_scope,
// so one HTTP page cannot stage an unbounded number of KV rows before the response byte budget is enforced.
// The remainder stays reachable through the returned `more`/`next_key` cursor. An unlimited deadline (the
// `-1` http-max-response-time-ms operator opt-in, modeled here by the fixture's `maximum()` deadline) is
// exempt, as is `all_rows` (asserted above) -- both leave `params_deadline` at `maximum()`. Only the deadline
// differs between the two runs below, so the differing outcomes isolate the clamp.
BOOST_FIXTURE_TEST_CASE(finite_deadline_clamps_limit_to_max_return_items, validating_tester) try {
   deploy_contract(*this);
   // A handful of rows past the cap is enough to prove the page stops at max_return_items with `more` set.
   constexpr uint32_t OVER_CAP  = read_only::max_return_items + 10;
   constexpr uint32_t BIG_LIMIT = 100'000; // far above both the cap and the populated row count
   populate_numobjs(*this, OVER_CAP);

   auto ro = make_read_only(*this);

   // Finite deadline -> clamp to max_return_items. `time_limit_ms` makes `params_deadline` finite even though
   // the fixture's HTTP deadline is `maximum()`; the generous 60s budget guarantees the count cap -- not the
   // clock -- is what stops the scan.
   auto capped = numobjs_params();
   capped.limit         = BIG_LIMIT;
   capped.time_limit_ms = 60'000;
   auto capped_res = run(ro, capped);
   BOOST_REQUIRE_EQUAL(capped_res.rows.size(), static_cast<size_t>(read_only::max_return_items));
   BOOST_CHECK(capped_res.more);
   BOOST_CHECK(!capped_res.next_key.empty());

   // Unlimited deadline (no `time_limit_ms` -> `params_deadline` stays `maximum()`): the same over-cap limit is
   // NOT clamped, so every populated row comes back in one page. Had the clamp wrongly engaged here it would
   // return max_return_items rows with `more` set.
   auto unbounded = numobjs_params();
   unbounded.limit = BIG_LIMIT;
   auto unbounded_res = run(ro, unbounded);
   BOOST_REQUIRE_EQUAL(unbounded_res.rows.size(), static_cast<size_t>(OVER_CAP));
   BOOST_CHECK(!unbounded_res.more);
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

// `all_rows` + `values_only` + `filter` composed together. Pins the interaction: the full scan runs, each row is
// unwrapped, and the filter sees the unwrapped shape. Independent coverage exists for each flag above, but this
// test catches future regressions where (say) the filter runs before unwrap or limit/deadline sneak back in under
// `all_rows`.
BOOST_FIXTURE_TEST_CASE(all_rows_with_values_only_and_filter_composed, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 20); // keys/sec64 = 0..19

   auto p = numobjs_params();
   p.all_rows    = true;
   p.values_only = true;
   p.filter = [](const fc::variant& row) {
      // Predicate sees the unwrapped value; expects `sec64` directly on the row (no `value` wrapper).
      return (row.get_object()["sec64"].as_uint64() % 2) == 0;
   };

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   BOOST_REQUIRE_EQUAL(res.rows.size(), 10u); // 0, 2, 4, ..., 18
   for (size_t i = 0; i < res.rows.size(); ++i) {
      // Unwrapped: no `value` key, field is directly on the row.
      BOOST_CHECK(!res.rows[i].get_object().contains("value"));
      BOOST_CHECK_EQUAL(res.rows[i].get_object()["sec64"].as_uint64(), i * 2);
   }
   BOOST_CHECK(!res.more);
   BOOST_CHECK(res.next_key.empty());
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

// Byte-identical compat between the variant-cb path (fc::variant + fc::json::to_string)
// and the streaming-cb path (fc::to_json_stream via reflector dispatch).  The HTTP
// /v1/chain/get_table_rows endpoint flips between these via add_api / add_api_stream;
// any drift in the streaming path's emitted JSON would break clients that depend on
// the exact byte sequence (eg keypair-string ordering, integer formatting).  This case
// pins parity for a representative populated result.
BOOST_FIXTURE_TEST_CASE(streaming_vs_variant_byte_identical, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 4);

   auto p = numobjs_params();
   p.all_rows  = true;
   p.show_payer = true;

   auto ro  = make_read_only(*this);
   auto res = run(ro, p);

   const std::string variant_path =
      fc::json::to_string(fc::variant(res), fc::time_point::maximum());
   const std::string stream_path = fc::to_json_string(res);

   BOOST_CHECK_EQUAL(variant_path, stream_path);
} FC_LOG_AND_RETHROW()

namespace {
   /// Run the new direct-streaming `get_table_rows_stream` to completion and return the
   /// emitted JSON.  Fails the test if the RPC produced an exception_ptr.
   std::string run_stream(read_only& ro, const read_only::get_table_rows_params& p) {
      auto outer = ro.get_table_rows_stream(p, fc::time_point::maximum())();
      BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(outer));
      auto emit = std::get<read_only::get_table_rows_stream_emit_fn>(std::move(outer));
      std::string out;
      {
         fc::json_writer w(out);
         emit(w);
         BOOST_REQUIRE(w.balanced());
      }
      return out;
   }
}

// Byte-identical compat for the new direct-streaming path.  `get_table_rows_stream`
// bypasses the per-row fc::variant assembly step; its emitted JSON must match
// `fc::json::to_string(fc::variant(get_table_rows().result))` for the same params.
// Any drift here would break HTTP clients that hit /v1/chain/get_table_rows after the
// chain_api_plugin migration to CHAIN_RO_CALL_STREAM_POST_DIRECT.
BOOST_FIXTURE_TEST_CASE(stream_direct_vs_variant_byte_identical, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 4);

   auto ro = make_read_only(*this);

   // Three shapes worth pinning: full wrapper + payer, all_rows escape hatch, and
   // values_only stripped form.  Each exercises a distinct branch in the stream
   // emit closure (wrapped object vs bare value, more/next_key suppression).
   for (auto setup : { +[](read_only::get_table_rows_params& q) { q.all_rows = true; q.show_payer = true; },
                       +[](read_only::get_table_rows_params& q) { q.show_payer = true; q.limit = 2; },
                       +[](read_only::get_table_rows_params& q) { q.values_only = true; q.all_rows = true; } }) {
      auto p = numobjs_params();
      setup(p);

      auto via_variant = run(ro, p);
      const std::string variant_json =
         fc::json::to_string(fc::variant(via_variant), fc::time_point::maximum());

      const std::string stream_json = run_stream(ro, p);

      BOOST_CHECK_EQUAL(variant_json, stream_json);
   }
} FC_LOG_AND_RETHROW()

// A row whose value fails ABI decode mid-emission must fall back to a hex string without
// corrupting the response.  The streaming path writes tokens as it walks the ABI, so a decode
// failure after some fields were already emitted has to rewind before the hex fallback -- a
// regression here produces structurally invalid JSON on the wire (a hex token appended after a
// half-written value object).  Force the failure by re-setting the contract ABI with an extra
// trailing field on `numobj`: stored rows then under-run the declared struct and throw
// "stream unexpectedly ended" after the five real fields were emitted.
BOOST_FIXTURE_TEST_CASE(stream_decode_failure_falls_back_to_hex_valid_json, validating_tester) try {
   deploy_contract(*this);
   populate_numobjs(*this, 3);

   abi_def abi = fc::json::from_string(
      std::string(test_contracts::get_table_test_abi().begin(),
                  test_contracts::get_table_test_abi().end())).as<abi_def>();
   for (auto& st : abi.structs) {
      if (st.name == "numobj") {
         st.fields.push_back({"extra_field", "uint64"});
      }
   }
   set_abi("test"_n, fc::json::to_string(fc::variant(abi), fc::time_point::maximum()));
   produce_block();

   auto ro = make_read_only(*this);
   auto p  = numobjs_params();
   p.show_payer = true;

   // The streamed body must be parseable JSON even though every row's value decode threw.
   const std::string stream_json = run_stream(ro, p);
   fc::variant parsed;
   BOOST_REQUIRE_NO_THROW(parsed = fc::json::from_string(stream_json));

   // Every row fell back to the bare hex-string form (not a half-decoded object).
   const auto& rows = parsed.get_object()["rows"].get_array();
   BOOST_REQUIRE_EQUAL(rows.size(), 3u);
   for (const auto& row : rows) {
      BOOST_CHECK(row.get_object()["value"].is_string());
   }

   // And the fallback shape stays byte-identical to the variant path's fallback.
   auto via_variant = run(ro, p);
   const std::string variant_json =
      fc::json::to_string(fc::variant(via_variant), fc::time_point::maximum());
   BOOST_CHECK_EQUAL(variant_json, stream_json);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
