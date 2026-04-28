#pragma once

#include <charconv>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <boost/multi_index_container_fwd.hpp>

#include <fmt/format.h>

#include <fc/container/deque_fwd.hpp>
#include <fc/container/flat_fwd.hpp>
#include <fc/int128.hpp>
#include <fc/int256_fwd.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/string.hpp>
#include <fc/time.hpp>

namespace fc
{
   /**
    * @defgroup serializable Serializable _types
    * @brief Classes that may be converted to/from an variant
    *
    * To make a class 'serializable' the following methods must be available
    * for your Serializable_type
    *
    *  @code
    *     void   to_variant( const Serializable_type& e, variant& v );
    *     void   from_variant( const variant& e, Serializable_type& ll );
    *  @endcode
    */

   class variant;
   class variant_object;
   class mutable_variant_object;
   class time_point;
   class time_point_sec;
   class microseconds;
   template<typename T> struct safe;

   // Streaming JSON writer overloads for variant / variant_object; implemented in
   // variant.cpp / variant_object.cpp.  Provides a seamless path for reflected structs
   // that embed a fc::variant or variant_object field.  Performance is bounded by the
   // underlying fc::json::to_string since variants can hold arbitrary shapes; callers
   // that want the full allocation-free path should flatten variant usage out of their
   // hot endpoints.
   void to_json_stream( const variant& v, json_writer& w );
   void to_json_stream( const variant_object& vo, json_writer& w );
   void to_json_stream( const mutable_variant_object& vo, json_writer& w );

   struct blob { std::vector<char> data; };

   void to_variant( const blob& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  blob& vo );


   template<typename T, typename... Args> void to_variant( const boost::multi_index_container<T,Args...>& s, fc::variant& v );
   template<typename T, typename... Args> void from_variant( const fc::variant& v, boost::multi_index_container<T,Args...>& s );

   template<typename T> void to_variant( const safe<T>& s, fc::variant& v );
   template<typename T> void from_variant( const fc::variant& v, safe<T>& s );
   template<typename T> void to_variant( const std::unique_ptr<T>& s, fc::variant& v );
   template<typename T> void from_variant( const fc::variant& v, std::unique_ptr<T>& s );

   template<typename... T> void to_variant( const std::variant<T...>& s, fc::variant& v );
   template<typename... T> void from_variant( const fc::variant& v, std::variant<T...>& s );

   void to_variant( const uint8_t& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  uint8_t& vo );
   void to_variant( const int8_t& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  int8_t& vo );

   void to_variant( const uint16_t& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  uint16_t& vo );
   void to_variant( const int16_t& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  int16_t& vo );

   void to_variant( const uint32_t& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  uint32_t& vo );
   void to_variant( const int32_t& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  int32_t& vo );

   // to_variant/from_variant for __int128 types declared in fc/int128.hpp

   void to_variant( const variant_object& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  variant_object& vo );
   void to_variant( const mutable_variant_object& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  mutable_variant_object& vo );
   void to_variant( const std::vector<char>& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  std::vector<char>& vo );
   /// JSON shape: lowercase-hex string (matches to_variant which produces a string variant).
   /// Concretely overrides the generic `to_json_stream(vector<T>)` template (which emits an
   /// array of int8s).  Empty vector emits `""` to match the to_variant(empty_vec) result.
   void to_json_stream( const std::vector<char>& var,  json_writer& w );

   template<typename K, typename T>
   void to_variant( const std::unordered_map<K,T>& var,  fc::variant& vo );
   template<typename K, typename T>
   void from_variant( const fc::variant& var,  std::unordered_map<K,T>& vo );

   template<typename K, typename T>
   void to_variant( const std::map<K,T>& var,  fc::variant& vo );
   template<typename K, typename T>
   void from_variant( const fc::variant& var,  std::map<K,T>& vo );
   template<typename K, typename T>
   void to_variant( const std::multimap<K,T>& var,  fc::variant& vo );
   template<typename K, typename T>
   void from_variant( const fc::variant& var,  std::multimap<K,T>& vo );


