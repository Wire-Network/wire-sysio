#include <fc/log/logger.hpp>
#include <iostream>
#include <sysio/outpost_client/ethereum/rlp_encoder.hpp>

namespace sysio::outpost_client::ethereum
{

void rlp::append(bytes& out, const rlp_input_data& in_var) {
   if (std::holds_alternative<bytes>(in_var)) {
      auto in = std::get<bytes>(in_var);
      out.insert(out.end(), in.begin(), in.end());
   } else if (std::holds_alternative<bytes32>(in_var)) {
      auto in = std::get<bytes32>(in_var);
      out.insert(out.end(), in.begin(), in.end());
   } else {
      auto in = std::get<std::span<const std::uint8_t>>(in_var);
      out.insert(out.end(), in.begin(), in.end());
   }

}

void rlp::append(bytes& out, std::uint8_t b) {
   out.push_back(b);
}

bytes rlp::encode_length(std::size_t len) {
   bytes out;
   bool  started = false;

   for (int shift = (sizeof(std::size_t) - 1) * 8; shift >= 0; shift -= 8) {
      std::uint8_t byte = static_cast<std::uint8_t>((len >> shift) & 0xff);
      if (byte == 0 && !started) continue;
      started = true;
      out.push_back(byte);
   }

   if (out.empty()) {
      out.push_back(0);
   }

   return out;
}

bytes rlp::encode_bytes(std::span<const std::uint8_t> data) {
   const std::size_t len = data.size();
   bytes             out;

   if (len == 1 && data[0] < 0x80) {
      out.push_back(data[0]);
      return out;
   }

   if (len <= 55) {
      out.push_back(static_cast<std::uint8_t>(0x80 + len));
      append(out, data);
   } else {
      bytes len_enc = encode_length(len);
      out.push_back(static_cast<std::uint8_t>(0xb7 + len_enc.size()));
      append(out, len_enc);
      append(out, data);
   }

   return out;
}

bytes rlp::encode_bytes(const bytes& b) {
   return encode_bytes(std::span<const std::uint8_t>(b.data(), b.size()));
}

bytes rlp::encode_string(const std::string& s) {
   return encode_bytes(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(s.data()), s.size()));
}

bytes rlp::encode_list(const std::vector<rlp_input_data>& items) {
   bytes payload;
   for (auto &item : items) {
      append(payload, item);
   }

   const std::size_t len = payload.size();
   bytes             out;

   if (len <= 55) {
      out.push_back(static_cast<std::uint8_t>(0xc0 + len));
      append(out, payload);
   } else {
      bytes len_enc = encode_length(len);
      out.push_back(static_cast<std::uint8_t>(0xf7 + len_enc.size()));
      append(out, len_enc);
      append(out, payload);
   }

   return out;
}

std::string rlp::to_hex_prefixed(const bytes& b) {
   std::ostringstream oss;
   oss << "0x";
   for (auto v : b) {
      oss << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<int>(v);
   }
   return oss.str();
}

bytes rlp::from_hex_noprefix(const std::string& hex) {
   bytes out;
   if (hex.size() % 2 != 0) return out;
   out.reserve(hex.size() / 2);

   auto hex_val = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
      return -1;
   };

   for (std::size_t i = 0; i < hex.size(); i += 2) {
      int hi = hex_val(hex[i]);
      int lo = hex_val(hex[i + 1]);
      if (hi < 0 || lo < 0) return {};
      out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
   }
   return out;
}

bytes rlp::from_hex_any(const std::string& hex) {
   if (hex.starts_with("0x") || hex.starts_with("0X")) {
      return from_hex_noprefix(hex.substr(2));
   }
   return from_hex_noprefix(hex);
}

bytes rlp::encode_eip1559_unsigned(const eip1559_tx& tx) {
   bytes to_bytes   = tx.to; // size 0 or 20
   bytes data_bytes = tx.data;

   bytes access_list_rlp = rlp::encode_list({}); // empty list => 0xc0

   // TODO: Encode `tx.access_list`
   return rlp::encode_list({
      rlp::encode_uint(tx.chain_id),
     rlp::encode_uint(tx.nonce),
     rlp::encode_uint(tx.max_priority_fee_per_gas),
     rlp::encode_uint(tx.max_fee_per_gas),
     rlp::encode_uint(tx.gas_limit),
     rlp::encode_bytes(to_bytes),
     rlp::encode_uint(tx.value),
     rlp::encode_bytes(data_bytes),
     access_list_rlp
   });
}

bytes rlp::encode_eip1559_signed(const eip1559_tx& tx, std::uint8_t y_parity, std::span<const std::uint8_t, 32> r,
                                 std::span<const std::uint8_t, 32> s) {
   bytes to_bytes   = tx.to;
   bytes data_bytes = tx.data;

   // TODO: Encode `tx.access_list`
   bytes access_list_rlp = rlp::encode_list({});

   // r/s are raw 32-byte big-endian scalars
   bytes r_bytes(r.begin(), r.end());
   bytes s_bytes(s.begin(), s.end());

   bytes y_parity_bytes = rlp::encode_uint(static_cast<std::uint64_t>(y_parity));

   return rlp::encode_list({
      rlp::encode_uint(tx.chain_id),
      rlp::encode_uint(tx.nonce),
      rlp::encode_uint(tx.max_priority_fee_per_gas),
      rlp::encode_uint(tx.max_fee_per_gas),
      rlp::encode_uint(tx.gas_limit),
      rlp::encode_bytes(to_bytes),
      rlp::encode_uint(tx.value),
      rlp::encode_bytes(data_bytes),
      access_list_rlp,
      y_parity_bytes,
      rlp::encode_bytes(r_bytes),
      rlp::encode_bytes(s_bytes)
   });
}

bytes rlp::encode_eip1559_signed_typed(const eip1559_tx& tx, std::uint8_t y_parity, std::span<const std::uint8_t, 32> r,
                                       std::span<const std::uint8_t, 32> s) {
   bytes body = encode_eip1559_signed(tx, y_parity, r, s);
   bytes out;
   out.reserve(1 + body.size());
   out.push_back(0x02);
   out.insert(out.end(), body.begin(), body.end());
   return out;
}

std::string rlp::build_eth_send_raw_transaction_json(const bytes& signed_tx, std::uint32_t id) {
   std::string hex = rlp::to_hex_prefixed(signed_tx);

   std::ostringstream oss;
   oss << R"({"jsonrpc":"2.0","id":)"
      << id
      << R"(,"method":"eth_sendRawTransaction","params":[")"
      << hex
      << R"("]})";
   return oss.str();
}
}