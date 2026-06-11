#include <sysio/chain/name.hpp>
#include <fc/variant.hpp>
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

namespace fc {
   void to_variant(const sysio::chain::name& c, fc::variant& v) { v = c.to_string(); }
   void from_variant(const fc::variant& v, sysio::chain::name& check) {
      check = sysio::chain::name{ v.get_string() };
   }
} // fc