   template<typename T>
   void to_variant( const std::unordered_set<T>& var,  fc::variant& vo );
   template<typename T>
   void from_variant( const fc::variant& var,  std::unordered_set<T>& vo );

   template<typename T>
   void to_variant( const std::deque<T>& var,  fc::variant& vo );
   template<typename T>
   void from_variant( const fc::variant& var,  std::deque<T>& vo );

   template<typename T, typename... U>
   void to_variant( const boost::container::deque<T, U...>& d, fc::variant& vo );
   template<typename T, typename... U>
   void from_variant( const fc::variant& v, boost::container::deque<T, U...>& d );

   template<typename T>
   void to_variant( const std::set<T>& var,  fc::variant& vo );
   template<typename T>
   void from_variant( const fc::variant& var,  std::set<T>& vo );

   template<typename T, std::size_t S>
   void to_variant( const std::array<T,S>& var,  fc::variant& vo );
   template<typename T, std::size_t S>
   void from_variant( const fc::variant& var,  std::array<T,S>& vo );

   template<typename T>
   void to_variant( const std::initializer_list<T>& var,  fc::variant& vo );

   void to_variant( const time_point& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  time_point& vo );
   void to_json_stream( const time_point& var, json_writer& w );

   void to_variant( const time_point_sec& var,  fc::variant& vo );
   void from_variant( const fc::variant& var,  time_point_sec& vo );
   void to_json_stream( const time_point_sec& var, json_writer& w );

   void to_variant( const microseconds& input_microseconds,  fc::variant& output_variant );
   void from_variant( const fc::variant& input_variant,  microseconds& output_microseconds );

   #ifdef __APPLE__
   void to_variant( size_t s, fc::variant& v );
   #elif !defined(_MSC_VER)
   void to_variant( long long int s, fc::variant& v );
   void to_variant( unsigned long long int s, fc::variant& v );
   #endif
   void to_variant( const std::string& s, fc::variant& v );

   template<typename T>
   void to_variant( const std::shared_ptr<T>& var,  fc::variant& vo );

   template<typename T>
   void from_variant( const fc::variant& var,  std::shared_ptr<T>& vo );

   using variants = std::vector<fc::variant>;
   template<typename A, typename B>
   void to_variant( const std::pair<A,B>& t, fc::variant& v );
   template<typename A, typename B>
   void from_variant( const fc::variant& v, std::pair<A,B>& p );



   /**
    * @brief stores null, int64, uint64, double, bool, string, std::vector<variant>,
    *        and variant_object's.
    *
    * variant's allocate everything but strings, arrays, and objects on the
    * stack and are 'move aware' for values allcoated on the heap.
    *
    * Memory usage on 64 bit systems is 16 bytes and 12 bytes on 32 bit systems.
    */
   class variant
   {
      public:
        enum type_id
        {
           null_type       = 0,
           int64_type      = 1,
           uint64_type     = 2,
           int128_type     = 3,
           uint128_type    = 4,
           int256_type     = 5,
           uint256_type    = 6,
           double_type     = 7,
           bool_type       = 8,
           string_type     = 9,  // heap-allocated std::string
           array_type      = 10,
           object_type     = 11,
           blob_type       = 12,
           string_sso_type = 13  // inline short string (<= 14 bytes); content in bytes 0..13, length in byte 14
        };
        /// Maximum string length stored inline (rest of the 16-byte buffer:
        /// 14 bytes content + 1 byte length + 1 byte type tag).
        static constexpr std::size_t sso_max_length = 14;

        /// Constructs a null_type variant
        variant();
        /// Constructs a null_type variant
        variant( nullptr_t );

