#pragma once

#include <sysio/chain/exceptions.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace sysio {

/// Bytes in one MiB, the unit of the `*-mb` options.
inline constexpr uint64_t bytes_per_mebibyte = 1024 * 1024;

/// Largest MiB count whose byte count fits in uint64_t.
inline constexpr uint64_t max_mebibytes = std::numeric_limits<uint64_t>::max() / bytes_per_mebibyte;

/**
 * Convert the value of a MiB option to bytes.
 * @param value        configured MiB, of the option's own type so that a negative value stays negative
 * @param option_name  option @p value was read from, named in the error
 * @param max_value    largest MiB the option accepts, capped at max_mebibytes
 * @return @p value in bytes
 * @throws chain::plugin_config_exception if @p value is outside 1 to @p max_value
 */
template <std::integral T>
uint64_t checked_mebibytes(T value, std::string_view option_name, uint64_t max_value = max_mebibytes) {
   const uint64_t limit = std::min(max_value, max_mebibytes);
   SYS_ASSERT(std::cmp_greater_equal(value, 1) && std::cmp_less_equal(value, limit), chain::plugin_config_exception,
              "{} {} must be between 1 and {} MiB", option_name, value, limit);
   return static_cast<uint64_t>(value) * bytes_per_mebibyte;
}

} // namespace sysio
