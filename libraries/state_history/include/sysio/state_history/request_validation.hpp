#pragma once

#include <sysio/chain/config.hpp>
#include <sysio/state_history/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace sysio::state_history {

/// Maximum number of resume positions accepted in a single SHiP blocks request.
inline constexpr std::size_t max_have_positions_per_request = 4096;

/// Count the last block produced before the pause and the irreversible checkpoint.
inline constexpr std::uint64_t extra_resume_positions = 2;
static_assert(max_have_positions_per_request >=
                  std::uint64_t{chain::config::default_max_reversible_blocks} + extra_resume_positions,
              "SHiP resume must accommodate the default reversible range, final produced block, and checkpoint");

/// Error reported when a blocks request exceeds the resume position limit.
inline constexpr std::string_view have_positions_limit_exceeded = "state history have_positions limit exceeded";

/// Check the resume list before a request is dispatched for block ID lookups.
inline bool have_positions_within_limit(const state_request& request) {
   return std::visit([]<typename Request>(const Request& message) {
      if constexpr(std::is_base_of_v<get_blocks_request_v0, Request>)
         return message.have_positions.size() <= max_have_positions_per_request;
      else
         return true;
   }, request);
}

} // namespace sysio::state_history
