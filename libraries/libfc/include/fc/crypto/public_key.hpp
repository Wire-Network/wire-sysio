#pragma once
#include <fc-lite/crypto/chain_types.hpp>

#include <fc/crypto/elliptic.hpp>
#include <fc/crypto/elliptic_r1.hpp>
#include <fc/crypto/elliptic_webauthn.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/static_variant.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/elliptic_ed.hpp>


namespace fc::crypto {
   namespace constants {
      constexpr const char* public_key_legacy_prefix = "SYS";
      constexpr const char* public_key_base_prefix = "PUB";

      constexpr const char* public_key_prefix[] = {
         "K1",
         "R1",
         "WA",
         "EM",
         "ED",
         "BLS"
      };

      constexpr auto public_key_wire_prefixes = std::array{public_key_legacy_prefix, public_key_base_prefix};
   };

   class public_key
   {
      public:
         using storage_type = std::variant<ecc::public_key_shim, r1::public_key_shim, webauthn::public_key, em::public_key_shim, ed::public_key_shim, bls::public_key_shim>;

         static public_key::storage_type parse_base58(const std::string& base58str);

         public_key() = default;
         public_key( public_key&& ) = default;
         public_key( const public_key& ) = default;
         public_key& operator= (const public_key& ) = default;

         public_key( const signature& c, const sha256& digest, bool check_canonical = true );

         explicit public_key( storage_type&& other_storage )
            :_storage(std::move(other_storage))
         {}

         bool valid()const;

         size_t which()const;

         // serialize to/from string
         explicit public_key(const std::string& base58str);
         std::string to_string(const fc::yield_function_t& yield) const;

         std::string to_native_string(const fc::yield_function_t& yield) const;

         template<typename T>
         bool contains() const { return std::holds_alternative<T>(_storage); }

         template<typename T>
         const T& get() const { return std::get<T>(_storage); }


         storage_type _storage{};

      private:
         friend std::ostream& operator<<(std::ostream& s, const public_key& k);
         friend bool operator==( const public_key& p1, const public_key& p2);
         friend bool operator!=( const public_key& p1, const public_key& p2);
         friend bool operator<( const public_key& p1, const public_key& p2);
         friend struct reflector<public_key>;
         friend class private_key;
   }; // public_key


   chain_key_type_t get_public_key_type(const std::variant<std::string, public_key>& pub_key_var);

} // fc::crypto

namespace fc {
   void to_variant(const crypto::public_key& var, variant& vo, const fc::yield_function_t& yield = fc::yield_function_t());

   void from_variant(const variant& var, crypto::public_key& vo);
} // namespace fc

FC_REFLECT(fc::crypto::public_key, (_storage) )

