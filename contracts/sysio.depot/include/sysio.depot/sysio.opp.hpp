#pragma once

#include <tuple>
#include <variant>

#include <fc/tuples.hpp>
#include <fc/lut.hpp>
#include <sysio/asset.hpp>
#include <sysio.system/non_wasm_types.hpp>

namespace sysiosystem {
class system_contract;
}

namespace sysio {

enum chain_kind : uint8_t {
   chain_unknown       = 0,
   chain_kind_wire     = 1,
   chain_kind_ethereum = 2,
   chain_kind_solana   = 3,
   chain_kind_sui      = 4,
   chain_kind_count    = 5,
};

enum chain_key_type : uint8_t {
   chain_key_type_unknown  = 0,
   chain_key_type_wire     = 1,
   chain_key_type_ethereum = 2,
   chain_key_type_solana   = 3,
   chain_key_type_sui      = 4,
   chain_key_type_count    = 5,
};

// SIZES INCLUDE SCHEME FLAG
constexpr std::size_t key_size_ed25519  = 33;
constexpr std::size_t key_size_sec256k1 = 34;
constexpr std::size_t key_size_sec256r1 = key_size_sec256k1;

constexpr std::size_t chain_address_size_sui      = 32; // base 64 (Blake2b256(pubkey))
constexpr std::size_t chain_address_size_solana   = 32; // Base 58 (pubkey direct)
constexpr std::size_t chain_address_size_ethereum = 20; // base 64 (Keccak256(pubkey[12:]))
constexpr std::size_t chain_address_size_wire     = key_size_sec256k1;


// fc::lut<chain_kind, std::pair<chain_key_type, std::size_t>, chain_kind_count>
constexpr auto chain_public_key_types = std::tuple{
   std::pair{chain_unknown,       std::pair{chain_key_type_unknown, std::size_t{0}}    },
   std::pair{chain_kind_wire,     std::pair{chain_key_type_wire, key_size_sec256k1}    },
   std::pair{chain_kind_ethereum, std::pair{chain_key_type_ethereum, key_size_sec256k1}},
   std::pair{chain_kind_solana,   std::pair{chain_key_type_solana, key_size_ed25519}   },
   std::pair{chain_kind_sui,      std::pair{chain_key_type_sui, key_size_ed25519}      },
};

// fc::lut<chain_kind, std::pair<chain_key_type, std::size_t>, chain_kind_count>
constexpr auto chain_address_sizes = std::tuple{
   std::pair{chain_unknown,       std::pair{chain_key_type_unknown, chain_key_type_unknown}  },
   std::pair{chain_kind_wire,     std::pair{chain_key_type_wire, chain_key_type_wire}        },
   std::pair{chain_kind_ethereum, std::pair{chain_key_type_ethereum, chain_key_type_ethereum}},
   std::pair{chain_kind_solana,   std::pair{chain_key_type_solana, chain_key_type_solana}    },
   std::pair{chain_kind_sui,      std::pair{chain_key_type_sui, chain_key_type_sui}          },
};


enum swap_status_type : uint8_t {
   swap_status_unknown                      = 0u,
   swap_status_created_on_outpost           = 1,
   swap_status_received_on_wire             = 2,
   swap_status_validated_withdraw_scheduled = 3,
   swap_status_withdrawal_sent_to_outpost   = 4,
};

/**
 * @brief Operator Propagation Protocol (OPP) namespace
 * @details Contains types and utilities for processing operator messages
 */
namespace opp {
/**
 * @brief Types of messages supported in the Operator Propagation Protocol
 */
enum message_type : uint8_t {
   message_type_unknown = 0,            ///< Unknown message type
   message_type_purchase,               ///< Purchase transaction
   message_type_stake,                  ///< Staking operation
   message_type_unstake,                ///< Unstaking operation
   message_type_balance_sheet,          ///< Balance sheet update
   message_type_swap,                   ///< Token swap operation
   message_type_operator_registration,  ///< New operator registration
   message_type_operator_deregistration ///< Operator deregistration
                                        // message_type_challenge_agree,         ///< Challenge agreement
                                        // message_type_challenge_reject,        ///< Challenge rejection
                                        // message_type_no_challenge             ///< No challenge present
};


struct message_metadata {
   const message_type type;
   explicit message_metadata(message_type type)
      : type(type) {};

   virtual ~message_metadata() = default;
};

/**
 * Abstract message class defining pack & unpack
 *
 * @tparam Type The message type
 * @brief Base structure for all message types
 */
template <message_type Type>
struct message_base : message_metadata {
   explicit message_base()
      : message_metadata(Type) {};
   virtual ~message_base() = default;

   virtual void                  unpack(const uint8_t* data, std::size_t size) = 0;
   virtual std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes)             = 0;
};

/**
 * @brief Message structures for each message type
 */
struct message_unknown : message_base<message_type_unknown> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_unknown() = default;
};

/**
 * TOKEN PURCHASE
 */
struct message_purchase : message_base<message_type_purchase> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_purchase() = default;
};

/**
 * STAKE
 */
struct message_stake : message_base<message_type_stake> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_stake() = default;
};

struct message_unstake : message_base<message_type_unstake> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_unstake() = default;
};

struct message_balance_sheet : message_base<message_type_balance_sheet> {
   constexpr static auto asset_size = sizeof(asset);
   static_assert(asset_size == sizeof(uint128_t), "Asset size is not 16 bytes");

