/**
 * @file council_math_tests.cpp
 * @brief Final YES qualification, submitted-ballot thresholds, and deterministic draw helpers.
 */
#include <boost/test/unit_test.hpp>

#include <sysio.councl/council_math.hpp>

#include <array>
#include <cstdint>

using namespace sysio::councl_math;
BOOST_AUTO_TEST_SUITE(council_math_tests)

BOOST_AUTO_TEST_CASE(thresholds) {
   BOOST_CHECK_EQUAL(win_threshold(1), 1u);
   BOOST_CHECK_EQUAL(win_threshold(10), 7u);
   BOOST_CHECK_EQUAL(win_threshold(20), 14u);
   BOOST_CHECK_EQUAL(win_threshold(21), 15u);
   BOOST_CHECK_EQUAL(win_threshold(30), 21u);
   BOOST_CHECK_EQUAL(win_threshold(84), 57u);
   BOOST_CHECK_EQUAL(win_threshold(100), 67u);
   BOOST_CHECK_EQUAL(win_threshold(1000), 667u);
   for (uint64_t n = 1; n <= 2000; ++n) {
      BOOST_CHECK_EQUAL(win_threshold(n), (2 * n) / 3 + 1);
      BOOST_CHECK_EQUAL(win_threshold(n) + elim_threshold(n) - 1, n);
   }
}

BOOST_AUTO_TEST_CASE(later_candidate_qualifies_without_earlier_no_elimination) {
   const auto result = resolve_final({9, 14, 0}, {false, false, false}, 20);
   BOOST_REQUIRE(result.result == round_result::WIN);
   BOOST_CHECK_EQUAL(result.winner_index, 1u);
   BOOST_CHECK_EQUAL(resolve_final({13, 13, 14}, {false, false, false}, 20).winner_index, 2u);
}

BOOST_AUTO_TEST_CASE(priority_and_already_elected_candidates) {
   BOOST_CHECK_EQUAL(resolve_final({14, 20, 20}, {false, false, false}, 20).winner_index, 0u);
   BOOST_CHECK_EQUAL(resolve_final({20, 14, 20}, {true, false, false}, 20).winner_index, 1u);
   BOOST_CHECK(resolve_final({20, 13, 13}, {true, false, false}, 20).result == round_result::FAIL);
   BOOST_CHECK(resolve_final({20, 20, 20}, {true, true, true}, 20).result == round_result::FAIL);
}

BOOST_AUTO_TEST_CASE(empty_small_and_submitted_ballot_tiers) {
   BOOST_CHECK(resolve_final({1, 1, 1}, {false, false, false}, 0).result == round_result::FAIL);
   BOOST_CHECK(resolve_final({0, 0, 0}, {false, false, false}, 1).result == round_result::FAIL);
   BOOST_CHECK_EQUAL(resolve_final({0, 1, 0}, {false, false, false}, 1).winner_index, 1u);
   BOOST_CHECK(resolve_final({1, 1, 1}, {false, false, false}, 2).result == round_result::FAIL);
   BOOST_CHECK(resolve_final({2, 2, 2}, {false, false, false}, 3).result == round_result::FAIL);
   BOOST_CHECK(resolve_final({13, 0, 0}, {false, false, false}, 20).result == round_result::FAIL);
   BOOST_CHECK_EQUAL(resolve_final({13, 0, 0}, {false, false, false}, 13).winner_index, 0u);
   BOOST_CHECK_EQUAL(resolve_final({14, 15, 0}, {false, false, false}, 21).winner_index, 1u);
}

BOOST_AUTO_TEST_CASE(final_resolution_matches_first_qualified_unelected_candidate) {
   for (uint64_t n = 0; n <= 1000; ++n)
      for (uint8_t mask = 0; mask < 8; ++mask)
         for (uint8_t qualified = 0; qualified < 8; ++qualified) {
            std::array<uint64_t, 3> yes{};
            std::array<bool, 3> elected{};
            int expected = -1;
            for (uint8_t i = 0; i < 3; ++i) {
               yes[i] = qualified & (1 << i) ? win_threshold(n) : win_threshold(n) - 1;
               elected[i] = mask & (1 << i);
               if (n && !elected[i] && (qualified & (1 << i)) && expected == -1)
                  expected = i;
            }
            const auto result = resolve_final(yes, elected, n);
            BOOST_REQUIRE((result.result == round_result::WIN) == (expected != -1));
            if (expected != -1)
               BOOST_REQUIRE_EQUAL(result.winner_index, expected);
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


static_assert(resolve_final({1, 0, 0}, {false, false, false}, 1).result == round_result::WIN,
              "final resolution remains constexpr");
BOOST_AUTO_TEST_SUITE_END()
