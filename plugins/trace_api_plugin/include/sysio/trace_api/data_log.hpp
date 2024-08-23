#pragma once
#include <fc/variant.hpp>
#include <sysio/trace_api/trace.hpp>
#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/protocol_feature_activation.hpp>

namespace sysio { namespace trace_api {

   using data_log_entry = std::variant<
      block_trace_v0,
      block_trace_v1,
      block_trace_v2
   >;

}}