        /// @param str - UTF8 string
        variant( const char* str );
        variant( char* str );
        variant( wchar_t* str );
        variant( const wchar_t* str );
        variant( float val );
        variant( uint8_t val );
        variant( int8_t val );
        variant( uint16_t val );
        variant( int16_t val );
        variant( uint32_t val );
        variant( int32_t val );
        variant( uint64_t val );
        variant( int64_t val );
        variant( fc::uint128 val );
        variant( fc::int128 val );
        variant( const fc::uint256& val );
        variant( const fc::int256& val );
        variant( double val );
        variant( bool val );
        variant( blob val );
        variant( std::string val );
        variant( std::string_view val );
        variant( variant_object );
        variant( mutable_variant_object );
        variant( variants );
        variant( const variant& );
        variant( variant&& );
       ~variant();

        /**
         *  Read-only access to the content of the variant.
         */
        class visitor
        {
           public:
              virtual ~visitor(){}
              /// handles null_type variants
              virtual void handle()const                         = 0;
              virtual void handle( const int64_t& v )const       = 0;
              virtual void handle( const uint64_t& v )const      = 0;
              virtual void handle( const fc::int128_t& v )const       = 0;
              virtual void handle( const fc::uint128_t& v )const      = 0;
              virtual void handle( const fc::int256_t& v )const       = 0;
              virtual void handle( const fc::uint256_t& v )const      = 0;
              virtual void handle( const double& v )const        = 0;
              virtual void handle( const bool& v )const          = 0;
              virtual void handle( std::string_view v )const     = 0;
              virtual void handle( const variant_object& v)const = 0;
              virtual void handle( const variants& v)const       = 0;
              virtual void handle( const blob& v)const           = 0;
        };

        void  visit( const visitor& v )const;

        type_id                     get_type()const;

        bool                        is_null()const;
        bool                        is_string()const;
        bool                        is_bool()const;
        bool                        is_int64()const;
        bool                        is_uint64()const;
        bool                        is_int128() const;
        bool                        is_uint128() const;
        bool                        is_int256()const;
        bool                        is_uint256()const;
        bool                        is_double()const;
        bool                        is_object()const;
        bool                        is_array()const;
        bool                        is_blob()const;
        /**
         *   int64, uint64, double,bool
         */
        bool                        is_numeric()const;
        /**
         *   int64, uint64, bool
         */
        bool                        is_integer()const;

        int64_t                     as_int64()const;
        uint64_t                    as_uint64()const;
        fc::int128                  as_int128()const;
        fc::uint128                 as_uint128()const;
        fc::int256                  as_int256()const;
        fc::uint256                 as_uint256()const;

