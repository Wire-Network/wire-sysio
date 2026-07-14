/**
 * @file council_math_tests.cpp
 * @brief Host-side unit tests for the dependency-free council election math
 *        (`sysio::councl_math`, contracts/sysio.councl/include/sysio.councl/council_math.hpp).
 *
 * The kernel is pure integer / std::array C++ (no contract intrinsics), so it is exercised
 * directly on the host. Coverage:
 *   - win / elim thresholds at representative electorate sizes and the win+elim-1 == N dual.
 *   - Strict-priority resolution: outright win, priority hold, elimination-then-promotion,
 *     all-eliminated fail, deadline / full-turnout fail, and the auto-yes N==1 instant win.
 *   - Seed helpers: determinism, input sensitivity, in-range mapping, and the empty-set guard.
 *
 * The exact seed-mixing formula is intentionally tweakable, so tests here assert *properties*
 * of the seed→index mapping rather than golden hash values (see DESIGN.md §5, §12).
 */

#include <algorithm>
#include <array>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <sysio.councl/council_math.hpp>

using namespace sysio::councl_math;

namespace {
resolution R(std::array<uint64_t, 3> yes, std::array<uint64_t, 3> no, uint64_t N, bool all_voted, bool deadline_hit) {
   return resolve(yes, no, N, all_voted, deadline_hit);
}
} // namespace

BOOST_AUTO_TEST_SUITE(council_math_tests)

BOOST_AUTO_TEST_CASE(thresholds) {
   BOOST_CHECK_EQUAL(win_threshold(20), 14u);
   BOOST_CHECK_EQUAL(elim_threshold(20), 7u);
   BOOST_CHECK_EQUAL(win_threshold(84), 57u);
   BOOST_CHECK_EQUAL(elim_threshold(84), 28u);
   BOOST_CHECK_EQUAL(win_threshold(1000), 667u);
   BOOST_CHECK_EQUAL(elim_threshold(1000), 334u);
   BOOST_CHECK_EQUAL(win_threshold(1), 1u);
   BOOST_CHECK_EQUAL(elim_threshold(1), 1u);
   BOOST_CHECK_EQUAL(win_threshold(2), 2u);
   BOOST_CHECK_EQUAL(elim_threshold(2), 1u);
   BOOST_CHECK_EQUAL(win_threshold(3), 3u);
   BOOST_CHECK_EQUAL(elim_threshold(3), 1u);

   // Dual: floor(2n/3)+1 YES to win, ceil(n/3) NO to eliminate, summing to n+1.
   for (uint64_t n = 1; n <= 2000; ++n)
      BOOST_CHECK_EQUAL(win_threshold(n) + (elim_threshold(n) - 1), n);
}

BOOST_AUTO_TEST_CASE(candidate_one_wins_outright) {
   auto r = R({14, 0, 0}, {0, 0, 0}, 20, false, false);
   BOOST_CHECK(r.result == round_result::WIN);
   BOOST_CHECK_EQUAL(r.winner_index, 0u);

   // one short, round still open
   BOOST_CHECK(R({13, 0, 0}, {0, 0, 0}, 20, false, false).result == round_result::PENDING);
}

BOOST_AUTO_TEST_CASE(strict_priority_holds_higher_candidate) {
   // c2/c3 already at threshold, but c1 alive and below -> must not resolve to c2.
   auto r = R({5, 14, 14}, {2, 0, 0}, 20, false, false);
   BOOST_CHECK(r.result == round_result::PENDING);
}

BOOST_AUTO_TEST_CASE(elimination_then_promotion) {
   // c1 eliminated (7 no), c2 at threshold -> c2 wins.
   auto r2 = R({5, 14, 0}, {7, 0, 0}, 20, false, false);
   BOOST_CHECK(r2.result == round_result::WIN);
   BOOST_CHECK_EQUAL(r2.winner_index, 1u);

   // c1 & c2 eliminated, c3 at threshold -> c3 wins.
   auto r3 = R({7, 7, 14}, {7, 7, 0}, 20, false, false);
   BOOST_CHECK(r3.result == round_result::WIN);
   BOOST_CHECK_EQUAL(r3.winner_index, 2u);
}

