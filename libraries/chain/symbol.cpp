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
      // Range-checked before narrowing, so a value outside uint8_t cannot wrap into range.
      const int64_t p = fc::to_int64(std::string{prec_part});
      std::string_view name_part = s.substr(comma_pos + 1);
      SYS_ASSERT( p >= 0, symbol_type_exception, "precision {} should be >= 0", p);
      SYS_ASSERT( p <= max_precision, symbol_type_exception, "precision {} should be <= {}", p, max_precision);
      return symbol(string_to_symbol(static_cast<uint8_t>(p), name_part));
   } FC_CAPTURE_LOG_AND_RETHROW("{}", from);
}

} // namespace sysio::chain
