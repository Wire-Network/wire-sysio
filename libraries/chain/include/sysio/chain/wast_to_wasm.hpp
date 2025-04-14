#pragma once
#include <vector>
#include <string>
#include <stdint.h>

namespace sysio { namespace chain {

std::vector<uint8_t> wast_to_wasm( const std::string& wast );

} } /// sysio::chain
