#include <sysio/chain/s_root_extension.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>

namespace sysio { namespace chain {

void validate_s_root_extensions_match(const extensions_type& received,
                                      const extensions_type& constructed) {
   const auto is_sre = [](const auto& e) { return e.first == s_root_extension::extension_id(); };

   auto rcvd_it = std::find_if(received.begin(),    received.end(),    is_sre);
   auto crtd_it = std::find_if(constructed.begin(), constructed.end(), is_sre);
   uint32_t slot = 0;

   while (rcvd_it != received.end() || crtd_it != constructed.end()) {
      ++slot;
      const bool rcvd_has = rcvd_it != received.end();
      const bool crtd_has = crtd_it != constructed.end();

      SYS_ASSERT(rcvd_has == crtd_has, block_validate_exception,
                 "s_root_extension count mismatch at slot {}: received did{} have an entry, "
                 "constructed did{}",
                 slot, rcvd_has ? "" : " not", crtd_has ? "" : " not");

      if (rcvd_it->second != crtd_it->second) {
         s_header rcvd_h = fc::raw::unpack<s_header>(rcvd_it->second);
         s_header crtd_h = fc::raw::unpack<s_header>(crtd_it->second);
         SYS_THROW(block_validate_exception,
                   "s_root_extension payload mismatch at slot {}: received {}; constructed {}",
                   slot, rcvd_h.to_string(), crtd_h.to_string());
      }

      rcvd_it = std::find_if(std::next(rcvd_it), received.end(),    is_sre);
      crtd_it = std::find_if(std::next(crtd_it), constructed.end(), is_sre);
   }
}

}} // namespace sysio::chain
