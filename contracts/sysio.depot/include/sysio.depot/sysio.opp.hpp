#pragma once

#include <fc/traits.hpp>
#include <sysio/asset.hpp>
#include <tuple>
#include <type_traits>
#include <variant>

#include <sysio.system/non_wasm_types.hpp>

namespace sysiosystem {
class system_contract;
}

namespace sysio {
/**
 * @brief Operator Propagation Protocol (OPP) namespace
 * @details Contains types and utilities for processing operator messages
 */
namespace opp {
/**
 * @brief Types of messages supported in the Operator Propagation Protocol
 */
enum message_type : uint8_t {
  message_type_unknown = 0,             ///< Unknown message type
  message_type_purchase,                ///< Purchase transaction
  message_type_stake,                   ///< Staking operation
  message_type_unstake,                 ///< Unstaking operation
  message_type_balance_sheet,           ///< Balance sheet update
  message_type_swap,                    ///< Token swap operation
  message_type_operator_registration,   ///< New operator registration
  message_type_operator_deregistration, ///< Operator deregistration
  message_type_challenge_agree,         ///< Challenge agreement
  message_type_challenge_reject,        ///< Challenge rejection
  message_type_no_challenge             ///< No challenge present
};

/**
 * @brief Message structures for each message type
 */
struct message_unknown {
  // Placeholder for unknown message data
};

struct message_purchase {
  // Placeholder for purchase transaction data
};

struct message_stake {
  // Placeholder for staking operation data
};

struct message_unstake {
  // Placeholder for unstaking operation data
};

struct message_balance_sheet {
  // Placeholder for balance sheet update data
};

struct message_swap {
  // Placeholder for token swap operation data
};

struct message_operator_registration {
  // Placeholder for operator registration data
};

struct message_operator_deregistration {
  // Placeholder for operator deregistration data
};

struct message_challenge_agree {
  // Placeholder for challenge agreement data
};

struct message_challenge_reject {
  // Placeholder for challenge rejection data
};

struct message_no_challenge {
  // Placeholder for no challenge data
};

/**
 * @brief Mapping of message types to their corresponding structures
 */
constexpr auto message_type_mapping = std::tuple{
    std::pair{message_type_unknown, fc::type_tag<message_unknown>{}},
    std::pair{message_type_purchase, fc::type_tag<message_purchase>{}},
    std::pair{message_type_stake, fc::type_tag<message_stake>{}},
    std::pair{message_type_unstake, fc::type_tag<message_unstake>{}},
    std::pair{message_type_balance_sheet, fc::type_tag<message_balance_sheet>{}},
    std::pair{message_type_swap, fc::type_tag<message_swap>{}},
    std::pair{message_type_operator_registration, fc::type_tag<message_operator_registration>{}},
    std::pair{message_type_operator_deregistration, fc::type_tag<message_operator_deregistration>{}},
    std::pair{message_type_challenge_agree, fc::type_tag<message_challenge_agree>{}},
    std::pair{message_type_challenge_reject, fc::type_tag<message_challenge_reject>{}},
    std::pair{message_type_no_challenge, fc::type_tag<message_no_challenge>{}}
};

using message_variant_type =
    fc::tuple_pairs_to_variant_t<decltype(message_type_mapping)>;


template <message_type Type>
using mapped_t = typename decltype(fc::get_type_tag_by_key<message_type_mapping,Type>(Type))::type;

// template <message_type Type>
// constexpr mapped_t<Type> make_value() {
//     return fc::get_value_by_key<message_type_mapping>(Type);
// }

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
  uint8_t protocol_version;
  uint8_t encoding_flags;
  uint16_t total_length;
  uint16_t message_count;
  uint32_t payload_checksum;
  uint64_t timestamp;
};

class message_chain_payload {

public:
  struct message_data_header {
    message_type type;
    uint16_t length;

    explicit message_data_header(const uint8_t *payload_data);
  };

  struct message_data {
    message_data_header header;

    const uint8_t *payload_data;
    const uint8_t *data;

    explicit message_data(const uint8_t *payload_data);

    template <typename T> T *get_message() {
      return reinterpret_cast<T *>(data);
    }
    std::size_t payload_size() {
      return header.length + sizeof(message_data_header);
    }

    std::size_t message_size() { return header.length; }
  };

private:
  std::vector<message_data> _message_datas{};

public:
  const message_chain_payload_header *header;
  const uint8_t *data;
  const std::size_t size;

  /**
   * message_chain_payload constructor
   *
   * @param header pointer to payload header data
   * @param data Payload data, exclusive of payload header
   * @param size Size of payload data, exclusive of payload header
   */
  explicit message_chain_payload(const message_chain_payload_header *header,
                                 const uint8_t *data, std::size_t size);

  uint16_t message_count() const;

  std::optional<message_data> get_message_data(std::size_t idx);
  std::optional<message_data_header> get_message_data_header(std::size_t idx);
  std::optional<message_type> get_message_type(std::size_t idx) const;

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
  const uint8_t *data;

public:
  explicit message_chain(const std::vector<uint8_t> &bytes);
  const message_chain_header *chain_header;
  const message_chain_payload_header *payload_header;
  message_chain_payload payload;
};

} // namespace opp

} // namespace sysio
