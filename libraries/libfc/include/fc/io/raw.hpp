#pragma once
#include <fc/io/raw_variant.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/io/datastream.hpp>
#include <fc/io/varint.hpp>
#include <fc/fwd.hpp>
#include <fc/array.hpp>
#include <fc/time.hpp>
#include <fc/filesystem.hpp>
#include <fc/exception/exception.hpp>
#include <fc/safe.hpp>
#include <fc/static_variant.hpp>
#include <fc/io/raw_fwd.hpp>
#include <array>
#include <map>
#include <deque>
#include <list>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <fc/crypto/hex.hpp>

namespace fc {
    namespace raw {

    namespace bip = boost::interprocess;
    using shared_string = bip::basic_string< char, std::char_traits< char >, bip::allocator<char, bip::managed_mapped_file::segment_manager> >;

    template<size_t Size>
    using UInt = boost::multiprecision::number<
          boost::multiprecision::cpp_int_backend<Size, Size, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, void> >;
    template<size_t Size>
    using Int = boost::multiprecision::number<
          boost::multiprecision::cpp_int_backend<Size, Size, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked, void> >;
    template<typename Stream> void pack( Stream& s, const UInt<256>& n );
    template<typename Stream> void unpack( Stream& s,  UInt<256>& n );
    template<typename Stream> void pack( Stream& s, const Int<256>& n );
    template<typename Stream> void unpack( Stream& s,  Int<256>& n );
    template<typename Stream, typename T> void pack( Stream& s, const boost::multiprecision::number<T>& n );
    template<typename Stream, typename T> void unpack( Stream& s,  boost::multiprecision::number<T>& n );

    template<typename Stream, typename Arg0, typename... Args>
    inline void pack( Stream& s, const Arg0& a0, Args... args ) {
       pack( s, a0 );
       pack( s, args... );
    }

    template<typename Stream>
    inline void pack( Stream& s, const fc::exception& e )
    {
       fc::raw::pack( s, e.code() );
       fc::raw::pack( s, std::string(e.name()) );
       fc::raw::pack( s, std::string(e.what()) );
       fc::raw::pack( s, e.get_log() );
    }
    template<typename Stream>
    inline void unpack( Stream& s, fc::exception& e )
    {
       int64_t code;
       std::string name, what;
       log_messages msgs;

       fc::raw::unpack( s, code );
       fc::raw::unpack( s, name );
       fc::raw::unpack( s, what );
       fc::raw::unpack( s, msgs );

       e = fc::exception( fc::move(msgs), code, name, what );
    }

    template<typename Stream>
    inline void pack( Stream& s, const fc::log_message& msg )
    {
       fc::raw::pack( s, variant(msg) );
    }
    template<typename Stream>
    inline void unpack( Stream& s, fc::log_message& msg )
    {
       fc::variant vmsg;
       fc::raw::unpack( s, vmsg );
       msg = vmsg.as<log_message>();
    }

    template<typename Stream>
    inline void pack( Stream& s, const std::filesystem::path& tp )
    {
       fc::raw::pack( s, tp.generic_string() );
    }

    template<typename Stream>
    inline void unpack( Stream& s, std::filesystem::path& tp )
    {
       std::string p;
       fc::raw::unpack( s, p );
       tp = p;
    }

    template<typename Stream>
    inline void pack( Stream& s, const fc::time_point_sec& tp )
    {
       uint32_t usec = tp.sec_since_epoch();
       s.write( (const char*)&usec, sizeof(usec) );
    }

    template<typename Stream>
    inline void unpack( Stream& s, fc::time_point_sec& tp )
    { try {
       uint32_t sec;
       s.read( (char*)&sec, sizeof(sec) );
       tp = fc::time_point_sec{sec};
    } FC_RETHROW_EXCEPTIONS( warn, "" ) }

    template<typename Stream>
    inline void pack( Stream& s, const fc::time_point& tp )
    {
       uint64_t usec = tp.time_since_epoch().count();
       s.write( (const char*)&usec, sizeof(usec) );
    }

