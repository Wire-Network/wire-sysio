#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace sysio::state_history {

/// Adds a SHiP acknowledgment grant up to the maximum representable credit balance.
constexpr uint32_t add_send_credits(uint32_t balance, uint32_t grant) {
   constexpr auto max_credits = std::numeric_limits<uint32_t>::max();
   return balance + std::min(grant, max_credits - balance);
}

} // namespace sysio::state_history
