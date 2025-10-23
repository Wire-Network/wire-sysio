

#include <sysio.depot/sysio.opp.hpp>

// This contract is intentionally left as a stub with no actions.
namespace sysio::opp {
message_chain_payload::message_data_header::message_data_header(
    const uint8_t *payload_data) {
  check(payload_data != nullptr, "message_data_header data is nullptr");

  type = static_cast<enum message_type>(*payload_data);
  length = static_cast<uint16_t>(*(payload_data + sizeof(enum message_type)));
}

message_chain_payload::message_data::message_data(const uint8_t *payload_data)
    : header(payload_data), payload_data(payload_data),
      data(payload_data + sizeof(message_data_header)) {
  check(data != nullptr, "message_data data is nullptr");
}

message_chain_payload::message_chain_payload(
    const message_chain_payload_header *header, const uint8_t *data,
    std::size_t size)
    : header(header), data(data), size(size) {
  check(data != nullptr, "message_chain_payload data is nullptr");

  // messages.resize(header->message_count);
  std::size_t i = 0;
  std::size_t pos = 0;

  while (i < message_count() && pos < size - sizeof(message_data_header)) {
    auto &mdh = _message_datas.emplace_back(data + pos);

    pos += mdh.payload_size();
    i++;
  }

  check(pos == size, "message_chain_payload data size != read size");
}
uint16_t message_chain_payload::message_count() const {
  return header->message_count;
}
std::optional<message_chain_payload::message_data>
message_chain_payload::get_message_data(std::size_t idx) {
  if (idx < _message_datas.size()) {
    return _message_datas[idx];
  }
  return std::nullopt;
}
std::optional<message_chain_payload::message_data_header>
message_chain_payload::get_message_data_header(std::size_t idx) {
  if (idx < _message_datas.size()) {
    return _message_datas[idx].header;
  }
  return std::nullopt;
}
std::optional<message_type>
message_chain_payload::get_message_type(std::size_t idx) const {
  if (idx < _message_datas.size()) {
    return _message_datas[idx].header.type;
  }
  return std::nullopt;
}

message_chain::message_chain(const std::vector<uint8_t> &bytes)
    : bytes(bytes), data(bytes.data()),
      chain_header(reinterpret_cast<const message_chain_header *>(data)),
      payload_header(reinterpret_cast<const message_chain_payload_header *>(
          data + sizeof(message_chain_header))),
      payload(payload_header,
              data + sizeof(message_chain_header) +
                  sizeof(message_chain_payload_header),
              payload_header->total_length) {
  check(data != nullptr, "message_chain data is nullptr");
}
} // namespace sysio::opp
