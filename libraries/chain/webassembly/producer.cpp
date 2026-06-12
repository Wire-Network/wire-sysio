#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/apply_context.hpp>

namespace sysio { namespace chain { namespace webassembly {
   int32_t interface::get_active_producers( span<char> producers ) const {
      auto active_producers = context.get_active_producers();

      // The buffer length is a byte count (the CDT-side ABI passes bytes), so the
      // parameter is a byte span: its size() is the caller's byte length and only
      // that many bytes of wasm memory are validated and written. A span of
      // account_name here would treat the raw length as an element count and
      // validate sizeof(account_name) times the caller's buffer.
      size_t len = active_producers.size();
      auto s = len * sizeof(chain::account_name);
      if( producers.size() == 0 ) return s;

      auto copy_size = std::min( producers.size(), s );
      std::memcpy( producers.data(), active_producers.data(), copy_size );

      return copy_size;
   }
}}} // ns sysio::chain::webassembly
