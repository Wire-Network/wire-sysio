#pragma once

#include <cstddef>
#include <cstdint>

namespace sysio::net_utils {

inline constexpr uint32_t def_max_trx_in_progress_size = 100u * 1024u * 1024u; // 100 MiB

/// True when a transaction of estimated trx_size bytes fits in the peer's remaining in-flight budget.
constexpr bool fits_trx_in_progress_budget(uint32_t in_progress, size_t trx_size) noexcept {
   return in_progress <= def_max_trx_in_progress_size && trx_size <= def_max_trx_in_progress_size - in_progress;
}

} // namespace sysio::net_utils
