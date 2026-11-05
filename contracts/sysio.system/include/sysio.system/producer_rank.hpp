#pragma once

#include <magic_enum/magic_enum.hpp>

#include <sysio/kv_table.hpp>
#include <sysio/serialize.hpp>

#include <sysio.system/producer_rank_constants.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <sysio/name.hpp>

// Producer ranking -- the packed sort key and the governance-tunable weights behind it.
//
// This header is deliberately LIGHT: it carries no sysio.opreg dependency, so
// sysio.system.hpp can include it for `producer_info::rank_score`'s default. The factors that read
// the operator registry live in producer_score.hpp, included only by the translation units that
// actually compute a score -- the same split opreg_status.hpp uses for the same reason.
//
// `rank` is NOT stored. It is position in the "prodrank" index among producers that pass the
// schedulable predicate, derived by iteration. What IS stored on producer_info is `rank_score`:
// the packed key this header builds.

namespace sysiosystem {

   /**
    * Ordering tier -- the high bits of the packed producer sort key.
    *
    * A tier is CATEGORICAL: no score within a lower tier can overtake a higher one, so no amount of
    * seniority or collateral buys a demoted producer back into the schedule.
    *
    * Ordered so ascending index iteration yields healthy producers first, then the bootstrapped
    * backstop, then producers demoted for missing rounds. A demoted producer is demonstrably
    * offline right now; a bootstrap is the foundation-run backstop and must backfill ahead of it.
    */
   enum class producer_tier : uint8_t {
      healthy      = 0,
      bootstrapped = 1,
      demoted      = 2
   };

   namespace producer_rank {

      /**
       * Saturating add over the composite's range.
       *
       * @param lhs first addend; values past `composite_max` count as `composite_max`.
       * @param rhs second addend; values past `composite_max` count as `composite_max`.
       * @return the sum, clamped to `composite_max`.
       */
      inline uint64_t add_sat(uint64_t lhs, uint64_t rhs) {
         const uint64_t a = std::min(lhs, composite_max);
         const uint64_t b = std::min(rhs, composite_max);
         return a > composite_max - b ? composite_max : a + b;
      }

      /**
       * One weighted factor term: `factor * weight / score_scale`, saturating.
       *
       * @param factor a factor in basis points.
       * @param weight seconds of seniority the factor is worth at `score_scale`.
       * @return the term in seconds, clamped to `composite_max`.
       */
      inline uint64_t scale_sat(uint64_t factor, uint32_t weight) {
         const unsigned __int128 term =
            static_cast<unsigned __int128>(factor) * static_cast<unsigned __int128>(weight)
            / static_cast<unsigned __int128>(score_scale);
         return term > static_cast<unsigned __int128>(composite_max)
                   ? composite_max
                   : static_cast<uint64_t>(term);
      }

      /**
       * The composite: the collateral multiple times the time term, in hundredths of the minimum bond
       * times whole hours, saturating.
       *
       * @param multiple_bps the collateral multiple in basis points (`score_scale` is 1x); uncapped.
       * @param time_sec     seniority plus snapshot credit plus the seniority base, in seconds.
       * @return the composite, clamped to `composite_max`.
       */
      inline uint64_t weighted_composite(uint64_t multiple_bps, uint64_t time_sec) {
         const unsigned __int128 product =
            static_cast<unsigned __int128>(multiple_bps / ratio_per_multiple_unit)
            * static_cast<unsigned __int128>(time_sec / seniority_quantum_sec);
         return product > static_cast<unsigned __int128>(composite_max)
                   ? composite_max
                   : static_cast<uint64_t>(product);
      }

      /**
       * The seniority a producer's service time scores: whole hours, in seconds.
       *
       * Quantized so a served round moves the key, and so costs a rescore, only when it crosses an
       * hour rather than on every round.
       *
       * @param service_sec the producer's accrued `service_sec`.
       * @return `service_sec` floored to a multiple of `seniority_quantum_sec`.
       */
      inline uint64_t seniority_credit(uint64_t service_sec) {
         return service_sec / seniority_quantum_sec * seniority_quantum_sec;
      }

