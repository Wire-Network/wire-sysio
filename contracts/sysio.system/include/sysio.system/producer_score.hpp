#pragma once

#include <sysio/name.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opreg/sysio.opreg.hpp>

#include <sysio.system/opreg_status.hpp>
#include <sysio.system/producer_rank.hpp>
#include <sysio.system/sysio.system.hpp>

#include <cstdint>
#include <limits>

// The producer-score FACTORS -- everything that reads the operator registry to turn a producer's
// on-chain standing into the composite score `producer_rank::pack` encodes.
//
// Split from producer_rank.hpp, which sysio.system.hpp includes, because this header pulls in the
// sysio.opreg table declarations plus the OPP protobuf types. Only the translation units that
// actually compute a score include it -- the same reason opreg_status.hpp exists separately.

namespace sysiosystem {

   namespace producer_rank {

      /**
       * The bond a producer has posted on one (chain, token) pair, read from its sysio.opreg row.
       *
       * This is what a slash seizes from a producer, and it is deliberately the BALANCE rather than
       * opreg's `available()`: available() subtracts pending withdraws, and withdraw / cancelwtdw are
       * both free, uncapped and cooldown-free, so an operator could oscillate their own rank without
       * moving funds. Score tracks what can be taken from you, and a queued withdraw does not reduce
       * exposure. Nothing is subtracted for sysio.uwrit locks: an account holds exactly one operator
       * row of one type, and only ACTIVE underwriters ever carry locks, so a PRODUCER's whole balance
       * is slashable.
       *
       * @param op         the operator row read from sysio.opreg.
       * @param chain_code the chain slug of the pair.
       * @param token_code the token slug of the pair.
       * @return the pair's balance, or 0 when the operator holds no row for it.
       */
      inline uint64_t bonded_balance(const sysio::opreg::operator_entry& op,
                                     sysio::slug_name chain_code,
                                     sysio::slug_name token_code) {
         for (const auto& entry : op.balances) {
            if (entry.chain_code == chain_code && entry.token_code == token_code) {
               return entry.balance;
            }
         }
         return 0;
      }

      /**
       * Collateral factor: the MINIMUM, across every (chain, token) pair in `req_prod_collat`, of
       * bonded / min_bond -- in basis points, linear and uncapped (saturating only at the bit
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

            // uint128 intermediate is mandatory: a bond runs to 2^62 and multiplying by
            // score_scale overflows uint64.
            const unsigned __int128 scaled =
               static_cast<unsigned __int128>(bonded_balance(op, req.chain_code, req.token_code))
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
       * The operator half of the schedulable predicate: an active `producers` row whose owner is
       * an ACTIVE OPERATOR_TYPE_PRODUCER operator in sysio.opreg.
       *
       * This is the predicate PEER DISCOVERY walks (`getpeerkeys`), and it deliberately stops
       * short of the finalizer-key requirement. A producer scheduled through `setprods` -- the
       * bootstrap window, and every harness that publishes schedules directly -- produces blocks
       * and needs the BP gossip mesh whether or not it has registered a finalizer key yet; hiding
       * it from `getpeerkeys` cuts a live block producer out of that mesh. Ranking, pay and
       * snapshot-provider eligibility walk `is_schedulable` below.
       *
       * @param producer the producer row under consideration.
       * @return true iff the producer is a live PRODUCER operator.
       */
      inline bool is_eligible_operator(const producer_info& producer) {
         return producer.active()
             && is_op_active(producer.owner, sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER);
      }