    template<typename Stream>
    inline void unpack( Stream& s, fc::time_point& tp )
    { try {
       uint64_t usec;
       s.read( (char*)&usec, sizeof(usec) );
       tp = fc::time_point() + fc::microseconds(usec);
    } FC_RETHROW_EXCEPTIONS( warn, "" ) }

    template<typename Stream>
    inline void pack( Stream& s, const fc::microseconds& usec )
    {
       uint64_t usec_as_int64 = usec.count();
       s.write( (const char*)&usec_as_int64, sizeof(usec_as_int64) );
    }

    template<typename Stream>
    inline void unpack( Stream& s, fc::microseconds& usec )
    { try {
       uint64_t usec_as_int64;
       s.read( (char*)&usec_as_int64, sizeof(usec_as_int64) );
       usec = fc::microseconds(usec_as_int64);
    } FC_RETHROW_EXCEPTIONS( warn, "" ) }

    template<typename Stream, typename T, size_t N>
    inline auto pack( Stream& s, const fc::array<T,N>& v) -> std::enable_if_t<!is_trivial_array<T>>
    {
       static_assert( N <= MAX_NUM_ARRAY_ELEMENTS, "number of elements in array is too large" );
       for (uint64_t i = 0; i < N; ++i)
         fc::raw::pack(s, v.data[i]);
    }

    template<typename Stream, typename T, size_t N>
    inline auto pack( Stream& s, const fc::array<T,N>& v) -> std::enable_if_t<is_trivial_array<T>>
    {
       static_assert( N <= MAX_NUM_ARRAY_ELEMENTS, "number of elements in array is too large" );
       s.write((const char*)&v.data[0], N*sizeof(T));
    }

    template<typename Stream, typename T, size_t N>
    inline auto unpack( Stream& s, fc::array<T,N>& v) -> std::enable_if_t<!is_trivial_array<T>>
    { try {
       static_assert( N <= MAX_NUM_ARRAY_ELEMENTS, "number of elements in array is too large" );
       for (uint64_t i = 0; i < N; ++i)
          fc::raw::unpack(s, v.data[i]);
    } FC_RETHROW_EXCEPTIONS( warn, "fc::array<${type},${length}>", ("type",fc::get_typename<T>::name())("length",N) ) }

    template<typename Stream, typename T, size_t N>
    inline auto unpack( Stream& s, fc::array<T,N>& v) -> std::enable_if_t<is_trivial_array<T>>
    { try {
       static_assert( N <= MAX_NUM_ARRAY_ELEMENTS, "number of elements in array is too large" );
       s.read((char*)&v.data[0], N*sizeof(T));
    } FC_RETHROW_EXCEPTIONS( warn, "fc::array<${type},${length}>", ("type",fc::get_typename<T>::name())("length",N) ) }

    template<typename Stream, typename T, size_t N>
    inline void pack( Stream& s, T (&v)[N]) {
      fc::raw::pack( s, unsigned_int((uint32_t)N) );
      for (uint64_t i = 0; i < N; ++i)
         fc::raw::pack(s, v[i]);
    }

    template<typename Stream, typename T, size_t N>
    inline void unpack( Stream& s, T (&v)[N])
    { try {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value == N );
      for (uint64_t i = 0; i < N; ++i)
         fc::raw::unpack(s, v[i]);
    } FC_RETHROW_EXCEPTIONS( warn, "${type} (&v)[${length}]", ("type",fc::get_typename<T>::name())("length",N) ) }

    template<typename Stream, typename T>
    inline void pack( Stream& s, const std::shared_ptr<T>& v)
    {
      fc::raw::pack( s, bool(!!v) );
      if( !!v ) fc::raw::pack( s, *v );
    }

    template<typename Stream, typename T>
    inline void unpack( Stream& s, std::shared_ptr<T>& v)
    { try {
      bool b; fc::raw::unpack( s, b );
      if( b ) { v = std::make_shared<T>(); fc::raw::unpack( s, *v ); }
    } FC_RETHROW_EXCEPTIONS( warn, "std::shared_ptr<T>", ("type",fc::get_typename<T>::name()) ) }

