#pragma once
#include <fc/variant.hpp>
#include <sysio/trace_api/trace.hpp>
#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/protocol_feature_activation.hpp>

namespace sysio::trace_api {
   struct block_entry_v0 {
      chain::block_id_type   id;
      uint32_t               number = 0;
      uint64_t               offset = 0;
   };

   struct lib_entry_v0 {
      uint32_t               lib    = 0;
   };

   using metadata_log_entry = std::variant<
      block_entry_v0,
      lib_entry_v0,
      block_trxs_entry
   >;

} // namespace sysio::trace_api

FC_REFLECT(sysio::trace_api::block_entry_v0, (id)(number)(offset));
FC_REFLECT(sysio::trace_api::lib_entry_v0, (lib));
