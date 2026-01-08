#pragma once
#include <fc/reflect/variant.hpp>
#include <fc/io/varint.hpp>
#include <fc/exception/exception.hpp>
#include <bls12-381/bls12-381.hpp>
#include <fc/crypto/sha256.hpp>

namespace fc::crypto::bls {

   constexpr std::size_t private_key_data_size = 256;
   constexpr std::size_t public_key_data_size = 96;
   constexpr std::size_t signature_data_size = 192;

   using public_key_data = std::array<std::uint8_t,public_key_data_size>;
   using private_key_secret = std::array<uint64_t, 4>;

   using signature_data = std::array<std::uint8_t,signature_data_size>;
   using signature_data_span = std::span<std::uint8_t,signature_data_size>;
   using compact_signature_data = signature_data;
   using compact_signature = signature_data;

   // Immutable after construction (although operator= is provided).
   //   Attributes are not const because FC_REFLECT only works for non-const members.
   // Provides an efficient wrapper around bls12_381::g1.
   // Serialization form:
   //   Non-Montgomery form and little-endian encoding for the field elements.
   //   Affine form for the group element (the z component is 1 and not included in the serialization).
   //   Binary serialization encodes size(96), x component, followed by y component.
   // Cached g1 in Jacobian Montgomery is used for efficient BLS math.
   // Keeping the serialized data allows for efficient serialization without the expensive conversion
   // from Jacobian Montgomery to Affine Non-Montgomery.
   class public_key : fc::reflect_init {
   public:
      public_key() = default;
      public_key(public_key&&) = default;
      public_key(const public_key&) = default;
      public_key& operator=(const public_key& rhs) = default;
      public_key& operator=(public_key&& rhs) = default;

      // throws if unable to convert to valid bls12_381::g1
      explicit public_key(std::span<const uint8_t, public_key_data_size> affine_non_montgomery_le);
      explicit public_key(const public_key_data& affine_non_montgomery_le);

      // affine non-montgomery base64url with bls_public_key_prefix
      explicit public_key(const std::string& base64urlstr);
      public_key( const compact_signature& c, const fc::sha256& digest, bool check_canonical = true );
      public_key( const compact_signature& c, const unsigned char* digest, bool check_canonical = true );

      bool valid()const;
      public_key_data serialize()const;
      // affine non-montgomery base64url with bls_public_key_prefix
      std::string to_string() const;
      static std::string to_string(const public_key_data& key);

      const bls12_381::g1&    jacobian_montgomery_le() const { return _jacobian_montgomery_le; }
      const public_key_data&  affine_non_montgomery_le() const { return _affine_non_montgomery_le; }

      bool equal(const public_key& pkey) const {
         return _jacobian_montgomery_le.equal(pkey._jacobian_montgomery_le);
      }

      auto operator<=>(const public_key& rhs) const {
         return _affine_non_montgomery_le <=> rhs._affine_non_montgomery_le;
      }
      auto operator==(const public_key& rhs) const {
         return _affine_non_montgomery_le == rhs._affine_non_montgomery_le;
      }

      template<typename T>
      friend T& operator<<(T& ds, const public_key& sig) {
         // Serialization as variable length array when it is stored as a fixed length array. This makes for easier deserialization by external tools
         fc::raw::pack(ds, fc::unsigned_int(static_cast<uint32_t>(sizeof(sig._affine_non_montgomery_le))));
         ds.write(reinterpret_cast<const char*>(sig._affine_non_montgomery_le.data()), sizeof(sig._affine_non_montgomery_le));
         return ds;
      }

      friend std::ostream& operator<<(std::ostream& os, const public_key& k) {
         os << "bls_public_key(0x" << std::hex;
         for (auto c : k.affine_non_montgomery_le())
            os << std::setfill('0') << std::setw(2) << static_cast<int>(c);
         os << std::dec << ")";
         return os;
      }

      template<typename T>
      friend T& operator>>(T& ds, public_key& sig) {
         // Serialization as variable length array when it is stored as a fixed length array. This makes for easier deserialization by external tools
         fc::unsigned_int size;
         fc::raw::unpack( ds, size );
         FC_ASSERT(size.value == sizeof(sig._affine_non_montgomery_le));
         ds.read(reinterpret_cast<char*>(sig._affine_non_montgomery_le.data()), sizeof(sig._affine_non_montgomery_le));
         sig._jacobian_montgomery_le = from_affine_bytes_le(sig._affine_non_montgomery_le);
         return ds;
      }

      static bls12_381::g1 from_affine_bytes_le(const public_key_data& affine_non_montgomery_le);
   private:
      public_key_data         _affine_non_montgomery_le{};
      bls12_381::g1           _jacobian_montgomery_le; // cached g1
   };

}  // fc::crypto::bls

namespace fc {
   void to_variant(const crypto::bls::public_key& var, variant& vo);
   void from_variant(const variant& var, crypto::bls::public_key& vo);
} // namespace fc