    template<typename Stream> inline void pack( Stream& s, const signed_int& v ) {
      uint32_t val = (v.value<<1) ^ (v.value>>31);              //apply zigzag encoding
      do {
        uint8_t b = uint8_t(val) & 0x7f;
        val >>= 7;
        b |= ((val > 0) << 7);
        s.write((char*)&b,1);//.put(b);
      } while( val );
    }

    template<typename Stream> inline void pack( Stream& s, const unsigned_int& v ) {
      uint64_t val = v.value;
      do {
        uint8_t b = uint8_t(val) & 0x7f;
        val >>= 7;
        b |= ((val > 0) << 7);
        s.write((char*)&b,1);//.put(b);
      }while( val );
    }

    template<typename Stream> inline void unpack( Stream& s, signed_int& vi ) {
      uint32_t v = 0; char b = 0; int by = 0;
      do {
        s.get(b);
        v |= uint32_t(uint8_t(b) & 0x7f) << by;
        by += 7;
      } while( uint8_t(b) & 0x80 );
      vi.value= (v>>1) ^ (~(v&1)+1ull);                         //reverse zigzag encoding
    }

    template<typename Stream> inline void unpack( Stream& s, unsigned_int& vi ) {
      uint64_t v = 0; char b = 0; uint8_t by = 0;
      do {
          s.get(b);
          v |= uint32_t(uint8_t(b) & 0x7f) << by;
          by += 7;
      } while( uint8_t(b) & 0x80 && by < 32 );
      vi.value = static_cast<uint32_t>(v);
    }

    template<typename Stream, typename T> inline void unpack( Stream& s, const T& vi )
    {
       T tmp;
       fc::raw::unpack( s, tmp );
       FC_ASSERT( vi == tmp );
    }

    template<typename Stream> inline void pack( Stream& s, const char* v ) { fc::raw::pack( s, std::string(v) ); }

    template<typename Stream, typename T>
    void pack( Stream& s, const safe<T>& v ) { fc::raw::pack( s, v.value ); }

    template<typename Stream, typename T>
    void unpack( Stream& s, fc::safe<T>& v ) { fc::raw::unpack( s, v.value ); }

    template<typename Stream, typename T, unsigned int S, typename Align>
    void pack( Stream& s, const fc::fwd<T,S,Align>& v ) {
       fc::raw::pack( *v );
    }

    template<typename Stream, typename T, unsigned int S, typename Align>
    void unpack( Stream& s, fc::fwd<T,S,Align>& v ) {
       fc::raw::unpack( *v );
    }

    // optional
    template<typename Stream, typename T>
    void pack( Stream& s, const std::optional<T>& v ) {
      fc::raw::pack( s, v.has_value() );
      if( v ) fc::raw::pack( s, *v );
    }

    template<typename Stream, typename T>
    void unpack( Stream& s, std::optional<T>& v )
    { try {
      bool b; fc::raw::unpack( s, b );
      if( b ) { v = T(); fc::raw::unpack( s, *v ); }
    } FC_RETHROW_EXCEPTIONS( warn, "optional<${type}>", ("type",fc::get_typename<T>::name() ) ) }

    // std::vector<char>
    template<typename Stream> inline void pack( Stream& s, const std::vector<char>& value ) {
      FC_ASSERT( value.size() <= MAX_SIZE_OF_BYTE_ARRAYS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      if( value.size() )
        s.write( &value.front(), (uint32_t)value.size() );
    }
    template<typename Stream> inline void unpack( Stream& s, std::vector<char>& value ) {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_SIZE_OF_BYTE_ARRAYS );
      value.resize(size.value);
      if( value.size() )
        s.read( value.data(), value.size() );
    }

    // fc::string
    template<typename Stream> inline void pack( Stream& s, const std::string& v )  {
      FC_ASSERT( v.size() <= MAX_SIZE_OF_BYTE_ARRAYS );
      fc::raw::pack( s, unsigned_int((uint32_t)v.size()));
      if( v.size() ) s.write( v.c_str(), v.size() );
    }

