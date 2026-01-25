// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/variant.hpp>

namespace fc::network::solana {

// Forward declarations
struct pubkey;
struct signature;

/**
 * @brief 32-byte Solana public key (base58 encoded in Solana)
 */
struct pubkey {
   static constexpr size_t SIZE = 32;
   std::array<uint8_t, SIZE> data{};

   pubkey() = default;
   explicit pubkey(const std::array<uint8_t, SIZE>& d) : data(d) {}
   explicit pubkey(const uint8_t* d) { std::copy_n(d, SIZE, data.begin()); }

   /**
    * @brief Convert public key to base58 string
    */
   std::string to_base58() const;

   /**
    * @brief Create public key from base58 string
    */
   static pubkey from_base58(const std::string& str);

   /**
    * @brief Create public key from fc::crypto::public_key (ED25519)
    */
   static pubkey from_public_key(const fc::crypto::public_key& pk);

   /**
    * @brief Create public key from ED25519 public key shim
    */
   static pubkey from_ed_public_key(const fc::crypto::ed::public_key_shim& pk);

   /**
    * @brief Check if the public key is all zeros (default/uninitialized)
    */
   bool is_zero() const;

   bool operator==(const pubkey& other) const { return data == other.data; }
   bool operator!=(const pubkey& other) const { return data != other.data; }
   bool operator<(const pubkey& other) const { return data < other.data; }
};

/**
 * @brief 64-byte Solana signature
 */
struct signature {
   static constexpr size_t SIZE = 64;
   std::array<uint8_t, SIZE> data{};

   signature() = default;
   explicit signature(const std::array<uint8_t, SIZE>& d) : data(d) {}
   explicit signature(const uint8_t* d) { std::copy_n(d, SIZE, data.begin()); }

   /**
    * @brief Convert signature to base58 string
    */
   std::string to_base58() const;

   /**
    * @brief Create signature from base58 string
    */
   static signature from_base58(const std::string& str);

   /**
    * @brief Create signature from ED25519 signature shim
    */
   static signature from_ed_signature(const fc::crypto::ed::signature_shim& sig);

   bool operator==(const signature& other) const { return data == other.data; }
   bool operator!=(const signature& other) const { return data != other.data; }
};

/**
 * @brief Commitment levels for RPC queries
 */
enum class commitment_t {
   processed,  // Query the most recent block processed by the node
   confirmed,  // Query the most recent block that has reached 1 confirmation
   finalized   // Query the most recent block that has been finalized
};

/**
 * @brief Convert commitment enum to string
 */
std::string to_string(commitment_t commitment);

/**
 * @brief Account metadata for instructions
 */
struct account_meta {
   pubkey key;
   bool is_signer = false;
   bool is_writable = false;

   account_meta() = default;
   account_meta(const pubkey& k, bool signer, bool writable)
      : key(k), is_signer(signer), is_writable(writable) {}

   static account_meta readonly(const pubkey& k, bool signer = false) {
      return account_meta(k, signer, false);
   }

   static account_meta writable(const pubkey& k, bool signer = false) {
      return account_meta(k, signer, true);
   }

   static account_meta signer(const pubkey& k, bool writable = true) {
      return account_meta(k, true, writable);
   }
};

/**
 * @brief Single instruction to be executed by a program
 */
struct instruction {
   pubkey program_id;
   std::vector<account_meta> accounts;
   std::vector<uint8_t> data;

   instruction() = default;
   instruction(const pubkey& prog_id, std::vector<account_meta> accts, std::vector<uint8_t> d)
      : program_id(prog_id), accounts(std::move(accts)), data(std::move(d)) {}
};

/**
 * @brief Compiled instruction with account indices
 */
struct compiled_instruction {
   uint8_t program_id_index = 0;
   std::vector<uint8_t> account_indices;
   std::vector<uint8_t> data;
};

/**
 * @brief Message header containing signature counts
 */
struct message_header {
   uint8_t num_required_signatures = 0;
   uint8_t num_readonly_signed_accounts = 0;
   uint8_t num_readonly_unsigned_accounts = 0;
};

/**
 * @brief Transaction message (legacy format)
 */
struct message {
   message_header header;
   std::vector<pubkey> account_keys;
   pubkey recent_blockhash;
   std::vector<compiled_instruction> instructions;

   /**
    * @brief Serialize the message for signing or transmission
    */
   std::vector<uint8_t> serialize() const;

   /**
    * @brief Deserialize a message from bytes
    */
   static message deserialize(const uint8_t* data, size_t len);
   static message deserialize(const std::vector<uint8_t>& data) {
      return deserialize(data.data(), data.size());
   }
};

/**
 * @brief Address lookup table entry for v0 transactions
 */
struct address_lookup_table {
   pubkey key;
   std::vector<uint8_t> writable_indices;
   std::vector<uint8_t> readonly_indices;
};

/**
 * @brief Versioned message (v0 format)
 */
struct versioned_message {
   static constexpr uint8_t VERSION_0_PREFIX = 0x80;

