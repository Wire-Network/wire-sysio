#pragma once
/**
 * @file group_election.hpp
 * @brief One operator's batch-op group standing for one epoch, plus the
 *        diagnostic that reports a non-election.
 */

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <fc/variant.hpp>
#include <sysio/chain/name.hpp>

namespace sysio::batch_operator_detail {

/// Group-index sentinel. For `my_group` it means "we are not in any
/// batch-op group"; for `current_group`, "no epoch state parsed yet".
inline constexpr uint8_t GROUP_NONE = 255;

/// This operator's standing against one `sysio.epoch::epochstate` reading.
///
/// `current_group` is the group ON DUTY, taken verbatim from
/// `epochstate.current_batch_op_group`. The sliding window keeps the group on
/// duty at the FRONT of `batch_op_groups` — `sysio.epoch::advance` pops the
/// expiring group off — so the on-duty index is NOT a function of the epoch
/// index. Anything reporting the active group reads it from here; deriving it
/// (`epoch_index % groups`) is the static-rotation anti-pattern the sliding
/// window replaced.
struct group_election {
   uint8_t                  my_group      = GROUP_NONE;
   uint8_t                  current_group = GROUP_NONE;
   bool                     is_elected    = false;
   std::vector<chain::name> current_group_members;
};

/// Evaluates `operator_account`'s standing from the epochstate's on-duty group
/// index and its `batch_op_groups` window.
inline group_election evaluate_group_election(uint8_t             current_batch_op_group,
                                              const fc::variants& batch_op_groups,
                                              chain::name         operator_account) {
   group_election election;
   election.current_group = current_batch_op_group;

   for (uint8_t g = 0; g < batch_op_groups.size(); ++g) {
      const auto& group = batch_op_groups[g].get_array();
      for (const auto& member : group) {
         if (chain::name(member.as_string()) == operator_account) {
            election.my_group = g;
         }
      }
      if (g == election.current_group) {
         for (const auto& member : group) {
            election.current_group_members.push_back(chain::name(member.as_string()));
         }
      }
   }

   election.is_elected = (election.my_group == election.current_group);
   return election;
}

/// The non-election diagnostic. `active_group` is `election.current_group` —
/// the value the election itself compared against — so the line can never name
/// a group the decision did not consult.
inline std::string not_elected_message(uint32_t epoch_index, const group_election& election) {
   return std::format("batch_operator: not elected for epoch {} (my_group={}, active_group={})",
                      epoch_index, election.my_group, election.current_group);
}

} // namespace sysio::batch_operator_detail