    template<typename Stream> inline void unpack( Stream& s, std::string& v )  {
      std::vector<char> tmp; fc::raw::unpack(s,tmp);
      if( tmp.size() )
         v = std::string(tmp.data(),tmp.data()+tmp.size());
      else v = std::string();
    }

    // bip::basic_string
    template<typename Stream> inline void pack( Stream& s, const shared_string& v )  {
      FC_ASSERT( v.size() <= MAX_SIZE_OF_BYTE_ARRAYS );
      fc::raw::pack( s, unsigned_int((uint32_t)v.size()));
      if( v.size() ) s.write( v.c_str(), v.size() );
    }

    template<typename Stream> inline void unpack( Stream& s, shared_string& v )  {
      std::vector<char> tmp; fc::raw::unpack(s,tmp);
      FC_ASSERT(v.size() == 0);
      if( tmp.size() ) {
         v.append(tmp.begin(), tmp.end());
      }
    }

    // bool
    template<typename Stream> inline void pack( Stream& s, const bool& v ) { fc::raw::pack( s, uint8_t(v) );             }
    template<typename Stream> inline void unpack( Stream& s, bool& v )
    {
       uint8_t b;
       fc::raw::unpack( s, b );
       FC_ASSERT( (b & ~1) == 0 );
       v=(b!=0);
    }

    namespace detail {

      template<typename Stream, typename Class>
      struct pack_object_visitor {
        pack_object_visitor(const Class& _c, Stream& _s)
        :c(_c),s(_s){}

        template<typename T, typename C, T(C::*p)>
        void operator()( const char* name )const {
          fc::raw::pack( s, c.*p );
        }
        private:
          const Class& c;
          Stream&      s;
      };

      template<typename Stream, typename Class>
      struct unpack_object_visitor : public fc::reflector_init_visitor<Class> {
        unpack_object_visitor(Class& _c, Stream& _s)
        : fc::reflector_init_visitor<Class>(_c), s(_s){}

        template<typename T, typename C, T(C::*p)>
        inline void operator()( const char* name )const
        { try {
          fc::raw::unpack( s, this->obj.*p );
        } FC_RETHROW_EXCEPTIONS( warn, "Error unpacking field ${field}", ("field",name) ) }

        private:
          Stream& s;
      };

      template<typename IsClass=fc::true_type>
      struct if_class{
        template<typename Stream, typename T>
        static inline void pack( Stream& s, const T& v ) { s << v; }
        template<typename Stream, typename T>
        static inline void unpack( Stream& s, T& v ) { s >> v; }
      };

      template<>
      struct if_class<fc::false_type> {
        template<typename Stream, typename T>
        static inline void pack( Stream& s, const T& v ) {
          s.write( (char*)&v, sizeof(v) );
        }
        template<typename Stream, typename T>
        static inline void unpack( Stream& s, T& v ) {
          s.read( (char*)&v, sizeof(v) );
        }
      };

      template<typename IsEnum=fc::false_type>
      struct if_enum {
        template<typename Stream, typename T>
        static inline void pack( Stream& s, const T& v ) {
          fc::reflector<T>::visit( pack_object_visitor<Stream,T>( v, s ) );
        }
        template<typename Stream, typename T>
        static inline void unpack( Stream& s, T& v ) {
          fc::reflector<T>::visit( unpack_object_visitor<Stream,T>( v, s ) );
        }
      };
      template<>
      struct if_enum<fc::true_type> {
        template<typename Stream, typename T>
        static inline void pack( Stream& s, const T& v ) {
          fc::raw::pack(s, (int64_t)v);
        }
        template<typename Stream, typename T>
        static inline void unpack( Stream& s, T& v ) {
          int64_t temp;
          fc::raw::unpack(s, temp);
          v = (T)temp;
        }
      };

