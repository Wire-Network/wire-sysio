#pragma once

#include <fc-lite/lut.hpp>
#include <fc-lite/tuples.hpp>
#include <sysio.system/non_wasm_types.hpp>
#include <tuple>
#include <variant>

#if defined(NO_WASM) && !defined(WASMTIME)
   #include <fc/crypto/public_key.hpp>
   #include <fc/io/datastream.hpp>
   #include <fc/reflect/reflect.hpp>
   #include <fc/utility.hpp>
   #include <sysio/chain/asset.hpp>
   #define META_REFLECT(...) FC_REFLECT(__VA_ARGS__)
   #define META_REFLECT_ENUM(...) FC_REFLECT_ENUM(__VA_ARGS__)
   #define META_REFLECT_TEMPLATE(...) FC_REFLECT_TEMPLATE(__VA_ARGS__)
   #define META_REFLECT_DERIVED(...) FC_REFLECT_DERIVED(__VA_ARGS__)
   #define META_REFLECT_DERIVED_EMPTY(...) FC_REFLECT_DERIVED_EMPTY(__VA_ARGS__)
   #define META_REFLECT_DERIVED_TEMPLATE(...) FC_REFLECT_DERIVED_TEMPLATE(__VA_ARGS__)

   #define META_DATASTREAM fc::datastream<char*>

   #define PUBLIC_KEY_TYPE fc::crypto::public_key
   #define NAME_TYPE sysio::chain::name
   #define ASSET_TYPE sysio::chain::asset
#else
   #include <sysio/asset.hpp>
   #include <sysio/crypto.hpp>
   #include <sysio/fixed_bytes.hpp>
   #include <sysio/serialize.hpp>
   #include <sysio/sysio.hpp>
   #define META_REFLECT(...) SYSLIB_SERIALIZE(__VA_ARGS__)
   #define META_REFLECT_ENUM(...) SYSLIB_REFLECT_ENUM(__VA_ARGS__)
   #define META_REFLECT_TEMPLATE(...) SYSLIB_SERIALIZE_TEMPLATE(__VA_ARGS__)
   #define META_REFLECT_DERIVED(...) SYSLIB_SERIALIZE_DERIVED(__VA_ARGS__)
   #define META_REFLECT_DERIVED_EMPTY(...) SYSLIB_SERIALIZE_DERIVED_EMPTY(__VA_ARGS__)
   #define META_REFLECT_DERIVED_TEMPLATE(...) SYSLIB_SERIALIZE_DERIVED_TEMPLATE(__VA_ARGS__)

   #define META_DATASTREAM sysio::datastream<char*>

   #define PUBLIC_KEY_TYPE sysio::public_key
   #define NAME_TYPE sysio::name
   #define ASSET_TYPE sysio::asset
#endif

#define META_REFLECT_NIL(x) (x)

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
   message_type_unstake,                ///< Un-staking operation
   message_type_balance_sheet,          ///< Balance sheet update
   message_type_swap,                   ///< Token swap operation
   message_type_operator_registration,  ///< New operator registration
   message_type_operator_deregistration ///< Operator deregistration
#ifdef WASM
// , META_REFLECT_ENUM(sysio::opp::message_type,
// (message_type_unknown)(message_type_purchase)(message_type_stake)(message_type_unstake)(message_type_balance_sheet)(message_type_swap)(message_type_operator_registration)(message_type_operator_deregistration)
// );
#endif

};



/**
 * Abstract message class defining pack & unpack
 *
 * @tparam Type The message type
 * @brief Base structure for all message types
 */

// struct message_base {
//    message_type type;
//    message_base()
//       : type(message_type_unknown) {};
//    explicit message_base(message_type type)
//       : type(type) {}
//    ~message_base() = default;
//
//    bool             unpack(META_DATASTREAM* ds);
//    META_DATASTREAM* pack(META_DATASTREAM* ds);
// #ifdef WASM
//    META_REFLECT(sysio::opp::message_base, (type));
// #endif
// };

// META_REFLECT( sysio::opp::message_base, (type) );

/**
 * @brief Message structures for each message type
 */
struct message_unknown { // :  std::enable_shared_from_this<message_unknown>
   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);

   message_type type; // {message_type_unknown};

#ifdef WASM
   META_REFLECT(sysio::opp::message_unknown, (type));
#endif
};

constexpr auto is_message_unknown_standard = std::is_standard_layout_v<message_unknown>;