   uint8_t version = 0;  // 0 for v0 transactions
   message_header header;
   std::vector<pubkey> static_account_keys;
   pubkey recent_blockhash;
   std::vector<compiled_instruction> instructions;
   std::vector<address_lookup_table> address_table_lookups;

   /**
    * @brief Serialize the versioned message
    */
   std::vector<uint8_t> serialize() const;

   /**
    * @brief Check if this is a v0 message
    */
   bool is_v0() const { return version == 0; }
};

/**
 * @brief Full transaction with signatures (legacy format)
 */
struct transaction {
   std::vector<signature> signatures;
   message msg;

   /**
    * @brief Serialize the transaction for transmission
    */
   std::vector<uint8_t> serialize() const;

   /**
    * @brief Deserialize a transaction from bytes
    */
   static transaction deserialize(const uint8_t* data, size_t len);
   static transaction deserialize(const std::vector<uint8_t>& data) {
      return deserialize(data.data(), data.size());
   }

   /**
    * @brief Get the number of required signatures
    */
   size_t num_required_signatures() const {
      return msg.header.num_required_signatures;
   }

   /**
    * @brief Check if the transaction is fully signed
    */
   bool is_signed() const {
      return signatures.size() >= num_required_signatures();
   }
};

/**
 * @brief Versioned transaction with signatures
 */
struct versioned_transaction {
   std::vector<signature> signatures;
   versioned_message msg;

   /**
    * @brief Serialize the versioned transaction
    */
   std::vector<uint8_t> serialize() const;
};

/**
 * @brief Account information returned from RPC
 */
struct account_info {
   uint64_t lamports = 0;
   pubkey owner;
   std::vector<uint8_t> data;
   bool executable = false;
   uint64_t rent_epoch = 0;
};

/**
 * @brief RPC response context
 */
struct rpc_context {
   uint64_t slot = 0;
};

/**
 * @brief Generic RPC response wrapper
 */
template <typename T>
struct rpc_response {
   rpc_context context;
   T value;
};

/**
 * @brief Signature status information
 */
struct signature_status {
   uint64_t slot = 0;
   std::optional<uint64_t> confirmations;
   std::optional<std::string> err;
   std::string confirmation_status;  // "processed", "confirmed", "finalized"
};

/**
 * @brief Blockhash information with validity window
 */
struct blockhash_info {
   std::string blockhash;
   uint64_t last_valid_block_height = 0;
};

/**
 * @brief Transaction error information
 */
struct transaction_error {
   std::string error_type;
   std::optional<std::string> message;
   std::optional<uint8_t> instruction_index;
};

// Variant type for pubkey or string (for API compatibility)
using pubkey_compat_t = std::variant<pubkey, std::string>;

/**
 * @brief Convert pubkey_compat_t to pubkey
 */
pubkey to_pubkey(const pubkey_compat_t& pk);

/**
 * @brief Compact-u16 encoding/decoding helpers
 * Solana uses compact-u16 for array lengths in serialized data
 */
namespace compact_u16 {
   /**
    * @brief Encode a value as compact-u16
    */
   std::vector<uint8_t> encode(uint16_t value);

   /**
    * @brief Decode a compact-u16 value, returns bytes consumed
    */
   std::pair<uint16_t, size_t> decode(const uint8_t* data, size_t len);
}  // namespace compact_u16

}  // namespace fc::network::solana

// Reflection macros for serialization
FC_REFLECT_ENUM(fc::network::solana::commitment_t, (processed)(confirmed)(finalized))
FC_REFLECT(fc::network::solana::pubkey, (data))
FC_REFLECT(fc::network::solana::signature, (data))
FC_REFLECT(fc::network::solana::account_meta, (key)(is_signer)(is_writable))
FC_REFLECT(fc::network::solana::instruction, (program_id)(accounts)(data))
FC_REFLECT(fc::network::solana::compiled_instruction, (program_id_index)(account_indices)(data))
FC_REFLECT(fc::network::solana::message_header,
           (num_required_signatures)(num_readonly_signed_accounts)(num_readonly_unsigned_accounts))
FC_REFLECT(fc::network::solana::message, (header)(account_keys)(recent_blockhash)(instructions))
FC_REFLECT(fc::network::solana::address_lookup_table, (key)(writable_indices)(readonly_indices))
FC_REFLECT(fc::network::solana::versioned_message,
           (version)(header)(static_account_keys)(recent_blockhash)(instructions)(address_table_lookups))
FC_REFLECT(fc::network::solana::transaction, (signatures)(msg))
FC_REFLECT(fc::network::solana::versioned_transaction, (signatures)(msg))
FC_REFLECT(fc::network::solana::account_info, (lamports)(owner)(data)(executable)(rent_epoch))
FC_REFLECT(fc::network::solana::rpc_context, (slot))
FC_REFLECT(fc::network::solana::signature_status, (slot)(confirmations)(err)(confirmation_status))
FC_REFLECT(fc::network::solana::blockhash_info, (blockhash)(last_valid_block_height))
FC_REFLECT(fc::network::solana::transaction_error, (error_type)(message)(instruction_index))
