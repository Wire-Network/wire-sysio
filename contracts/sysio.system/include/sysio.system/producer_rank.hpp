#pragma once

#include <magic_enum/magic_enum.hpp>

#include <sysio/kv_table.hpp>
#include <sysio/serialize.hpp>

#include <cstdint>
#include <vector>

#include <sysio/name.hpp>

// Producer ranking -- the packed sort key and the governance-tunable weights behind it.
//
// This header is deliberately LIGHT: it carries no sysio.opreg / sysio.uwrit dependency, so
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
    * A tier is CATEGORICAL: no score within a lower tier can overtake a higher one. That is what
    * makes the uncapped collateral term safe -- no amount of money buys a demoted producer back
    * into the schedule.
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

      /// Fixed-point scale for every factor and weight: basis points. A factor at `score_scale` is
      /// "100%"; a collateral ratio of exactly the configured minimum bond is also `score_scale`.
      constexpr uint64_t score_scale = 10'000;

      /// Bits the packed key reserves for producer_tier (the two high bits).
      constexpr unsigned tier_bits = 2;

      /// Bits left for the composite score.
      constexpr unsigned composite_bits = 64 - tier_bits;

      /// Largest representable composite, and the saturation point.
      ///
      /// The collateral factor is deliberately uncapped as a POLICY -- producers compete for rank by
      /// posting more -- so this ceiling is a bit-budget artifact, not a policy cap, and must stay
      /// economically unreachable. At a collateral weight of `score_scale` it corresponds to a bond
      /// roughly 4.6e10 times the configured minimum.
      constexpr uint64_t composite_max = (uint64_t{1} << composite_bits) - 1;

      /**
       * Saturating add over the composite's range.
       *
       * @param lhs first addend.
       * @param rhs second addend.
       * @return the sum, clamped to `composite_max`.
       */
      inline uint64_t add_sat(uint64_t lhs, uint64_t rhs) {
         return lhs > composite_max - rhs ? composite_max : lhs + rhs;
      }

      /**
       * Saturating multiply of a factor by its weight.
       *
       * Mandatory, not defensive: the collateral factor is deliberately UNCAPPED as a policy, so it
       * runs to `composite_max` (~2^62) on a large enough bond. Multiplying that by any weight
       * above 1 overflows uint64 and would wrap a top-ranked producer to the bottom of the index.
       * The uint128 intermediate makes the saturation explicit.
       *
       * @param factor a normalised factor, in basis points.
       * @param weight the configured weight for that factor.
       * @return factor * weight, clamped to `composite_max`.
       */
      inline uint64_t mul_sat(uint64_t factor, uint32_t weight) {
         const unsigned __int128 product =
            static_cast<unsigned __int128>(factor) * static_cast<unsigned __int128>(weight);
         return product > static_cast<unsigned __int128>(composite_max)
                   ? composite_max
                   : static_cast<uint64_t>(product);
      }

      /**
       * Pack a tier + composite score into the `prodrank` sort key.
       *
       * The composite is INVERTED so a higher score sorts EARLIER under the index's ascending
       * iteration, while the tier is not -- a higher tier must sort later. Equal keys fall back to
       * primary-key order (the account name value), which supplies the name tiebreak for free.
       *
       * @param tier      the producer's ordering tier.
       * @param composite the weighted composite score, saturated to `composite_max`.
       * @return the packed sort key stored as `producer_info::rank_score`.
       */
      inline uint64_t pack(producer_tier tier, uint64_t composite) {
         const uint64_t bounded = composite > composite_max ? composite_max : composite;
         return (static_cast<uint64_t>(magic_enum::enum_integer(tier)) << composite_bits)
              | (composite_max - bounded);
      }

      /**
       * The tier encoded in a packed sort key.
       *
       * @param rank_score a key produced by `pack`.
       * @return the encoded tier; `demoted` for any unrecognised value, so an unknown tier sorts
       *         last rather than being trusted.
       */
      inline producer_tier tier_of(uint64_t rank_score) {
         const auto raw = static_cast<uint8_t>(rank_score >> composite_bits);
         return magic_enum::enum_cast<producer_tier>(raw).value_or(producer_tier::demoted);
      }

      /**
       * The sort key of a producer that has never been scored: worst composite in the demoted tier.
       *
       * This is `producer_info::rank_score`'s default, and it is the safe one. A zero key would
       * decode as tier `healthy` with a MAXIMUM inverted composite -- i.e. a registered-but-unscored
       * row would sort ahead of every real producer.
       *
       * @return the packed key for an unscored producer.
       */
      inline uint64_t unscored() {
         return pack(producer_tier::demoted, 0);
      }

      /**
       * Participation factor, derived from the same counter the demotion model maintains -- no
       * additional state.
       *
       * It only orders producers across the misses BEFORE demotion fires; the categorical
       * consequence of being offline is the tier, not this term.
       *
       * A miss short of demotion still has a lasting consequence, and it is deliberate. The streak
       * clears only when the producer PRODUCES, so a producer whose penalty drops it below the
       * active schedule stops being scheduled, stops producing, and holds the penalty until it
       * acts: `regproducer` clears the streak, and enough additional collateral outranks it. That
       * is the intended shape -- a producer that missed is worth less than an identical one that
       * did not, and the way back is an explicit assertion of readiness rather than the passage of
       * time. The alternative, clearing the streak for producers outside the schedule, would let a
       * producer sitting on the boundary flap in and out at every rebuild.
       *
       * @param consecutive_missed_rounds     the producer's current miss streak.
       * @param max_consecutive_missed_rounds the configured demotion threshold.
       * @return the participation factor in basis points, clamped to [0, score_scale].
       */
      inline uint64_t participation_factor(uint32_t consecutive_missed_rounds,
                                           uint32_t max_consecutive_missed_rounds) {
         if (max_consecutive_missed_rounds == 0) return score_scale;
         if (consecutive_missed_rounds >= max_consecutive_missed_rounds) return 0;
         const uint64_t missed = static_cast<uint64_t>(consecutive_missed_rounds) * score_scale
                               / static_cast<uint64_t>(max_consecutive_missed_rounds);
         return score_scale - missed;
      }

      /**
       * Per-factor weights for the composite producer score, plus the demotion threshold.
       *
       * Adding a scoring factor is a new weight field defaulting to 0 plus a new factor function --
       * the packed key's LAYOUT never changes, so the mere existence of a new factor invalidates no
       * stored key. Only a weight CHANGE invalidates scores, and that is what the rescore cursor on
       * the global singleton drains.
       *
       * `relay` / `api` / `benchmark` ship at 0 deliberately. A `peerkeys` row proves registration,
       * not service, and nothing on chain observes an API node or a CPU benchmark at all; weighting
       * a self-declared factor is a free-points vector. They are enabled when an attestation path
       * exists.
       */
      struct [[sysio::table("prodscorecfg"), sysio::contract("sysio.system")]] producer_score_config {
         /// Weight on the collateral ratio (linear, uncapped, min across the required pairs).
         uint32_t collateral_weight    = static_cast<uint32_t>(score_scale);
         /// Weight on the participation factor derived from consecutive missed rounds.
         uint32_t participation_weight = static_cast<uint32_t>(score_scale);
         /// Weight on the snapshot-provider attestation rate. A TENTH of the collateral weight,
         /// deliberately: at parity a single quorum attestation moved the composite by as much as
         /// an entire minimum bond, so among producers bonded near each other the credit decided
         /// the top-21 boundary and the pay-period reset decided it back -- two producer-schedule
         /// and finalizer-policy proposals per snapshot event, with no change in real standing.
         /// Snapshot service should separate producers the collateral term has left tied, not
         /// outrank collateral.
         uint32_t snapshot_weight      = static_cast<uint32_t>(score_scale) / 10;
         /// Reserved -- needs an attestation path before it can carry weight.
         uint32_t relay_weight         = 0;
         /// Reserved -- needs an attestation path before it can carry weight.
         uint32_t api_weight           = 0;
         /// Reserved -- needs an attestation path before it can carry weight.
         uint32_t benchmark_weight     = 0;

         /// Consecutive missed rounds that demote a producer to standby. There is no cooldown and
         /// no expiry: a demoted producer recovers by re-registering, or by producing a block while
         /// still in the active schedule -- the window a schedule too small to rebuild holds open.
         uint32_t max_consecutive_missed_rounds = 3;

         /// Snapshot attestations within one pay period that earn full marks on the snapshot
         /// factor. The counter is reset on the same cadence as the block counters, so this is the
         /// window's target rather than an all-time total.
         uint32_t snapshot_target_attestations = 1;
         /// Rolling window the miss RATE below is measured over, in milliseconds. Mirrors
         /// `sysio.opreg`'s `terminate_window_ms`, which is the batch-operator equivalent: a
         /// producer and a batch operator should answer to the same shape of availability test
         /// even though a producer is DEMOTED where an operator is terminated.
         uint64_t missed_round_window_ms = 24ULL * 60 * 60 * 1000;

         /// Percent of its scheduled rounds a producer may miss inside that window before it is
         /// demoted. Mirrors `terminate_max_pct_misses_24h`. Zero disables the rate gate, leaving
         /// only the consecutive one.
         uint32_t max_pct_missed_rounds_in_window = 5;


         SYSLIB_SERIALIZE(producer_score_config,
            (collateral_weight)(participation_weight)(snapshot_weight)
            (relay_weight)(api_weight)(benchmark_weight)
            (max_consecutive_missed_rounds)(snapshot_target_attestations)
            (missed_round_window_ms)(max_pct_missed_rounds_in_window))
      };

      /// The `prodscorecfg` singleton. Mirrors `emitcfg_t`: absent until governance installs it, so
      /// every read goes through `get_or_default(producer_score_config{})`.
      using producer_score_config_t = sysio::kv::global<"prodscorecfg"_n, producer_score_config>;

      /**
       * The observed-round sample the rate gate needs before it may fire, DERIVED rather than
       * configured: the count at which the two gates agree. Below it the consecutive gate is
       * strictly the stricter of the two, so the rate gate would add nothing except the power to
       * demote a producer on its very first missed round -- at a sample of one, a single miss is a
       * 100% miss rate.
       *
       * @param weights the live score configuration.
       * @return the minimum observed rounds before the rate gate applies.
       */
      inline uint32_t rate_gate_minimum_sample(const producer_score_config& weights) {
         if (weights.max_pct_missed_rounds_in_window == 0) return 0;
         return weights.max_consecutive_missed_rounds * 100u
              / weights.max_pct_missed_rounds_in_window;
      }

      /**
       * Whether a producer's recorded window breaches the miss-RATE gate.
       *
       * @param rounds  scheduled rounds observed in the window.
       * @param missed  how many of them went unproduced.
       * @param weights the live score configuration.
       * @return true iff the rate gate is armed and exceeded.
       */
      inline bool exceeds_miss_rate(uint32_t rounds, uint32_t missed,
                                    const producer_score_config& weights) {
         if (weights.max_pct_missed_rounds_in_window == 0) return false;
         if (rounds < rate_gate_minimum_sample(weights)) return false;
         if (rounds == 0) return false;
         return (missed * 100u / rounds) > weights.max_pct_missed_rounds_in_window;
      }

      /**
       * Whether a producer's recorded misses warrant demotion, under BOTH gates.
       *
       * The two mirror `sysio.opreg::termcheck`: a consecutive run says "you are offline right
       * now", a rate over a rolling window says "you are chronically unreliable". Either demotes.
       *
       * @param consecutive_missed_rounds the producer's current streak.
       * @param rounds                    scheduled rounds observed in the window.
       * @param missed                    how many of them went unproduced.
       * @param weights                   the live score configuration.
       * @return true iff the producer should be demoted.
       */
      inline bool warrants_demotion(uint32_t consecutive_missed_rounds, uint32_t rounds,
                                    uint32_t missed, const producer_score_config& weights) {
         const bool exceeds_consecutive = weights.max_consecutive_missed_rounds > 0
            && consecutive_missed_rounds >= weights.max_consecutive_missed_rounds;
         return exceeds_consecutive || exceeds_miss_rate(rounds, missed, weights);
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
       * Kept in its OWN singleton rather than on `sysio_global_state` because the global is read and
       * written on every block through a cached handle; a 21-name vector on it would widen every
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
