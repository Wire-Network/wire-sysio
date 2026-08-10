#include <boost/test/unit_test.hpp>

#include <sysio/underwriter_plugin/routing_detail.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

/**
 * Regression tests for the source-deposit hash preimage the underwriter
 * recomputes before it will commit to a swap.
 *
 * The preimage must reproduce, byte for byte, what the SOURCE OUTPOST hashed
 * when it accepted the deposit — `ReserveManager.requestSwap`'s
 * `abi.encodePacked(...)` on EVM, `request_swap.rs::correlation_hash` on SVM.
 * The outpost hashes the terms the CALLER submitted, so the destination slot is
 * `SwapRequest.target_amount`.
 *
 * The bug these pin: the depot overwrites the UWREQ's `dst_amount` with its own
 * AMM quote (WNS-02), minted long after the outpost hashed the deposit. Packing
 * that quote reproduces the wrong preimage for every swap whose quote differs
 * from the target — i.e. every swap that pays a WIRE-leg fee — and the
 * underwriter then declines to commit with a bare "SwapDeposit hash mismatch",
 * stalling the swap rather than failing loudly.
 *
 * Boost.Test module is defined once in `test/main.cpp`; this file only adds a
 * suite.
 */

using namespace sysio::underwriter_detail;

namespace {

/// Distinct, easily-spotted values for every slot, so a transposition shows up
/// as a wrong byte rather than a coincidental match.
swap_deposit_terms sample_terms() {
   return {
      .src_amount             = 0x1122334455667788ull,
      .src_token_code         = 0x0000000000000011ull,
      .src_reserve_code       = 0x0000000000000022ull,
      .dst_chain_code         = 0x0000000000000033ull,
      .dst_token_code         = 0x0000000000000044ull,
      .dst_reserve_code       = 0x0000000000000055ull,
      .target_amount          = 0x00000000000003e8ull,   // 1000 — the caller's ask
      .variance_tolerance_bps = 0x00000032u,             // 50
   };
}

/// Read a big-endian uint64 back out of the packed buffer.
uint64_t be64_at(const std::vector<uint8_t>& packed, size_t offset) {
   uint64_t v = 0;
   for (size_t i = 0; i < 8; ++i) v = (v << 8) | packed[offset + i];
   return v;
}

/// Read a big-endian uint32 back out of the packed buffer.
uint32_t be32_at(const std::vector<uint8_t>& packed, size_t offset) {
   uint32_t v = 0;
   for (size_t i = 0; i < 4; ++i) v = (v << 8) | packed[offset + i];
   return v;
}

} // namespace

BOOST_AUTO_TEST_SUITE(underwriter_source_deposit_tests)

// The EVM preimage: 20-byte address, then the seven u64s in outpost order, then
// the tolerance. Sizes and offsets are the contract with ReserveManager.
BOOST_AUTO_TEST_CASE(evm_preimage_layout) {
   const std::vector<char> depositor(20, '\x0a');
   const auto terms  = sample_terms();
   const auto packed = pack_swap_deposit_preimage(depositor, terms);

   BOOST_REQUIRE_EQUAL(packed.size(), 20u + 8 * 7 + 4);
   for (size_t i = 0; i < 20; ++i) BOOST_REQUIRE_EQUAL(packed[i], 0x0a);

   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 0 * 8), terms.src_amount);
   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 1 * 8), terms.src_token_code);
   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 2 * 8), terms.src_reserve_code);
   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 3 * 8), terms.dst_chain_code);
   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 4 * 8), terms.dst_token_code);
   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 5 * 8), terms.dst_reserve_code);
   // The destination slot carries the caller's ask.
   BOOST_REQUIRE_EQUAL(be64_at(packed, 20 + 6 * 8), terms.target_amount);
   BOOST_REQUIRE_EQUAL(be32_at(packed, 20 + 7 * 8), terms.variance_tolerance_bps);
}

// The SVM preimage differs ONLY in the depositor width (32-byte Ed25519 pubkey);
// every other offset shifts by the same 12 bytes and nothing else changes.
BOOST_AUTO_TEST_CASE(svm_preimage_layout) {
   const std::vector<char> depositor(32, '\x0b');
   const auto terms  = sample_terms();
   const auto packed = pack_swap_deposit_preimage(depositor, terms);

   BOOST_REQUIRE_EQUAL(packed.size(), 32u + 8 * 7 + 4);
   for (size_t i = 0; i < 32; ++i) BOOST_REQUIRE_EQUAL(packed[i], 0x0b);

   BOOST_REQUIRE_EQUAL(be64_at(packed, 32 + 0 * 8), terms.src_amount);
   BOOST_REQUIRE_EQUAL(be64_at(packed, 32 + 6 * 8), terms.target_amount);
   BOOST_REQUIRE_EQUAL(be32_at(packed, 32 + 7 * 8), terms.variance_tolerance_bps);

   // Same terms, same trailing bytes on both chains — the two verifiers cannot
   // drift because they share this one packing.
   const auto evm = pack_swap_deposit_preimage(std::vector<char>(20, '\x0a'), terms);
   BOOST_REQUIRE(std::equal(packed.begin() + 32, packed.end(), evm.begin() + 20));
}

// The whole point: the preimage tracks the caller's target, so a depot quote
// that lands anywhere else cannot change it. Two swaps identical except for the
// amount the caller asked for must hash differently; the same ask must hash the
// same however the depot later re-prices it (the quote is not an input here at
// all — `swap_deposit_terms` has no `dst_amount` field to pass it through).
BOOST_AUTO_TEST_CASE(preimage_binds_the_callers_target_not_the_depot_quote) {
   const std::vector<char> depositor(20, '\x0a');

   auto asked_1000 = sample_terms();
   auto asked_997  = sample_terms();
   asked_997.target_amount = 997;   // e.g. a 30 bps WIRE-leg fee off 1000

   const auto packed_1000 = pack_swap_deposit_preimage(depositor, asked_1000);
   const auto packed_997  = pack_swap_deposit_preimage(depositor, asked_997);

   BOOST_REQUIRE(packed_1000 != packed_997);
   BOOST_REQUIRE_EQUAL(be64_at(packed_1000, 20 + 6 * 8), 1000u);
   BOOST_REQUIRE_EQUAL(be64_at(packed_997,  20 + 6 * 8), 997u);

   // Re-packing the untouched terms is stable — nothing in the preimage depends
   // on depot-side state that settlement may have moved.
   BOOST_REQUIRE(pack_swap_deposit_preimage(depositor, asked_1000) == packed_1000);
}

BOOST_AUTO_TEST_SUITE_END()
