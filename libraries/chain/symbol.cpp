#include <sysio/chain/symbol.hpp>
#include <boost/algorithm/string.hpp>

#include <charconv>
#include <system_error>

namespace sysio::chain {

   symbol symbol::from_string(std::string_view from)
{
   try {
      std::string_view s = boost::algorithm::trim_copy(from);
      SYS_ASSERT(!s.empty(), symbol_type_exception, "creating symbol from empty string");
      auto comma_pos = s.find(',');
      SYS_ASSERT(comma_pos != std::string_view::npos, symbol_type_exception, "missing comma in symbol");
      std::string_view prec_part = s.substr(0, comma_pos);
      // std::from_chars is locale-independent and avoids the std::string round-trip
      // that fc::to_int64(const std::string&) would otherwise force.
      uint8_t p = 0;
      auto [ptr, ec] = std::from_chars(prec_part.data(), prec_part.data() + prec_part.size(), p);
      SYS_ASSERT(ec == std::errc{} && ptr == prec_part.data() + prec_part.size(),
                 symbol_type_exception, "precision is not a valid unsigned integer");
      std::string_view name_part = s.substr(comma_pos + 1);
      SYS_ASSERT( p <= max_precision, symbol_type_exception, "precision {} should be <= 18", p);
      return symbol(string_to_symbol(p, name_part));
   } FC_CAPTURE_LOG_AND_RETHROW("{}", from);
}
   
} // namespace sysio::chain
