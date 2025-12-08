#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <span>
#include <array>
#include <sstream>
#include <iomanip>
#include <fc/exception/exception.hpp>
#include <sysio/outpost_client/ethereum/types.hpp>

namespace sysio::outpost_client::ethereum::rlp {


// ---------------------------------------------------------
// Helpers
// ---------------------------------------------------------

using rlp_input_data = std::variant<bytes,bytes32,std::span<const std::uint8_t>>;

void append(bytes &out, const rlp_input_data &in_var);

void append(bytes &out, std::uint8_t b);

bytes encode_length(std::size_t len);

// ---------------------------------------------------------
// Core RLP encoders
// ---------------------------------------------------------

bytes encode_bytes(std::span<const std::uint8_t> data);

bytes encode_bytes(const bytes &b);

bytes encode_string(const std::string &s);

// Ethereum-style uint (0 => empty byte string => 0x80)

// inline rlp_input_data encode_uint(fc::uint256 value) {
//    if (value == 0) {
//       bytes empty;
//       return encode_bytes(empty);
//    }
//
//    bytes buf;
//    bool started = false;
//    auto size = value.backend().size();
//    for (int shift = (size - 8); shift >= 0; shift -= 8) {
//       std::uint8_t byte = static_cast<std::uint8_t>((value >> shift) & 0xff);
//       if (byte == 0 && !started) continue;
//       started = true;
//       buf.push_back(byte);
//    }
//
//    return encode_bytes(buf);
// }

// Ethereum-style uint (0 => empty byte string => 0x80)
template <typename T>
bytes encode_uint(T value) {
   if constexpr (std::is_same_v<T, std::uint64_t>) {
      return encode_uint<fc::uint256>(value);
   } else if constexpr (std::is_same_v<T, fc::uint256>) {
      if (value == 0) {
         bytes empty;
         return encode_bytes(empty);
      }

      bytes buf;
      bool started = false;

      for (int shift = 56; shift >= 0; shift -= 8) {
         std::uint8_t byte = static_cast<std::uint8_t>((value >> shift) & 0xff);
         if (byte == 0 && !started) continue;
         started = true;
         buf.push_back(byte);
      }

      return encode_bytes(buf);
   }
   FC_THROW_EXCEPTION(fc::exception, "Unsupported type for encode_uint");
}

bytes encode_list(const std::vector<rlp_input_data> &items);

// ---------------------------------------------------------
// Generic encode(T)
// ---------------------------------------------------------

template <typename T>
bytes encode(const T &value);

template <>
inline bytes encode<bytes>(const bytes &value) {
    return encode_bytes(value);
}

template <>
inline bytes encode<std::string>(const std::string &value) {
    return encode_string(value);
}

template <>
inline bytes encode<std::uint64_t>(const std::uint64_t &value) {
    return encode_uint(value);
}

template <typename... Ts>
bytes make_list(const Ts &... args) {
    std::vector<rlp_input_data> items;
    items.reserve(sizeof...(Ts));
    (items.push_back(encode(args)), ...);
    return encode_list(items);
}

// ---------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------

std::string to_hex_prefixed(const bytes &b);

bytes from_hex_noprefix(const std::string &hex);

inline bytes from_hex_any(const std::string &hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X")) {
        return from_hex_noprefix(hex.substr(2));
    }
    return from_hex_noprefix(hex);
}


// RLP of the unsigned tx body (for signing):
// [chainId, nonce, maxPriorityFeePerGas, maxFeePerGas,
//  gasLimit, to, value, data, accessList]
inline bytes encode_eip1559_unsigned(const eip1559_tx &tx) {
    bytes to_bytes = tx.to;          // size 0 or 20
    bytes data_bytes = tx.data;

    bytes access_list_rlp = rlp::encode_list({}); // empty list => 0xc0

    return rlp::encode_list({
        fc::to_bytes_be(tx.chain_id),
        fc::to_bytes_be(tx.nonce),
        fc::to_bytes_be(tx.max_priority_fee_per_gas),
        fc::to_bytes_be(tx.max_fee_per_gas),
        fc::to_bytes_be(tx.gas_limit),
        rlp::encode_bytes(to_bytes),
        fc::to_bytes_be(tx.value),
        rlp::encode_bytes(data_bytes),
        access_list_rlp
    });
}

// RLP of the *signed* tx body (EIP-1559):
// [chainId, nonce, maxPriorityFeePerGas, maxFeePerGas,
//  gasLimit, to, value, data, accessList, yParity, r, s]
inline bytes encode_eip1559_signed(const eip1559_tx &tx,
                                   std::uint8_t y_parity,
                                   std::span<const std::uint8_t, 32> r,
                                   std::span<const std::uint8_t, 32> s) {
    bytes to_bytes = tx.to;
    bytes data_bytes = tx.data;

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

// Final typed-transaction wire encoding: 0x02 || rlp(signed_body)
inline bytes encode_eip1559_signed_typed(const eip1559_tx &tx,
                                         std::uint8_t y_parity,
                                         std::span<const std::uint8_t, 32> r,
                                         std::span<const std::uint8_t, 32> s) {
    bytes body = encode_eip1559_signed(tx, y_parity, r, s);
    bytes out;
    out.reserve(1 + body.size());
    out.push_back(0x02);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Build JSON body for eth_sendRawTransaction
inline std::string build_eth_send_raw_transaction_json(const bytes &signed_tx,
                                                       std::uint32_t id = 1) {
    std::string hex = rlp::to_hex_prefixed(signed_tx);

    std::ostringstream oss;
    oss << R"({"jsonrpc":"2.0","id":)"
        << id
        << R"(,"method":"eth_sendRawTransaction","params":[")"
        << hex
        << R"("]})";
    return oss.str();
}

} // namespace eth

// ============================================================================
// Optional: simple send_raw_transaction over HTTP JSON-RPC (libcurl)
// ============================================================================

#ifdef ETH_WITH_LIBCURL
#include <curl/curl.h>
#include <stdexcept>

namespace eth {

inline std::size_t curl_write_callback(char *ptr,
                                       std::size_t size,
                                       std::size_t nmemb,
                                       void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Returns full HTTP response body as string (JSON).
inline std::string send_raw_transaction(const std::string &rpc_url,
                                        const bytes &signed_tx,
                                        std::uint32_t id = 1) {
    std::string json_body = build_eth_send_raw_transaction_json(signed_tx, id);

    CURL *curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, rpc_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error(std::string("curl_easy_perform failed: ") +
                                 curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

} // namespace eth
#endif
