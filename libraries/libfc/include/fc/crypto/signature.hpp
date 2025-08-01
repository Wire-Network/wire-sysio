#pragma once
#include <fc/static_variant.hpp>
#include <fc/crypto/elliptic.hpp>
#include <fc/crypto/elliptic_r1.hpp>
#include <fc/crypto/elliptic_webauthn.hpp>
#include <fc/crypto/elliptic_em.hpp> 
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>

namespace fc { namespace crypto {
   namespace config {
      constexpr const char* signature_base_prefix = "SIG";
      constexpr const char* signature_prefix[] = {
         "K1",
         "R1",
         "WA",
         "EM",
         "ED"
      };
   };

   class signature
   {
      public:
         using storage_type = std::variant<ecc::signature_shim, r1::signature_shim, webauthn::signature, em::signature_shim, ed::signature_shim>;

         signature() = default;
         signature( signature&& ) = default;
         signature( const signature& ) = default;
         signature& operator= (const signature& ) = default;

         // serialize to/from string
         explicit signature(const std::string& base58str);
         std::string to_string(const fc::yield_function_t& yield = fc::yield_function_t()) const;

         size_t which() const;

         size_t variable_size() const;

         template<typename T>
         bool contains() const { return std::holds_alternative<T>(_storage); }

         template<typename T>
         const T& get() const { return std::get<T>(_storage); }

         /**  
          *  True if this signature variant should be handled by recover(sig, digest)  
          *  rather than a verify(sig, pubkey, digest)  
          */
         bool is_recoverable() const {
            return std::visit([](auto const& shim){
               return std::decay_t<decltype(shim)>::is_recoverable;
            }, _storage);
         }

         template<typename Visitor>
         decltype(auto) visit( Visitor&& v ) const {
            return std::visit( std::forward<Visitor>(v), _storage );
         }

      private:
         storage_type _storage;

         signature( storage_type&& other_storage )
         :_storage(std::move(other_storage))
         {}

         friend bool operator == ( const signature& p1, const signature& p2);
         friend bool operator != ( const signature& p1, const signature& p2);
         friend bool operator < ( const signature& p1, const signature& p2);
         friend std::size_t hash_value(const signature& b); //not cryptographic; for containers
         friend struct reflector<signature>;
         friend class private_key;
         friend class public_key;
   }; // public_key

   size_t hash_value(const signature& b);

} }  // fc::crypto

namespace fc {
   void to_variant(const crypto::signature& var, variant& vo, const fc::yield_function_t& yield = fc::yield_function_t());

   void from_variant(const variant& var, crypto::signature& vo);
} // namespace fc

namespace std {
   template <> struct hash<fc::crypto::signature> {
      std::size_t operator()(const fc::crypto::signature& k) const {
         return fc::crypto::hash_value(k);
      }
   };
} // std

FC_REFLECT(fc::crypto::signature, (_storage) )
