#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/utility.hpp>
#include <fc/crypto/common.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/bls_common.hpp>

namespace fc::crypto::bls {

using from_mont = bls12_381::from_mont;

public_key private_key::get_public_key() const {
   bls12_381::g1 pk = bls12_381::public_key(_sk);
   return public_key(pk.toAffineBytesLE(from_mont::yes));
}

signature private_key::proof_of_possession() const {
   bls12_381::g2 proof = bls12_381::pop_prove(_sk);
   return signature(proof.toAffineBytesLE(from_mont::yes));
}

bls::signature private_key::sign(const fc::sha256& digest) const {
   return sign(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(digest.data()), digest.data_size()));
}

signature private_key::sign(std::span<const uint8_t> message) const {
   bls12_381::g2 sig = bls12_381::sign(_sk, message);
   return signature(sig.toAffineBytesLE(from_mont::yes));
}

private_key private_key::generate() {
   std::vector<uint8_t> v(32);
   rand_bytes(reinterpret_cast<char*>(&v[0]), 32);
   return private_key(v);
}

sha512 private_key_shim::generate_shared_secret(const public_key_type& pub_key) const {
   FC_THROW_EXCEPTION(fc::unsupported_exception, "BLS does not support shared secrets");
}

signature_shim::public_key_type signature_shim::recover(const sha256& digest, bool check_canonical) const {
   FC_THROW_EXCEPTION(fc::unsupported_exception, "BLS Signature Recovery is not supported");
}

static fc::sha256::uint64_array_type priv_parse_base64url(const std::string& base64urlstr) {
   auto res = std::mismatch(bls::constants::bls_private_key_prefix.begin(),
                            bls::constants::bls_private_key_prefix.end(),
                            base64urlstr.begin());
   FC_ASSERT(res.first == bls::constants::bls_private_key_prefix.end(), "BLS Private Key has invalid format : ${str}",
             ("str", base64urlstr));

   auto data_str = base64urlstr.substr(bls::constants::bls_private_key_prefix.size());

   return fc::crypto::bls::deserialize_base64url<fc::sha256::uint64_array_type>(data_str);
}

private_key::private_key(const std::string& base64urlstr)
   : _sk(priv_parse_base64url(base64urlstr)) {}

std::string private_key::to_string() const {
   std::string data_str = fc::crypto::bls::serialize_base64url<sha256::hash_array_type>(_sk);

   return bls::constants::bls_private_key_prefix + data_str;
}

bool operator ==(const private_key& pk1, const private_key& pk2) {
   return pk1._sk == pk2._sk;
}

} // fc::crypto::bls

namespace fc {
void to_variant(const crypto::bls::private_key& var, variant& vo) {
   vo = var.to_string();
}

void from_variant(const variant& var, crypto::bls::private_key& vo) {
   vo = crypto::bls::private_key(var.as_string());
}

} // fc