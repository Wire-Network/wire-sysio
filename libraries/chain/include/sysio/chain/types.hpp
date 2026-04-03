#pragma once
#include <sysio/chain/name.hpp>
#include <sysio/chain/chain_id_type.hpp>

#include <chainbase/chainbase.hpp>

#include <fc/interprocess/container.hpp>
#include <fc/io/varint.hpp>
#include <fc/io/enum_type.hpp>
#include <fc/crypto/sha224.hpp>
#include <fc/container/flat.hpp>
#include <fc/string.hpp>
#include <fc/io/raw.hpp>
#include <fc/static_variant.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/fixed_string.hpp>
#include <fc/crypto/private_key.hpp>

#include <boost/version.hpp>
#include <boost/container/deque.hpp>

#include <memory>
#include <vector>
#include <deque>
#include <cstdint>

#define OBJECT_CTOR1(NAME) \
    public: \
    template<typename Constructor> \
    NAME(Constructor&& c, chainbase::constructor_tag) \
    { c(*this); }
#define OBJECT_CTOR2_MACRO(x, y, field) ,field()
#define OBJECT_CTOR2(NAME, FIELDS) \
    public: \
    template<typename Constructor> \
    NAME(Constructor&& c, chainbase::constructor_tag)            \
    : id(0) BOOST_PP_SEQ_FOR_EACH(OBJECT_CTOR2_MACRO, _, FIELDS) \
    { c(*this); }
#define OBJECT_CTOR(...) BOOST_PP_OVERLOAD(OBJECT_CTOR, __VA_ARGS__)(__VA_ARGS__)

#define _V(n, v)  fc::mutable_variant_object(n, v)

namespace sysio::chain {
   using                               std::map;
   using                               std::vector;
   using                               std::unordered_map;
   using                               std::string;
   using                               std::shared_ptr;
   using                               std::weak_ptr;
   using                               std::unique_ptr;
   using                               std::set;
   using                               std::pair;
   using                               std::make_pair;
   using                               std::enable_shared_from_this;
   using                               std::tie;
   using                               std::move;
   using                               std::forward;
   using                               std::to_string;
   using                               std::all_of;

   using                               std::filesystem::path;
   using                               fc::variant_object;
   using                               fc::variant;
   using                               fc::enum_type;
   using                               fc::unsigned_int;
   using                               fc::signed_int;
   using                               fc::time_point_sec;
   using                               fc::time_point;
   using                               fc::flat_map;
   using                               fc::flat_multimap;
   using                               fc::flat_set;

   using public_key_type  = fc::crypto::public_key;
   using private_key_type = fc::crypto::private_key;
   using signature_type   = fc::crypto::signature;

   // configurable boost deque (for boost >= 1.71) performs much better than std::deque in our use cases
   using block_1024_option_t = boost::container::deque_options< boost::container::block_size<1024u> >::type;
   template<typename T>
   using deque = boost::container::deque< T, void, block_1024_option_t >;

   // ── KV key encoding utilities ──────────────────────────────────────────────
   // Used throughout chain, plugins, and tests for constructing KV 24-byte keys:
   //   [table:8B BE][scope:8B BE][pk:8B BE]

   /// Encode a uint64_t as 8 bytes big-endian into buf.
   inline void kv_encode_be64(char* buf, uint64_t v) {
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
   }

   /// Decode 8 bytes big-endian from buf into a uint64_t.
   inline uint64_t kv_decode_be64(const char* buf) {
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(buf[i]);
      return v;
   }

   /// Standard KV key sizes (bytes)
   inline constexpr size_t kv_key_size        = 24;  ///< [table:8B][scope:8B][pk:8B]
   inline constexpr size_t kv_prefix_size     = 16;  ///< [table:8B][scope:8B]
   inline constexpr size_t kv_table_prefix_size = 8; ///< [table:8B]

   /// Build a 24-byte KV key: [table:8B BE][scope:8B BE][pk:8B BE]
   struct kv_key_t {
      static constexpr size_t size = kv_key_size;
      char data[size];
      std::string_view to_string_view() const { return {data, size}; }
   };

