#pragma once

#include <sysio/name.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.uwrit/sysio.uwrit.hpp>

#include <sysio.system/opreg_status.hpp>
#include <sysio.system/producer_rank.hpp>
#include <sysio.system/sysio.system.hpp>

#include <cstdint>
#include <limits>

// The producer-score FACTORS -- everything that reads the operator registry to turn a producer's
// on-chain standing into the composite score `producer_rank::pack` encodes.
//
// Split from producer_rank.hpp, which sysio.system.hpp includes, because this header pulls in the
// sysio.opreg and sysio.uwrit table declarations plus the OPP protobuf types. Only the translation
// units that actually compute a score include it -- the same reason opreg_status.hpp exists
// separately.

namespace sysiosystem {

   namespace producer_rank {

      /// Well-known sysio.uwrit account -- `locksums` lives here and is the O(1) read cache for a
      /// bucket's active locks.
      namespace uwrit_refs {
         constexpr sysio::name account = "sysio.uwrit"_n;
      }

      /**
       * Balance minus active locks for one (chain, token) pair, read cross-contract from sysio.opreg
       * and sysio.uwrit.
       *
       * This is opreg's `slashable_now`, NOT its `available()`. available() subtracts pending
       * withdraws, and withdraw / cancelwtdw are both free and uncapped -- an operator could
       * oscillate their own rank without moving funds. available() also walks the withdraw queue per
       * account with no row cap, where this is two O(1) lookups. Ranking on what is actually
       * slashable is also the right principle: score should track what can be taken from you, and a
       * queued withdraw does not reduce exposure.
       *
       * @param op         the operator row read from sysio.opreg.
       * @param chain_code the chain slug of the pair.
       * @param token_code the token slug of the pair.
       * @return balance minus active locks, saturating at zero.
       */
      inline uint64_t slashable_now(const sysio::opreg::operator_entry& op,
                                    sysio::slug_name chain_code,
                                    sysio::slug_name token_code) {
         uint64_t balance = 0;
         bool     found   = false;
         for (const auto& entry : op.balances) {
            if (entry.chain_code == chain_code && entry.token_code == token_code) {
               balance = entry.balance;
               found   = true;
               break;
            }
         }
         if (!found) return 0;

         sysio::uwrit::locksums_t sums(uwrit_refs::account);
         const sysio::uwrit::lock_sum_key key{op.account, chain_code, token_code};
         const uint64_t locked = sums.contains(key) ? sums.get(key).amount : 0;
         return balance > locked ? balance - locked : 0;
      }

      /**
       * Collateral factor: the MINIMUM, across every (chain, token) pair in `req_prod_collat`, of
       * slashable / min_bond -- in basis points, linear and uncapped (saturating only at the bit
       * budget).
       *
       * `min` rather than a sum: posting extra on the cheapest chain must do nothing, so raising the
       * score requires lifting EVERY pair and the marginal cost is dominated by the most expensive
       * chain. There is no secondary sum term -- it existed only to stop every min compressing to
       * 1.0 and the ordering falling through to alphabetical, and participation and snapshot now
       * break that tie.
       *
       * An empty `req_prod_collat` scores 0, consistent with opreg's `meets_role_min` returning
       * false for it: such a producer cannot be ACTIVE and so is not schedulable anyway.
       *
       * @param op  the operator row read from sysio.opreg.
       * @param cfg the live opreg configuration carrying `req_prod_collat`.
       * @return the collateral factor in basis points.
       */
      inline uint64_t collateral_factor(const sysio::opreg::operator_entry& op,
                                        const sysio::opreg::op_config& cfg) {
         if (cfg.req_prod_collat.empty()) return 0;

         uint64_t lowest = std::numeric_limits<uint64_t>::max();
         for (const auto& req : cfg.req_prod_collat) {
            // setconfig's require_positive_min_bond already guarantees this (SEC-22); assert rather
            // than divide by zero if that ever regresses.
            check(req.min_bond > 0, "req_prod_collat entry has a zero min_bond");

            // uint128 intermediate is mandatory: slashable_now runs to 2^62 and multiplying by
            // score_scale overflows uint64.
            const unsigned __int128 scaled =
               static_cast<unsigned __int128>(slashable_now(op, req.chain_code, req.token_code))
               * static_cast<unsigned __int128>(score_scale);
            const unsigned __int128 ratio = scaled / static_cast<unsigned __int128>(req.min_bond);
            const uint64_t bounded = ratio > static_cast<unsigned __int128>(composite_max)
                                        ? composite_max
                                        : static_cast<uint64_t>(ratio);
            if (bounded < lowest) lowest = bounded;
         }
         return lowest;
      }

      /**
       * Snapshot-service factor: how much of the configured attestation target the producer met in
       * the current pay period.
       *
       * Scored on ATTESTATIONS, not on registration. A `snapprovs` row is free to create; actually
       * voting a snapshot hash that reaches quorum is not. The counter is maintained by
       * `snapshot_attest::votesnaphash` and reset on the same `payepoch` cadence as the block
       * counters, which supplies the trailing window with no extra machinery.
       *
       * @param snapshot_attestations the producer's attestation count this pay period.
       * @param target                the configured count that earns full marks.
       * @return the snapshot factor in basis points, clamped to [0, score_scale].
       */
      inline uint64_t snapshot_factor(uint32_t snapshot_attestations, uint32_t target) {
         if (target == 0) return 0;
         if (snapshot_attestations >= target) return score_scale;
         return static_cast<uint64_t>(snapshot_attestations) * score_scale
              / static_cast<uint64_t>(target);
      }

