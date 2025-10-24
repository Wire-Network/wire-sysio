
#include <sysio.depot/sysio.opp.hpp>

// This contract is intentionally left as a stub with no actions.
namespace sysio::opp {

namespace {
template <typename T, std::size_t S = sizeof(T)>
bool write_to_stream(char* ds, T value) {
   return !!std::memcpy(ds, reinterpret_cast<char*>(&value), S);
}


} // namespace

void message_balance_sheet::unpack(const uint8_t* data, std::size_t size) {
   // SYS_ASSERT(size >= (sizeof(uint8_t) * 2) /* chain_kind(1) & asset_count(1) */, chain::action_validate_exception,
   //            "Invalid message size");
   chain                      = static_cast<chain_kind>(data[0]);
   uint8_t        asset_count = data[1];
   const uint8_t* assets_data = data + (sizeof(chain) + sizeof(asset_count));
   assets.reserve(asset_count);

   for (std::size_t i = 0; i < asset_count; i++) {
      const uint8_t* asset_data = assets_data + (i * asset_size);
      int64_t        amount     = *reinterpret_cast<const int64_t*>(asset_data);
      uint64_t       sym_raw    = *reinterpret_cast<const uint64_t*>(asset_data);
      symbol         sym(sym_raw);

      assets.emplace_back(amount, sym);
   }
}

std::vector<uint8_t>& message_balance_sheet::pack(std::vector<uint8_t>& bytes) {
   auto asset_count = static_cast<uint8_t>(assets.size());
   auto buf_size    = sizeof(chain) + sizeof(asset_count) + (sizeof(asset) * asset_count);
   auto buf         = std::vector<char>(buf_size);

   // fc::datastream<char*> ds(buf.data(), buf_size);
   auto buf_ptr = buf.data();
   write_to_stream<uint8_t>(buf_ptr, chain);
   write_to_stream<uint8_t>(buf_ptr + 1, asset_count);

   for (auto& asset : assets) {
      auto buf_asset_ptr = buf_ptr + (sizeof(chain) + sizeof(asset_count)) + (asset_count * asset_size);
      write_to_stream<int64_t>(buf_asset_ptr, asset.amount);
      write_to_stream<uint64_t>(buf_asset_ptr + sizeof(int64_t), asset.symbol.code().raw());
   }

   std::copy(buf.begin(), buf.end(), std::back_inserter(bytes));

   return bytes;
}

void                  message_swap::unpack(const uint8_t* data, std::size_t size) {}
std::vector<uint8_t>& message_swap::pack(std::vector<uint8_t>& bytes) {
   return bytes;
}
void                  message_operator_registration::unpack(const uint8_t* data, std::size_t size) {}
std::vector<uint8_t>& message_operator_registration::pack(std::vector<uint8_t>& bytes) {
   return bytes;
}
void                  message_operator_deregistration::unpack(const uint8_t* data, std::size_t size) {}
std::vector<uint8_t>& message_operator_deregistration::pack(std::vector<uint8_t>& bytes) {
   return bytes;
}
void                  message_purchase::unpack(const uint8_t* data, std::size_t size) {}
std::vector<uint8_t>& message_purchase::pack(std::vector<uint8_t>& bytes) {
   return bytes;
}
void                  message_stake::unpack(const uint8_t* data, std::size_t size) {}
std::vector<uint8_t>& message_stake::pack(std::vector<uint8_t>& bytes) {
   return bytes;
}
void                  message_unstake::unpack(const uint8_t* data, std::size_t size) {}
std::vector<uint8_t>& message_unstake::pack(std::vector<uint8_t>& bytes) {
   return bytes;
}



message_chain_payload::message_data_header::message_data_header(const uint8_t* payload_data) {
   check(payload_data != nullptr, "message_data_header data is nullptr");

   type   = static_cast<enum message_type>(*payload_data);
   length = static_cast<uint16_t>(*(payload_data + sizeof(enum message_type)));
}

message_chain_payload::message_data::message_data(const uint8_t* payload_data)
   : header(payload_data)
   , payload_data(payload_data)
   , data(payload_data + sizeof(message_data_header)) {
   check(data != nullptr, "message_data data is nullptr");
}

message_chain_payload::message_chain_payload(const message_chain_payload_header* header, const uint8_t* data,
                                             std::size_t size)
   : header(header)
   , data(data)
   , size(size) {
   check(data != nullptr, "message_chain_payload data is nullptr");

   // messages.resize(header->message_count);
   std::size_t i   = 0;
   std::size_t pos = 0;

   while (i < message_count() && pos < size - sizeof(message_data_header)) {
      auto& mdh = _message_datas.emplace_back(data + pos);

      pos += mdh.payload_size();
      i++;
   }

   check(pos == size, "message_chain_payload data size != read size");
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
   check(data != nullptr, "message_chain data is nullptr");
}



} // namespace sysio::opp
