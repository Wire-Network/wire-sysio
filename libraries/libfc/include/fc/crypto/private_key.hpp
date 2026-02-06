#pragma once
#include <fc/crypto/elliptic.hpp>
#include <fc/crypto/elliptic_r1.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/static_variant.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>

#include <utility>
#include <variant>
#include <string>

namespace fc { namespace crypto {

   namespace constants {
      constexpr const char* private_key_base_prefix = "PVT";
      constexpr const char* private_key_prefix[] = {
         "K1",
         "R1", // Associated with r1::public_key and webauthn::public_key
         "EM",
         "ED",
         "BLS"
      };
   };
   class public_key;
   class signature;
   class private_key
   {
      public:
         using storage_type = std::variant<ecc::private_key_shim, r1::private_key_shim,
                                           em::private_key_shim, ed::private_key_shim, bls::private_key_shim>;
         enum class key_type : uint8_t {
            k1 = fc::get_index<storage_type, ecc::private_key_shim>(),
            r1 = fc::get_index<storage_type, r1::private_key_shim>(),
            em = fc::get_index<storage_type, em::private_key_shim>(),
            ed = fc::get_index<storage_type, ed::private_key_shim>(),
            bls = fc::get_index<storage_type, bls::private_key_shim>(),
            unknown
         };
         static_assert(std::variant_size_v<storage_type> == static_cast<uint8_t>(key_type::unknown), "Missing private_key key_type");
         static_assert(std::size(constants::private_key_prefix) == static_cast<size_t>(key_type::unknown), "Missing private_key prefix");

         constexpr static const char* key_prefix(key_type t) { return constants::private_key_prefix[std::to_underlying(t)]; };

         private_key() = default;
         private_key( private_key&& ) = default;
         private_key( const private_key& ) = default;
         private_key& operator=(const private_key& ) = default;

         explicit private_key( storage_type&& other_storage )
             :_storage(std::move(other_storage))
         {}

         key_type type()const { return static_cast<key_type>(_storage.index()); }

         public_key     get_public_key() const;
         signature      sign( const sha256& digest, bool require_canonical = true ) const;
         sha512         generate_shared_secret( const public_key& pub ) const;

         template< typename KeyType = ecc::private_key_shim >
         static private_key generate() {
            return private_key(storage_type(KeyType::generate()));
         }

         template< typename KeyType = r1::private_key_shim >
         static private_key generate_r1() {
            return private_key(storage_type(KeyType::generate()));
         }

         template< typename KeyType = ecc::private_key_shim >
         static private_key regenerate( const typename KeyType::data_type& data ) {
            return private_key(storage_type(KeyType(data)));
         }

         // If type is unknown, attempt to infer the key type from the string.
         static private_key from_string(const std::string& str, key_type type = key_type::unknown);

         // If include_prefix is true, the prefix will be included in the string representation.
         // Note for Wire native types (k1, r1, bls) the prefix is always included.
         // For k1 if include_prefix is false, then the legacy wif format is used instead of PVT_K1_
         std::string to_string(const fc::yield_function_t& yield, bool include_prefix = false) const;

         template<typename... Args>
         bool contains() const { return (std::holds_alternative<Args>(_storage) || ...); }

         template<typename... Args>
         bool contains_type(Args... types) const {
            static_assert((std::is_same_v<Args, key_type> && ...), "Args must be of type private_key::key_type");
            auto current_index = _storage.index();
            return ((current_index == static_cast<size_t>(types)) || ...);
         }

         template<typename T>
         const T& get() const { return std::get<T>(_storage); }

      private:
         storage_type _storage{};

         friend bool operator==( const private_key& p1, const private_key& p2 );
         friend bool operator<( const private_key& p1, const private_key& p2 );
         friend struct reflector<private_key>;
   }; // private_key

} }  // fc::crypto

namespace fc {
   void to_variant(const crypto::private_key& var, variant& vo, const fc::yield_function_t& yield = fc::yield_function_t());

   void from_variant(const variant& var, crypto::private_key& vo);
} // namespace fc

FC_REFLECT(fc::crypto::private_key, (_storage) )
FC_REFLECT_ENUM(fc::crypto::private_key::key_type, (k1)(r1)(em)(ed)(bls)(unknown))
