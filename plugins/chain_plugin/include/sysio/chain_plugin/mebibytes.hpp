#pragma once

#include <sysio/chain/exceptions.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

namespace sysio {

/// Bytes in one MiB, the unit of the `*-mb` options.
inline constexpr uint64_t bytes_per_mebibyte = 1024 * 1024;

/**
 * Convert the value of a MiB option to bytes.
 * @param value        configured MiB
 * @param option_name  option @p value was read from, named in the error
 * @return @p value in bytes
 * @throws chain::plugin_config_exception if @p value is 0 or its byte count does not fit in uint64_t
 */
inline uint64_t checked_mebibytes(uint64_t value, std::string_view option_name) {
   constexpr uint64_t max_mebibytes = std::numeric_limits<uint64_t>::max() / bytes_per_mebibyte;
   SYS_ASSERT(value > 0 && value <= max_mebibytes, chain::plugin_config_exception, "{} {} must be between 1 and {} MiB",
              option_name, value, max_mebibytes);
   return value * bytes_per_mebibyte;
}

} // namespace sysio
