// SPDX-License-Identifier: MIT
#include <fc/network/solana/solana_types.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fc::network::solana {

//=============================================================================
// pubkey implementation
//=============================================================================

std::string pubkey::to_base58() const {
   return fc::to_base58(reinterpret_cast<const char*>(data.data()), data.size(), fc::yield_function_t{});
}

pubkey pubkey::from_base58(const std::string& str) {
   auto bytes = fc::from_base58(str);
   FC_ASSERT(bytes.size() == SIZE, "Invalid Solana pubkey length: expected ${e}, got ${g}",
             ("e", SIZE)("g", bytes.size()));
   pubkey result;
   std::memcpy(result.data.data(), bytes.data(), SIZE);
   return result;
}

pubkey pubkey::from_public_key(const fc::crypto::public_key& pk) {
   FC_ASSERT(pk.contains<fc::crypto::ed::public_key_shim>(),
             "Public key must be ED25519 type for Solana");
   return from_ed_public_key(pk.get<fc::crypto::ed::public_key_shim>());
}

pubkey pubkey::from_ed_public_key(const fc::crypto::ed::public_key_shim& pk) {
   pubkey result;
   static_assert(sizeof(pk._data) == SIZE, "ED25519 public key size mismatch");
   std::copy(pk._data.begin(), pk._data.end(), result.data.begin());
   return result;
}

bool pubkey::is_zero() const {
   for (auto b : data) {
      if (b != 0)
         return false;
   }
   return true;
}

//=============================================================================
// signature implementation
//=============================================================================

std::string signature::to_base58() const {
   return fc::to_base58(reinterpret_cast<const char*>(data.data()), data.size(), fc::yield_function_t{});
}

signature signature::from_base58(const std::string& str) {
   auto bytes = fc::from_base58(str);
   FC_ASSERT(bytes.size() == SIZE, "Invalid Solana signature length: expected ${e}, got ${g}",
             ("e", SIZE)("g", bytes.size()));
   signature result;
   std::memcpy(result.data.data(), bytes.data(), SIZE);
   return result;
}

signature signature::from_ed_signature(const fc::crypto::ed::signature_shim& sig) {
   signature result;
   static_assert(sizeof(sig._data) == SIZE, "ED25519 signature size mismatch");
   std::copy(sig._data.begin(), sig._data.end(), result.data.begin());
   return result;
}

//=============================================================================
// commitment_t implementation
//=============================================================================

std::string to_string(commitment_t commitment) {
   switch (commitment) {
      case commitment_t::processed:
         return "processed";
      case commitment_t::confirmed:
         return "confirmed";
      case commitment_t::finalized:
         return "finalized";
      default:
         FC_THROW_EXCEPTION(fc::invalid_arg_exception, "Unknown commitment level");
   }
}

//=============================================================================
// compact_u16 implementation
//=============================================================================

namespace compact_u16 {

std::vector<uint8_t> encode(uint16_t value) {
   std::vector<uint8_t> result;

   if (value < 0x80) {
      // Single byte: value fits in 7 bits
      result.push_back(static_cast<uint8_t>(value));
   } else if (value < 0x4000) {
      // Two bytes: value fits in 14 bits
      result.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
      result.push_back(static_cast<uint8_t>(value >> 7));
   } else {
      // Three bytes: value uses up to 16 bits
      result.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
      result.push_back(static_cast<uint8_t>(((value >> 7) & 0x7F) | 0x80));
      result.push_back(static_cast<uint8_t>(value >> 14));
   }

   return result;
}

std::pair<uint16_t, size_t> decode(const uint8_t* data, size_t len) {
   FC_ASSERT(len > 0, "Empty data for compact-u16 decode");

   uint16_t value = 0;
   size_t bytes_read = 0;

   // First byte
   uint8_t b = data[bytes_read++];
   value = b & 0x7F;

   if (b < 0x80) {
      return {value, bytes_read};
   }

   // Second byte
   FC_ASSERT(bytes_read < len, "Truncated compact-u16");
   b = data[bytes_read++];
   value |= static_cast<uint16_t>(b & 0x7F) << 7;

   if (b < 0x80) {
      return {value, bytes_read};
   }

   // Third byte
   FC_ASSERT(bytes_read < len, "Truncated compact-u16");
   b = data[bytes_read++];
   FC_ASSERT(b <= 0x03, "Compact-u16 overflow");
   value |= static_cast<uint16_t>(b) << 14;

   return {value, bytes_read};
}

}  // namespace compact_u16