   inline kv_key_t make_kv_key(uint64_t table, uint64_t scope, uint64_t pk) {
      kv_key_t key;
      kv_encode_be64(key.data,      table);
      kv_encode_be64(key.data + 8,  scope);
      kv_encode_be64(key.data + 16, pk);
      return key;
   }

   inline kv_key_t make_kv_key(name table, name scope, uint64_t pk) {
      return make_kv_key(table.to_uint64_t(), scope.to_uint64_t(), pk);
   }

   /// Build a 16-byte KV prefix: [table:8B BE][scope:8B BE]
   struct kv_prefix_t {
      static constexpr size_t size = kv_prefix_size;
      char data[size];
      std::string_view to_string_view() const { return {data, size}; }
      /// True if kv starts with this prefix
      bool matches(std::string_view kv) const { return kv.size() >= size && memcmp(kv.data(), data, size) == 0; }
   };

   inline kv_prefix_t make_kv_prefix(uint64_t table, uint64_t scope) {
      kv_prefix_t prefix;
      kv_encode_be64(prefix.data,     table);
      kv_encode_be64(prefix.data + 8, scope);
      return prefix;
   }

   inline kv_prefix_t make_kv_prefix(name table, name scope) {
      return make_kv_prefix(table.to_uint64_t(), scope.to_uint64_t());
   }

   /// Build an 8-byte KV table prefix: [table:8B BE]
   struct kv_table_prefix_t {
      static constexpr size_t size = kv_table_prefix_size;
      char data[size];
      std::string_view to_string_view() const { return {data, size}; }
      /// True if kv starts with this prefix
      bool matches(std::string_view kv) const { return kv.size() >= size && memcmp(kv.data(), data, size) == 0; }
   };

   inline kv_table_prefix_t make_kv_table_prefix(uint64_t table) {
      kv_table_prefix_t prefix;
      kv_encode_be64(prefix.data, table);
      return prefix;
   }

   inline kv_table_prefix_t make_kv_table_prefix(name table) {
      return make_kv_table_prefix(table.to_uint64_t());
   }

   struct void_t{};

   using chainbase::allocator;
   using shared_string = chainbase::shared_string;

   template<typename T>
   using shared_vector = chainbase::shared_vector<T>;

   template<typename K, typename V>
   using shared_flat_multimap = boost::interprocess::flat_multimap< K, V, std::less<K>, allocator< std::pair<K,V> > >;

   /**
    * For bugs in boost interprocess we moved our blob data to shared_string
    * this wrapper allows us to continue that while also having a type-level distinction for
    * serialization and to/from variant
    */
   class shared_blob : public shared_string {
   public:
      shared_blob() = default;

      shared_blob(shared_blob&&) = default;
      shared_blob(const shared_blob& s) = default;

      explicit shared_blob(std::string_view s) : shared_string(s) {}

      template <typename InputIterator>
      shared_blob(InputIterator f, InputIterator l) : shared_string(f, l) {}

      shared_blob& operator=(const shared_blob& s) = default;
      shared_blob& operator=(shared_blob&& ) = default;
      shared_blob& operator=(std::string_view sv) {
         static_cast<shared_string&>(*this) = sv;
         return *this;
      }
   };

   using action_name      = name;
   using scope_name       = name;
   using account_name     = name;
   using permission_name  = name;
   using table_name       = name;


   /**
    * List all object types from all namespaces here so they can
    * be easily reflected and displayed in debug output.  If a 3rd party
    * wants to extend the core code then they will have to change the
    * packed_object::type field from enum_type to uint16 to avoid
    * warnings when converting packed_objects to/from json.
    *
    * After launch, removing or reordering entries is a shared-memory
    * breaking change. Add new types before OBJECT_TYPE_COUNT. To retire
    * a type, rename it UNUSED_<name> and leave it in place as a
    * placeholder so subsequent ordinals remain stable.
    */
   enum object_type
   {
      null_object_type = 0,
      account_object_type,
      account_metadata_object_type,
      permission_object_type,
      permission_link_object_type,
      global_property_object_type,
      dynamic_global_property_object_type,
      block_summary_object_type,
      UNUSED_transaction_object_type,
      resource_object_type,
      resource_pending_object_type,
      resource_limits_state_object_type,
      resource_limits_config_object_type,
      protocol_state_object_type,
      code_object_type,
      database_header_object_type,
      contract_root_object_type,
      kv_object_type,
      kv_index_object_type,
      OBJECT_TYPE_COUNT ///< Sentry value which contains the number of different object types
   };

