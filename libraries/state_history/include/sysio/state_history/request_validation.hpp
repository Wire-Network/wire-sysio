#pragma once

#include <sysio/state_history/types.hpp>

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace sysio::state_history {

/// Maximum number of resume positions accepted in a single SHiP blocks request.
inline constexpr std::size_t max_have_positions_per_request = 4096;

/// Error reported when a blocks request exceeds the resume position limit.
inline constexpr char have_positions_limit_exceeded[] = "state history have_positions limit exceeded";

/// Reject oversized resume lists before a request is dispatched for block ID lookups.
inline void validate_have_positions(const state_request& request) {
   std::visit([]<typename Request>(const Request& message) {
      if constexpr(std::is_base_of_v<get_blocks_request_v0, Request>) {
         if(message.have_positions.size() > max_have_positions_per_request)
            throw std::runtime_error(have_positions_limit_exceeded);
      }
   }, request);
}

} // namespace sysio::state_history