        /**
         * Convert variant to an enum value. Handles both integer and string
         * representations (ABI serializer may return either depending on
         * whether enum definitions are present).
         *
         * @tparam EnumType A C++ enum type (e.g., sysio::opp::types::ChainKind)
         * @return The enum value cast from the variant's integer value
         */
        template<typename EnumType>
        EnumType as_enum_value() const {
           if (is_integer() || is_numeric())
              return static_cast<EnumType>(as_int64());
           if (is_string()) {
              // std::from_chars is non-throwing: avoids the stoll exception
              // round-trip on the invalid-text fallback path, which was the
              // dominant cost on bad input (~4 us per call) and ~25% of the
              // valid-text path too.  Behaviour matches stoll for the
              // domain we hit: leading minus accepted, leading whitespace
              // and leading '+' rejected, suffix garbage silently ignored
              // (so "1abc" still yields 1, matching the prior behaviour).
              std::string_view s = get_string();
              int64_t parsed = 0;
              auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), parsed);
              if (ec == std::errc{}) {
                 return static_cast<EnumType>(parsed);
              }
           }
           throw std::runtime_error("Cannot convert variant to enum value");
        }

        bool                        as_bool()const;
        double                      as_double()const;

        blob&                       get_blob();
        const blob&                 get_blob()const;
        blob                        as_blob()const;

        /** Convert's double, ints, bools, etc to a string
         * @throw if get_type() == array_type | get_type() == object_type
         */
        std::string                 as_string()const;

        /// @pre  get_type() == string_type
        ///
        /// Returns a non-owning view of the variant's string bytes.
        /// Was `const std::string&` historically; changed to
        /// `std::string_view` so a future inline-string (SSO) encoding
        /// can return a view of the inline bytes without materialising
        /// a heap std::string.  Callers that need an owning copy should
        /// use as_string() (returns std::string by value); callers that
        /// need a null-terminated c_str must construct std::string
        /// explicitly first.
        std::string_view            get_string()const;

        /// @throw if get_type() != array_type | null_type
        variants&                   get_array();

        /// @throw if get_type() != array_type
        const variants&             get_array()const;

        /// @throw if get_type() != object_type | null_type
        variant_object&             get_object();

        /// @throw if get_type() != object_type
        const variant_object&       get_object()const;

        /// @pre is_object()
        const variant&              operator[]( const char* )const;
        /// @pre is_array()
        const variant&              operator[]( size_t pos )const;
        /// @pre is_array()
        size_t                      size()const;

        size_t                      estimated_size()const;
        /**
         *  _types that use non-intrusive variant conversion can implement the
         *  following method to implement conversion from variant to T.
         *
         *  <code>
         *  void from_variant( const Variant& var, T& val )
         *  </code>
         *
         *  The above form is not always convienant, so the this templated
         *  method is used to enable conversion from Variants to other
         *  types.
         */
        template<typename T>
        T as()const
        {
           T tmp;
           from_variant( *this, tmp );
           return tmp;
        }

        template<typename T>
        void as( T& v )const
        {
           from_variant( *this, v );
        }

        variant& operator=( variant&& v );
        variant& operator=( const variant& v );

        template<typename T>
        variant& operator=( T&& v )
        {
           return *this = variant( fc::forward<T>(v) );
        }

        template<typename T>
        explicit variant( const std::optional<T>& v )
        {
           if( v.has_value() ) *this = variant(*v);
        }

        template<typename T>
        explicit variant( const T& val );

        template<typename T>
        explicit variant( const T& val, const fc::yield_function_t& yield );

        void    clear();
      private:
        void    init();
        //enough room to store pointers, doubles, uint64s. doubled to allow 1 extra byte to store type at the end
        alignas(double) std::array<char, std::max(sizeof(uintmax_t ), sizeof(double)) * 2> _data = {};
   };

   typedef std::optional<variant> ovariant;

   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  std::string& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  fc::variants& vo );
   void from_variant( const fc::variant& var,  fc::variant& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  int64_t& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  uint64_t& vo );
   /** @ingroup Serializable */
   void to_variant( const fc::int256& val, fc::variant& vo );
   /** @ingroup Serializable */
   void to_variant( const fc::uint256& val, fc::variant& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  fc::int256& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  fc::uint256& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  bool& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  double& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  float& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  int32_t& vo );
   /** @ingroup Serializable */
   void from_variant( const fc::variant& var,  uint32_t& vo );

   /** @ingroup Serializable */
   template<typename T>
   void to_json_stream( const std::optional<T>& o, json_writer& w )
   {
      // Top-level std::optional emits null when unset; reflected struct fields use the
      // visitor's emit() overload that omits the field instead, mirroring to_variant_visitor::add.
      if( o ) to_json_stream( *o, w );
      else    w.value_null();
   }

   template<typename T>
   void from_variant( const variant& var,  std::optional<T>& vo )
   {
      if( var.is_null() ) vo = std::optional<T>();
      else
      {
          vo = T();
          from_variant( var, *vo );
      }
   }

   template<typename T>
   void to_json_stream( const std::unordered_set<T>& var, json_writer& w )
   {
      if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& e : var ) to_json_stream( e, w );
      w.end_array();
   }
   template<typename T>
   void to_variant( const std::unordered_set<T>& var,  fc::variant& vo )
   {
       if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
       variants vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = fc::variant(*itr);
       vo = std::move(vars);
   }
   template<typename T>
   void from_variant( const fc::variant& var,  std::unordered_set<T>& vo )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      vo.clear();
      vo.reserve( vars.size() );
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as<T>() );
   }


   template<typename K, typename T>
   void to_json_stream( const std::unordered_map<K, T>& var, json_writer& w )
   {
      // Mirrors fc::to_variant<std::unordered_map>: emits as an array of [key, value]
      // pairs (NOT a JSON object) so non-string keys round-trip cleanly.
      if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& kv : var ) to_json_stream( kv, w );
      w.end_array();
   }
   template<typename K, typename T>
   void to_variant( const std::unordered_map<K, T>& var,  fc::variant& vo )
   {
       if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
       std::vector< fc::variant > vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = fc::variant(*itr);
       vo = vars;
   }
   template<typename K, typename T>
   void from_variant( const fc::variant& var,  std::unordered_map<K, T>& vo )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      vo.clear();
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as< std::pair<K,T> >() );

   }
   template<typename K, typename T>
   void to_json_stream( const std::map<K, T>& var, json_writer& w )
   {
      if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& kv : var ) to_json_stream( kv, w );
      w.end_array();
   }
   template<typename K, typename T>
   void to_variant( const std::map<K, T>& var,  fc::variant& vo )
   {
       if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
       std::vector< fc::variant > vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = fc::variant(*itr);
       vo = vars;
   }
   template<typename K, typename T>
   void from_variant( const fc::variant& var,  std::map<K, T>& vo )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      vo.clear();
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as< std::pair<K,T> >() );
   }

   template<typename K, typename T>
   void to_json_stream( const std::multimap<K, T>& var, json_writer& w )
   {
      if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& kv : var ) to_json_stream( kv, w );
      w.end_array();
   }
   template<typename K, typename T>
   void to_variant( const std::multimap<K, T>& var,  fc::variant& vo )
   {
       if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
       std::vector< fc::variant > vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = fc::variant(*itr);
       vo = vars;
   }
   template<typename K, typename T>
   void from_variant( const fc::variant& var,  std::multimap<K, T>& vo )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      vo.clear();
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as< std::pair<K,T> >() );
   }


   template<typename T>
   void to_json_stream( const std::set<T>& var, json_writer& w )
   {
      if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& e : var ) to_json_stream( e, w );
      w.end_array();
   }
   template<typename T>
   void to_variant( const std::set<T>& var,  fc::variant& vo )
   {
       if( var.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
       variants vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = fc::variant(*itr);
       vo = std::move(vars);
   }
   template<typename T>
   void from_variant( const fc::variant& var,  std::set<T>& vo )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      vo.clear();
      //vo.reserve( vars.size() );
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as<T>() );
   }

   /** @ingroup Serializable */
   template<typename T>
   void to_json_stream( const std::deque<T>& t, json_writer& w )
   {
      if( t.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& e : t ) to_json_stream( e, w );
      w.end_array();
   }
   /** @ingroup Serializable */
   template<typename T>
   void to_variant( const std::deque<T>& t, fc::variant& v )
   {
      if( t.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      variants vars(t.size());
      for( size_t i = 0; i < t.size(); ++i )
         vars[i] = fc::variant(t[i]);
      v = std::move(vars);
   }
   /** @ingroup Serializable */
   template<typename T>
   void from_variant( const fc::variant& var, std::deque<T>& tmp )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      tmp.clear();
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         tmp.push_back( itr->as<T>() );
   }

   /** @ingroup Serializable */
   template<typename T, typename... U>
   void to_json_stream( const boost::container::deque<T, U...>& d, json_writer& w )
   {
      if( d.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& e : d ) to_json_stream( e, w );
      w.end_array();
   }
   /** @ingroup Serializable */
   template<typename T, typename... U>
   void to_variant( const boost::container::deque<T, U...>& d, fc::variant& vo )
   {
      if( d.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      variants vars(d.size());
      for( size_t i = 0; i < d.size(); ++i ) {
         vars[i] = fc::variant( d[i] );
      }
      vo = std::move( vars );
   }
   /** @ingroup Serializable */
   template<typename T, typename... U>
   void from_variant( const fc::variant& v, boost::container::deque<T, U...>& d )
   {
      const variants& vars = v.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      d.clear();
      d.resize( vars.size() );
      for( uint32_t i = 0; i < vars.size(); ++i ) {
         from_variant( vars[i], d[i] );
      }
   }

   /** @ingroup Serializable */
   template<typename T, typename A>
   void to_json_stream( const std::vector<T, A>& v, json_writer& w )
   {
      if( v.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      w.begin_array();
      for( const auto& e : v ) to_json_stream( e, w );
      w.end_array();
   }
   /** @ingroup Serializable */
   template<typename T>
   void to_variant( const std::vector<T>& t, fc::variant& v )
   {
      if( t.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      variants vars(t.size());
       for( size_t i = 0; i < t.size(); ++i )
          vars[i] = fc::variant(t[i]);
       v = std::move(vars);
   }
   /** @ingroup Serializable */
   template<typename T>
   void from_variant( const fc::variant& var, std::vector<T>& tmp )
   {
      const variants& vars = var.get_array();
      if( vars.size() > MAX_NUM_ARRAY_ELEMENTS ) throw std::range_error( "too large" );
      tmp.clear();
      tmp.reserve( vars.size() );
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         tmp.push_back( itr->as<T>() );
   }

   /** @ingroup Serializable */
   template<typename T, std::size_t S>
   void to_json_stream( const std::array<T,S>& t, json_writer& w )
   {
      w.begin_array();
      for( const auto& e : t ) to_json_stream( e, w );
      w.end_array();
   }
   /** @ingroup Serializable */
   template<typename T, std::size_t S>
   void from_variant( const fc::variant& var, std::array<T,S>& tmp )
   {
      const variants& vars = var.get_array();
      if( vars.size() != S) throw std::length_error( "mismatch between variant vector size and expected array size" );
      for( std::size_t i = 0; i < S; ++i )
         tmp[i] = vars.at(i).as<T>();
   }

   /** @ingroup Serializable */
   template<size_t N>
   void to_variant( const std::array<char,N>& bi, variant& v )
   {
      v = std::vector<char>( static_cast<const char*>(bi.data()), static_cast<const char*>(bi.data()) + sizeof(bi) );
   }

   /** @ingroup Serializable */
   template<size_t N>
   void from_variant( const variant& v, std::array<char,N>& bi )
   {
      std::vector<char> ve = v.as< std::vector<char> >();
      if( ve.size() )
      {
         memcpy(bi.begin(), ve.data(), fc::min<size_t>(ve.size(),sizeof(bi)) );
      }
      else
         memset( bi.begin(), char(0), sizeof(bi) );
   }

   /** @ingroup Serializable */
   template<typename T, std::size_t S>
   void to_variant( const std::array<T,S>& t, fc::variant& v )
   {
      variants vars(S);
      for( std::size_t i = 0; i < S; ++i )
         vars[i] = fc::variant(t[i]);
      v = std::move(vars);
   }

   /** @ingroup Serializable */
   template<typename T>
   void to_variant( const std::initializer_list<T>& t, fc::variant& v )
   {
      auto sz{t.size()};
      variants vars(sz);
      for( std::size_t i = 0; i < sz; ++i )
         vars[i] = fc::variant(*(t.begin()+i));
      v = std::move(vars);
   }

   /** @ingroup Serializable */
   template<typename A, typename B>
   void to_json_stream( const std::pair<A,B>& t, json_writer& w )
   {
      // Mirrors to_variant: emits a 2-element array [first, second].
      w.begin_array();
      to_json_stream( t.first, w );
      to_json_stream( t.second, w );
      w.end_array();
   }
   /** @ingroup Serializable */
   template<typename A, typename B>
   void to_variant( const std::pair<A,B>& t, fc::variant& v )
   {
      variants vars(2);
      vars[0] = fc::variant(t.first);
      vars[1] = fc::variant(t.second);
      v = std::move(vars);
   }

   /** @ingroup Serializable */
   template<typename A, typename B>
   void from_variant( const fc::variant& v, std::pair<A,B>& p )
   {
      const variants& vars = v.get_array();
      if( vars.size() > 0 )
         vars[0].as<A>( p.first );
      if( vars.size() > 1 )
         vars[1].as<B>( p.second );
   }

   template<typename T>
   variant::variant( const T& val )
   {
      to_variant( val, *this );
   }

   template<typename T>
   variant::variant( const T& val, const fc::yield_function_t& yield )
   {
      to_variant( val, *this, yield );
   }

   #ifdef __APPLE__
   inline void to_variant( size_t s, fc::variant& v ) { v = fc::variant(uint64_t(s)); }
   #endif
   template<typename T>
   void to_variant( const std::shared_ptr<T>& var,  fc::variant& vo )
   {
      if( var ) to_variant( *var, vo );
      else vo = nullptr;
   }

   /// JSON shape: dereference and emit the pointee, matching to_variant; null
   /// pointers emit JSON `null`.
   template<typename T>
   void to_json_stream( const std::shared_ptr<T>& var, json_writer& w )
   {
      if( var ) to_json_stream( *var, w );
      else     w.value_null();
   }

   template<typename T>
   void from_variant( const fc::variant& var,  std::shared_ptr<T>& vo )
   {
      if( var.is_null() ) vo = nullptr;
      else if( vo ) from_variant( var, *vo );
      else {
          vo = std::make_shared<T>();
          from_variant( var, *vo );
      }
   }
   template<typename T>
   void to_variant( const std::unique_ptr<T>& var,  fc::variant& vo )
   {
      if( var ) to_variant( *var, vo );
      else vo = nullptr;
   }

   template<typename T>
   void from_variant( const fc::variant& var,  std::unique_ptr<T>& vo )
   {
      if( var.is_null() ) vo.reset();
      else if( vo ) from_variant( var, *vo );
      else {
          vo.reset( new T() );
          from_variant( var, *vo );
      }
   }


   template<typename T>
   void to_variant( const safe<T>& s, fc::variant& v ) { v = s.value; }

   template<typename T>
   void from_variant( const fc::variant& v, safe<T>& s ) { s.value = v.as_uint64(); }

   template<typename T, typename... Args> void to_variant( const boost::multi_index_container<T,Args...>& c, fc::variant& v )
   {
       variants vars;
       vars.reserve( c.size() );
       for( const auto& item : c )
          vars.emplace_back( fc::variant(item) );
       v = std::move(vars);
   }

   template<typename T, typename... Args> void from_variant( const fc::variant& v, boost::multi_index_container<T,Args...>& c )
   {
      const variants& vars = v.get_array();
      c.clear();
      for( const auto& item : vars )
         c.insert( item.as<T>() );
   }
   // Generic boost::multiprecision to_variant/from_variant moved to fc/variant_multiprecision.hpp

   // Arithmetic on variants is not supported. The previous implementations
   // were unused dead code and `operator-` contained a never-reached loop bug.
   // If a use case ever appears, perform the conversion explicitly
   // (e.g. `a.as_int64() + b.as_int64()`) rather than relying on a generic
   // multi-type operator with surprising coercion rules.
   fc::variant operator + ( const fc::variant& a, const fc::variant& b ) = delete;
   fc::variant operator - ( const fc::variant& a, const fc::variant& b ) = delete;
   fc::variant operator * ( const fc::variant& a, const fc::variant& b ) = delete;
   fc::variant operator / ( const fc::variant& a, const fc::variant& b ) = delete;

   bool operator == ( const fc::variant& a, const fc::variant& b );
   bool operator != ( const fc::variant& a, const fc::variant& b );
   bool operator < ( const fc::variant& a, const fc::variant& b );
   bool operator > ( const fc::variant& a, const fc::variant& b );
   bool operator ! ( const fc::variant& a );
} // namespace fc

#include <fc/reflect/reflect.hpp>
FC_REFLECT_TYPENAME( fc::variant )
FC_REFLECT_ENUM( fc::variant::type_id, (null_type)(int64_type)(uint64_type)(int128_type)(uint128_type)(int256_type)(uint256_type)(double_type)(bool_type)(string_type)(array_type)(object_type)(blob_type)(string_sso_type) )
FC_REFLECT( fc::blob, (data) );
