#pragma once

#include <sysio/chain/types.hpp>

namespace sysio {
namespace state_history {

using chain::bytes;

bytes zlib_compress_bytes(const bytes& in);
bytes zlib_decompress(std::string_view);

} // namespace state_history
} // namespace sysio
