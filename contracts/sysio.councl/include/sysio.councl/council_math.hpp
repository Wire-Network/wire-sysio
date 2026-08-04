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
 * See DESIGN.md §5 (randomness) and §6 (strict-priority resolution).
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

/// Outcome of evaluating one voting round.
enum class round_result : uint8_t {
   PENDING = 0, ///< keep collecting votes
   WIN = 1,     ///< `winner_index` (0..2) has reached the win threshold
   FAIL = 2     ///< no candidate can win this round -> escalate
};

struct resolution {
   round_result result;
   uint8_t winner_index; ///< only meaningful when result == WIN
};

/**
 * @brief Strict-priority resolution over a 3-candidate slate.
 *
 * Candidate `i` is *eliminated* once `no[i] >= elim_threshold(N)`. The *active* candidate is
 * the lowest index that is not eliminated. A round is WON the instant the active candidate
 * reaches `win_threshold(N)` — a higher-index candidate can never win while a lower-index one
 * is still alive. A round FAILS when all three are eliminated, or when voting has closed
 * (`all_voted` or `deadline_hit`) with the active candidate still short of the threshold.
 *
 * @param yes          per-candidate YES tallies (includes tier-2/3 proposer auto-yes seeding)
 * @param no           per-candidate NO tallies
 * @param N            electorate size for this tier
 * @param all_voted    every eligible voter has cast a vote
 * @param deadline_hit the voting window has elapsed
 */
inline constexpr resolution resolve(const std::array<uint64_t, 3>& yes, const std::array<uint64_t, 3>& no, uint64_t N,
                                    bool all_voted, bool deadline_hit) {
   const uint64_t T = win_threshold(N);
   const uint64_t E = elim_threshold(N);

   int active = -1;
   for (int i = 0; i < 3; ++i) {
      if (no[i] < E) {
         active = i;
         break;
      }
   }
   if (active < 0)
      return {round_result::FAIL, 0}; // all three eliminated

   if (yes[static_cast<size_t>(active)] >= T)
      return {round_result::WIN, static_cast<uint8_t>(active)};

   if (all_voted || deadline_hit)
      return {round_result::FAIL, 0};

   return {round_result::PENDING, 0};
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
