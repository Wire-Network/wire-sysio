#pragma once

#include <fc-lite/crypto/chain_types.hpp>

#include <fc/crypto/elliptic.hpp>
#include <fc/crypto/elliptic_r1.hpp>
#include <fc/crypto/elliptic_webauthn.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/static_variant.hpp>
#include <fc/utility.hpp>

#include <array>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
         using storage_type = std::variant<ecc::public_key_shim, r1::public_key_shim, webauthn::public_key,
                                           em::public_key_shim, ed::public_key_shim, bls::public_key_shim>;
         enum class key_type : uint8_t {
            k1 = fc::get_index<storage_type, ecc::public_key_shim>(),
            r1 = fc::get_index<storage_type, r1::public_key_shim>(),
            wa = fc::get_index<storage_type, webauthn::public_key>(),
            em = fc::get_index<storage_type, em::public_key_shim>(),
            ed = fc::get_index<storage_type, ed::public_key_shim>(),
            bls = fc::get_index<storage_type, bls::public_key_shim>(),
            unknown
         };
         static_assert(std::variant_size_v<storage_type> == static_cast<uint8_t>(key_type::unknown), "Missing public_key key_type");
         static_assert(std::size(constants::public_key_prefix) == static_cast<size_t>(key_type::unknown), "Missing public_key prefix");

         constexpr static const char* key_prefix(key_type t) { return constants::public_key_prefix[std::to_underlying(t)]; };

         public_key() = default;
         public_key( public_key&& ) = default;
         public_key( const public_key& ) = default;
         public_key& operator= (const public_key& ) = default;

         public_key( const signature& c, const sha256& digest, bool check_canonical = true );

         explicit public_key( storage_type&& other_storage )
            :_storage(std::move(other_storage))
         {}

         bool valid()const;

         size_t which()const { return _storage.index(); }
         key_type type()const { return static_cast<key_type>(which()); }

         // If type is unknown, attempt to infer the key type from the string.
         static public_key from_string(const std::string& str, key_type type = key_type::unknown);

         // If include_prefix is true, the prefix will be included in the string representation.
         // Note for Wire native types (k1, r1, wa, bls) the prefix is always included.
         // For k1 if include_prefix is false, then the legacy prefix is used instead of PUB_K1_
         std::string to_string(const fc::yield_function_t& yield, bool include_prefix = false) const;

         template<typename... Args>
         bool contains() const { return (std::holds_alternative<Args>(_storage) || ...); }

         template<typename... Args>
         bool contains_type(Args... types) const {
            static_assert((std::is_same_v<Args, key_type> && ...), "Args must be of type public_key::key_type");
            auto current_index = _storage.index();
            return ((current_index == static_cast<size_t>(types)) || ...);
         }

         template<typename T>
         const T& get() const { return std::get<T>(_storage); }

         const storage_type& storage() const { return _storage; }

      private:
         storage_type _storage{};

         friend std::ostream& operator<<(std::ostream& s, const public_key& k);
         friend bool operator==( const public_key& p1, const public_key& p2);
         friend bool operator<( const public_key& p1, const public_key& p2);
         friend struct reflector<public_key>;
         friend class private_key;
   }; // public_key

} // fc::crypto

namespace fc {
   void to_variant(const crypto::public_key& var, variant& vo, const fc::yield_function_t& yield = fc::yield_function_t());

   void from_variant(const variant& var, crypto::public_key& vo);
} // namespace fc

FC_REFLECT(fc::crypto::public_key, (_storage) )
FC_REFLECT_ENUM(fc::crypto::public_key::key_type, (k1)(r1)(wa)(em)(ed)(bls)(unknown))