//=============================================================================
// message implementation
//=============================================================================

std::vector<uint8_t> message::serialize() const {
   std::vector<uint8_t> result;

   // Header (3 bytes)
   result.push_back(header.num_required_signatures);
   result.push_back(header.num_readonly_signed_accounts);
   result.push_back(header.num_readonly_unsigned_accounts);

   // Account keys (compact-u16 length + keys)
   auto len_bytes = compact_u16::encode(static_cast<uint16_t>(account_keys.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());
   for (const auto& key : account_keys) {
      result.insert(result.end(), key.data.begin(), key.data.end());
   }

   // Recent blockhash (32 bytes)
   result.insert(result.end(), recent_blockhash.data.begin(), recent_blockhash.data.end());

   // Instructions (compact-u16 length + instructions)
   len_bytes = compact_u16::encode(static_cast<uint16_t>(instructions.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());

   for (const auto& instr : instructions) {
      // Program ID index
      result.push_back(instr.program_id_index);

      // Account indices (compact-u16 length + indices)
      auto acct_len = compact_u16::encode(static_cast<uint16_t>(instr.account_indices.size()));
      result.insert(result.end(), acct_len.begin(), acct_len.end());
      result.insert(result.end(), instr.account_indices.begin(), instr.account_indices.end());

      // Data (compact-u16 length + data)
      auto data_len = compact_u16::encode(static_cast<uint16_t>(instr.data.size()));
      result.insert(result.end(), data_len.begin(), data_len.end());
      result.insert(result.end(), instr.data.begin(), instr.data.end());
   }

   return result;
}

message message::deserialize(const uint8_t* data, size_t len) {
   FC_ASSERT(len >= 3, "Message too short for header");

   message msg;
   size_t offset = 0;

   // Header
   msg.header.num_required_signatures = data[offset++];
   msg.header.num_readonly_signed_accounts = data[offset++];
   msg.header.num_readonly_unsigned_accounts = data[offset++];

   // Account keys
   auto [num_keys, key_len_bytes] = compact_u16::decode(data + offset, len - offset);
   offset += key_len_bytes;

   FC_ASSERT(offset + num_keys * pubkey::SIZE <= len, "Message too short for account keys");
   msg.account_keys.reserve(num_keys);
   for (uint16_t i = 0; i < num_keys; ++i) {
      pubkey key;
      std::memcpy(key.data.data(), data + offset, pubkey::SIZE);
      msg.account_keys.push_back(key);
      offset += pubkey::SIZE;
   }

   // Recent blockhash
   FC_ASSERT(offset + pubkey::SIZE <= len, "Message too short for blockhash");
   std::memcpy(msg.recent_blockhash.data.data(), data + offset, pubkey::SIZE);
   offset += pubkey::SIZE;

   // Instructions
   auto [num_instructions, instr_len_bytes] = compact_u16::decode(data + offset, len - offset);
   offset += instr_len_bytes;

   msg.instructions.reserve(num_instructions);
   for (uint16_t i = 0; i < num_instructions; ++i) {
      compiled_instruction instr;

      // Program ID index
      FC_ASSERT(offset < len, "Message too short for instruction program index");
      instr.program_id_index = data[offset++];

      // Account indices
      auto [num_accounts, acct_len_bytes] = compact_u16::decode(data + offset, len - offset);
      offset += acct_len_bytes;

      FC_ASSERT(offset + num_accounts <= len, "Message too short for instruction accounts");
      instr.account_indices.resize(num_accounts);
      std::memcpy(instr.account_indices.data(), data + offset, num_accounts);
      offset += num_accounts;

      // Data
      auto [data_size, data_len_bytes] = compact_u16::decode(data + offset, len - offset);
      offset += data_len_bytes;

      FC_ASSERT(offset + data_size <= len, "Message too short for instruction data");
      instr.data.resize(data_size);
      std::memcpy(instr.data.data(), data + offset, data_size);
      offset += data_size;

      msg.instructions.push_back(std::move(instr));
   }

   return msg;
}

//=============================================================================
// versioned_message implementation
//=============================================================================

std::vector<uint8_t> versioned_message::serialize() const {
   std::vector<uint8_t> result;

   // Version prefix for v0 messages
   if (version == 0) {
      result.push_back(VERSION_0_PREFIX);
   }

   // Header (3 bytes)
   result.push_back(header.num_required_signatures);
   result.push_back(header.num_readonly_signed_accounts);
   result.push_back(header.num_readonly_unsigned_accounts);

   // Static account keys (compact-u16 length + keys)
   auto len_bytes = compact_u16::encode(static_cast<uint16_t>(static_account_keys.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());
   for (const auto& key : static_account_keys) {
      result.insert(result.end(), key.data.begin(), key.data.end());
   }

   // Recent blockhash (32 bytes)
   result.insert(result.end(), recent_blockhash.data.begin(), recent_blockhash.data.end());

   // Instructions (compact-u16 length + instructions)
   len_bytes = compact_u16::encode(static_cast<uint16_t>(instructions.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());

   for (const auto& instr : instructions) {
      // Program ID index
      result.push_back(instr.program_id_index);

      // Account indices
      auto acct_len = compact_u16::encode(static_cast<uint16_t>(instr.account_indices.size()));
      result.insert(result.end(), acct_len.begin(), acct_len.end());
      result.insert(result.end(), instr.account_indices.begin(), instr.account_indices.end());

      // Data
      auto data_len = compact_u16::encode(static_cast<uint16_t>(instr.data.size()));
      result.insert(result.end(), data_len.begin(), data_len.end());
      result.insert(result.end(), instr.data.begin(), instr.data.end());
   }

   // Address table lookups (compact-u16 length + lookups)
   len_bytes = compact_u16::encode(static_cast<uint16_t>(address_table_lookups.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());

   for (const auto& lookup : address_table_lookups) {
      // Lookup table key
      result.insert(result.end(), lookup.key.data.begin(), lookup.key.data.end());

      // Writable indices
      auto writable_len = compact_u16::encode(static_cast<uint16_t>(lookup.writable_indices.size()));
      result.insert(result.end(), writable_len.begin(), writable_len.end());
      result.insert(result.end(), lookup.writable_indices.begin(), lookup.writable_indices.end());

      // Readonly indices
      auto readonly_len = compact_u16::encode(static_cast<uint16_t>(lookup.readonly_indices.size()));
      result.insert(result.end(), readonly_len.begin(), readonly_len.end());
      result.insert(result.end(), lookup.readonly_indices.begin(), lookup.readonly_indices.end());
   }

   return result;
}

//=============================================================================
// transaction implementation
//=============================================================================

std::vector<uint8_t> transaction::serialize() const {
   std::vector<uint8_t> result;

   // Signatures (compact-u16 length + signatures)
   auto len_bytes = compact_u16::encode(static_cast<uint16_t>(signatures.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());

   for (const auto& sig : signatures) {
      result.insert(result.end(), sig.data.begin(), sig.data.end());
   }

   // Message
   auto msg_bytes = msg.serialize();
   result.insert(result.end(), msg_bytes.begin(), msg_bytes.end());

   return result;
}

transaction transaction::deserialize(const uint8_t* data, size_t len) {
   FC_ASSERT(len > 0, "Empty transaction data");

   transaction tx;
   size_t offset = 0;

   // Signatures
   auto [num_sigs, sig_len_bytes] = compact_u16::decode(data + offset, len - offset);
   offset += sig_len_bytes;

   FC_ASSERT(offset + num_sigs * signature::SIZE <= len, "Transaction too short for signatures");
   tx.signatures.reserve(num_sigs);
   for (uint16_t i = 0; i < num_sigs; ++i) {
      signature sig;
      std::memcpy(sig.data.data(), data + offset, signature::SIZE);
      tx.signatures.push_back(sig);
      offset += signature::SIZE;
   }

   // Message
   tx.msg = message::deserialize(data + offset, len - offset);

   return tx;
}

//=============================================================================
// versioned_transaction implementation
//=============================================================================

std::vector<uint8_t> versioned_transaction::serialize() const {
   std::vector<uint8_t> result;

   // Signatures (compact-u16 length + signatures)
   auto len_bytes = compact_u16::encode(static_cast<uint16_t>(signatures.size()));
   result.insert(result.end(), len_bytes.begin(), len_bytes.end());

   for (const auto& sig : signatures) {
      result.insert(result.end(), sig.data.begin(), sig.data.end());
   }

   // Versioned message
   auto msg_bytes = msg.serialize();
   result.insert(result.end(), msg_bytes.begin(), msg_bytes.end());

   return result;
}

//=============================================================================
// pubkey_compat_t implementation
//=============================================================================

pubkey to_pubkey(const pubkey_compat_t& pk) {
   if (std::holds_alternative<pubkey>(pk)) {
      return std::get<pubkey>(pk);
   }
   return pubkey::from_base58(std::get<std::string>(pk));
}

}  // namespace fc::network::solana
