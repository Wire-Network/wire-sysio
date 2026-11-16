#include <string>
#include <sysio/chain/asset.hpp>
#include <boost/rational.hpp>
#include <boost/algorithm/string.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/safe.hpp>
#include <fc/string.hpp>

namespace sysio { namespace chain {

using fc::safe;

namespace {

constexpr char minus_sign = '-';

} // anonymous namespace

uint8_t asset::decimals()const {
   return sym.decimals();
}

string asset::symbol_name()const {
   return sym.name();
}

int64_t asset::precision()const {
   return sym.precision();
}

string asset::to_string()const {
   string sign = amount < 0 ? "-" : "";
   int64_t abs_amount = std::abs(amount);
   string result = std::to_string( static_cast<int64_t>(abs_amount) / precision());
   if( decimals() )
   {
      auto fract = static_cast<int64_t>(abs_amount) % precision();
      result += "." + std::to_string(precision() + fract).erase(0,1);
   }
   return sign + result + " " + symbol_name();
}

asset asset::from_string(std::string_view from)
{
   try {
      std::string_view s = boost::algorithm::trim_copy(from);

      // Find space in order to split amount and symbol
      auto space_pos = s.find(' ');
      SYS_ASSERT((space_pos != std::string_view::npos), asset_type_exception, "Asset's amount and symbol should be separated with space");
      std::string_view symbol_str = boost::algorithm::trim_copy(s.substr(space_pos + 1));
      std::string_view amount_str = s.substr(0, space_pos);

      // Ensure that if decimal point is used (.), decimal fraction is specified
      auto dot_pos = amount_str.find('.');
      if (dot_pos != std::string_view::npos) {
         SYS_ASSERT((dot_pos != amount_str.size() - 1), asset_type_exception, "Missing decimal fraction after decimal point");
      }

      // The amount is [-]digits[.digits]: one leading minus is the only sign, and neither part takes anything else
      const bool negative = amount_str.starts_with(minus_sign);
      std::string_view int_digits = amount_str.substr(0, dot_pos);
      if (negative) int_digits.remove_prefix(1);
      SYS_ASSERT(!int_digits.empty() && fc::all_digits(int_digits), asset_type_exception,
                 "Asset's integer part should be digits with an optional leading minus sign");
      if (dot_pos != std::string_view::npos) {
         // an empty fraction was rejected above
         SYS_ASSERT(fc::all_digits(amount_str.substr(dot_pos + 1)), asset_type_exception,
                    "Asset's decimal fraction should be digits only");
      }

      // Parse symbol
      string precision_digit_str;
      if (dot_pos != std::string_view::npos) {
         precision_digit_str = sysio::chain::to_string(amount_str.size() - dot_pos - 1);
      } else {
         precision_digit_str = "0";
      }

      string symbol_part = precision_digit_str + ',' + std::string{symbol_str};
      symbol sym = symbol::from_string(symbol_part);

      // Parse amount
      safe<int64_t> int_part, fract_part;
      if (dot_pos != std::string_view::npos) {
         int_part = fc::to_int64(std::string{amount_str.substr(0, dot_pos)});
         fract_part = fc::to_int64(std::string{amount_str.substr(dot_pos + 1)});
         if (negative) fract_part *= -1;
      } else {
         int_part = fc::to_int64(std::string{amount_str});
      }

      safe<int64_t> amount = int_part;
      amount *= safe<int64_t>(sym.precision());
      amount += fract_part;

      return asset(amount.value, sym);
   }
   FC_CAPTURE_LOG_AND_RETHROW( "{}", from )
}

} }  // sysio::types
