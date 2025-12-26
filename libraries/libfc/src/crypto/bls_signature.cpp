#include <fc/crypto/bls_signature.hpp>
#include <fc/crypto/common.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/bls_common.hpp>

namespace fc::crypto::bls {

size_t signature::get_hash() const {
   auto raw_data = _affine_non_montgomery_le.data();
   return *(size_t*)&raw_data[32-sizeof(size_t)] + *(size_t*)&raw_data[64-sizeof(size_t)];
}

bls12_381::g2 signature::to_jacobian_montgomery_le(const bls::signature_data& affine_non_montgomery_le) {
   auto g2 = bls12_381::g2::fromAffineBytesLE(affine_non_montgomery_le, {.check_valid = true, .to_mont = true});
   FC_ASSERT(g2, "Invalid bls_signature");
   return *g2;
}

inline std::array<uint8_t, 192> from_span(std::span<const uint8_t, 192> affine_non_montgomery_le) {
   std::array<uint8_t, 192> r;
   std::ranges::copy(affine_non_montgomery_le, r.begin());
   return r;
}

signature::signature(const bls::signature_data& affine_non_montgomery_le)
   : _affine_non_montgomery_le(affine_non_montgomery_le)
   , _jacobian_montgomery_le(to_jacobian_montgomery_le(_affine_non_montgomery_le)) {}

signature::signature(std::span<const uint8_t, 192> affine_non_montgomery_le)
   : _affine_non_montgomery_le(from_span(affine_non_montgomery_le))
     , _jacobian_montgomery_le(to_jacobian_montgomery_le(_affine_non_montgomery_le)) {}

static std::array<uint8_t, 192> sig_parse_base64url(const std::string& base64urlstr) {
   try {
      auto res = std::mismatch(constants::signature_prefix.begin(), bls::constants::signature_prefix.end(),
                               base64urlstr.begin());
      FC_ASSERT(res.first == bls::constants::signature_prefix.end(), "BLS Signature has invalid format : ${str}",
                ("str", base64urlstr));
      auto data_str = base64urlstr.substr(bls::constants::signature_prefix.size());
      return fc::crypto::bls::deserialize_base64url<std::array<uint8_t, 192>>(data_str);
   } FC_RETHROW_EXCEPTIONS(warn, "error parsing bls_signature", ("str", base64urlstr ))
}

signature::signature(const std::string& base64urlstr)
   : _affine_non_montgomery_le(sig_parse_base64url(base64urlstr))
     , _jacobian_montgomery_le(to_jacobian_montgomery_le(_affine_non_montgomery_le)) {}

std::string signature::to_string() const {
   std::string data_str = fc::crypto::bls::serialize_base64url<std::array<uint8_t, 192>>(_affine_non_montgomery_le);
   return bls::constants::signature_prefix + data_str;
}

aggregate_signature::aggregate_signature(const std::string& base64_url_str)
   : _jacobian_montgomery_le(signature::to_jacobian_montgomery_le(sig_parse_base64url(base64_url_str))) {}

std::string aggregate_signature::to_string() const {
   std::array<uint8_t, 192> affine_non_montgomery_le = _jacobian_montgomery_le.toAffineBytesLE(
      bls12_381::from_mont::yes);
   std::string data_str = fc::crypto::bls::serialize_base64url<std::array<uint8_t, 192>>(affine_non_montgomery_le);
   return bls::constants::signature_prefix + data_str;
}

} // fc::crypto::bls

namespace fc {

void to_variant(const crypto::bls::signature& var, variant& vo) {
   vo = var.to_string();
}

void from_variant(const variant& var, crypto::bls::signature& vo) {
   vo = crypto::bls::signature(var.as_string());
}

void to_variant(const crypto::bls::aggregate_signature& var, variant& vo) {
   vo = var.to_string();
}

void from_variant(const variant& var, crypto::bls::aggregate_signature& vo) {
   vo = crypto::bls::aggregate_signature(var.as_string());
}
} // fc