   /**
    *  Important notes on using chainbase objects in SYSIO code:
    *
    *  There are several constraints that need to be followed when using chainbase objects.
    *  Some of these constraints are due to the requirements imposed by the chainbase library,
    *  others are due to requirements to ensure determinism in the SYSIO chain library.
    *
    *  Before listing the constraints, the "restricted field set" must be defined.
    *
    *  Every chainbase object includes a field called id which has the type id_type.
    *  The id field is always included in the restricted field set.
    *
    *  A field of a chainbase object is considered to be in the restricted field set if it is involved in the
    *  derivation of the key used for one of the indices in the chainbase multi-index unless its only involvement
    *  is through being included in composite_keys that end with the id field.
    *
    *  So if the multi-index includes an index like the following
    *  ```
    *    ordered_unique< tag<by_sender_id>,
    *       composite_key< generated_transaction_object,
    *          BOOST_MULTI_INDEX_MEMBER( generated_transaction_object, account_name, sender),
    *          BOOST_MULTI_INDEX_MEMBER( generated_transaction_object, uint128_t, sender_id)
    *       >
    *    >
    *  ```
    *  both `sender` and `sender_id` fields are part of the restricted field set.
    *
    *  On the other hand, an index like the following
    *  ```
    *    ordered_unique< tag<by_expiration>,
    *       composite_key< generated_transaction_object,
    *          BOOST_MULTI_INDEX_MEMBER( generated_transaction_object, time_point, expiration),
    *          BOOST_MULTI_INDEX_MEMBER( generated_transaction_object, generated_transaction_object::id_type, id)
    *       >
    *    >
    *  ```
    *  would not by itself require the `expiration` field to be part of the restricted field set.
    *
    *  The restrictions on usage of the chainbase objects within this code base are:
    *     + The chainbase object includes the id field discussed above.
    *     + The multi-index must include an ordered_unique index tagged with by_id that is based on the id field as the sole key.
    *     + No other types of indices other than ordered_unique are allowed.
    *       If an index is desired that does not enforce uniqueness, then use a composite key that ends with the id field.
    *     + When creating a chainbase object, the constructor lambda should never mutate the id field.
    *     + When modifying a chainbase object, the modifier lambda should never mutate any fields in the restricted field set.
    */

   class account_object;

   using block_id_type       = fc::sha256;
   using checksum_type       = fc::sha256;
   using checksum256_type    = fc::sha256;
   using checksum512_type    = fc::sha512;
   using checksum160_type    = fc::ripemd160;
   using transaction_id_type = checksum_type;
   using digest_type         = checksum_type;
   using weight_type         = uint16_t;
   using block_num_type      = uint32_t;
   using share_type          = int64_t;
   using int128_t            = __int128;
   using uint128_t           = unsigned __int128;
   using bytes               = vector<char>;
   using digests_t           = deque<digest_type>;

   /**
    *  Extentions are prefixed with type and are a buffer that can be
    *  interpreted by code that is aware and ignored by unaware code.
    */
   typedef vector<std::pair<uint16_t,vector<char>>> extensions_type;

   /**
    * emplace an extension into the extensions type such that it is properly ordered by extension id
    * this assumes exts is already sorted by extension id
    */
   inline auto emplace_extension( extensions_type& exts, uint16_t eid, vector<char>&& data) {
      auto insert_itr = std::upper_bound(exts.begin(), exts.end(), eid, [](uint16_t id, const auto& ext){
         return id < ext.first;
      });

      return exts.emplace(insert_itr, eid, std::move(data));
   }


   template<typename Container>
   class end_insert_iterator
   {
   protected:
      Container* container;