BOOST_AUTO_TEST_CASE(round_failures) {
   // all three eliminated -> FAIL immediately
   BOOST_CHECK(R({0, 0, 0}, {7, 7, 7}, 20, false, false).result == round_result::FAIL);
   // deadline, active below threshold and not eliminated -> FAIL (escalate)
   BOOST_CHECK(R({10, 0, 0}, {3, 0, 0}, 20, false, true).result == round_result::FAIL);
   // all voted, active below threshold -> FAIL
   BOOST_CHECK(R({10, 0, 0}, {10, 0, 0}, 20, true, false).result == round_result::FAIL);
}

BOOST_AUTO_TEST_CASE(degenerate_and_full_turnout) {
   // tier-2/3 N==1 with proposer auto-yes seeded -> instant win c1
   auto r = R({1, 1, 1}, {0, 0, 0}, 1, true, false);
   BOOST_CHECK(r.result == round_result::WIN);
   BOOST_CHECK_EQUAL(r.winner_index, 0u);

   // tier-1 full turnout: c1 has 14 yes / 6 no -> win
   auto f = R({14, 3, 3}, {6, 17, 17}, 20, true, false);
   BOOST_CHECK(f.result == round_result::WIN);
   BOOST_CHECK_EQUAL(f.winner_index, 0u);
}

BOOST_AUTO_TEST_CASE(win_result_always_meets_active_threshold) {
   for (uint64_t n = 1; n <= 100; ++n) {
      const uint64_t threshold = win_threshold(n);
      const uint64_t eliminated = elim_threshold(n);
      const std::array<uint64_t, 4> active_yes = {0, threshold - 1, threshold, std::min(n, threshold + 1)};
      for (uint8_t eliminated_prefix = 0; eliminated_prefix <= 3; ++eliminated_prefix) {
         for (const uint64_t yes_count : active_yes) {
            for (const bool all_voted : {false, true}) {
               for (const bool deadline_hit : {false, true}) {
                  std::array<uint64_t, 3> yes = {n, n, n};
                  std::array<uint64_t, 3> no{};
                  for (uint8_t i = 0; i < eliminated_prefix; ++i)
                     no[i] = eliminated;
                  if (eliminated_prefix < 3)
                     yes[eliminated_prefix] = yes_count;

                  const auto result = resolve(yes, no, n, all_voted, deadline_hit);
                  if (result.result == round_result::WIN) {
                     BOOST_REQUIRE_LT(result.winner_index, 3u);
                     BOOST_CHECK_EQUAL(result.winner_index, eliminated_prefix);
                     BOOST_CHECK_LT(no[result.winner_index], eliminated);
                     BOOST_CHECK_GE(yes[result.winner_index], threshold);
                  } else if (eliminated_prefix < 3 && yes_count < threshold) {
                     BOOST_CHECK(result.result != round_result::WIN);
                  }
               }
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(seed_helpers) {
   std::array<uint8_t, 32> h1{};
   for (int i = 0; i < 32; ++i)
      h1[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 7 + 1);
   std::array<uint8_t, 32> h2 = h1;
   h2[0] ^= 0xFF;

   BOOST_CHECK_EQUAL(seed_u64(h1), seed_u64(h1)); // deterministic
   BOOST_CHECK(seed_u64(h1) != seed_u64(h2));     // input-sensitive

   for (uint64_t m = 1; m <= 1000; ++m)
      BOOST_CHECK_LT(bounded_index(seed_u64(h1), m), m); // always in range

   BOOST_CHECK_EQUAL(bounded_index(12345, 0), 0u); // empty-set guard
}

BOOST_AUTO_TEST_CASE(seed_big_endian_golden_vectors) {
   std::array<uint8_t, 32> zeros{};
   BOOST_CHECK_EQUAL(seed_u64(zeros), UINT64_C(0));

   std::array<uint8_t, 32> ascending{};
   for (uint8_t i = 0; i < 8; ++i)
      ascending[i] = i;
   BOOST_CHECK_EQUAL(seed_u64(ascending), UINT64_C(0x0001020304050607));

   std::array<uint8_t, 32> ones{};
   ones.fill(0xFF);
   BOOST_CHECK_EQUAL(seed_u64(ones), UINT64_MAX);
}

static_assert(resolve({1, 0, 0}, {0, 0, 0}, 1, false, false).result == round_result::WIN,
              "the resolver must remain constexpr");

BOOST_AUTO_TEST_SUITE_END()
