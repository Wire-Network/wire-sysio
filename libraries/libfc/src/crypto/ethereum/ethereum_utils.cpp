#include <fc/crypto/hex.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>

#include <fc-lite/traits.hpp>

#include <ethash/keccak.hpp>
#include <algorithm>

namespace fc::crypto::ethereum {



keccak256_hash_t keccak256(const uint8_t* data, size_t size) {
   // TODO: Find a way of avoiding copying the result
   auto k256 = ethash::keccak256(data, size);
   keccak256_hash_t hash;
   std::copy_n(k256.bytes, sizeof(k256.bytes), hash.data());
   return hash;
}

keccak256_hash_t keccak256(const std::string& digest) {
   return keccak256(reinterpret_cast<const uint8_t*>(digest.data()), digest.size());
}
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

   FC_ASSERT(std::ranges::contains(public_key_string_lengths, len), "Invalid public key length(${len}): ${hex}",
             ("len", len)("hex", hex));

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

/**
 * Creates an Ethereum-styled message hash from a payload string
 * Uses the format: "\x19Ethereum Signed Message:\n" + length + message
 * @param payload The message to hash
 * @return The keccak256 hash of the formatted message
 */
em::message_hash_type hash_message(const em::message_body_type& payload) {

   em::message_hash_type     eth_message_digest{};
   std::vector<std::uint8_t> eth_message;
   if (std::holds_alternative<std::vector<std::uint8_t>>(payload)) {
      eth_message = std::get<std::vector<std::uint8_t>>(payload);
   } else if (std::holds_alternative<std::string>(payload)) {
      auto& payload_str = std::get<std::string>(payload);

      eth_message.resize(payload_str.size());
      std::ranges::copy(payload_str, eth_message.begin());
   } else {
      auto& payload_hash     = std::get<fc::sha256>(payload);
      auto eth_message_size = payload_hash.data_size();
      eth_message.resize(eth_message_size);

      std::copy_n(payload_hash.data(),
                  payload_hash.data_size(),
                  eth_message.begin());

   }

   auto h256 = ethash::keccak256(eth_message.data(), eth_message.size());
   std::copy_n(h256.bytes, sizeof(h256.bytes), eth_message_digest.data());

   return eth_message_digest;
}

em::message_hash_type hash_user_message(const em::message_body_type& payload) {

   em::message_hash_type     eth_message_digest{};
   std::vector<std::uint8_t> eth_message;
   if (std::holds_alternative<std::vector<std::uint8_t>>(payload)) {
      auto& payload_bytes = std::get<std::vector<std::uint8_t>>(payload);
      auto payload_str   = std::string{reinterpret_cast<const char*>(payload_bytes.data()), payload_bytes.size()};
      return hash_user_message(payload_str);
   } else if (std::holds_alternative<std::string>(payload)) {
      auto& payload_str        = std::get<std::string>(payload);
      auto payload_length_str = std::to_string(payload_str.size());

      // std::string       eth_message;
      eth_message.resize(crc155_message_prefix.size() + payload_length_str.size() + payload_str.size());
      std::copy(crc155_message_prefix.begin(), crc155_message_prefix.end(), eth_message.begin());
      std::copy(payload_length_str.begin(), payload_length_str.end(),
                eth_message.begin() + crc155_message_prefix.size());
      std::copy(payload_str.begin(), payload_str.end(),
                eth_message.begin() + crc155_message_prefix.size() + payload_length_str.size());
   } else {
      auto payload_hash       = std::get<fc::sha256>(payload);
      auto payload_length_str = std::to_string(payload_hash.data_size());
      auto eth_message_size   = crc155_message_prefix.size() + payload_length_str.length() + payload_hash.data_size();
      eth_message.resize(eth_message_size);

      std::copy_n(crc155_message_prefix.data(),
                  crc155_message_prefix.size(),
                  eth_message.begin());
      std::copy_n(payload_length_str.data(),
                  payload_length_str.size(),
                  eth_message.begin() + crc155_message_prefix.size());
      std::copy_n(payload_hash.data(),
                  payload_hash.data_size(),
                  eth_message.begin() + crc155_message_prefix.size() + payload_length_str.length());

   }

   auto h256 = ethash::keccak256(eth_message.data(), eth_message.size());
   std::copy_n(h256.bytes, sizeof(h256.bytes), eth_message_digest.data());

   return eth_message_digest;
}

fc::crypto::public_key to_public_key(const std::string& pubkey_hex) {
   auto em_pubkey = to_em_public_key(pubkey_hex);
   auto em_pubkey_data = em_pubkey.serialize();
   auto em_pubkey_shim = fc::em::public_key_shim(em_pubkey_data);
   return fc::crypto::public_key(em_pubkey_shim);
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
   FC_ASSERT(privkey_bytes.size() == fc::data_size(sk), "Invalid private key size, expected ${e}, received ${r}",
             ("s", fc::data_size(sk))("r", privkey_bytes.size()));
   std::copy_n(privkey_bytes.data(), privkey_bytes.size(), reinterpret_cast<std::uint8_t*>(sk.data()));
   return fc::em::private_key::regenerate(sk);
}

fc::em::compact_signature to_em_signature(const std::string& signature_hex) {
   auto signature_bytes = fc::from_hex(fc::crypto::ethereum::trim(signature_hex));
   FC_ASSERT(signature_bytes.size() == std::tuple_size_v<fc::em::compact_signature>,
             "Invalid signature size, expected ${s}, received ${r} character hex string",
             ("s", std::tuple_size_v<fc::em::compact_signature>)("r", signature_bytes.size()));
   fc::em::compact_signature sig;
   std::copy_n(signature_bytes.data(), signature_bytes.size(), sig.data());
   return sig;
}
}