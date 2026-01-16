#pragma once
#include <fc/crypto/common.hpp>
#include <fc/crypto/base64.hpp>

namespace fc::crypto::bls {

namespace constants {

constexpr std::string bls_public_key_prefix = "PUB_BLS_";
constexpr std::string bls_private_key_prefix = "PVT_BLS_";
constexpr std::string signature_prefix = "SIG_BLS_";

} // namespace constants

   template <typename Container>
   Container deserialize_base64url(const std::string& data_str) {
      using wrapper = checksum_data<Container>;
      wrapper wrapped;

      auto bin = fc::base64url_decode(data_str);
      fc::datastream<const char*> unpacker(bin.data(), bin.size());
      fc::raw::unpack(unpacker, wrapped);
      FC_ASSERT(!unpacker.remaining(), "decoded base64url length too long");
      auto checksum = wrapper::calculate_checksum(wrapped.data, nullptr);
      FC_ASSERT(checksum == wrapped.check);

      return wrapped.data;
   }

   template <typename Container>
   std::string serialize_base64url(const Container& data) {
      using wrapper = checksum_data<Container>;
      wrapper wrapped;

      wrapped.data = data;
      wrapped.check = wrapper::calculate_checksum(wrapped.data, nullptr);
      auto packed = raw::pack( wrapped );
      auto data_str = fc::base64url_encode( packed.data(), packed.size());
 
      return data_str;
   }

   inline public_key_data deserialize_bls_base64url(const std::string& base64urlstr) {
      using namespace fc::crypto::bls::constants;
      auto res = std::mismatch(bls_public_key_prefix.begin(), bls_public_key_prefix.end(), base64urlstr.begin());
      FC_ASSERT(res.first == bls_public_key_prefix.end(), "BLS Public Key has invalid format : ${str}", ("str", base64urlstr));
      auto data_str = base64urlstr.substr(bls_public_key_prefix.size());
      return fc::crypto::bls::deserialize_base64url<public_key_data>(data_str);
   }

   inline signature_data sig_parse_base64url(const std::string& base64urlstr) {
      auto res = std::mismatch(constants::signature_prefix.begin(), bls::constants::signature_prefix.end(), base64urlstr.begin());
      FC_ASSERT(res.first == bls::constants::signature_prefix.end(), "BLS Signature has invalid format : ${str}", ("str", base64urlstr));
      auto data_str = base64urlstr.substr(bls::constants::signature_prefix.size());
      return fc::crypto::bls::deserialize_base64url<bls::signature_data>(data_str);
   }

}  // fc::crypto::bls
