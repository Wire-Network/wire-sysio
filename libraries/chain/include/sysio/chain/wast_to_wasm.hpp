#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sysio { namespace chain {

std::vector<uint8_t> wast_to_wasm( const std::string& wast );

} } /// sysio::chain
