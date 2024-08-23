#pragma once

#include <sysio/state_history/types.hpp>

namespace sysio {
namespace state_history {

std::vector<table_delta> create_deltas(const chainbase::database& db, bool full_snapshot);

} // namespace state_history
} // namespace sysio
