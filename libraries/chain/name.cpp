#include <sysio/chain/name.hpp>
#include <fc/exception/exception.hpp>
#include <sysio/chain/exceptions.hpp>

namespace sysio::chain {

   // Out-of-line so name.hpp need not pull in <sysio/chain/exceptions.hpp>.
   void sysio_name_traits::throw_invalid( std::string_view in, const char* why ) {
      SYS_ASSERT( false, name_type_exception,
                  "Invalid name ({}): {}", std::string(in), why );
      __builtin_unreachable();
   }

} // namespace sysio::chain
