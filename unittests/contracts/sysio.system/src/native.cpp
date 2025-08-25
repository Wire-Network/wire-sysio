#include <sysio.system/native.hpp>

#include <sysio/check.hpp>

namespace sysiosystem {

   void native::onerror( ignore<uint128_t>, ignore<std::vector<char>> ) {
      sysio::check( false, "the onerror action cannot be called directly" );
   }

}
