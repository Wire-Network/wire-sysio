#pragma once

#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <fc/container/ordered_diff.hpp>
#include <boost/container/flat_set.hpp>

namespace sysio::chain {

using producer_auth_differ = fc::ordered_diff<producer_authority, uint16_t>;
// Verify producer_auth_differ::size_type can represent all index values in the
// diff between two policies that could each hold up to max_proposers entries.
static_assert(std::numeric_limits<producer_auth_differ::size_type>::max() >= config::max_proposers - 1);
using producer_auth_diff_t = producer_auth_differ::diff_result;

struct proposer_policy_diff {
   uint32_t                    version = 0; ///< sequentially incrementing version number of producer_authority_schedule
   block_timestamp_type        proposal_time; // block when schedule was proposed
   producer_auth_diff_t        producer_auth_diff;
};

struct proposer_policy {
   // Useful for light clients, not necessary for nodeos
   block_timestamp_type        proposal_time; // block when schedule was proposed
   producer_authority_schedule proposer_schedule;

   proposer_policy_diff create_diff(const proposer_policy& target) const {
      return {.version = target.proposer_schedule.version,
              .proposal_time = target.proposal_time,
              .producer_auth_diff = producer_auth_differ::diff(proposer_schedule.producers, target.proposer_schedule.producers)};
   }

   template <typename X>
   requires std::same_as<std::decay_t<X>, proposer_policy_diff>
   [[nodiscard]] proposer_policy apply_diff(X&& diff) const {
      proposer_policy result;
      result.proposer_schedule.version = diff.version;
      result.proposal_time = diff.proposal_time;
      auto copy = proposer_schedule.producers;
      result.proposer_schedule.producers = producer_auth_differ::apply_diff(std::move(copy),
                                                                            std::forward<X>(diff).producer_auth_diff);
      return result;
   }

   // Validates structural well-formedness of the policy. Single source of truth
   // reused by the set_proposed_producers host function and snapshot loading.
   // Two things are intentionally NOT checked here and stay at the intrinsic
   // call site instead:
   //  - account existence (requires apply_context)
   //  - K1/R1 key type enforcement (uses unactivated_key_type to signal that
   //    non-K1/R1 keys need a protocol feature; distinct from structural errors)
   // Throws producer_schedule_exception on violation.
   void validate() const {
      const auto& producers = proposer_schedule.producers;
      SYS_ASSERT(!producers.empty(), producer_schedule_exception,
                 "producer schedule must not be empty");
      SYS_ASSERT(producers.size() <= config::max_producers, producer_schedule_exception,
                 "producer count ({}) exceeds max ({})",
                 producers.size(), config::max_producers);
      boost::container::flat_set<account_name> unique_producers;
      unique_producers.reserve(producers.size());
      for (const auto& p : producers) {
         SYS_ASSERT(unique_producers.insert(p.producer_name).second, producer_schedule_exception,
                    "duplicate producer name {}", p.producer_name);
         std::visit([&](const auto& a) {
            SYS_ASSERT(a.threshold > 0, producer_schedule_exception,
                       "producer {} authority threshold must be positive", p.producer_name);
            boost::container::flat_set<public_key_type> unique_keys;
            unique_keys.reserve(a.keys.size());
            uint32_t sum_weights = 0;
            for (const auto& kw : a.keys) {
               SYS_ASSERT(unique_keys.insert(kw.key).second, producer_schedule_exception,
                          "producer {} authority has duplicate key", p.producer_name);
               if (std::numeric_limits<uint32_t>::max() - sum_weights <= kw.weight) {
                  sum_weights = std::numeric_limits<uint32_t>::max();
               } else {
                  sum_weights += kw.weight;
               }
            }
            SYS_ASSERT(sum_weights >= a.threshold, producer_schedule_exception,
                       "producer {} authority threshold ({}) not satisfiable by sum of key weights ({})",
                       p.producer_name, a.threshold, sum_weights);
         }, p.authority);
      }
   }
};

using proposer_policy_ptr = std::shared_ptr<proposer_policy>;

} /// sysio::chain

FC_REFLECT( sysio::chain::proposer_policy, (proposal_time)(proposer_schedule) )
FC_REFLECT( sysio::chain::producer_auth_diff_t, (remove_indexes)(insert_indexes) )
FC_REFLECT( sysio::chain::proposer_policy_diff, (version)(proposal_time)(producer_auth_diff) )
