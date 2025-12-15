#include <fc/log/logger.hpp>
#include <ranges>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

namespace fc::network::ethereum::rlp {
namespace {
using namespace fc::crypto;
using namespace fc::crypto::ethereum;

struct rlp_visitor {
   bytes& out;

   rlp_visitor() = delete;
   explicit rlp_visitor(bytes& out) : out(out) {}

   void operator()(auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, rlp_input_data_items>) {
         for (const auto& item : arg) {
            std::visit(*this, item); // Recurse
         }
      } else {
         out.insert(out.end(), arg.begin(), arg.end());
      }
   }
};
}
void append(bytes& out, std::vector<rlp_input_variant>& in_vars) {
   // auto rlp_visitor = [&](this const auto& self, auto&& arg) {
   //    using T = std::decay_t<decltype(arg)>;
   //    if constexpr (std::is_same_v<T, rlp_input_data_items>) {
   //       for (const auto& item : arg) {
   //          std::visit(self, item); // Recurse
   //       }
   //    } else {
   //       out.insert(out.end(), arg.begin(), arg.end());
   //    }
   // };

   rlp_visitor visitor{out};
   for (auto& in_var : in_vars) {
      std::visit(visitor, in_var);
   }
}

void append(bytes& out, const std::uint8_t* data, std::size_t len) {
   std::copy_n(data, len, std::back_inserter(out));
}

void append_byte(bytes& out, std::uint8_t b) {
   out.push_back(b);
}

bytes encode_length(std::size_t len,std::size_t offset) {
   if (len < 56)
      return bytes{static_cast<std::uint8_t>(len + offset)};

   auto hex_len = to_hex(len, false);
   auto lit_len = hex_len.size() / 2;
   auto first_byte_hex = to_hex(offset + 55 + lit_len, false);
   auto enc_len_hex = first_byte_hex + hex_len;
   return from_hex_no_prefix(enc_len_hex);
}

// bytes encode_bytes(std::span<std::uint8_t>& data) {
//    return encode_bytes(static_cast<const std::span<std::uint8_t>&>(data));
// }
bytes encode_bytes(const std::span<std::uint8_t>& data) {
   return encode_bytes(data.data(), data.size());
}
bytes encode_bytes(const std::uint8_t* data, std::size_t len) {

   bytes             out;

   if (len == 1 && data[0] < 0x80) {
      out.push_back(data[0]);
      return out;
   }

   if (len <= 55) {
      out.push_back(static_cast<std::uint8_t>(0x80 + len));
      append(out, data, len);
      return out;
   }
   bytes len_enc = encode_length(len, 128);
   append(out, len_enc);
   append(out, data, len);

   return out;
}

bytes encode_bytes(const bytes& b) {
   bytes tmp = b;
   return encode_bytes(std::span<std::uint8_t>(tmp.data(), tmp.size()));
}

bytes encode_bytes(bytes32& b) {
   return encode_bytes(std::span<std::uint8_t>(b.data(), b.size()));
}

bytes encode_bytes(const bytes32& b) {
   bytes32 tmp = b;
   return encode_bytes(tmp);
}


bytes encode_bytes(const address& data) {
   return encode_bytes(data.data(), data.size());
}

bytes encode_string(std::string& s) {
   return encode_bytes(std::span<std::uint8_t>(
      reinterpret_cast<std::uint8_t*>(s.data()), s.size()));
}

bytes encode_list(std::vector<rlp_input_variant> items) {
   bytes payload;
   append(payload, items);

   const std::size_t len = payload.size();
   bytes             out;

   bytes len_enc = encode_length(len,192);
   append(out, len_enc,payload);


   return out;
}

std::string to_hex(const bytes32& b, bool prefixed) {
   std::ostringstream oss;
   if (prefixed)
      oss << "0x";
   for (auto v : b) {
      oss << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<int>(v);
   }
   return oss.str();
}