/**
 * TOKEN PURCHASE
 */
struct message_purchase : std::enable_shared_from_this<message_purchase> {
   ASSET_TYPE amount{};
   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);
   message_purchase() {};
   message_type type   = message_type_purchase;
   ~message_purchase() = default;
#ifdef WASM
   META_REFLECT(sysio::opp::message_purchase, (type)(amount));
#endif
};

/**
 * STAKE
 */
struct message_stake : std::enable_shared_from_this<message_stake> {
   ASSET_TYPE amount{};

   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);
   message_stake() {};
   message_type type = message_type_stake;
   ~message_stake()  = default;
#ifdef WASM
   META_REFLECT(sysio::opp::message_stake, (type)(amount));
#endif
};

struct message_unstake : std::enable_shared_from_this<message_unstake> {
   ASSET_TYPE amount{};

   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);
   ~message_unstake() = default;
   message_unstake() {};
   message_type type = message_type_unstake;
#ifdef WASM
   META_REFLECT(sysio::opp::message_unstake, (type)(amount));
#endif
};

struct message_balance_sheet : std::enable_shared_from_this<message_balance_sheet> {
   constexpr static auto asset_size = sizeof(ASSET_TYPE);
   static_assert(asset_size == sizeof(uint128_t), "Asset size is not 16 bytes");

   chain_kind              chain{chain_unknown};
   std::vector<ASSET_TYPE> assets{};

   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);
   message_balance_sheet() {};
   message_type type        = message_type_balance_sheet;
   ~message_balance_sheet() = default;
#ifdef WASM
   META_REFLECT(sysio::opp::message_balance_sheet, (type)(chain)(assets));
#endif
};

struct message_swap : std::enable_shared_from_this<message_swap> {
   chain_kind source_chain{chain_unknown};
   ASSET_TYPE source_amount{};

   uint64_t divisor{};

   chain_kind target_chain{chain_unknown};
   ASSET_TYPE target_amount{};

   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);
   ~message_swap() = default;
   message_swap() {};
   message_type type = message_type_swap;
#ifdef WASM
   META_REFLECT(sysio::opp::message_swap, (type)(source_chain)(source_amount)(divisor)(target_chain)(target_amount));
#endif
};

struct message_operator_registration : std::enable_shared_from_this<message_operator_registration> {
   NAME_TYPE       operator_account{};
   PUBLIC_KEY_TYPE operator_key{};
   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);

   message_operator_registration() {};
   message_type type                = message_type_operator_registration;
   ~message_operator_registration() = default;
#ifdef WASM
   META_REFLECT(sysio::opp::message_operator_registration, (type)(operator_account)(operator_key));
#endif
};

struct message_operator_deregistration : std::enable_shared_from_this<message_operator_deregistration> {
   NAME_TYPE       operator_account{};
   PUBLIC_KEY_TYPE operator_key{};

   // bool             unpack(META_DATASTREAM* ds);
   // META_DATASTREAM* pack(META_DATASTREAM* ds);
   message_operator_deregistration() {};
   message_type type                  = message_type_operator_deregistration;
   ~message_operator_deregistration() = default;
#ifdef WASM
   META_REFLECT(sysio::opp::message_operator_deregistration, (type)(operator_account)(operator_key));
#endif
};

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

#if defined(NO_WASM) && !defined(WASMTIME)
META_REFLECT_ENUM(
   sysio::opp::message_type,
   (message_type_unknown)(message_type_purchase)(message_type_stake)(message_type_unstake)(message_type_balance_sheet)(message_type_swap)(message_type_operator_registration)(message_type_operator_deregistration))

// META_REFLECT(sysio::opp::message_base, (type));

META_REFLECT(sysio::opp::message_unknown, (type));
META_REFLECT(sysio::opp::message_purchase, (type)(amount));
META_REFLECT(sysio::opp::message_stake, (type)(amount));
META_REFLECT(sysio::opp::message_unstake, (type)(amount));
META_REFLECT(sysio::opp::message_balance_sheet, (type)(chain)(assets));
META_REFLECT(sysio::opp::message_swap, (type)(source_chain)(source_amount)(divisor)(target_chain)(target_amount));
META_REFLECT(sysio::opp::message_operator_registration, (type)(operator_account)(operator_key));
META_REFLECT(sysio::opp::message_operator_deregistration, (type)(operator_account)(operator_key));
#endif