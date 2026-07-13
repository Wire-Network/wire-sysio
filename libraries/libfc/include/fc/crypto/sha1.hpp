#pragma once
#include <fc/fwd.hpp>
#include <fc/string.hpp>
#include <fc/crypto/packhash.hpp>
#include <fc/serialize_as_string.hpp>

namespace fc{

class sha1 : public add_packhash_to_hash<sha1>
{
  public:
    sha1();
    explicit sha1( std::string_view hex_str );

    std::string str()const;
    std::string to_string() const { return str(); }
    /// Validating parse used by the FC_SERIALIZE_AS_STRING trait: rejects odd-length
    /// hex (the strictness the pre-trait from_variant vector<char> path enforced).
    static sha1 from_string(std::string_view s);
    operator std::string()const;

    char*       data();
    const char* data()const;
    size_t data_size()const { return 20; }

    static sha1 hash( const char* d, uint32_t dlen );
    static sha1 hash( const std::string& );

    template<typename T>
    static sha1 hash( const T& t ) 
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
        sha1 result();

      private:
        struct      impl;
        fc::fwd<impl,96> my;
    };

    template<typename T>
    inline friend T& operator<<( T& ds, const sha1& ep ) {
      ds.write( ep.data(), sizeof(ep) );
      return ds;
    }

    template<typename T>
    inline friend T& operator>>( T& ds, sha1& ep ) {
      ds.read( ep.data(), sizeof(ep) );
      return ds;
    }
    friend sha1 operator << ( const sha1& h1, uint32_t i       );
    friend bool   operator == ( const sha1& h1, const sha1& h2 );
    friend bool   operator != ( const sha1& h1, const sha1& h2 );
    friend sha1 operator ^  ( const sha1& h1, const sha1& h2 );
    friend bool   operator >= ( const sha1& h1, const sha1& h2 );
    friend bool   operator >  ( const sha1& h1, const sha1& h2 ); 
    friend bool   operator <  ( const sha1& h1, const sha1& h2 ); 
                             
    uint32_t _hash[5]; 
};

} // namespace fc

namespace std
{
    template<>
    struct hash<fc::sha1>
    {
       size_t operator()( const fc::sha1& s )const
       {
           return  *((size_t*)&s);
       }
    };
}

FC_SERIALIZE_AS_STRING(fc::sha1)
