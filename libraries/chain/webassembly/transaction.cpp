#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/apply_context.hpp>

namespace sysio { namespace chain { namespace webassembly {
   /// Unpacks the action a contract passed to send_inline or send_context_free_inline.
   static action unpack_inline_action( span<const char> data, const char* intrinsic ) {
      action act;
      fc::datastream<const char*> ds( data.data(), data.size() );
      fc::raw::unpack( ds, act );
      assert_all_unpacked( ds, intrinsic );
      return act;
   }

   void interface::send_inline( span<const char> data ) {
      //TODO: Why is this limit even needed? And why is it not consistently checked on actions in input or deferred transactions
      SYS_ASSERT( data.size() < context.control.get_global_properties().configuration.max_inline_action_size, inline_action_too_big,
                 "inline action too big" );

      context.execute_inline(unpack_inline_action(data, "send_inline"));
   }

   void interface::send_context_free_inline( span<const char> data ) {
      //TODO: Why is this limit even needed? And why is it not consistently checked on actions in input or deferred transactions
      SYS_ASSERT( data.size() < context.control.get_global_properties().configuration.max_inline_action_size, inline_action_too_big,
                "inline action too big" );

      context.execute_context_free_inline(unpack_inline_action(data, "send_context_free_inline"));
   }

}}} // ns sysio::chain::webassembly