      template<typename IsReflected=fc::false_type>
      struct if_reflected {
        template<typename Stream, typename T>
        static inline void pack( Stream& s, const T& v ) {
          if_class<typename fc::is_class<T>::type>::pack(s,v);
        }
        template<typename Stream, typename T>
        static inline void unpack( Stream& s, T& v ) {
          if_class<typename fc::is_class<T>::type>::unpack(s,v);
        }
      };
      template<>
      struct if_reflected<fc::true_type> {
        template<typename Stream, typename T>
        static inline void pack( Stream& s, const T& v ) {
          if_enum< typename fc::reflector<T>::is_enum >::pack(s,v);
        }
        template<typename Stream, typename T>
        static inline void unpack( Stream& s, T& v ) {
          if_enum< typename fc::reflector<T>::is_enum >::unpack(s,v);
        }
      };

    } // namesapce detail

    // allow users to verify version of fc calls reflector_init on unpacked reflected types
    constexpr bool has_feature_reflector_init_on_unpacked_reflected_types = true;

    template<typename Stream, typename T>
    inline void pack( Stream& s, const std::unordered_set<T>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      auto itr = value.begin();
      auto end = value.end();
      while( itr != end ) {
        fc::raw::pack( s, *itr );
        ++itr;
      }
    }
    template<typename Stream, typename T>
    inline void unpack( Stream& s, std::unordered_set<T>& value ) {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      value.clear();
      value.reserve(size.value);
      for( uint32_t i = 0; i < size.value; ++i )
      {
          T tmp;
          fc::raw::unpack( s, tmp );
          value.insert( std::move(tmp) );
      }
    }


    template<typename Stream, typename K, typename V>
    inline void pack( Stream& s, const std::pair<K,V>& value ) {
       fc::raw::pack( s, value.first );
       fc::raw::pack( s, value.second );
    }
    template<typename Stream, typename K, typename V>
    inline void unpack( Stream& s, std::pair<K,V>& value )
    {
       fc::raw::unpack( s, value.first );
       fc::raw::unpack( s, value.second );
    }

   template<typename Stream, typename K, typename V>
    inline void pack( Stream& s, const std::unordered_map<K,V>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      auto itr = value.begin();
      auto end = value.end();
      while( itr != end ) {
        fc::raw::pack( s, *itr );
        ++itr;
      }
    }
    template<typename Stream, typename K, typename V>
    inline void unpack( Stream& s, std::unordered_map<K,V>& value )
    {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      value.clear();
      value.reserve(size.value);
      for( uint32_t i = 0; i < size.value; ++i )
      {
          std::pair<K,V> tmp;
          fc::raw::unpack( s, tmp );
          value.insert( std::move(tmp) );
      }
    }
    template<typename Stream, typename K, typename V>
    inline void pack( Stream& s, const std::map<K,V>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      auto itr = value.begin();
      auto end = value.end();
      while( itr != end ) {
        fc::raw::pack( s, *itr );
        ++itr;
      }
    }
    template<typename Stream, typename K, typename V>
    inline void unpack( Stream& s, std::map<K,V>& value )
    {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      value.clear();
      for( uint32_t i = 0; i < size.value; ++i )
      {
          std::pair<K,V> tmp;
          fc::raw::unpack( s, tmp );
          value.insert( std::move(tmp) );
      }
    }

    template<typename Stream, typename T>
    inline void pack( Stream& s, const std::deque<T>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      for( const auto& i : value ) {
         fc::raw::pack( s, i );
      }
    }

    template<typename Stream, typename T>
    inline void unpack( Stream& s, std::deque<T>& value ) {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      value.resize(size.value);
      for( auto& i : value ) {
         fc::raw::unpack( s, i );
      }
    }

    template<typename Stream, typename T, typename... U>
    inline void pack( Stream& s, const boost::container::deque<T, U...>& value ) {
       FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
       fc::raw::pack( s, unsigned_int( (uint32_t) value.size() ) );
       for( const auto& i : value ) {
          fc::raw::pack( s, i );
       }
    }

    template<typename Stream, typename T, typename... U>
    inline void unpack( Stream& s, boost::container::deque<T, U...>& value ) {
       unsigned_int size;
       fc::raw::unpack( s, size );
       FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
       value.resize( size.value );
       for( auto& i : value ) {
          fc::raw::unpack( s, i );
       }
    }

