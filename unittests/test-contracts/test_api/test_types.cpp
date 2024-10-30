#include <sysio/sysio.hpp>

#include "test_api.hpp"

void test_types::types_size() {

   sysio_assert( sizeof(int64_t)   ==  8, "int64_t size != 8"   );
   sysio_assert( sizeof(uint64_t)  ==  8, "uint64_t size != 8"  );
   sysio_assert( sizeof(uint32_t)  ==  4, "uint32_t size != 4"  );
   sysio_assert( sizeof(int32_t)   ==  4, "int32_t size != 4"   );
   sysio_assert( sizeof(uint128_t) == 16, "uint128_t size != 16");
   sysio_assert( sizeof(int128_t)  == 16, "int128_t size != 16" );
   sysio_assert( sizeof(uint8_t)   ==  1, "uint8_t size != 1"   );

   sysio_assert( sizeof(sysio::name) ==  8, "name size !=  8");
}

void test_types::char_to_symbol() {

   sysio_assert( sysio::name::char_to_value('1') ==  1, "sysio::char_to_symbol('1') !=  1" );
   sysio_assert( sysio::name::char_to_value('2') ==  2, "sysio::char_to_symbol('2') !=  2" );
   sysio_assert( sysio::name::char_to_value('3') ==  3, "sysio::char_to_symbol('3') !=  3" );
   sysio_assert( sysio::name::char_to_value('4') ==  4, "sysio::char_to_symbol('4') !=  4" );
   sysio_assert( sysio::name::char_to_value('5') ==  5, "sysio::char_to_symbol('5') !=  5" );
   sysio_assert( sysio::name::char_to_value('a') ==  6, "sysio::char_to_symbol('a') !=  6" );
   sysio_assert( sysio::name::char_to_value('b') ==  7, "sysio::char_to_symbol('b') !=  7" );
   sysio_assert( sysio::name::char_to_value('c') ==  8, "sysio::char_to_symbol('c') !=  8" );
   sysio_assert( sysio::name::char_to_value('d') ==  9, "sysio::char_to_symbol('d') !=  9" );
   sysio_assert( sysio::name::char_to_value('e') == 10, "sysio::char_to_symbol('e') != 10" );
   sysio_assert( sysio::name::char_to_value('f') == 11, "sysio::char_to_symbol('f') != 11" );
   sysio_assert( sysio::name::char_to_value('g') == 12, "sysio::char_to_symbol('g') != 12" );
   sysio_assert( sysio::name::char_to_value('h') == 13, "sysio::char_to_symbol('h') != 13" );
   sysio_assert( sysio::name::char_to_value('i') == 14, "sysio::char_to_symbol('i') != 14" );
   sysio_assert( sysio::name::char_to_value('j') == 15, "sysio::char_to_symbol('j') != 15" );
   sysio_assert( sysio::name::char_to_value('k') == 16, "sysio::char_to_symbol('k') != 16" );
   sysio_assert( sysio::name::char_to_value('l') == 17, "sysio::char_to_symbol('l') != 17" );
   sysio_assert( sysio::name::char_to_value('m') == 18, "sysio::char_to_symbol('m') != 18" );
   sysio_assert( sysio::name::char_to_value('n') == 19, "sysio::char_to_symbol('n') != 19" );
   sysio_assert( sysio::name::char_to_value('o') == 20, "sysio::char_to_symbol('o') != 20" );
   sysio_assert( sysio::name::char_to_value('p') == 21, "sysio::char_to_symbol('p') != 21" );
   sysio_assert( sysio::name::char_to_value('q') == 22, "sysio::char_to_symbol('q') != 22" );
   sysio_assert( sysio::name::char_to_value('r') == 23, "sysio::char_to_symbol('r') != 23" );
   sysio_assert( sysio::name::char_to_value('s') == 24, "sysio::char_to_symbol('s') != 24" );
   sysio_assert( sysio::name::char_to_value('t') == 25, "sysio::char_to_symbol('t') != 25" );
   sysio_assert( sysio::name::char_to_value('u') == 26, "sysio::char_to_symbol('u') != 26" );
   sysio_assert( sysio::name::char_to_value('v') == 27, "sysio::char_to_symbol('v') != 27" );
   sysio_assert( sysio::name::char_to_value('w') == 28, "sysio::char_to_symbol('w') != 28" );
   sysio_assert( sysio::name::char_to_value('x') == 29, "sysio::char_to_symbol('x') != 29" );
   sysio_assert( sysio::name::char_to_value('y') == 30, "sysio::char_to_symbol('y') != 30" );
   sysio_assert( sysio::name::char_to_value('z') == 31, "sysio::char_to_symbol('z') != 31" );

   for(unsigned char i = 0; i<255; i++) {
      if( (i >= 'a' && i <= 'z') || (i >= '1' || i <= '5') ) continue;
      sysio_assert( sysio::name::char_to_value((char)i) == 0, "sysio::char_to_symbol() != 0" );
   }
}

void test_types::string_to_name() {
   return;
   sysio_assert( sysio::name("a") == "a"_n, "sysio::string_to_name(a)" );
   sysio_assert( sysio::name("ba") == "ba"_n, "sysio::string_to_name(ba)" );
   sysio_assert( sysio::name("cba") == "cba"_n, "sysio::string_to_name(cba)" );
   sysio_assert( sysio::name("dcba") == "dcba"_n, "sysio::string_to_name(dcba)" );
   sysio_assert( sysio::name("edcba") == "edcba"_n, "sysio::string_to_name(edcba)" );
   sysio_assert( sysio::name("fedcba") == "fedcba"_n, "sysio::string_to_name(fedcba)" );
   sysio_assert( sysio::name("gfedcba") == "gfedcba"_n, "sysio::string_to_name(gfedcba)" );
   sysio_assert( sysio::name("hgfedcba") == "hgfedcba"_n, "sysio::string_to_name(hgfedcba)" );
   sysio_assert( sysio::name("ihgfedcba") == "ihgfedcba"_n, "sysio::string_to_name(ihgfedcba)" );
   sysio_assert( sysio::name("jihgfedcba") == "jihgfedcba"_n, "sysio::string_to_name(jihgfedcba)" );
   sysio_assert( sysio::name("kjihgfedcba") == "kjihgfedcba"_n, "sysio::string_to_name(kjihgfedcba)" );
   sysio_assert( sysio::name("lkjihgfedcba") == "lkjihgfedcba"_n, "sysio::string_to_name(lkjihgfedcba)" );
   sysio_assert( sysio::name("mlkjihgfedcba") == "mlkjihgfedcba"_n, "sysio::string_to_name(mlkjihgfedcba)" );
}
