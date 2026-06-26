/**
 * @file amm_math_tests.cpp
 * @brief Host-side unit tests for the deterministic weighted-Bancor swap kernel
 *        (`sysio::opp::amm`, contracts/sysio.opp.common/.../amm_math.hpp).
 *
 * The kernel is pure integer C++ (no contract intrinsics), so it is exercised
 * directly on the host here. Coverage:
 *   - 50/50 weights reduce bit-exactly to integer constant product.
 *   - Weighted outputs match high-precision references to within a few subunits.
 *   - Hard invariants: output never exceeds the pool, never exceeds the true
 *     mathematical output, is monotonic in `amount_in`, and degenerate inputs
 *     return 0.
 *   - The token<->WIRE convenience wrappers agree with `out_given_in`.
 */

#include <boost/test/unit_test.hpp>
#include <sysio.opp.common/amm_math.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace sysio::opp::amm;

namespace {

/// Exact integer constant product — the legacy `cp_output` definition, used to
/// pin the equal-weight fast path.
uint64_t cp_exact(uint64_t balance_in, uint64_t balance_out, uint64_t amount_in) {
   if (balance_in == 0 || balance_out == 0 || amount_in == 0) return 0;
   u128 num = static_cast<u128>(balance_out) * amount_in;
   u128 den = static_cast<u128>(balance_in) + amount_in;
   u128 out = num / den;
   return out >= balance_out ? balance_out : static_cast<uint64_t>(out);
}

/// `double` reference (generous tolerance — `double` itself is the less precise
/// party once balances exceed 2^53).
double ref_out(uint64_t bin, uint64_t win, uint64_t bout, uint64_t wout, uint64_t ain) {
   double base = static_cast<double>(bin) / (static_cast<double>(bin) + static_cast<double>(ain));
   return static_cast<double>(bout) * (1.0 - std::pow(base, static_cast<double>(win) / wout));
}

} // namespace

BOOST_AUTO_TEST_SUITE(amm_math_tests)

/// Weighted outputs match independently computed high-precision floors
/// (Decimal, 60-digit) to within a handful of 9-decimal subunits.
BOOST_AUTO_TEST_CASE(weighted_spot_checks) {
   struct C { uint64_t bin, win, bout, wout, ain, expect; };
   const std::vector<C> cases = {
      // token->wire, cw=2000 (token weight 8000 / wire weight 2000), e=4
      {1'000'000'000'000ULL, 8000, 1'000'000'000'000ULL, 2000, 100'000'000'000ULL, 316'986'544'634ULL},
      // token->wire, cw=8000 (token weight 2000 / wire weight 8000), e=0.25
      {1'000'000'000'000ULL, 2000, 1'000'000'000'000ULL, 8000, 100'000'000'000ULL,  23'545'910'323ULL},
      // cw=5000 reduces to constant product exactly
      {1'000'000'000'000ULL, 5000, 1'000'000'000'000ULL, 5000, 100'000'000'000ULL,  90'909'090'909ULL},
      // large balances, cw=3333
      {2'000'000'000'000'000ULL, 6667, 2'000'000'000'000'000ULL, 3333, 500'000'000'000'000ULL, 720'085'692'824'684ULL},
   };
   for (const auto& c : cases) {
      uint64_t got = out_given_in(c.bin, c.win, c.bout, c.wout, c.ain);
      int64_t  d   = static_cast<int64_t>(got) - static_cast<int64_t>(c.expect);
      BOOST_CHECK_MESSAGE(d <= 0 && d >= -16,
         "weighted out " << got << " vs expect " << c.expect << " (delta " << d << ")");
   }
}

