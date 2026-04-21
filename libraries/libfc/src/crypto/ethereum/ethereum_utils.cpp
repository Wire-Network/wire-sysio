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

   // For uncompressed keys (130 chars = 0x04 + 64 bytes x||y) strip the fixed
   // 0x04 prefix — it's reapplied unconditionally below. For compressed keys
   // (66 chars = 0x02/0x03 + 32 bytes x) the prefix encodes y-parity and MUST
   // be preserved; stripping it forced every caller through the bare-x path
   // which defaulted to 0x02 and silently corrupted odd-parity (0x03) keys.
   if (len == 130)
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
   switch (pubkey_byte_count) {
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
   case 33: {
      em::public_key_data pubkey_data{};
      std::copy_n(pubkey_bytes.data(), pubkey_bytes.size(), pubkey_data.data());
      return fc::em::public_key(pubkey_data);
   }
   default:
      FC_ASSERT(false,
                "Invalid EM public key byte count ({}). Expected 33 (compressed), 64 or 65 (uncompressed).",
                pubkey_byte_count);
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