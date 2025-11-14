
#include <sysio.depot/sysio.opp.hpp>

// This contract is intentionally left as a stub with no actions.
namespace sysio::opp {

namespace {
template <typename T>
META_DATASTREAM* pack_to_stream(META_DATASTREAM* ds, const std::shared_ptr<T>& storage_ptr) {
#ifdef NO_WASM
   fc::raw::pack(*ds, storage_ptr);
#else
   *ds << *storage_ptr.get();
#endif
   return ds;
}

template <typename T>
bool unpack_from_stream(META_DATASTREAM* ds, const std::shared_ptr<T>& storage_ptr) {
#ifdef NO_WASM
   fc::raw::unpack(*ds, storage_ptr);
#else
   *ds >> *storage_ptr.get();
#endif
   return true;
}

} // namespace


bool message_balance_sheet::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_balance_sheet::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

bool message_swap::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_swap::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

bool message_operator_registration::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_operator_registration::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

bool message_operator_deregistration::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_operator_deregistration::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

bool message_purchase::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_purchase::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

bool message_stake::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_stake::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

bool message_unstake::unpack(META_DATASTREAM* ds) {
   return unpack_from_stream(ds, shared_from_this());
}

META_DATASTREAM* message_unstake::pack(META_DATASTREAM* ds) {
   return pack_to_stream(ds, shared_from_this());
}

message_base::message_base()
   : type(message_type_unknown) {}
bool message_base::unpack(META_DATASTREAM* ds) {
   return !!ds;
}
META_DATASTREAM* message_base::pack(META_DATASTREAM* ds) {
   return ds;
}
bool message_unknown::unpack(META_DATASTREAM* ds) {
   return true;
}

META_DATASTREAM* message_unknown::pack(META_DATASTREAM* ds) {
   return ds;
}

message_chain_payload::message_data_header::message_data_header(const uint8_t* payload_data) {
   // check(payload_data != nullptr, "message_data_header data is nullptr");

   type   = static_cast<enum message_type>(*payload_data);
   length = static_cast<uint16_t>(*(payload_data + sizeof(enum message_type)));
}

message_chain_payload::message_data::message_data(const uint8_t* payload_data)
   : header(payload_data)
   , payload_data(payload_data)
   , data(payload_data + sizeof(message_data_header)) {
   // check(data != nullptr, "message_data data is nullptr");
}

message_chain_payload::message_chain_payload(const message_chain_payload_header* header, const uint8_t* data,
                                             std::size_t size)
   : header(header)
   , data(data)
   , size(size) {
   // check(data != nullptr, "message_chain_payload data is nullptr");

   std::size_t i   = 0;
   std::size_t pos = 0;

   while (i < message_count() && pos < size - sizeof(message_data_header)) {
      auto& mdh = _message_datas.emplace_back(data + pos);

      pos += mdh.payload_size();
      i++;
   }

   // check(pos == size, "message_chain_payload data size != read size");
}

uint16_t message_chain_payload::message_count() const {
   return header->message_count;
}

std::optional<message_chain_payload::message_data> message_chain_payload::get_message_data(std::size_t idx) {
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

std::optional<message_type> message_chain_payload::get_message_type(std::size_t idx) const {
   if (idx < _message_datas.size()) {
      return _message_datas[idx].header.type;
   }
   return std::nullopt;
}

message_chain::message_chain(const std::vector<uint8_t>& bytes)
   : bytes(bytes)
   , data(bytes.data())
   , chain_header(reinterpret_cast<const message_chain_header*>(data))
   , payload_header(reinterpret_cast<const message_chain_payload_header*>(data + sizeof(message_chain_header)))
   , payload(payload_header, data + sizeof(message_chain_header) + sizeof(message_chain_payload_header),
             payload_header->total_length) {
   // check(data != nullptr, "message_chain data is nullptr");
}

} // namespace sysio::opp