    template<typename Stream, typename T>
    inline void pack( Stream& s, const std::vector<T>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      for( const auto& i : value ) {
         fc::raw::pack( s, i );
      }
    }

    template<typename Stream, typename T>
    inline void unpack( Stream& s, std::vector<T>& value ) {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      value.resize(size.value);
      for( auto& i : value ) {
         fc::raw::unpack( s, i );
      }
    }

    template<typename Stream, typename T>
    inline void pack( Stream& s, const std::list<T>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      for( const auto& i : value ) {
         fc::raw::pack( s, i );
      }
    }

    template<typename Stream, typename T>
    inline void unpack( Stream& s, std::list<T>& value ) {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      while( size.value-- ) {
         T i;
         fc::raw::unpack( s, i );
         value.emplace_back( std::move( i ) );
      }
    }

    template<typename Stream, typename T>
    inline void pack( Stream& s, const std::set<T>& value ) {
      FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
      fc::raw::pack( s, unsigned_int((uint32_t)value.size()) );
      auto itr = value.begin();
      auto end = value.end();
      while( itr != end ) {
        fc::raw::pack( s, *itr );
        ++itr;
      }
    }

    template<typename Stream, typename T>
    inline void unpack( Stream& s, std::set<T>& value ) {
      unsigned_int size; fc::raw::unpack( s, size );
      FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
      for( uint64_t i = 0; i < size.value; ++i )
      {
        T tmp;
        fc::raw::unpack( s, tmp );
        value.insert( std::move(tmp) );
      }
    }

    template<typename Stream, typename T, std::size_t S>
    inline auto pack( Stream& s, const std::array<T, S>& value ) -> std::enable_if_t<is_trivial_array<T>>
    {
       s.write((const char*)value.data(), S * sizeof(T));
    }

    template<typename Stream, typename T, std::size_t S>
    inline auto pack( Stream& s, const std::array<T, S>& value ) -> std::enable_if_t<!is_trivial_array<T>>
    {
       for( std::size_t i = 0; i < S; ++i ) {
          fc::raw::pack( s, value[i] );
       }
    }

    template<typename Stream, typename T, std::size_t S>
    inline auto unpack( Stream& s, std::array<T, S>& value )  -> std::enable_if_t<is_trivial_array<T>>
    {
       s.read((char*)value.data(), S * sizeof(T));
    }

    template<typename Stream, typename T, std::size_t S>
    inline auto unpack( Stream& s, std::array<T, S>& value )  -> std::enable_if_t<!is_trivial_array<T>>
    {
       for( std::size_t i = 0; i < S; ++i ) {
          fc::raw::unpack( s, value[i] );
       }
    }

    template<typename Stream, typename T>
    inline void pack( Stream& s, const T& v ) {
      fc::raw::detail::if_reflected< typename fc::reflector<T>::is_defined >::pack(s,v);
    }
    template<typename Stream, typename T>
    inline void unpack( Stream& s, T& v )
    { try {
      fc::raw::detail::if_reflected< typename fc::reflector<T>::is_defined >::unpack(s,v);
    } FC_RETHROW_EXCEPTIONS( warn, "error unpacking ${type}", ("type",fc::get_typename<T>::name() ) ) }

    template<typename T>
    inline size_t pack_size(  const T& v )
    {
      datastream<size_t> ps;
      fc::raw::pack(ps,v );
      return ps.tellp();
    }

    template<typename T>
    inline std::vector<char> pack(  const T& v ) {
      datastream<size_t> ps;
      fc::raw::pack(ps,v );
      std::vector<char> vec(ps.tellp());

      if( vec.size() ) {
        datastream<char*>  ds( vec.data(), size_t(vec.size()) );
        fc::raw::pack(ds,v);
      }
      return vec;
    }

    template<typename T, typename... Next>
    inline std::vector<char> pack(  const T& v, Next... next ) {
      datastream<size_t> ps;
      fc::raw::pack(ps,v,next...);
      std::vector<char> vec(ps.tellp());

      if( vec.size() ) {
        datastream<char*>  ds( vec.data(), size_t(vec.size()) );
        fc::raw::pack(ds,v,next...);
      }
      return vec;
    }


