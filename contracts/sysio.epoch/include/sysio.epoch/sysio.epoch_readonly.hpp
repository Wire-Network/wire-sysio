#pragma once

// =============================================================================
// Read-only mirror of sysio.epoch singleton types, exposed for cross-contract
// readers (notably sysio.system's emissions). No [[sysio::table]] attribute
// on these structs -- they MUST NOT emit ABI entries on the consumer's side.
//
// INVARIANT: field order and types here MUST track the canonical definitions
// in sysio.epoch.hpp. A mismatch deserializes garbage bytes at runtime with
// no compile-time signal.
//
// When adding / reordering / renaming a field on the canonical epoch_state
// or epoch_config, update this file in the SAME commit. The two definitions
// live in the same directory so reviewers see them side by side.
// =============================================================================

#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/name.hpp>
#include <sysio/time.hpp>

#include <cstdint>
#include <vector>

namespace sysio::epoch::readonly {

struct epoch_state {
   uint32_t                              current_epoch_index    = 0;
   sysio::time_point                     current_epoch_start{};
   sysio::time_point                     next_epoch_start{};
   uint8_t                               current_batch_op_group = 0;
   std::vector<std::vector<sysio::name>> batch_op_groups;
   sysio::checksum256                    last_consensus_hash;
   bool                                  is_paused              = false;

   SYSLIB_SERIALIZE(epoch_state,
      (current_epoch_index)(current_epoch_start)(next_epoch_start)
      (current_batch_op_group)(batch_op_groups)(last_consensus_hash)(is_paused))
};

using epochstate_t = sysio::kv::global<"epochstate"_n, epoch_state>;

struct epoch_config {
   uint32_t epoch_duration_sec                = 0;
   uint32_t operators_per_epoch               = 0;
   uint32_t batch_operator_minimum_active     = 0;
   uint32_t batch_op_groups                   = 0;
   uint32_t attestation_retention_epoch_count = 0;

   SYSLIB_SERIALIZE(epoch_config,
      (epoch_duration_sec)(operators_per_epoch)
      (batch_operator_minimum_active)(batch_op_groups)
      (attestation_retention_epoch_count))
};

using epochcfg_t = sysio::kv::global<"epochcfg"_n, epoch_config>;

// Mirror of sysio::epoch::batchsnap_key. Field order / types MUST track
// the canonical definition in sysio.epoch.hpp.
struct batchsnap_key {
   uint64_t epoch_index;
   SYSLIB_SERIALIZE(batchsnap_key, (epoch_index))
};

// Mirror of sysio::epoch::batch_snapshot. Field order / types MUST track
// the canonical definition in sysio.epoch.hpp.
struct batch_snapshot {
   uint64_t                epoch_index        = 0;
   uint8_t                 active_group_index = 0;
   std::vector<sysio::name> active_members;

   SYSLIB_SERIALIZE(batch_snapshot,
      (epoch_index)(active_group_index)(active_members))
};

using batchsnaps_t = sysio::kv::table<"batchsnap"_n, batchsnap_key, batch_snapshot>;

} // namespace sysio::epoch::readonly
