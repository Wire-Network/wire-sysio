/**
 * @file safe_ops_tests.cpp
 * @brief Host-side unit tests for the shared never-throw OPP dispatch helpers
 *        (`sysio::opp::safe`, contracts/sysio.opp.common/.../safe_ops.hpp).
 *
 * These helpers are pure C++ (no contract intrinsics), so they are exercised
 * directly on the host. Coverage:
 *   - name-string validation now lives on the type itself
 *     (`name::is_valid_literal`, covered by name_tests); the hand-rolled mirror
 *     that used to be pinned here is gone. The domain it guarded — a legitimate
 *     13-byte name whose final symbol fits the 4-bit final slot — is pinned in
 *     name_tests::literal_and_runtime_validation_agree. Historical note: the old
 *     cap wrongly rejected every 13-character WIRE depositor/recipient.
 *   - `add_sat_u64` clamps at UINT64_MAX instead of wrapping (r3444213199).
 */

#include <boost/test/unit_test.hpp>
#include <sysio.opp.common/safe_ops.hpp>

#include <cstdint>
#include <string>

using sysio::opp::safe::add_sat_i64;
using sysio::opp::safe::add_sat_u64;
using sysio::opp::safe::depot_amount_max;
using sysio::opp::safe::to_depot_amount;

BOOST_AUTO_TEST_SUITE(safe_ops_tests)

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

// WSA-028: to_depot_amount is the fail-closed gate for inbound OPP TokenAmount
// values. TokenAmount.amount is signed on the wire; the historical foot-gun was
// static_cast<uint64_t>(static_cast<int64_t>(amount)), which turns -1 into
// 18446744073709551615. The gate accepts only (0, depot_amount_max] and returns
// std::nullopt otherwise, so callers drop / refund-revert without ever wrapping.
BOOST_AUTO_TEST_CASE(to_depot_amount_gates_signed_token_amounts) {
   // depot_amount_max is the Antelope asset magnitude limit (2^62 - 1), mirrored
   // locally so this header needs no <sysio/asset.hpp>. Pin it so it cannot drift
   // from the value a downstream WIRE asset() will accept without aborting.
   static_assert(depot_amount_max == (int64_t{1} << 62) - 1,
                 "depot_amount_max must equal the asset magnitude limit 2^62 - 1");

   // Rejected: zero and every negative value (the WSA-028 wrap class). A negative
   // int64 cast to uint64 lands in [2^63, 2^64) — far above any real balance.
   BOOST_CHECK(!to_depot_amount(0).has_value());
   BOOST_CHECK(!to_depot_amount(-1).has_value());           // pre-fix wrapped to UINT64_MAX
   BOOST_CHECK(!to_depot_amount(-1'000'000).has_value());
   BOOST_CHECK(!to_depot_amount(INT64_MIN).has_value());

   // Rejected: positive but out of asset range — would abort a later WIRE asset().
   BOOST_CHECK(!to_depot_amount(depot_amount_max + 1).has_value());   // 2^62
   BOOST_CHECK(!to_depot_amount(INT64_MAX).has_value());

   // Accepted: strictly positive and within range — returned unchanged.
   BOOST_CHECK_EQUAL(to_depot_amount(1).value(), uint64_t{1});
   BOOST_CHECK_EQUAL(to_depot_amount(1'000'000).value(), uint64_t{1'000'000});
   BOOST_CHECK_EQUAL(to_depot_amount(depot_amount_max).value(),       // 2^62 - 1 boundary
                     static_cast<uint64_t>(depot_amount_max));
}

BOOST_AUTO_TEST_SUITE_END()
