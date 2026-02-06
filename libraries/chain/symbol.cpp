#include <sysio/chain/symbol.hpp>
#include <boost/algorithm/string.hpp>

namespace sysio::chain {
   
   symbol symbol::from_string(const string& from)
{
   try {
      string s = boost::algorithm::trim_copy(from);
      SYS_ASSERT(!s.empty(), symbol_type_exception, "creating symbol from empty string");
      auto comma_pos = s.find(',');
      SYS_ASSERT(comma_pos != string::npos, symbol_type_exception, "missing comma in symbol");
      auto prec_part = s.substr(0, comma_pos);
      uint8_t p = fc::to_int64(prec_part);
      string name_part = s.substr(comma_pos + 1);
      SYS_ASSERT( p <= max_precision, symbol_type_exception, "precision {} should be <= 18", p);
      return symbol(string_to_symbol(p, name_part.c_str()));
   } FC_CAPTURE_LOG_AND_RETHROW("{}", from);
}
   
} // namespace sysio::chain
