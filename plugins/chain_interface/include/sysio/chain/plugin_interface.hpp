#pragma once

#include <appbase/channel.hpp>
#include <appbase/method.hpp>

#include <sysio/chain/block.hpp>
#include <sysio/chain/transaction_metadata.hpp>
#include <sysio/chain/trace.hpp>

#include <tuple>

namespace sysio::chain::plugin_interface {
   using namespace sysio::chain;
   using namespace appbase;
   struct chain_plugin_interface;

   namespace channels {
      /// Payload of the block channels. A delivery is posted to the executor and runs after the controller signal
      /// returns, so it owns its values; controller::block_signal_params only references them for the signal call.
      using block_params = std::tuple<signed_block_ptr, block_id_type>;

      using accepted_block_header  = channel_decl<struct accepted_block_header_tag, block_params>;
      using accepted_block         = channel_decl<struct accepted_block_tag,        block_params>;
      using irreversible_block     = channel_decl<struct irreversible_block_tag,    block_params>;
      using applied_transaction    = channel_decl<struct applied_transaction_tag,   transaction_trace_ptr>;
   }

   namespace methods {
      using get_block_by_id        = method_decl<chain_plugin_interface, signed_block_ptr(const block_id_type& block_id)>;
      using get_head_block_id      = method_decl<chain_plugin_interface, block_id_type ()>;
   }

   namespace incoming {
      namespace methods {
         using transaction_async     = method_decl<chain_plugin_interface, void(const packed_transaction_ptr&, bool, transaction_metadata::trx_type, bool, next_function<transaction_trace_ptr>), first_provider_policy>;
      }
   }

   namespace compat {
      namespace channels {
         using transaction_ack       = channel_decl<struct accepted_transaction_tag, std::pair<fc::exception_ptr, packed_transaction_ptr>>;
      }
   }

} // namespace sysio::chain::plugin_interface
