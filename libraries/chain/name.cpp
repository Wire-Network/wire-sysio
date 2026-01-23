#include <sysio/chain/name.hpp>
#include <fc/variant.hpp>
#include <boost/algorithm/string.hpp>
#include <fc/exception/exception.hpp>
#include <sysio/chain/exceptions.hpp>

namespace sysio::chain {

   void name::set( std::string_view str ) {
      const auto len = str.size();
      SYS_ASSERT(len <= 13, name_type_exception, "Name is longer than 13 characters ({}) ", std::string(str));
      value = string_to_uint64_t(str);
      SYS_ASSERT(to_string() == str, name_type_exception,
                 "Name not properly normalized (name: {}, normalized: {}) ",
                 std::string(str), to_string());
   }

   // keep in sync with name::to_string() in contract definition for name
   std::string name::to_string()const {
     static const char* charmap = ".12345abcdefghijklmnopqrstuvwxyz";

      std::string str(13,'.');

      uint64_t tmp = value;
      for( uint32_t i = 0; i <= 12; ++i ) {
         char c = charmap[tmp & (i == 0 ? 0x0f : 0x1f)];
         str[12-i] = c;
         tmp >>= (i == 0 ? 4 : 5);
      }

      boost::algorithm::trim_right_if( str, []( char c ){ return c == '.'; } );
      return str;
   }

} // sysio::chain

namespace fc {
  void to_variant(const sysio::chain::name& c, fc::variant& v) { v = c.to_string(); }
  void from_variant(const fc::variant& v, sysio::chain::name& check) { check.set( v.get_string() ); }
} // fc