      /**
       * The ONE schedulable predicate every rank consumer walks.
       *
       * `rank` is position among the producers this returns true for -- so every consumer must
       * COUNT matches while walking the index, never take the first N index entries. An unbonded
       * non-bootstrapped registrant is UNKNOWN in opreg and occupies an index slot ahead of the
       * bootstrap tier; taking the first N would let a handful of them crowd real producers out of
       * peer discovery and snapshot-provider eligibility.
       *
       * Before this existed the four consumers disagreed -- update_ranked_producers checked all
       * three conditions, emissions only the first two, peer_keys and snapshot_attest none. Making
       * emissions honour the finalizer-key check is a behavioural fix, not a regression: a producer
       * with no active finalizer key can never be scheduled, so it should not draw top-21 pay.
       *
       * @param producer   the producer row under consideration.
       * @param finalizers the sysio.system finalizers table.
       * @return true iff the producer is eligible to occupy a rank position.
       */
      inline bool is_schedulable(const producer_info& producer, finalizers_table& finalizers) {
         if (!producer.active()) return false;
         if (!is_op_active(producer.owner, sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER)) {
            return false;
         }
         const auto key = finalizer_key_t{producer.owner.value};
         if (!finalizers.contains(key)) return false;
         return !finalizers.get(key).active_key_binary.empty();
      }

      /**
       * The producer's ordering tier.
       *
       * Demotion outranks the bootstrap flag: a demoted bootstrap is still offline, and the whole
       * point of the bootstrapped tier is to be a live backstop.
       *
       * @param is_demoted      whether the producer is currently demoted for missed rounds.
       * @param is_bootstrapped the operator row's genesis flag.
       * @return the tier to encode in the packed key.
       */
      inline producer_tier tier_for(bool is_demoted, bool is_bootstrapped) {
         if (is_demoted)      return producer_tier::demoted;
         if (is_bootstrapped) return producer_tier::bootstrapped;
         return producer_tier::healthy;
      }

      /// The per-producer inputs a score needs from `producer_info`. Passed as a struct rather than
      /// four positional flags so a new factor's input is a new member, not a new parameter at every
      /// call site.
      struct score_inputs {
         /// Whether the producer is currently demoted for consecutive missed rounds.
         bool     is_demoted                 = false;
         /// The producer's current miss streak.
         uint32_t consecutive_missed_rounds  = 0;
         /// Snapshot attestations credited this pay period.
         uint32_t snapshot_attestations      = 0;
      };

      /**
       * Compute a producer's packed `rank_score` from its on-chain standing.
       *
       * The composite is a weighted sum of capped, normalised factors; the collateral term is the
       * one deliberately-unbounded input, so the sum saturates rather than wraps. Adding a factor
       * means adding a weight and a term here -- the packed layout never changes.
       *
       * @param producer the producer account being scored.
       * @param inputs   the per-producer counters read from `producer_info`.
       * @param weights  the live `prodscorecfg` weights.
       * @return the packed sort key to store on `producer_info::rank_score`.
       */
      inline uint64_t compute(const sysio::name& producer,
                              const score_inputs& inputs,
                              const producer_score_config& weights) {
         // A producer that is not a live, collateral-backed PRODUCER operator scores into the
         // demoted tier. That is correct on its own terms -- an unbonded registrant must never
         // outrank a bonded one -- and it is also what BOUNDS the rank walk: `regproducer` is
         // permissionless, so without this every consumer would scan an unbounded table. With it,
         // the healthy and bootstrapped tiers hold only ACTIVE producer operators, and a consumer
         // stops at the first demoted entry.
         if (!is_op_active(producer, sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER)) {
            return unscored();
         }

         sysio::opreg::operators_t ops(opreg_refs::account);
         const auto op_key = sysio::opreg::operator_key{producer.value};
         if (!ops.contains(op_key)) return unscored();
         const auto op = ops.get(op_key);

         sysio::opreg::opconfig_t opreg_cfg_tbl(opreg_refs::account);
         const auto opreg_cfg = opreg_cfg_tbl.get_or_default(sysio::opreg::op_config{});

         // Every term saturates: the collateral factor is uncapped by design, so factor * weight
         // must not be allowed to wrap.
         const uint64_t collateral =
            mul_sat(collateral_factor(op, opreg_cfg), weights.collateral_weight);
         const uint64_t participation =
            mul_sat(participation_factor(inputs.consecutive_missed_rounds,
                                         weights.max_consecutive_missed_rounds),
                    weights.participation_weight);
         const uint64_t snapshot =
            mul_sat(snapshot_factor(inputs.snapshot_attestations,
                                    weights.snapshot_target_attestations),
                    weights.snapshot_weight);

         const uint64_t composite = add_sat(add_sat(collateral, participation), snapshot);
         return pack(tier_for(inputs.is_demoted, op.is_bootstrapped), composite);
      }

   } // namespace producer_rank

} // namespace sysiosystem
