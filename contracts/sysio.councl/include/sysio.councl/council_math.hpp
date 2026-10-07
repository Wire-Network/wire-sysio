#pragma once

/**
 * @file council_math.hpp
 * @brief Pure, dependency-free election arithmetic for sysio.councl.
 *
 * Everything here is a `constexpr`/`inline` free function over plain integers and
 * `std::array`, with **no CDT / KV / chain dependencies**, so it can be unit-tested
 * host-side without a WASM build. The intentionally-tweakable randomness lives at the
 * seed-derivation boundary (`seed_u64` / `bounded_index`); keep exact-value assertions
 * for those in a small regeneratable golden table and assert *properties* everywhere else.
 *
 * Final tallies use tier priority followed by candidate priority; turnout never changes N.
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace sysio::councl_math {

/// YES votes needed to elect, for an electorate of size `n`:  floor(2n/3) + 1.
inline constexpr uint64_t win_threshold(uint64_t n) {
   return (2 * n) / 3 + 1;
}

/// NO votes that make a candidate impossible to elect (ceil(n/3)).
/// Dual of win_threshold: `win_threshold(n) + (elim_threshold(n) - 1) == n`.
inline constexpr uint64_t elim_threshold(uint64_t n) {
   return n - (2 * n) / 3;
}

/// Outcome of evaluating a closed flight.
enum class round_result : uint8_t {
   WIN = 1, ///< `winner_index` (0..2) has reached the win threshold
   FAIL = 2 ///< no unelected candidate qualifies
};

struct resolution {
   round_result result;
   uint8_t winner_index; ///< only meaningful when result == WIN
};

/**
 * @brief Select the first qualifying unelected candidate from one tier's final YES tallies.
 * Only call after the shared window closes. An earlier candidate below threshold does not
 * block a later one. Already elected candidates are skipped without redistributing approvals.
 */
inline constexpr resolution resolve_final(const std::array<uint64_t, 3>& yes, const std::array<bool, 3>& elected,
                                          uint64_t N) {
   if (N != 0)
      for (uint8_t i = 0; i < 3; ++i)
         if (!elected[i] && yes[i] >= win_threshold(N))
            return {round_result::WIN, i};
   return {round_result::FAIL, 0};
}

/// Fold a 32-byte hash into a uint64 (first 8 bytes, big-endian). Deterministic.
inline uint64_t seed_u64(const std::array<uint8_t, 32>& h) {
   uint64_t s = 0;
   for (int i = 0; i < 8; ++i)
      s = (s << 8) | static_cast<uint64_t>(h[static_cast<size_t>(i)]);
   return s;
}

/// Map a seed to an index in [0, m). Returns 0 when m == 0 (caller must guard empty sets).
inline uint64_t bounded_index(uint64_t seed, uint64_t m) {
   return m ? seed % m : 0;
}

} // namespace sysio::councl_math
