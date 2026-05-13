#include <sysio/chain/symbol.hpp>
#include <boost/algorithm/string.hpp>

namespace sysio::chain {
   
   symbol symbol::from_string(std::string_view from)
{
   try {
      std::string_view s = boost::algorithm::trim_copy(from);
      SYS_ASSERT(!s.empty(), symbol_type_exception, "creating symbol from empty string");
      auto comma_pos = s.find(',');
      SYS_ASSERT(comma_pos != std::string_view::npos, symbol_type_exception, "missing comma in symbol");
      std::string_view prec_part = s.substr(0, comma_pos);
      uint8_t p = fc::to_int64(std::string{prec_part});
      std::string_view name_part = s.substr(comma_pos + 1);
      SYS_ASSERT( p <= max_precision, symbol_type_exception, "precision {} should be <= 18", p);
      return symbol(string_to_symbol(p, name_part));
   } FC_CAPTURE_LOG_AND_RETHROW("{}", from);
}
   
} // namespace sysio::chain
