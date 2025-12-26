#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/common.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/bls_common.hpp>
#include <rapidjson/reader.h>

namespace fc::crypto::bls {
   namespace {
      const public_key_data empty_pub{};

      template<typename T, size_t N>
      std::array<T, N> deserialize_base64url_local(const std::string& base64urlstr) {
         auto res = std::mismatch(bls_public_key_prefix.begin(), bls_public_key_prefix.end(), base64urlstr.begin());
         FC_ASSERT(res.first == bls_public_key_prefix.end(), "BLS Public Key has invalid format : ${str}", ("str", base64urlstr));
         auto data_str = base64urlstr.substr(bls_public_key_prefix.size());
         return fc::crypto::bls::deserialize_base64url<std::array<T, N>>(data_str);
      }

      public_key_data from_span(const std::span<const public_key_data::value_type, public_key_data::value_count>& affine_non_montgomery_le) {
         public_key_data r;
         std::copy_n(affine_non_montgomery_le.data(), public_key_data_size, r.data);
         return r;
      }
      public_key_data from_std_array(const std::array<public_key_data::value_type, public_key_data::value_count>& affine_non_montgomery_le) {
         public_key_data r;
         std::copy_n(affine_non_montgomery_le.data(), public_key_data_size, r.data);
         return r;
      }

   }

   bls12_381::g1 public_key::from_affine_bytes_le(const public_key_data& affine_non_montgomery_le) {
      std::span<const uint8_t, public_key_data_size> affine_non_montgomery_le_span(
               affine_non_montgomery_le.data, public_key_data_size);
      std::optional<bls12_381::g1> g1 =
         bls12_381::g1::fromAffineBytesLE(affine_non_montgomery_le_span
            , {.check_valid = true, .to_mont = true});
      FC_ASSERT(g1);
      return *g1;
   }
   public_key::public_key(const public_key_data& affine_non_montgomery_le)
      : _affine_non_montgomery_le(affine_non_montgomery_le)
      , _jacobian_montgomery_le(from_affine_bytes_le(_affine_non_montgomery_le)) {
   }
   public_key::public_key(std::span<const uint8_t, public_key_data_size> affine_non_montgomery_le)
      : _affine_non_montgomery_le(from_span(affine_non_montgomery_le))
      , _jacobian_montgomery_le(from_affine_bytes_le(_affine_non_montgomery_le)) {
   }

   public_key::public_key(const std::string& base64urlstr)
      : _affine_non_montgomery_le(from_std_array(deserialize_base64url_local<uint8_t,96>(base64urlstr)))
      , _jacobian_montgomery_le(from_affine_bytes_le(_affine_non_montgomery_le)) {
   }

   public_key::public_key(const compact_signature& c, const fc::sha256& digest, bool check_canonical) {
     FC_ASSERT(false, "BLS public key recovery from a signature is unsupported");
   }

   public_key::public_key(const compact_signature& c, const unsigned char* digest, bool check_canonical) {
     FC_ASSERT(false, "BLS public key recovery from a signature is unsupported");
   }

   bool public_key::valid() const {
      return _affine_non_montgomery_le != empty_pub;
   }

   public_key_data public_key::serialize() const {
      return _affine_non_montgomery_le;
   }

   std::string public_key::to_string() const {
      std::array<public_key_data::value_type, public_key_data::value_count> data_array;
      std::copy_n(_affine_non_montgomery_le.data, public_key_data_size, data_array.data());
      std::string data_str = fc::crypto::bls::serialize_base64url<std::array<public_key_data::value_type, public_key_data::value_count>>(data_array);
      return bls_public_key_prefix + data_str;
   }

} // fc::crypto::bls

namespace fc {

   void to_variant(const crypto::bls::public_key& var, variant& vo) {
      vo = var.to_string();
   }

   void from_variant(const variant& var, crypto::bls::public_key& vo) {
      vo = crypto::bls::public_key(var.as_string());
   }

} // fc
