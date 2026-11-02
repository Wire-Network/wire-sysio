#pragma once

#include <sysio/chain/kv_store.hpp>

#include <memory>

namespace sysio::testing {

/**
 * @brief A kv_store over std::map plus an undo log, reachable from tests only.
 *
 * It exists to pin down the seam's contract: the KV suites run against it and against chainbase, and any difference
 * in what they observe is a finding. It keeps state on the heap, so it survives a controller restart only while the
 * same object is handed to the new controller, and nothing is written to disk.
 */
std::shared_ptr<chain::kv_store> make_map_kv_store();

} // namespace sysio::testing
