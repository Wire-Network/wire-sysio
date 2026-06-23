/**
 * @file safe_ops_tests.cpp
 * @brief Host-side unit tests for the shared never-throw OPP dispatch helpers
 *        (`sysio::opp::safe`, contracts/sysio.opp.common/.../safe_ops.hpp).
 *
 * These helpers are pure C++ (no contract intrinsics), so they are exercised
 * directly on the host. Coverage:
 *   - `is_valid_name_string` accepts the FULL CDT name domain — including a
 *     legitimate 13-byte name whose final symbol fits the 4-bit final slot
 *     ('.'/'1'-'5'/'a'-'j') — and rejects every string `name()` would abort on
 *     (length > 13, an out-of-alphabet character, or a 13th symbol > 15). This
 *     pins the parse_wire_name fix (PR #417, r3444212148): the old `size() > 12`
 *     cap wrongly rejected every 13-character WIRE depositor/recipient.
 *   - `add_sat_u64` clamps at UINT64_MAX instead of wrapping (r3444213199).
 */

#include <boost/test/unit_test.hpp>
#include <sysio.opp.common/safe_ops.hpp>

#include <cstdint>
#include <string>

using sysio::opp::safe::add_sat_i64;
using sysio::opp::safe::add_sat_u64;
using sysio::opp::safe::is_valid_name_string;

BOOST_AUTO_TEST_SUITE(safe_ops_tests)

BOOST_AUTO_TEST_CASE(is_valid_name_string_accepts_full_cdt_domain) {
   // Empty + short names within the alphabet.
   BOOST_CHECK(is_valid_name_string(""));
   BOOST_CHECK(is_valid_name_string("a"));
   BOOST_CHECK(is_valid_name_string("uwrit.alice"));     // 11, with a dot
   BOOST_CHECK(is_valid_name_string("aaaaaaaaaaaa"));    // 12 chars

   // The regressed case: a legitimate 13-character name whose final symbol fits
   // the 4-bit final slot. 'a' == 6 and 'j' == 15 are both <= 15.
   BOOST_CHECK(is_valid_name_string("aaaaaaaaaaaaa"));   // 13 chars, final 'a' (6)
   BOOST_CHECK(is_valid_name_string("aaaaaaaaaaaaj"));   // 13 chars, final 'j' (15)
   BOOST_CHECK(is_valid_name_string("abcdefghij.15"));   // 13 chars, mixed, final '5'

   // Full alphabet members are accepted in non-final positions.
   BOOST_CHECK(is_valid_name_string(".12345"));
   BOOST_CHECK(is_valid_name_string("zzzzzzzzzzzz"));    // 12 'z' (z == 31, fine pre-final)
}

BOOST_AUTO_TEST_CASE(is_valid_name_string_rejects_abortable_strings) {
   // A 13th symbol that exceeds the 4-bit final slot (value > 15) — name() aborts.
   // 'k' == 16, 'z' == 31.
   BOOST_CHECK(!is_valid_name_string("aaaaaaaaaaaak"));  // final 'k' (16)
   BOOST_CHECK(!is_valid_name_string("wirerecipient"));  // 13 chars, final 't' (25)
   BOOST_CHECK(!is_valid_name_string("aaaaaaaaaaaaz"));  // final 'z' (31)

   // Longer than 13 characters.
   BOOST_CHECK(!is_valid_name_string("aaaaaaaaaaaaaa")); // 14 chars

   // Characters outside ".12345abcdefghijklmnopqrstuvwxyz".
   BOOST_CHECK(!is_valid_name_string("Alice"));          // uppercase
   BOOST_CHECK(!is_valid_name_string("a6"));             // '6'..'9'/'0' not in alphabet
   BOOST_CHECK(!is_valid_name_string("a-b"));            // '-'
   BOOST_CHECK(!is_valid_name_string("hello world"));    // space
}

BOOST_AUTO_TEST_CASE(add_sat_u64_saturates_instead_of_wrapping) {
   constexpr uint64_t MAX = ~uint64_t{0};

   // Ordinary sums.
   BOOST_CHECK_EQUAL(add_sat_u64(0, 0), uint64_t{0});
   BOOST_CHECK_EQUAL(add_sat_u64(5, 7), uint64_t{12});
   BOOST_CHECK_EQUAL(add_sat_u64(MAX, 0), MAX);
   BOOST_CHECK_EQUAL(add_sat_u64(0, MAX), MAX);

   // Exactly at the boundary does not saturate.
   BOOST_CHECK_EQUAL(add_sat_u64(MAX - 10, 10), MAX);

   // One past the boundary clamps to MAX rather than wrapping to a small value.
   BOOST_CHECK_EQUAL(add_sat_u64(MAX, 1), MAX);
   BOOST_CHECK_EQUAL(add_sat_u64(MAX - 3, 10), MAX);
   BOOST_CHECK_EQUAL(add_sat_u64(uint64_t{1} << 63, uint64_t{1} << 63), MAX);
   BOOST_CHECK_EQUAL(add_sat_u64(100, MAX - 50), MAX);
}

BOOST_AUTO_TEST_CASE(add_sat_i64_saturates_instead_of_wrapping) {
   constexpr int64_t MAX = INT64_MAX;
   constexpr int64_t MIN = INT64_MIN;

   // Ordinary sums (including mixed sign) compute exactly.
   BOOST_CHECK_EQUAL(add_sat_i64(0, 0), int64_t{0});
   BOOST_CHECK_EQUAL(add_sat_i64(5, 7), int64_t{12});
   BOOST_CHECK_EQUAL(add_sat_i64(10, -3), int64_t{7});
   BOOST_CHECK_EQUAL(add_sat_i64(-10, 3), int64_t{-7});
   BOOST_CHECK_EQUAL(add_sat_i64(MAX, 0), MAX);
   BOOST_CHECK_EQUAL(add_sat_i64(MIN, 0), MIN);

   // Exactly at the boundary does not saturate.
   BOOST_CHECK_EQUAL(add_sat_i64(MAX - 10, 10), MAX);
   BOOST_CHECK_EQUAL(add_sat_i64(MIN + 10, -10), MIN);

   // One past the boundary clamps rather than wrapping to the opposite sign.
   BOOST_CHECK_EQUAL(add_sat_i64(MAX, 1), MAX);
   BOOST_CHECK_EQUAL(add_sat_i64(MAX - 3, 10), MAX);
   BOOST_CHECK_EQUAL(add_sat_i64(MIN, -1), MIN);
   BOOST_CHECK_EQUAL(add_sat_i64(MIN + 3, -10), MIN);
}

BOOST_AUTO_TEST_SUITE_END()
