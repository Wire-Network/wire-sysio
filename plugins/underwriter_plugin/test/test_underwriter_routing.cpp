#include <boost/test/unit_test.hpp>

#include <sysio/underwriter_plugin/routing_detail.hpp>

#include <set>

/**
 * SEC-13 / WSA-027 regression tests for the underwriter plugin's exact-chain
 * routing/accounting keys.
 *
 * The plugin must key its credit buckets and its local commit de-dup by the
 * EXACT v6 slug codes — `(chain_code, token_code[, reserve_code])` — NOT the
 * coarse `(ChainKind, TokenKind)` VM family. Otherwise two active chains of the
 * same family (e.g. two EVM outposts) collapse onto one key: their collateral
 * merges into a single bucket and a confirmed commit on one suppresses the
 * still-needed commit on the other. These tests pin the exact-keying on the
 * pure helpers lifted into `routing_detail.hpp`.
 *
 * Boost.Test module is defined once in `test/main.cpp`; this file only adds a
 * suite.
 */

using namespace sysio::underwriter_detail;

namespace {
// Stand-in `fc::slug_name` values. Real codes are packed `[A-Z0-9_]` (≤8 chars),
// but the helpers treat them as opaque `uint64`, so any distinct values
// exercise the keying. ETH and EVM2 model two DISTINCT chains of the SAME VM
// family (both EVM) — the exact scenario WSA-027 is about.
constexpr uint64_t ETH  = 0xE1;   // chain_code, e.g. "ETHEREUM"
constexpr uint64_t EVM2 = 0xE2;   // a SECOND active EVM chain, e.g. "POLYGON"
constexpr uint64_t USDC = 0x70;   // token_code
constexpr uint64_t PRIM = 0x91;   // reserve_code, e.g. "PRIMARY"
constexpr uint64_t SEC2 = 0x92;   // reserve_code, e.g. "SECOND"

// A non-participating leg (no outpost, no bond) — the shape of a WIRE depot leg.
const leg_bond NO_LEG{{0, 0}, 0};

// Pre-built bucket keys. Named so they can appear inside BOOST_* macros: a bare
// braced `{ETH, USDC}` inside a macro argument is split on its comma by the
// preprocessor ("too many arguments to function-like macro invocation").
const bucket_key B_ETH_USDC { ETH,  USDC };
const bucket_key B_EVM2_USDC{ EVM2, USDC };
} // namespace

BOOST_AUTO_TEST_SUITE(underwriter_routing_tests)

// ── commit_key: exact-identity de-dup ───────────────────────────────────────

BOOST_AUTO_TEST_CASE(commit_key_distinguishes_same_kind_chains) {
   // Two legs identical but for chain_code: a confirmed commit on ETH must NOT
   // suppress the still-needed commit on a second EVM chain.
   const commit_key on_eth { /*uwreq*/ 1, ETH,  USDC, PRIM };
   const commit_key on_evm2{ /*uwreq*/ 1, EVM2, USDC, PRIM };
   BOOST_CHECK(on_eth != on_evm2);

   std::set<commit_key> confirmed;
   confirmed.insert(on_eth);
   BOOST_CHECK(confirmed.contains(on_eth));
   BOOST_CHECK(!confirmed.contains(on_evm2));   // the EVM2 leg is still open
}

BOOST_AUTO_TEST_CASE(commit_key_distinguishes_reserves_on_same_chain_token) {
   // Same (uwreq, chain, token) but different reserve = distinct legs (a
   // same-(chain, token) swap with two reserves).
   const commit_key prim{7, ETH, USDC, PRIM};
   const commit_key sec {7, ETH, USDC, SEC2};
   BOOST_CHECK(prim != sec);

   std::set<commit_key> confirmed{prim};
   BOOST_CHECK(confirmed.contains(prim));
   BOOST_CHECK(!confirmed.contains(sec));
}

BOOST_AUTO_TEST_CASE(commit_key_identical_legs_dedup) {
   const commit_key a{3, ETH, USDC, PRIM};
   const commit_key b{3, ETH, USDC, PRIM};
   BOOST_CHECK(a == b);
   std::set<commit_key> s;
   s.insert(a);
   s.insert(b);
   BOOST_CHECK_EQUAL(s.size(), 1u);   // same leg recorded once
}

// ── credit buckets: per-exact-chain capacity ────────────────────────────────

BOOST_AUTO_TEST_CASE(same_kind_chains_have_independent_credit) {
   // Underwriter holds 100 USDC collateral on EACH of two EVM chains.
   credit_buckets credit{
      {B_ETH_USDC,  100},
      {B_EVM2_USDC, 100},
   };

   // A 100 swap leg on EVM2 debits ONLY the EVM2 bucket; ETH is untouched.
   const leg_bond on_evm2{B_EVM2_USDC, 100};
   BOOST_REQUIRE(try_debit_buckets(credit, on_evm2, NO_LEG));
   BOOST_CHECK_EQUAL(credit[B_EVM2_USDC], 0u);
   BOOST_CHECK_EQUAL(credit[B_ETH_USDC],  100u);   // NOT shared/collapsed
}

BOOST_AUTO_TEST_CASE(insufficient_on_one_chain_does_not_borrow_from_same_kind) {
   // 10 on EVM2, 100 on ETH. A 100 leg on EVM2 must FAIL — it cannot borrow
   // ETH's collateral just because both chains are EVM. (The pre-fix collapse
   // merged them into one 110 bucket and would have wrongly succeeded.)
   credit_buckets credit{
      {B_ETH_USDC,  100},
      {B_EVM2_USDC, 10},
   };
   const leg_bond on_evm2{B_EVM2_USDC, 100};
   BOOST_CHECK(!try_debit_buckets(credit, on_evm2, NO_LEG));
   // `remaining` untouched on failure.
   BOOST_CHECK_EQUAL(credit[B_EVM2_USDC], 10u);
   BOOST_CHECK_EQUAL(credit[B_ETH_USDC],  100u);
}

BOOST_AUTO_TEST_CASE(same_bucket_dual_leg_needs_combined_balance) {
   // ERC20 → native on ONE chain: both legs share the (chain, token) bucket,
   // so the single row must cover the SUM, not each leg independently.
   credit_buckets credit{{B_ETH_USDC, 150}};
   const leg_bond src{B_ETH_USDC, 100};
   const leg_bond dst{B_ETH_USDC, 100};   // 200 > 150 → must fail
   BOOST_CHECK(!try_debit_buckets(credit, src, dst));
   BOOST_CHECK_EQUAL(credit[B_ETH_USDC], 150u);

   credit[B_ETH_USDC] = 200;
   BOOST_REQUIRE(try_debit_buckets(credit, src, dst));
   BOOST_CHECK_EQUAL(credit[B_ETH_USDC], 0u);
}

BOOST_AUTO_TEST_CASE(depot_leg_requires_no_bucket) {
   // A to-WIRE swap has one real leg + one depot leg (require 0). The depot leg
   // consults no bucket; only the real leg must cover.
   credit_buckets credit{{B_ETH_USDC, 100}};
   const leg_bond real{B_ETH_USDC, 100};
   BOOST_REQUIRE(try_debit_buckets(credit, real, NO_LEG));
   BOOST_CHECK_EQUAL(credit[B_ETH_USDC], 0u);

   // both-depot (degenerate) is rejected — there is nothing to underwrite.
   BOOST_CHECK(!try_debit_buckets(credit, NO_LEG, NO_LEG));
}

BOOST_AUTO_TEST_SUITE_END()
