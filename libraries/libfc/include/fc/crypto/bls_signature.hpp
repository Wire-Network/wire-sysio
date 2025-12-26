#pragma once
#include <fc/reflect/variant.hpp>
#include <fc/io/varint.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <bls12-381/bls12-381.hpp>


namespace fc::crypto::bls {

   namespace constants {
      const std::string signature_prefix = "SIG_BLS_";
   };

   // Immutable after construction (although operator= is provided).
   // Provides an efficient wrapper around bls12_381::g2.
   // Serialization form:
   //   Non-Montgomery form and little-endian encoding for the field elements.
   //   Affine form for the group element (the z component is 1 and not included in the serialization).
   //   Binary serialization encodes size(192), x component, followed by y component.
   // Cached g2 in Jacobian Montgomery is used for efficient BLS math.
   // Keeping the serialized data allows for efficient serialization without the expensive conversion
   // from Jacobian Montgomery to Affine Non-Montgomery.
   class signature {
   public:
      signature() = default;
      signature(signature&&) = default;
      signature(const signature&) = default;
      signature& operator=(const signature&) = default;
      signature& operator=(signature&&) = default;

      // throws if unable to convert to valid bls12_381::g2
      explicit signature(const bls::signature_data& affine_non_montgomery_le);
      explicit signature(std::span<const uint8_t, 192> affine_non_montgomery_le);

      // affine non-montgomery base64url with signature_prefix
      explicit signature(const std::string& base64urlstr);

      // affine non-montgomery base64url with signature_prefix
      std::string to_string() const;

      const bls12_381::g2&            jacobian_montgomery_le() const { return _jacobian_montgomery_le; }
      const bls::signature_data&      affine_non_montgomery_le() const { return _affine_non_montgomery_le; }
      
      signature_data serialize() const {
         return signature_data{_affine_non_montgomery_le};
      }

      bool equal(const signature& sig) const {
         return _jacobian_montgomery_le.equal(sig._jacobian_montgomery_le);
      }

      auto operator<=>(const signature& rhs) const {
         return _affine_non_montgomery_le <=> rhs._affine_non_montgomery_le;
      }
      auto operator==(const signature& rhs) const {
         return _affine_non_montgomery_le == rhs._affine_non_montgomery_le;
      }

      template<typename T>
      friend T& operator<<(T& ds, const signature& sig) {
         // Serialization as variable length array when it is stored as a fixed length array. This makes for easier deserialization by external tools
         fc::raw::pack(ds, fc::unsigned_int(static_cast<uint32_t>(sizeof(sig._affine_non_montgomery_le))));
         ds.write(reinterpret_cast<const char*>(sig._affine_non_montgomery_le.data()), sizeof(sig._affine_non_montgomery_le));
         return ds;
      }

      // Could use FC_REFLECT, but to make it obvious serialization matches aggregate_signature implement via operator
      template<typename T>
      friend T& operator>>(T& ds, signature& sig) {
         // Serialization as variable length array when it is stored as a fixed length array. This makes for easier deserialization by external tools
         fc::unsigned_int size;
         fc::raw::unpack( ds, size );
         FC_ASSERT(size.value == sizeof(sig._affine_non_montgomery_le));
         ds.read(reinterpret_cast<char*>(sig._affine_non_montgomery_le.data()), sizeof(sig._affine_non_montgomery_le));
         sig._jacobian_montgomery_le = to_jacobian_montgomery_le(sig._affine_non_montgomery_le);
         return ds;
      }

      size_t get_hash() const;
      static bls12_381::g2 to_jacobian_montgomery_le(const bls::signature_data& affine_non_montgomery_le);
   private:
      bls::signature_data      _affine_non_montgomery_le{};
      bls12_381::g2            _jacobian_montgomery_le; // cached g2
   };

   // See signature comment above
   class aggregate_signature {
   public:
      aggregate_signature() = default;
      aggregate_signature(aggregate_signature&&) = default;
      aggregate_signature(const aggregate_signature&) = default;
      aggregate_signature& operator=(const aggregate_signature&) = default;
      aggregate_signature& operator=(aggregate_signature&&) = default;

      // affine non-montgomery base64url with signature_prefix
      explicit aggregate_signature(const std::string& base64_url_str);

      explicit aggregate_signature(const signature& sig)
         : _jacobian_montgomery_le(sig.jacobian_montgomery_le()) {}

      // aggregate signature into this
      void aggregate(const signature& sig) {
         _jacobian_montgomery_le.addAssign(sig.jacobian_montgomery_le());
      }
      // aggregate signature into this
      void aggregate(const aggregate_signature& sig) {
         _jacobian_montgomery_le.addAssign(sig.jacobian_montgomery_le());
      }

      // affine non-montgomery base64url with signature_prefix
      // Expensive as conversion from Jacobian Montgomery to Affine Non-Montgomery needed
      std::string to_string() const;

      const bls12_381::g2& jacobian_montgomery_le() const { return _jacobian_montgomery_le; }

      bool equal( const aggregate_signature& sig) const {
         return _jacobian_montgomery_le.equal(sig._jacobian_montgomery_le);
      }

      template<typename T>
      friend T& operator<<(T& ds, const aggregate_signature& sig) {
         std::array<uint8_t, 192> affine_non_montgomery_le =
            sig._jacobian_montgomery_le.toAffineBytesLE(bls12_381::from_mont::yes);
         // Serialization as variable length array when it is stored as a fixed length array.
         // This makes for easier deserialization by external tools
         fc::raw::pack(ds, fc::unsigned_int(static_cast<uint32_t>(sizeof(affine_non_montgomery_le))));
         ds.write(reinterpret_cast<const char*>(affine_non_montgomery_le.data()), sizeof(affine_non_montgomery_le));
         return ds;
      }

      // Could use FC_REFLECT, but to make it obvious serialization matches signature implement via operator
      template<typename T>
      friend T& operator>>(T& ds, aggregate_signature& sig) {
         // Serialization as variable length array when it is stored as a fixed length array.
         // This makes for easier deserialization by external tools
         fc::unsigned_int size;
         fc::raw::unpack( ds, size );
         std::array<uint8_t, 192> affine_non_montgomery_le;
         FC_ASSERT(size.value == sizeof(affine_non_montgomery_le));
         ds.read(reinterpret_cast<char*>(affine_non_montgomery_le.data()), sizeof(affine_non_montgomery_le));
         sig._jacobian_montgomery_le = signature::to_jacobian_montgomery_le(affine_non_montgomery_le);
         return ds;
      }

   private:
      bls12_381::g2  _jacobian_montgomery_le;
   };

}  // fc::crypto::bls

namespace fc {

   void to_variant(const crypto::bls::signature& var, variant& vo);
   void from_variant(const variant& var, crypto::bls::signature& vo);
   void to_variant(const crypto::bls::aggregate_signature& var, variant& vo);
   void from_variant(const variant& var, crypto::bls::aggregate_signature& vo);

} // namespace fc
