// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <type_traits>
#include <vector>
#include <fc/int256.hpp>
#include <fc/crypto/ethereum_utils.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/json.hpp>
#include <sysio/outpost_client/external_chain_json_rpc_client.hpp>
#include <sysio/outpost_client/ethereum/rlp_encoder.hpp>
#include <sysio/outpost_client/ethereum/types.hpp>

namespace sysio::outpost_client::ethereum {
using namespace sysio::outpost_client;

class external_ethereum_chain_json_rpc_client
   : public external_chain_json_rpc_client<
        fc::crypto::chain_kind_ethereum> {
   
   using base = external_chain_json_rpc_client;

public:
   using base::invoke_read;
   using base::invoke_write;

   external_ethereum_chain_json_rpc_client(std::shared_ptr<sysio::signature_provider> signing_provider,
                            std::string                                endpoint)
      : base(std::move(signing_provider), std::move(endpoint)) {}

   external_ethereum_chain_json_rpc_client(const signature_provider_id_t& sig_provider_query,
                            std::string                    endpoint)
      : base(sig_provider_query, std::move(endpoint)) {}

protected:
   envelope_t make_write_envelope(const std::string& method, const envelope_params_t& params) override {
      // Build a legacy (pre-EIP-1559) raw transaction, sign with secp256k1, and send via eth_sendRawTransaction.
      FC_ASSERT(_signing_provider, "A signing provider is required for write calls");
      FC_ASSERT(params.size() == 1 && params[0].is_object(),
                "Ethereum write expects a single transaction object parameter");

      const auto& tx = params[0].get_object();

      auto nonce     = get_hex_field(tx, "nonce");
      auto gas_price = get_hex_field(tx, "gasPrice");
      auto gas_limit = get_hex_field(tx, "gas");
      auto to        = get_hex_field(tx, "to");    // may be empty for contract creation
      auto value     = get_hex_field(tx, "value"); // may be empty
      auto data      = get_hex_field(tx, "data");  // may be empty
      auto chain_id  = get_hex_field(tx, "chainId");

      auto chain_id_int = to_uint64(chain_id);

      auto unsigned_rlp  = rlp_encode_unsigned_tx(nonce, gas_price, gas_limit, to, value, data, chain_id_int);

      auto keccak_digest = keccak_hash(unsigned_rlp);
      fc::sha256 digest(reinterpret_cast<const char*>(keccak_digest.data()), keccak_digest.size());

      auto sig_variant = _signing_provider->sign(digest);
      auto em_sig      = sig_variant.visit([](auto const& shim) -> fc::em::signature_shim {
         using sig_t = std::decay_t<decltype(shim)>;
         if constexpr (std::is_same_v<sig_t, fc::em::signature_shim>) {
            return shim;
         } else {
            FC_THROW("Ethereum signature provider returned unexpected signature type");
         }
      });

      auto v_r_s     = to_v_r_s(em_sig, chain_id_int);
      auto signed_rlp = rlp_encode_signed_tx(nonce, gas_price, gas_limit, to, value, data, v_r_s.v, v_r_s.r, v_r_s.s);

      auto raw_hex = std::string("0x") + fc::to_hex(signed_rlp);

      envelope_params_t rpc_params;
      rpc_params.emplace_back(raw_hex);

      return this->make_envelope(method.empty() ? "eth_sendRawTransaction" : method, rpc_params);
   }

   struct vrs_t {
      uint64_t                v;
      std::array<uint8_t, 32> r;
      std::array<uint8_t, 32> s;
   };

   vrs_t to_v_r_s(const fc::em::signature_shim& sig, uint64_t chain_id) const {
      const auto& compact = sig._data;
      vrs_t out{};
      // std::copy_n(compact.data.data(), 32, out.r.data());
      // std::copy_n(compact.data.data() + 32, 32, out.s.data());
      auto rec_id = static_cast<uint64_t>(compact.data[64] - 27);
      out.v       = chain_id ? (chain_id * 2 + 35 + rec_id) : (27 + rec_id);
      return out;
   }

   std::string get_hex_field(const fc::variant_object& obj, const char* field) const {
      auto itr = obj.find(field);
      FC_ASSERT(itr != obj.end(), "Missing required field \"${f}\"", ("f", field));
      FC_ASSERT(itr->value().is_string(), "Field \"${f}\" must be hex string", ("f", field));
      return itr->value().as_string();
   }

   static std::vector<uint8_t> hex_to_bytes(std::string hex) {
      if (hex.rfind("0x", 0) == 0) hex = hex.substr(2);
      if (hex.empty()) return {};
      if (hex.size() % 2) hex = "0" + hex;
      std::vector<uint8_t> out;
      out.reserve(hex.size() / 2);
      for (size_t i = 0; i < hex.size(); i += 2) {
         auto byte = std::stoul(hex.substr(i, 2), nullptr, 16);
         out.push_back(static_cast<uint8_t>(byte));
      }
      return out;
   }

   static std::vector<uint8_t> encode_length(size_t len, uint8_t offset) {
      if (len < 56) return {static_cast<uint8_t>(len + offset)};
      std::vector<uint8_t> len_bytes;
      size_t tmp = len;
      while (tmp) {
         len_bytes.insert(len_bytes.begin(), static_cast<uint8_t>(tmp & 0xff));
         tmp >>= 8;
      }
      std::vector<uint8_t> out{static_cast<uint8_t>(offset + 55 + len_bytes.size())};
      out.insert(out.end(), len_bytes.begin(), len_bytes.end());
      return out;
   }

   static std::vector<uint8_t> rlp_encode_bytes(const std::vector<uint8_t>& input) {
      if (input.size() == 1 && input[0] < 0x80) return input;
      auto prefix = encode_length(input.size(), 0x80);
      std::vector<uint8_t> out;
      out.reserve(prefix.size() + input.size());
      out.insert(out.end(), prefix.begin(), prefix.end());
      out.insert(out.end(), input.begin(), input.end());
      return out;
   }

   static std::vector<uint8_t> rlp_encode_string(const std::string& hex) {
      return rlp_encode_bytes(hex_to_bytes(hex));
   }

   static std::vector<uint8_t> rlp_encode_uint(uint64_t value) {
      if (value == 0) return rlp_encode_bytes({});
      std::vector<uint8_t> buf;
      while (value) {
         buf.insert(buf.begin(), static_cast<uint8_t>(value & 0xff));
         value >>= 8;
      }
      return rlp_encode_bytes(buf);
   }

   static std::vector<uint8_t> rlp_encode_list(const std::vector<std::vector<uint8_t>>& items) {
      size_t total = 0;
      for (auto& i : items) total += i.size();
      auto prefix = encode_length(total, 0xc0);
      std::vector<uint8_t> out;
      out.reserve(prefix.size() + total);
      out.insert(out.end(), prefix.begin(), prefix.end());
      for (auto& i : items) out.insert(out.end(), i.begin(), i.end());
      return out;
   }

   static std::vector<uint8_t> rlp_encode_unsigned_tx(const std::string& nonce,
                                                      const std::string& gas_price,
                                                      const std::string& gas_limit,
                                                      const std::string& to,
                                                      const std::string& value,
                                                      const std::string& data,
                                                      uint64_t chain_id) {
      return rlp_encode_list({
         rlp_encode_string(nonce),
         rlp_encode_string(gas_price),
         rlp_encode_string(gas_limit),
         rlp_encode_string(to),
         rlp_encode_string(value),
         rlp_encode_string(data),
         rlp_encode_uint(chain_id),
         rlp_encode_uint(0),
         rlp_encode_uint(0),
      });
   }

   static std::vector<uint8_t> rlp_encode_signed_tx(const std::string& nonce,
                                                    const std::string& gas_price,
                                                    const std::string& gas_limit,
                                                    const std::string& to,
                                                    const std::string& value,
                                                    const std::string& data,
                                                    uint64_t v,
                                                    const std::array<uint8_t,32>& r,
                                                    const std::array<uint8_t,32>& s) {
      return rlp_encode_list({
         rlp_encode_string(nonce),
         rlp_encode_string(gas_price),
         rlp_encode_string(gas_limit),
         rlp_encode_string(to),
         rlp_encode_string(value),
         rlp_encode_string(data),
         rlp_encode_uint(v),
         rlp_encode_bytes({r.begin(), r.end()}),
         rlp_encode_bytes({s.begin(), s.end()}),
      });
   }

   static uint64_t to_uint64(const std::string& hex) {
      auto v = hex;
      if (v.rfind("0x", 0) == 0) v = v.substr(2);
      if (v.empty()) return 0;
      return std::stoull(v, nullptr, 16);
   }

   static std::array<uint8_t, 32> keccak_hash(const std::vector<uint8_t>& data) {
      std::array<uint8_t, 32> out{};
      SHA3_CTX ctx;
      keccak_init(&ctx);
      keccak_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()),
                    static_cast<uint16_t>(data.size()));
      keccak_final(&ctx, out.data());
      return out;
   }
};

} // namespace sysio::outpost_client::ethereum
