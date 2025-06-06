#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/global_property_object.hpp>

namespace sysio { namespace chain { namespace webassembly {
   int32_t interface::read_action_data(legacy_span<char> memory) const {
      auto s = context.get_action().data.size();
      auto copy_size = std::min( static_cast<size_t>(memory.size()), s );
      if( copy_size == 0 ) return s;
      std::memcpy( memory.data(), context.get_action().data.data(), copy_size );

      return copy_size;
   }

   int32_t interface::action_data_size() const {
      return context.get_action().data.size();
   }

   name interface::current_receiver() const {
      return context.get_receiver();
   }

   void interface::set_action_return_value( span<const char> packed_blob ) {
      auto max_action_return_value_size = 
         context.control.get_global_properties().configuration.max_action_return_value_size;
      if( !context.trx_context.is_read_only() )
         SYS_ASSERT(packed_blob.size() <= max_action_return_value_size,
                    action_return_value_exception,
                    "action return value size must be less or equal to ${s} bytes", ("s", max_action_return_value_size));
      context.action_return_value.assign( packed_blob.data(), packed_blob.data() + packed_blob.size() );
   }
}}} // ns sysio::chain::webassembly