std::string to_hex(const bytes& b, bool prefixed) {
   std::ostringstream oss;
   if (prefixed)
      oss << "0x";
   for (auto v : b) {
      oss << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<int>(v);
   }
   return oss.str();
}

std::string to_hex(std::size_t num, bool prefixed) {
   std::ostringstream oss;

   oss << std::hex << num;
   auto s = oss.str();
   s = s.size() % 2 == 0 ? s : "0" + s;
   if (prefixed)
      s = "0x" + s;
   return s;
}

bytes from_hex_no_prefix(const std::string& hex) {
   bytes out;
   if (hex.size() % 2 != 0)
      return out;
   out.reserve(hex.size() / 2);

   auto hex_val = [](char c) -> int {
      if (c >= '0' && c <= '9')
         return c - '0';
      if (c >= 'a' && c <= 'f')
         return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F')
         return 10 + (c - 'A');
      return -1;
   };

   for (std::size_t i = 0; i < hex.size(); i += 2) {
      int hi = hex_val(hex[i]);
      int lo = hex_val(hex[i + 1]);
      if (hi < 0 || lo < 0)
         return {};
      out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
   }
   return out;
}

bytes from_hex_any(const std::string& hex) {
   if (hex.starts_with("0x") || hex.starts_with("0X")) {
      return from_hex_no_prefix(hex.substr(2));
   }
   return from_hex_no_prefix(hex);
}

bytes encode_access_list(const std::vector<access_list_entry>& access_list) {
   auto access_list_items =
      access_list | std::views::transform([](const auto& v) {
         auto storage_key_bytes = encode_list(v.storage_keys |
                                                   std::views::transform([](auto key) {
                                                      return encode_bytes(key);
                                                   }) | std::ranges::to<std::vector<rlp_input_variant>>());
         return rlp_input_variant(encode_list({
            v.addr,
            storage_key_bytes
         }));
      }) |
      std::ranges::to<std::vector>();

   return encode_list(access_list_items);
}

bytes encode_eip1559_unsigned(const eip1559_tx& tx) {
   bytes to_bytes   = encode_bytes(tx.to);
   bytes data_bytes = encode_bytes(tx.data);

   // std::vector<rlp_input_variant>

   return encode_list({
      encode_uint(tx.chain_id),
      encode_uint(tx.nonce),
      encode_uint(tx.max_priority_fee_per_gas),
      encode_uint(tx.max_fee_per_gas),
      encode_uint(tx.gas_limit),
      to_bytes,
      encode_uint(tx.value),
      data_bytes,
      encode_access_list(tx.access_list)
   });
}



bytes encode_eip1559_unsigned_typed(const eip1559_tx& tx) {
   bytes body = encode_eip1559_unsigned(tx);
   bytes out;
   out.reserve(1 + body.size());
   out.push_back(ethereum_eip1559_tx_type);
   out.insert(out.end(), body.begin(), body.end());
   return out;
}

bytes encode_eip1559_signed(const eip1559_tx& tx) {
   bytes to_bytes   = encode_bytes(tx.to);
   bytes data_bytes = encode_bytes(tx.data);

   auto v = encode_uint(tx.v);
   auto r = encode_bytes(tx.r);
   auto s = encode_bytes(tx.s);

   return encode_list({
      encode_uint(tx.chain_id),
      encode_uint(tx.nonce),
      encode_uint(tx.max_priority_fee_per_gas),
      encode_uint(tx.max_fee_per_gas),
      encode_uint(tx.gas_limit),
      to_bytes,
      encode_uint(tx.value),
      data_bytes,
      encode_access_list(tx.access_list),
      v,
       r,
       s
   });
}

bytes encode_eip1559_signed_typed(
   const eip1559_tx& tx) {
   bytes body = encode_eip1559_signed(tx);
   bytes out;
   out.reserve(1 + body.size());
   out.push_back(ethereum_eip1559_tx_type);
   out.insert(out.end(), body.begin(), body.end());
   return out;
}

}