/// Equal weights (incl. 50/50) are bit-identical to integer constant product.
BOOST_AUTO_TEST_CASE(equal_weight_is_exact_constant_product) {
   const std::vector<uint64_t> bals = {1000ULL, 1'000'000'000ULL, 1'000'000'000'000ULL,
                                       2'000'000'000'000'000ULL};
   const std::vector<double>   frac = {0.0001, 0.01, 0.5, 1.0, 5.0, 1000.0};
   for (uint32_t w : {1u, 2500u, 5000u, 7500u, 9999u})
      for (uint64_t bin : bals)
         for (uint64_t bout : bals)
            for (double f : frac) {
               uint64_t ain = static_cast<uint64_t>(static_cast<double>(bin) * f);
               if (ain == 0) ain = 1;
               BOOST_CHECK_EQUAL(out_given_in(bin, w, bout, w, ain), cp_exact(bin, bout, ain));
            }
}

/// Hard invariants across a wide grid: never exceed the pool, never exceed the
/// true output, monotonic in amount_in.
BOOST_AUTO_TEST_CASE(invariants_grid) {
   const std::vector<uint32_t> cws  = {1, 100, 500, 2000, 3333, 5000, 6667, 8000, 9500, 9999};
   const std::vector<uint64_t> bals = {1000ULL, 1'000'000'000ULL, 1'000'000'000'000ULL,
                                       2'000'000'000'000'000ULL};
   const std::vector<double>   frac = {0.0001, 0.001, 0.01, 0.1, 0.5, 1.0, 5.0, 100.0, 1000.0};
   for (uint32_t cw : cws) {
      uint32_t wt = WEIGHT_TOTAL_BPS - cw;
      for (uint64_t bin : bals)
         for (uint64_t bout : bals) {
            uint64_t prev = 0; bool first = true;
            for (double f : frac) {
               uint64_t ain = static_cast<uint64_t>(static_cast<double>(bin) * f);
               if (ain == 0) ain = 1;
               uint64_t got = out_given_in(bin, wt, bout, cw, ain);
               BOOST_REQUIRE_LE(got, bout);                       // never exceed the pool (exact invariant)
               // Never materially exceed the true output. The `double` reference
               // here itself loses a few ULPs once balances pass 2^53 (the
               // integer kernel is the MORE precise party), so allow a small
               // relative slack; the tight accuracy check is in
               // weighted_spot_checks (vs 60-digit Decimal references).
               const double ref = ref_out(bin, wt, bout, cw, ain);
               BOOST_CHECK_LE(static_cast<double>(got), ref * (1.0 + 1e-9) + 2.0);
               if (!first) BOOST_CHECK_GE(got, prev);             // monotonic in amount_in
               prev = got; first = false;
            }
         }
   }
}

/// Degenerate inputs return 0.
BOOST_AUTO_TEST_CASE(degenerate_inputs) {
   BOOST_CHECK_EQUAL(out_given_in(0, 5000, 1000, 5000, 10), 0u);
   BOOST_CHECK_EQUAL(out_given_in(1000, 5000, 0, 5000, 10), 0u);
   BOOST_CHECK_EQUAL(out_given_in(1000, 5000, 1000, 5000, 0), 0u);
   BOOST_CHECK_EQUAL(out_given_in(1000, 0, 1000, 5000, 10), 0u);
   BOOST_CHECK_EQUAL(out_given_in(1000, 5000, 1000, 0, 10), 0u);
}

/// The token<->WIRE convenience wrappers encode the weight convention
/// (`cw` = WIRE-side weight) and agree with `out_given_in`.
BOOST_AUTO_TEST_CASE(convenience_wrappers) {
   const uint64_t rc = 1'000'000'000'000ULL, rw = 3'000'000'000'000ULL;
   for (uint32_t cw : {2000u, 5000u, 8000u}) {
      uint64_t amt = 50'000'000'000ULL;
      BOOST_CHECK_EQUAL(token_to_wire(rc, rw, cw, amt),
                        out_given_in(rc, WEIGHT_TOTAL_BPS - cw, rw, cw, amt));
      BOOST_CHECK_EQUAL(wire_to_token(rw, rc, cw, amt),
                        out_given_in(rw, cw, rc, WEIGHT_TOTAL_BPS - cw, amt));
   }
}

/// SEC-26 / WSA-042: a 100% fee (`fee_bps == BPS_TOTAL`) consumes the entire
/// WIRE leg, leaving `net == 0`. That degenerate post-fee leg is what let a
/// from-WIRE / token-to-token swap debit destination reserve liquidity while
/// crediting zero WIRE. Any fee below 100% leaves a positive remainder for
/// every positive input — so rejecting `fee_bps >= BPS_TOTAL` at
/// `sysio.uwrit::setconfig` makes `net == 0` unconstructible at settlement.
BOOST_AUTO_TEST_CASE(split_wire_fee_boundaries) {
   // 100% fee: fee == input, net == 0, and the fee splits exactly.
   {
      const auto f = split_wire_fee(1'000'000'000ULL, BPS_TOTAL, /*reward_share_bps*/5000);
      BOOST_CHECK_EQUAL(f.fee, 1'000'000'000ULL);
      BOOST_CHECK_EQUAL(f.net, 0u);
      BOOST_CHECK_EQUAL(f.reward_share + f.emissions_share, f.fee);
   }
   // Over-100% is clamped to 100% (split_wire_fee guards fee_bps > BPS_TOTAL),
   // still net == 0 — also rejected upstream by setconfig.
   {
      const auto f = split_wire_fee(500ULL, BPS_TOTAL + 12'345, 5000);
      BOOST_CHECK_EQUAL(f.fee, 500u);
      BOOST_CHECK_EQUAL(f.net, 0u);
   }
   // Every sub-100% fee leaves net > 0 for every positive input, incl. dust,
   // and conserves the leg exactly (net + fee == input).
   for (uint32_t bps : {1u, 10u, 100u, 5'000u, 9'000u, BPS_TOTAL - 1}) {
      for (uint64_t amt : {1ULL, 2ULL, 7ULL, 1'000ULL, 1'000'000'000'000ULL}) {
         const auto f = split_wire_fee(amt, bps, 5000);
         BOOST_CHECK_GT(f.net, 0u);
         BOOST_CHECK_EQUAL(f.net + f.fee, amt);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