    template<typename T>
    inline T unpack( const std::vector<char>& s )
    { try  {
      T tmp;
      datastream<const char*>  ds( s.data(), size_t(s.size()) );
      fc::raw::unpack(ds,tmp);
      return tmp;
    } FC_RETHROW_EXCEPTIONS( warn, "error unpacking ${type}", ("type",fc::get_typename<T>::name() ) ) }

    template<typename T>
    inline void unpack( const std::vector<char>& s, T& tmp )
    { try  {
      datastream<const char*>  ds( s.data(), size_t(s.size()) );
      fc::raw::unpack(ds,tmp);
    } FC_RETHROW_EXCEPTIONS( warn, "error unpacking ${type}", ("type",fc::get_typename<T>::name() ) ) }

    template<typename T>
    inline void pack( char* d, uint32_t s, const T& v ) {
      datastream<char*> ds(d,s);
      fc::raw::pack(ds,v );
    }

    template<typename T>
    inline T unpack( const char* d, uint32_t s )
    { try {
      T v;
      datastream<const char*>  ds( d, s );
      fc::raw::unpack(ds,v);
      return v;
    } FC_RETHROW_EXCEPTIONS( warn, "error unpacking ${type}", ("type",fc::get_typename<T>::name() ) ) }

    template<typename T>
    inline void unpack( const char* d, uint32_t s, T& v )
    { try {
      datastream<const char*>  ds( d, s );
      fc::raw::unpack(ds,v);
    } FC_RETHROW_EXCEPTIONS( warn, "error unpacking ${type}", ("type",fc::get_typename<T>::name() ) ) }

   template<typename Stream>
   struct pack_static_variant
   {
      Stream& stream;
      pack_static_variant( Stream& s ):stream(s){}

      typedef void result_type;
      template<typename T> void operator()( const T& v )const
      {
         fc::raw::pack( stream, v );
      }
   };

   template<typename Stream>
   struct unpack_static_variant
   {
      Stream& stream;
      unpack_static_variant( Stream& s ):stream(s){}

      typedef void result_type;
      template<typename T> void operator()( T& v )const
      {
         fc::raw::unpack( stream, v );
      }
   };


    template<typename Stream, typename... T>
    void pack( Stream& s, const std::variant<T...>& sv )
    {
       fc::raw::pack( s, unsigned_int(sv.index()) );
       std::visit( pack_static_variant<Stream>(s), sv );
    }

    template<typename Stream, typename... T> void unpack( Stream& s, std::variant<T...>& sv )
    {
       unsigned_int w;
       fc::raw::unpack( s, w );
       fc::from_index(sv, w.value);
       std::visit( unpack_static_variant<Stream>(s), sv );
    }



    template<typename Stream, typename T> void pack( Stream& s, const boost::multiprecision::number<T>& n ) {
      static_assert( sizeof( n ) == (std::numeric_limits<boost::multiprecision::number<T>>::digits+1)/8, "unexpected padding" );
      s.write( (const char*)&n, sizeof(n) );
    }
    template<typename Stream, typename T> void unpack( Stream& s,  boost::multiprecision::number<T>& n ) {
      static_assert( sizeof( n ) == (std::numeric_limits<boost::multiprecision::number<T>>::digits+1)/8, "unexpected padding" );
      s.read( (char*)&n, sizeof(n) );
    }

    template<typename Stream> void pack( Stream& s, const UInt<256>& n ) {
       pack( s, static_cast<UInt<128>>(n) );
       pack( s, static_cast<UInt<128>>(n >> 128) );
    }
    template<typename Stream> void unpack( Stream& s,  UInt<256>& n ) {
       UInt<128> tmp[2];
       unpack( s, tmp[0] );
       unpack( s, tmp[1] );
       n = tmp[1];
       n <<= 128;
       n |= tmp[0];
    }

} } // namespace fc::raw