      /**
       * Pack a tier, composite score and registration order into the `prodrank` sort key.
       *
       * Layout, high to low: tier (`tier_bits`), INVERTED composite (`composite_bits`), registration
       * order (`registration_bits`). The composite is inverted so a higher score sorts EARLIER under
       * the index's ascending iteration; the tier is not, so a higher tier sorts later. Equal
       * composites sort by ascending registration order, so the earlier registration ranks first.
       * Equal keys fall back to primary-key order (the account name value).
       *
       * @param tier             the producer's ordering tier.
       * @param composite        the composite score, saturated to `composite_max`.
       * @param registration_seq the producer's registration order; 0 (unassigned) sorts last.
       * @return the packed sort key stored as `producer_info::rank_score`.
       */
      inline uint64_t pack(producer_tier tier, uint64_t composite, uint64_t registration_seq) {
         const uint64_t bounded = std::min(composite, composite_max);
         const uint64_t order   = registration_seq == 0 ? registration_max
                                                        : std::min(registration_seq, registration_max);
         return (static_cast<uint64_t>(magic_enum::enum_integer(tier)) << tier_shift)
              | ((composite_max - bounded) << registration_bits)
              | order;
      }

      /**
       * The tier encoded in a packed sort key.
       *
       * @param rank_score a key produced by `pack`.
       * @return the encoded tier; `demoted` for any unrecognised value, so an unknown tier sorts
       *         last rather than being trusted.
       */
      inline producer_tier tier_of(uint64_t rank_score) {
         const auto raw = static_cast<uint8_t>(rank_score >> tier_shift);
         return magic_enum::enum_cast<producer_tier>(raw).value_or(producer_tier::demoted);
      }

      /**
       * The sort key of a producer that has never been scored: worst composite and order in the
       * demoted tier.
       *
       * This is `producer_info::rank_score`'s default, and it is the safe one. A zero key would
       * decode as tier `healthy` with a MAXIMUM inverted composite -- i.e. a registered-but-unscored
       * row would sort ahead of every real producer.
       *
       * @return the packed key for an unscored producer.
       */
      inline uint64_t unscored() {
         return pack(producer_tier::demoted, 0, 0);
      }

      /**
       * Score configuration: the snapshot weight, the seniority base, the demotion threshold and the
       * miss penalty.
       *
       * The composite is `M x (seniority + snapshot credit + seniority_base_sec)`: M is the collateral
       * multiple and the time term is in whole hours; a weight is the seconds of seniority its factor
       * is worth at full credit. Adding a factor is a new weight defaulting to 0 plus a term in
       * `compute`; the key's layout never changes. A config change invalidates every stored score,
       * which is what the rescore cursor on the `prodstate` singleton drains.
       *
       * `relay` / `api` / `benchmark` ship at 0 deliberately. A `peerkeys` row proves registration,
       * not service, and nothing on chain observes an API node or a CPU benchmark at all; weighting
       * a self-declared factor is a free-points vector. They are enabled when an attestation path
       * exists.
       */
      struct [[sysio::table("prodscorecfg"), sysio::contract("sysio.system")]] producer_score_config {
         /// Seconds of seniority full snapshot credit is worth, pro rata below the target; scaled by M
         /// with the rest of the time term.
         uint32_t snapshot_weight      = default_snapshot_weight_sec;
         /// Reserved -- needs an attestation path before it can carry weight.
         uint32_t relay_weight         = 0;
         /// Reserved -- needs an attestation path before it can carry weight.
         uint32_t api_weight           = 0;
         /// Reserved -- needs an attestation path before it can carry weight.
         uint32_t benchmark_weight     = 0;

         /// Consecutive UNSERVED rounds that demote a producer to standby -- a round is served at
         /// `min_blocks_per_round` or better, so one that delivered too few blocks counts as much as
         /// an empty one. No cooldown and no expiry: a demoted producer recovers by SERVING a round
         /// while still in the active schedule -- the window a schedule too small to rebuild holds
         /// open -- or, once the schedule has dropped it, by re-registering. `regproducer` is
         /// refused as a pardon while the producer still holds a slot.
         uint32_t max_consecutive_missed_rounds = 3;

