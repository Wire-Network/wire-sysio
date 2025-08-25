#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/apply_context.hpp>

namespace sysio { namespace chain { namespace webassembly {
   void interface::send_inline( legacy_span<const char> data ) {
      //TODO: Why is this limit even needed? And why is it not consistently checked on actions in input or deferred transactions
      SYS_ASSERT( data.size() < context.control.get_global_properties().configuration.max_inline_action_size, inline_action_too_big,
                 "inline action too big" );

      action act;
      fc::raw::unpack<action>(data.data(), data.size(), act);
      context.execute_inline(std::move(act));
   }

   void interface::send_context_free_inline( legacy_span<const char> data ) {
      //TODO: Why is this limit even needed? And why is it not consistently checked on actions in input or deferred transactions
      SYS_ASSERT( data.size() < context.control.get_global_properties().configuration.max_inline_action_size, inline_action_too_big,
                "inline action too big" );

      action act;
      fc::raw::unpack<action>(data.data(), data.size(), act);
      context.execute_context_free_inline(std::move(act));
   }

}}} // ns sysio::chain::webassembly
