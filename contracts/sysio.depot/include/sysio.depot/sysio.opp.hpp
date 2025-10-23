#pragma once

#include <sysio/asset.hpp>

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