         /// Snapshot attestations within one pay period that earn full marks on the snapshot
         /// factor. The counter is reset on the same cadence as the block counters, so this is the
         /// window's target rather than an all-time total.
         uint32_t snapshot_target_attestations = 1;
         /// Blocks a producer must deliver within its own round for that round to count as SERVED.
         ///
         /// This is the whole round verdict. A round is a contiguous run of slots held by one
         /// producer; delivering this many or more clears the miss streak, anything less --
         /// including nothing at all -- increments it. Without a threshold, one block of twelve was
         /// indistinguishable from twelve, so a producer could hold a scheduled slot indefinitely
         /// while delivering a fraction of it.
         ///
         /// Zero disables the check, leaving only a wholly unproduced round as a miss. The default
         /// is half a standard 12-slot round, matching the threshold the retired per-round pay
         /// model used.
         uint32_t min_blocks_per_round = 6;

         /// Seconds of seniority an UNSERVED round costs, floored at zero, so a miss costs M times
         /// this in the composite. Zero disables the penalty.
         uint32_t miss_penalty_sec = default_miss_penalty_sec;

         /// Seconds added to every producer's time term, so a producer with no seniority still
         /// scores and standbys order by stake. A standby at multiple k_s passes an incumbent at k_i
         /// with seniority S only when k_s x B > k_i x (S + B). Zero ranks unseasoned producers on
         /// snapshot credit alone.
         ///
         /// DECLARED LAST, matching the tail of SYSLIB_SERIALIZE below.
         uint32_t seniority_base_sec = default_seniority_base_sec;

         SYSLIB_SERIALIZE(producer_score_config,
            (snapshot_weight)
            (relay_weight)(api_weight)(benchmark_weight)
            (max_consecutive_missed_rounds)(snapshot_target_attestations)
            (min_blocks_per_round)(miss_penalty_sec)(seniority_base_sec))
      };

      /// The `prodscorecfg` singleton. Mirrors `emitcfg_t`: absent until governance installs it, so
      /// every read goes through `get_or_default(producer_score_config{})`.
      using producer_score_config_t = sysio::kv::global<"prodscorecfg"_n, producer_score_config>;

      /**
       * Whether a producer's miss streak warrants demotion.
       *
       * One gate, one question: has it failed to serve its round this many times running? A round
       * is served at `min_blocks_per_round` blocks or more, so a producer delivering a fraction of
       * every round accrues the streak exactly as one that produces nothing does.
       *
       * @param consecutive_missed_rounds rounds in a row it failed to serve.
       * @param weights                   the live score configuration.
       * @return true iff the streak has reached the configured limit.
       */
      inline bool warrants_demotion(uint32_t consecutive_missed_rounds,
                                    const producer_score_config& weights) {
         return weights.max_consecutive_missed_rounds > 0
             && consecutive_missed_rounds >= weights.max_consecutive_missed_rounds;
      }

      /**
       * The active producer schedule as `onblock` last observed it.
       *
       * Miss attribution walks the span between the previous block's producer and this one's, so it
       * is only meaningful while the schedule is unchanged: across a change the old names may not be
       * in the new set, and a producer newly added to the schedule has not yet had a slot to miss.
       * There is no schedule-version intrinsic in CDT -- `get_active_producers()` returns only the
       * name list -- so the comparison is against this stored snapshot.
       *
       * Kept in its OWN singleton rather than on `producer_runtime_state` because that row is read
       * and written on every round through a cached handle; a 21-name vector on it would widen every
       * one of those reads. This row is read per block and written only when the schedule actually
       * changes.
       */
      struct [[sysio::table("prodsched"), sysio::contract("sysio.system")]] observed_schedule {
         /// The active producer names, in schedule order, as of the last observation.
         std::vector<sysio::name> producers;

         SYSLIB_SERIALIZE(observed_schedule, (producers))
      };

      /// The `prodsched` singleton -- absent until the first block observes a schedule.
      using observed_schedule_t = sysio::kv::global<"prodsched"_n, observed_schedule>;

   } // namespace producer_rank

} // namespace sysiosystem
