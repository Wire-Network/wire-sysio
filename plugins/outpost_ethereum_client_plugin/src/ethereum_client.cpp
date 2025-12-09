#include <fc/log/logger.hpp>
#include <iostream>
#include <sysio/outpost_client/ethereum/ethereum_client.hpp>

namespace sysio::outpost_client::ethereum {
using namespace fc::network::json_rpc;
ethereum_client::ethereum_client(const signature_provider_ptr& sig_provider, const std::variant<std::string, fc::url>& url_source)
    : _signature_provider(sig_provider), _client(json_rpc_client::create(url_source))  {}

fc::variant ethereum_client::execute(const std::string& method, const fc::variant& params) {
   return _client.call(method, params);
}

// JSON parsing helpers removed in favor of fc::variant returned by json_rpc_client

std::string ethereum_client::get_transaction_count(const std::string& address) {
   fc::variants params{ address };
   auto resp = execute("eth_getTransactionCount", params);
   return resp.as_string();
}

std::string ethereum_client::get_chain_id() {
   fc::variants params; // empty array
   auto resp = execute("eth_chainId", params);
   return resp.as_string();
}

std::string ethereum_client::get_network_version() {
   fc::variants params; // Empty params array
   auto resp = execute("net_version", params);
   return resp.as_string();
}

fc::variant ethereum_client::get_syncing_status() {
   fc::variants params; // empty
   return execute("eth_syncing", params);
}

std::string ethereum_client::get_block_number() {
   fc::variants params; // empty
   auto resp = execute("eth_blockNumber", params);
   return resp.as_string();
}

fc::variant ethereum_client::get_block_by_number(const std::string& blockNumber,
                                                 bool               fullTransactionData) {
   fc::variants params{ blockNumber, fullTransactionData };
   return execute("eth_getBlockByNumber", params);
}

fc::variant ethereum_client::get_block_by_hash(const std::string& block_hash, bool full_transaction_data) {
   fc::variants params{ block_hash, full_transaction_data };
   return execute("eth_getBlockByHash", params);
}

fc::variant ethereum_client::get_transaction_by_hash(const std::string& tx_hash) {
   fc::variants params{ tx_hash };
   return execute("eth_getTransactionByHash", params);
}

std::string ethereum_client::estimate_gas(const std::string& from, const std::string& to, const std::string& value) {
   fc::mutable_variant_object tx;
   tx("from", from)("to", to)("value", value);
   fc::variants params{ fc::variant(tx) };
   auto resp = execute("eth_estimateGas", params);
   return resp.as_string();
}

std::string ethereum_client::get_gas_price() {
   fc::variants params; // empty
   auto resp = execute("eth_gasPrice", params);
   return resp.as_string();
}

std::string ethereum_client::send_transaction(const std::string& raw_tx_data) {
   fc::variants params{ raw_tx_data };
   auto resp = execute("eth_sendTransaction", params);
   return resp.as_string();
}

fc::variant ethereum_client::get_logs(const fc::variant& params) {
   return execute("eth_getLogs", params);
}

fc::variant ethereum_client::get_transaction_receipt(const std::string& tx_hash) {
   fc::variants params{ tx_hash };
   return execute("eth_getTransactionReceipt", params);
}

} // namespace sysio::outpost_client::ethereum