      /**
       * The ONE schedulable predicate ranking, pay and snapshot eligibility walk:
       * `is_eligible_operator` plus an active finalizer key.
       *
       * `rank` is position among the producers this returns true for -- so every consumer must
       * COUNT matches while walking the index, never take the first N index entries. An unbonded
       * non-bootstrapped registrant is UNKNOWN in opreg and occupies an index slot ahead of the
       * bootstrap tier; taking the first N would let a handful of them crowd real producers out of
       * peer discovery and snapshot-provider eligibility.
       *
       * Before this existed the consumers disagreed -- update_ranked_producers checked all three
       * conditions, emissions only the first two, peer_keys and snapshot_attest none. Making
       * emissions honour the finalizer-key check is a behavioural fix, not a regression: a producer
       * with no active finalizer key can never be scheduled, so it should not draw top-21 pay.
       *
       * @param producer   the producer row under consideration.
       * @param finalizers the sysio.system finalizers table.
       * @return true iff the producer is eligible to occupy a rank position.
       */
      inline bool is_schedulable(const producer_info& producer, finalizers_table& finalizers) {
         if (!is_eligible_operator(producer)) return false;
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
         /// Whether the producers row is active -- false after `unregprod` parks it.
         bool     is_active                  = true;
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
         // A producer that is not a live, collateral-backed PRODUCER operator -- parked by
         // `unregprod`, unbonded, slashed, terminated -- scores into the demoted tier. That is
         // correct on its own terms (an unbonded registrant must never outrank a bonded one) and it
         // is also what BOUNDS the rank walk: `regproducer` is permissionless, so without this
         // every consumer would scan an unbounded table. With it, the healthy and bootstrapped
         // tiers hold only producers that were live at their LAST rescore, and a consumer stops at
         // the first demoted entry. Every event that can end a producer's standing rescores it --
         // `unregprod` directly, and sysio.opreg through its `processprod` notification, which it
         // dispatches on every balance change AND on slash and termination. The one row a rescore
         // cannot reach is one `prune` erased, and the termination before it already sank the
         // key; every consumer still tests the live predicate before counting a position.
         if (!inputs.is_active) return unscored();

         sysio::opreg::operators_t ops(opreg_refs::account);
         const auto op_key = sysio::opreg::operator_key{producer.value};
         if (!ops.contains(op_key)) return unscored();
         const auto op = ops.get(op_key);
         if (op.status != sysio::opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE
             || op.type != sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER) {
            return unscored();
         }

         sysio::opreg::opconfig_t opreg_cfg_tbl(opreg_refs::account);
         const auto opreg_cfg = opreg_cfg_tbl.get_or_default(sysio::opreg::op_config{});

         // The LIVE minimum decides eligibility, not the stored status. `sysio.opreg::setconfig`
         // rewrites the requirement vectors and re-evaluates nobody: it is the one event that can
         // leave an operator ACTIVE while it no longer meets the bar, and the status it wrote
         // under the old minimums would otherwise keep it scheduled and paid indefinitely.
         //
         // The test costs nothing extra. `collateral_factor` is the ratio of posted bond to the
         // required minimum, taken across every required pair, so a value below `score_scale` IS
         // "short on at least one pair" -- the same question `meets_role_min` answers, asked of
         // the numbers already in hand. Calling opreg's own predicate instead would drag in its
         // pending-withdraw walk, which is unbounded per account, on a path that runs for every
         // scored row.
         //
         // Only the config case needs catching here: any BALANCE movement already re-evaluates
         // status in opreg and notifies this contract. And the sweep that `setconfig` opens is
         // what carries the new minimums across the table, so convergence is bounded rather than
         // immediate -- which is all it needs to be.
         //
         // Bootstrapped producers are exempt, exactly as they are in `meets_role_min`: they are
         // ACTIVE by fiat and hold no bond to measure.
         const uint64_t collateral_ratio = collateral_factor(op, opreg_cfg);
         if (!op.is_bootstrapped && collateral_ratio < score_scale) return unscored();

         // Every term saturates: the collateral factor is uncapped by design, so factor * weight
         // must not be allowed to wrap.
         const uint64_t collateral = mul_sat(collateral_ratio, weights.collateral_weight);
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

      /**
       * Recompute one producer's packed `rank_score` from its live standing and store it if it
       * moved.
       *
       * The ONE write path for the key. Every event that moves a scoring input ends here: a
       * collateral change (via the opreg notification), a missed or produced round, a snapshot
       * attestation credit, the pay-period counter reset, and the rescore sweep a weight or
       * collateral-minimum change opens. A factor whose event does not reach this function never
       * reaches the index.
       *
       * @param self      the sysio.system contract account.
       * @param producers the producers table.
       * @param producer  the producer to rescore; a name with no row is ignored.
       */
      inline void rescore(const sysio::name& self, producers_table& producers, const sysio::name& producer) {
         const auto key = producer_key_t{producer.value};
         if (!producers.contains(key)) return;

         producer_score_config_t weights_tbl(self);
         const auto weights = weights_tbl.get_or_default(producer_score_config{});

         const auto info  = producers.get(key);
         const auto score = compute(
            producer,
            score_inputs{
               .is_active                 = info.active(),
               .is_demoted                = info.is_demoted,
               .consecutive_missed_rounds = info.consecutive_missed_rounds,
               .snapshot_attestations     = info.snapshot_attestations
            },
            weights);

         // The period's snapshot credit is cleared at the EVENTS that drop a producer out of the
         // pay walk -- demotion and `unregprod` -- not inferred here from a tier change. Inferring
         // it was wrong: a row can be re-entering and freshly credited in the same block, and a
         // tier comparison cannot tell that credit apart from a stale one, so it consumed credits
         // that had just been earned.
         if (score == info.rank_score) return;   // no index move needed
         producers.modify(same_payer, key, [&](auto& row) { row.rank_score = score; });
      }

   } // namespace producer_rank

} // namespace sysiosystem
