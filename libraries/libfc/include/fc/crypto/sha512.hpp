#pragma once
#include <fc/fwd.hpp>
#include <fc/string.hpp>
#include <fc/crypto/packhash.hpp>
#include <fc/serialize_as_string.hpp>

namespace fc
{

class sha512 : public add_packhash_to_hash<sha512>
{
  public:
    sha512();
    explicit sha512( std::string_view hex_str );

    std::string str()const;
    std::string to_string() const { return str(); }
    /// Validating parse used by the FC_SERIALIZE_AS_STRING trait: rejects odd-length
    /// hex (the strictness the pre-trait from_variant vector<char> path enforced).
    static sha512 from_string(std::string_view s);
    operator std::string()const;

    char*       data();
    const char* data()const;
    size_t data_size()const { return 512 / 8; }

    static sha512 hash( const char* d, uint32_t dlen );
    static sha512 hash( const std::string& );

    template<typename T>
    static sha512 hash( const T& t ) 
    { 
      return packhash(t);
    } 

    class encoder 
    {
      public:
        encoder();
        ~encoder();

        void write( const char* d, uint32_t dlen );
        void put( char c ) { write( &c, 1 ); }
        void reset();
        sha512 result();

      private:
        struct      impl;
        fc::fwd<impl,216> my;
    };

    template<typename T>
    inline friend T& operator<<( T& ds, const sha512& ep ) {
      ds.write( ep.data(), sizeof(ep) );
      return ds;
    }

    template<typename T>
    inline friend T& operator>>( T& ds, sha512& ep ) {
      ds.read( ep.data(), sizeof(ep) );
      return ds;
    }
    friend sha512 operator << ( const sha512& h1, uint32_t i       );
    friend bool   operator == ( const sha512& h1, const sha512& h2 );
    friend bool   operator != ( const sha512& h1, const sha512& h2 );
    friend sha512 operator ^  ( const sha512& h1, const sha512& h2 );
    friend bool   operator >= ( const sha512& h1, const sha512& h2 );
    friend bool   operator >  ( const sha512& h1, const sha512& h2 ); 
    friend bool   operator <  ( const sha512& h1, const sha512& h2 ); 
                             
    uint64_t _hash[8]; 
};

  typedef fc::sha512 uint512;

} // fc

#include <fc/reflect/reflect.hpp>
FC_REFLECT_TYPENAME( fc::sha512 )
FC_SERIALIZE_AS_STRING(fc::sha512)
