#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>

#include <fc-lite/traits.hpp>

#include <algorithm>

namespace fc::crypto::ethereum {

/**
 * Removes "0x" prefix if present and converts hex string to lowercase
 * @param hex The hex string to trim
 * @return The trimmed and lowercase hex string
 */
std::string trim(const std::string& hex) {

   std::string result = hex.starts_with("0x") ? hex.substr(2) : hex;
   std::ranges::transform(result, result.begin(), ::tolower);
   return result;
}

std::string trim_public_key(const std::string& hex) {
   auto clean_hex = trim(hex);
   auto len       = clean_hex.length();

   FC_ASSERT(std::ranges::contains(public_key_string_lengths, len), "Invalid public key length({}): {}", len, hex);

   // If len mod 64 remainder is > 0, that means that the
   // first two bytes denote the type and will be truncated
   if (len % 64 > 0)
      clean_hex = clean_hex.substr(2);

   return clean_hex;
}

/**
 * Converts a byte array to hexadecimal string
 * @param bytes The byte array to convert
 * @param add_prefix If true, adds "0x" prefix to the result
 * @return The hexadecimal string representation
 */
std::string bytes_to_hex(const std::vector<uint8_t>& bytes, bool add_prefix) {
   return (add_prefix ? "0x" : "") + fc::to_hex(bytes);
}

/**
 * Converts a hexadecimal string to byte array
 * @param hex The hexadecimal string to convert
 * @return The resulting byte array
 */
std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
   auto clean_hex = trim(hex);
   if (clean_hex.length() == 130) {
      clean_hex = clean_hex.substr(2);
   }

   return fc::from_hex(clean_hex);
}

fc::crypto::keccak256 hash_message(std::span<const uint8_t> payload) {
   return keccak256::hash(payload);
}

fc::crypto::keccak256 hash_user_message(std::span<const uint8_t> payload) {
   std::vector<uint8_t> eth_message;
   auto payload_length_str = std::to_string(payload.size());

   eth_message.resize(crc155_message_prefix.size() + payload_length_str.size() + payload.size());
   std::copy(crc155_message_prefix.begin(), crc155_message_prefix.end(), eth_message.begin());
   std::copy(payload_length_str.begin(), payload_length_str.end(),
             eth_message.begin() + crc155_message_prefix.size());
   std::copy(payload.begin(), payload.end(),
             eth_message.begin() + crc155_message_prefix.size() + payload_length_str.size());

   return keccak256::hash(eth_message);
}

fc::em::public_key to_em_public_key(const std::string& pubkey_hex) {
   auto clean_hex         = trim_public_key(pubkey_hex);
   auto pubkey_bytes      = hex_to_bytes(clean_hex);
   auto pubkey_byte_count = pubkey_bytes.size();
   auto copy_to_offset    = 0;
   if (pubkey_byte_count == 65) {
      FC_ASSERT(pubkey_bytes[0] == 0x04);
   }
   if (pubkey_byte_count == 33) {
      FC_ASSERT(pubkey_bytes[0] == 0x02 || pubkey_bytes[0] == 0x03);
   }
   switch (pubkey_bytes.size()) {
   case 64:
      copy_to_offset = 1;
   case 65: {
      em::public_key_data_uncompressed pubkey_data;
      std::copy_n(pubkey_bytes.data(), pubkey_bytes.size(), pubkey_data.data() + copy_to_offset);
      if (copy_to_offset) {
         pubkey_data[0] = 0x04;
      }
      return fc::em::public_key(pubkey_data);
   }
   case 32:
      copy_to_offset = 1;
   case 33: {
      em::public_key_data pubkey_data{};
      std::copy_n(pubkey_bytes.data(), pubkey_bytes.size(), pubkey_data.data() + copy_to_offset);
      if (copy_to_offset) {
         pubkey_data[0] = 0x02;
      }
      return fc::em::public_key(pubkey_data);
   }
   default:
      FC_ASSERT(
         false,
         "Invalid public key size, expected 64/65 character hex string for compressed key OR 128/130 character hex string for public key")
      ;
   }
}

fc::em::private_key to_em_private_key(const std::string& privkey_hex) {
   auto privkey_bytes = fc::from_hex(fc::crypto::ethereum::trim(privkey_hex));
   em::private_key_secret sk{};
   FC_ASSERT(privkey_bytes.size() == fc::data_size(sk), "Invalid private key size, expected {}, received {}",
             fc::data_size(sk), privkey_bytes.size());
   std::memcpy(sk.data(), privkey_bytes.data(), fc::data_size(sk));
   return fc::em::private_key::regenerate(sk);
}

fc::em::compact_signature to_em_signature(const std::string& signature_hex) {
   auto signature_bytes = fc::from_hex(fc::crypto::ethereum::trim(signature_hex));
   FC_ASSERT(signature_bytes.size() == std::tuple_size_v<fc::em::compact_signature>,
             "Invalid signature size, expected {}, received {} character hex string",
             std::tuple_size_v<fc::em::compact_signature>, signature_bytes.size());
   fc::em::compact_signature sig;
   std::memcpy(sig.data(), signature_bytes.data(), signature_bytes.size());
   return sig;
}
}