   chain_kind         chain{chain_unknown};
   std::vector<asset> assets{};

   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_balance_sheet() = default;
};

struct message_swap : message_base<message_type_swap> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_swap() = default;
};

struct message_operator_registration : message_base<message_type_operator_registration> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_operator_registration() = default;
};

struct message_operator_deregistration : message_base<message_type_operator_deregistration> {
   void                  unpack(const uint8_t* data, std::size_t size) override;
   std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
   virtual ~message_operator_deregistration() = default;
};

// struct message_challenge_agree : message_base<message_type_challenge_agree> {
//    void                  unpack(const uint8_t* data, std::size_t size) override;
//    std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
//    virtual ~message_challenge_agree() = default;
// };
//
// struct message_challenge_reject : message_base<message_type_challenge_reject> {
//    void                  unpack(const uint8_t* data, std::size_t size) override;
//    std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
//    virtual ~message_challenge_reject() = default;
// };
//
// struct message_no_challenge : message_base<message_type_no_challenge> {
//    void                  unpack(const uint8_t* data, std::size_t size) override;
//    std::vector<uint8_t>& pack(std::vector<uint8_t>& bytes) override;
//    virtual ~message_no_challenge() = default;
// };
/**
 * @brief Mapping of message types to their corresponding structures
 */
constexpr auto message_type_mapping = std::tuple{
   std::pair{message_type_unknown,                 fc::type_tag<message_unknown>{}                },
   std::pair{message_type_purchase,                fc::type_tag<message_purchase>{}               },
   std::pair{message_type_stake,                   fc::type_tag<message_stake>{}                  },
   std::pair{message_type_unstake,                 fc::type_tag<message_unstake>{}                },
   std::pair{message_type_balance_sheet,           fc::type_tag<message_balance_sheet>{}          },
   std::pair{message_type_swap,                    fc::type_tag<message_swap>{}                   },
   std::pair{message_type_operator_registration,   fc::type_tag<message_operator_registration>{}  },
   std::pair{message_type_operator_deregistration, fc::type_tag<message_operator_deregistration>{}}
};

using message_variant_type = fc::tuple_pairs_to_variant_t<decltype(message_type_mapping)>;


template <message_type Type>
using mapped_t = decltype(fc::find_mapped_type<message_type_mapping, message_type, Type>());

template <message_type Type>
constexpr auto create_message() {
   return mapped_t<Type>();
}
/**
 * @brief Header for a chain of operator protocol messages
 * @details Contains message identification and verification data
 */
struct message_chain_header {
   uint32_t current_message_id;
   uint32_t previous_message_id;
   uint32_t payload_checksum;
   uint64_t timestamp;
};

struct message_chain_payload_header {
   uint8_t  protocol_version;
   uint8_t  encoding_flags;
   uint16_t total_length;
   uint16_t message_count;
   uint32_t payload_checksum;
   uint64_t timestamp;
};

/**
 * @brief Represents the payload of a message chain
 * @details Contains individual messages and their metadata
 */
class message_chain_payload {

public:
   /**
    * Message data header
    */
   struct message_data_header {
      message_type type;
      uint16_t     length;

      explicit message_data_header(const uint8_t* payload_data);
   };

   struct message_data {
      message_data_header header;

      const uint8_t* payload_data;
      const uint8_t* data;

      explicit message_data(const uint8_t* payload_data);

      template <typename T>
      T* get_message() {
         return reinterpret_cast<T*>(data);
      }
      std::size_t payload_size() { return header.length + sizeof(message_data_header); }

      std::size_t message_size() { return header.length; }
   };

private:
   std::vector<message_data> _message_datas{};

public:
   const message_chain_payload_header* header;
   const uint8_t*                      data;
   const std::size_t                   size;

   /**
    * message_chain_payload constructor
    *
    * @param header pointer to payload header data
    * @param data Payload data, exclusive of payload header
    * @param size Size of payload data, exclusive of payload header
    */
   explicit message_chain_payload(const message_chain_payload_header* header, const uint8_t* data, std::size_t size);

   uint16_t message_count() const;

   std::optional<message_data>        get_message_data(std::size_t idx);
   std::optional<message_data_header> get_message_data_header(std::size_t idx);
   std::optional<message_type>        get_message_type(std::size_t idx) const;

   /**
    * Get message of type at index
    *
    * @tparam T type relative to message type, this can be implemented via an LUT
    * in the future
    * @param idx Index of the desired message
    * @return Optional `T*` pointer to the message data if found, otherwise
    * `std::nullopt`
    */
   template <typename T>
   std::optional<T*> get_message(std::size_t idx) {
      if (idx < _message_datas.size()) {
         return _message_datas[idx].get_message<T>();
      }
      return std::nullopt;
   }
};

/**
 * @brief Represents a complete chain of operator protocol messages
 *
 * Message chain representation (OPP):
 * https://wire-network.atlassian.net/wiki/spaces/WNC/pages/19005500/Outpost+Propagation+Protocol
 *
 * @details Container for protocol message data including headers and payload
 */
class message_chain {
   const std::vector<uint8_t> bytes;
   const uint8_t*             data;

public:
   explicit message_chain(const std::vector<uint8_t>& bytes);
   const message_chain_header*         chain_header;
   const message_chain_payload_header* payload_header;
   message_chain_payload               payload;
};

} // namespace opp

} // namespace sysio
