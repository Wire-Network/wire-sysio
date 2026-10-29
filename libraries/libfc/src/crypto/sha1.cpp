#include <fc/crypto/hex.hpp>
#include <fc/fwd_impl.hpp>
#include <openssl/sha.h>
#include <string.h>
#include <fc/crypto/sha1.hpp>
#include <fc/variant.hpp>
#include <vector>
#include "_digest_common.hpp"
#include <fc/exception/exception.hpp>

namespace fc
{

sha1::sha1() { memset( _hash, 0, sizeof(_hash) ); }
sha1::sha1( std::string_view hex_str ) {
   auto bytes_written = fc::from_hex( hex_str, (char*)_hash, sizeof(_hash) );
   if( bytes_written < sizeof(_hash) )
      memset( (char*)_hash + bytes_written, 0, (sizeof(_hash) - bytes_written) ); 
}

std::string sha1::str()const {
  return fc::to_hex( (char*)_hash, sizeof(_hash) );
}
sha1::operator std::string()const { return  str(); }

char* sha1::data() { return (char*)&_hash[0]; }
const char* sha1::data()const { return (char*)&_hash[0]; }


struct sha1::encoder::impl {
   SHA_CTX ctx;
};

sha1::encoder::~encoder() {}
sha1::encoder::encoder() {
  reset();
}

sha1 sha1::hash( const char* d, uint32_t dlen ) {
  encoder e;
  e.write(d,dlen);
  return e.result();
}
sha1 sha1::hash( const std::string& s ) {
  return hash( s.c_str(), s.size() );
}

void sha1::encoder::write( const char* d, uint32_t dlen ) {
  SHA1_Update( &my->ctx, d, dlen);
}
sha1 sha1::encoder::result() {
  sha1 h;
  SHA1_Final((uint8_t*)h.data(), &my->ctx );
  return h;
}
void sha1::encoder::reset() {
  SHA1_Init( &my->ctx);
}

sha1 operator << ( const sha1& h1, uint32_t i ) {
  sha1 result;
  fc::detail::shift_l( h1.data(), result.data(), result.data_size(), i );
  return result;
}
sha1 operator ^ ( const sha1& h1, const sha1& h2 ) {
  sha1 result;
  result._hash[0] = h1._hash[0] ^ h2._hash[0];
  result._hash[1] = h1._hash[1] ^ h2._hash[1];
  result._hash[2] = h1._hash[2] ^ h2._hash[2];
  result._hash[3] = h1._hash[3] ^ h2._hash[3];
  result._hash[4] = h1._hash[4] ^ h2._hash[4];
  return result;
}
bool operator >= ( const sha1& h1, const sha1& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) >= 0;
}
bool operator > ( const sha1& h1, const sha1& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) > 0;
}
bool operator < ( const sha1& h1, const sha1& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) < 0;
}
bool operator != ( const sha1& h1, const sha1& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) != 0;
}
bool operator == ( const sha1& h1, const sha1& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) == 0;
}

  sha1 sha1::from_string(std::string_view s) {
    // The pre-trait from_variant path (vector<char>) rejected odd-length hex;
    // keep that strictness so a trailing lone nibble is malformed input, not
    // silently zero-extended.
    FC_ASSERT(s.size() % 2 == 0, "sha1 hex string length must be even, got {}", s.size());
    return sha1(s);
  }

} // fc
