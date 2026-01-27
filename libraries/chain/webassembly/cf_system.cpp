#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/apply_context.hpp>

namespace sysio { namespace chain { namespace webassembly {
   inline static constexpr size_t max_assert_message = 1024;
   void interface::abort() const {
      SYS_ASSERT( false, abort_called, "abort() called" );
   }

   void interface::sysio_assert( bool condition, null_terminated_ptr msg ) const {
      if( BOOST_UNLIKELY( !condition ) ) {
         const size_t sz = strnlen( msg.data(), max_assert_message );
         std::string message( msg.data(), sz );
         SYS_THROW( sysio_assert_message_exception, "assertion failure with message: {}", message );
      }
   }

   void interface::sysio_assert_message( bool condition, legacy_span<const char> msg ) const {
      if( BOOST_UNLIKELY( !condition ) ) {
         const size_t sz = msg.size() > max_assert_message ? max_assert_message : msg.size();
         std::string message( msg.data(), sz );
         SYS_THROW( sysio_assert_message_exception, "assertion failure with message: {}", message );
      }
   }

   void interface::sysio_assert_code( bool condition, uint64_t error_code ) const {
      if( BOOST_UNLIKELY( !condition ) ) {
         if( error_code >= static_cast<uint64_t>(system_error_code::generic_system_error) ) {
            restricted_error_code_exception e( FC_LOG_MESSAGE(
                                                   error,
                                                   "sysio_assert_code called with reserved error code: {}",
                                                   error_code
            ) );
            e.error_code = static_cast<uint64_t>(system_error_code::contract_restricted_error_code);
            throw e;
         } else {
            sysio_assert_code_exception e( FC_LOG_MESSAGE(
                                             error,
                                             "assertion failure with error code: {}",
                                             error_code
            ) );
            e.error_code = error_code;
            throw e;
         }
      }
   }

   //be aware that SYS VM OC handles sysio_exit internally and this function will not be called by OC
   void interface::sysio_exit( int32_t code ) const {
      throw wasm_exit{};
   }
}}} // ns sysio::chain::webassembly