   public:
      using iterator_category = std::output_iterator_tag;
      using value_type = void;
      using difference_type = void;
      using pointer = void;
      using reference = void;

      using container_type = Container;

      explicit end_insert_iterator( Container& c )
      :container(&c)
      {}

      end_insert_iterator& operator=( typename Container::const_reference value ) {
         container->insert( container->cend(), value );
         return *this;
      }

      end_insert_iterator& operator*() { return *this; }
      end_insert_iterator& operator++() { return *this; }
      end_insert_iterator  operator++(int) { return *this; }
   };

   template<typename Container>
   inline end_insert_iterator<Container> end_inserter( Container& c ) {
      return end_insert_iterator<Container>( c );
   }

   template<typename T>
   struct enum_hash
   {
      static_assert( std::is_enum<T>::value, "enum_hash can only be used on enumeration types" );

      using underlying_type = typename std::underlying_type<T>::type;

      std::size_t operator()(T t) const
      {
           return std::hash<underlying_type>{}( static_cast<underlying_type>(t) );
      }
   };
   // enum_hash needed to support old gcc compiler of Ubuntu 16.04

   namespace detail {
      struct extract_match {
         bool enforce_unique = false;
      };

      template<typename... Ts>
      struct decompose;

      template<>
      struct decompose<> {
         template<typename ResultVariant>
         static auto extract( uint16_t id, const vector<char>& data, ResultVariant& result )
         -> std::optional<extract_match>
         {
            return {};
         }
      };

      template<typename T, typename... Rest>
      struct decompose<T, Rest...> {
         using head_t = T;
         using tail_t = decompose< Rest... >;

         template<typename ResultVariant>
         static auto extract( uint16_t id, const vector<char>& data, ResultVariant& result )
         -> std::optional<extract_match>
         {
            if( id == head_t::extension_id() ) {
               result = fc::raw::unpack<head_t>( data );
               return { extract_match{ head_t::enforce_unique() } };
            }

            return tail_t::template extract<ResultVariant>( id, data, result );
         }
      };

      template<typename T, typename ... Ts>
      struct is_any_of {
         static constexpr bool value = std::disjunction_v<std::is_same<T, Ts>...>;
      };

      template<typename T, typename ... Ts>
      constexpr bool is_any_of_v = is_any_of<T, Ts...>::value;
   }

   template<typename E, typename F>
   static constexpr auto has_field( F flags, E field )
   -> std::enable_if_t< std::is_integral<F>::value && std::is_unsigned<F>::value &&
                        std::is_enum<E>::value && std::is_same< F, std::underlying_type_t<E> >::value, bool>
   {
      return ( (flags & static_cast<F>(field)) != 0 );
   }

   template<typename E, typename F>
   static constexpr auto set_field( F flags, E field, bool value = true )
   -> std::enable_if_t< std::is_integral<F>::value && std::is_unsigned<F>::value &&
                        std::is_enum<E>::value && std::is_same< F, std::underlying_type_t<E> >::value, F >
   {
      if( value )
         return ( flags | static_cast<F>(field) );
      else
         return ( flags & ~static_cast<F>(field) );
   }

   template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
   template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

   // next_function is a function passed to an API (like send_transaction) and which is called at the end of
   // the API processing on the main thread. The type T is a description of the API result that can be
   // serialized as output.
   // The function accepts a variant which can contain an exception_ptr (if an exception occured while
   // processing the API) or the result T.
   // The third option is a function which can be executed in a multithreaded context (likely on the
   // http_plugin thread pool) and which completes the API processing and returns the result T.
   // -------------------------------------------------------------------------------------------------------
   template<typename T>
   using t_or_exception = std::variant<T, fc::exception_ptr>;

   template<typename T>
   using next_function_variant = std::variant<fc::exception_ptr, T, std::function<t_or_exception<T>()>>;

   template<typename T>
   using next_function = std::function<void(const next_function_variant<T>&)>;

   // to configure whether a process should be done asynchronously or not
   enum class async_t { no, yes };

}  // sysio::chain

FC_REFLECT_EMPTY( sysio::chain::void_t )
