#include <fc/log/logger.hpp>
#include <iostream>
#include <ethash/keccak.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>

namespace fc::network::ethereum {

namespace {
using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::json_rpc;
}

ethereum_client::ethereum_client(const signature_provider_ptr&             sig_provider,
                                 const std::variant<std::string, fc::url>& url_source,
                                 std::optional<fc::uint256>                chain_id)
   : _signature_provider(sig_provider), _client(json_rpc_client::create(url_source)), _chain_id(chain_id) {}

fc::variant ethereum_client::execute(const std::string& method, const fc::variant& params) {
   return _client.call(method, params);
}

fc::variant ethereum_client::execute_contract_call(const address& contract_address, const ethereum_contract_abi& abi,
                                                   const std::string& block_tag,
                                                   const fc::variants& params) {
   auto abi_call_encoded = ethereum_contract_call_encode(abi, params);
   auto to_data_mvo      = fc::mutable_variant_object("to", to_hex(contract_address, true))
      ("data", abi_call_encoded);
   fc::variants rpc_params = {to_data_mvo, fc::variant(block_tag)};
   return execute("eth_call", rpc_params);
}

fc::variant ethereum_client::execute_contract_tx(const eip1559_tx&   source_tx, const ethereum_contract_abi& abi,
                                                 const fc::variants& params, bool                            sign) {
   eip1559_tx tx   = source_tx;
   tx.data         = from_hex(ethereum_contract_call_encode(abi, params));
   auto tx_encoded = rlp::encode_eip1559_unsigned_typed(tx);
   if (sign) {
      // auto tx_encoded_unsigned     = rlp::encode_eip1559_unsigned_typed(tx);
      auto       tx_hash_data = fc::crypto::ethereum::hash_message(tx_encoded);
      fc::sha256 tx_hash(reinterpret_cast<const char*>(tx_hash_data.data()), tx_hash_data.size());
      auto       tx_sig = _signature_provider->sign(tx_hash);
      FC_ASSERT(tx_sig.contains<fc::em::signature_shim>());
      auto& tx_sig_shim = tx_sig.get<fc::em::signature_shim>();
      auto& tx_sig_data = tx_sig_shim.serialize();
      std::copy_n(tx_sig_data.begin(), 32, tx.r.begin());
      std::copy_n(tx_sig_data.begin() + 32, 32, tx.s.begin());
      tx.v       = tx_sig_data.data[64] - 27; // recovery id
      tx_encoded = rlp::encode_eip1559_signed_typed(tx);
   }

   return send_raw_transaction(to_hex(tx_encoded));
}


// JSON parsing helpers removed in favor of fc::variant returned by json_rpc_client

fc::uint256 ethereum_client::get_transaction_count(const address_compat_type& address, const std::string& block_tag) {
   auto         from_addr     = fc::crypto::ethereum::to_address(address);
   auto         from_addr_hex = to_hex(from_addr, true);
   fc::variants params{from_addr_hex, block_tag};
   auto         res = execute("eth_getTransactionCount", params);
   dlogf("tx_count: {}", res.as_string());
   return to_uint256(res);
}

fc::uint256 ethereum_client::get_chain_id() {
   static std::mutex            mutex;
   std::scoped_lock<std::mutex> lock(mutex);
   if (_chain_id.has_value()) {
      return *_chain_id;
   }

   fc::variants params; // empty array
   _chain_id = to_uint256(execute("eth_chainId", params));
   // FC_ASSERT(_chain_id.has_value());
   return _chain_id.value();
}

fc::uint256 ethereum_client::get_network_version() {
   fc::variants params; // Empty params array
   return to_uint256(execute("net_version", params));
}

fc::variant ethereum_client::get_syncing_status() {
   fc::variants params; // empty
   return execute("eth_syncing", params);
}

ethereum::address ethereum_client::get_signer_address() {
   return to_address(_signature_provider->public_key);
}

eip1559_tx ethereum_client::create_default_tx(const address_compat_type& to) {
   return eip1559_tx{
      .chain_id = get_chain_id(),
      .nonce = get_transaction_count(get_signer_address(), "pending"),
      .max_priority_fee_per_gas = 2000000000,
      .max_fee_per_gas = 2000101504,
      .gas_limit = 0x18c80,
      .to = to_address(to),
      .value = 0,
      .data = {},
      .access_list = {}
   };
}

fc::uint256 ethereum_client::get_block_number() {
   fc::variants params; // empty

   // ilogf("Block number: {}", fc::json::to_pretty_string(resp));
   return to_uint256(execute("eth_blockNumber", params));
}

fc::variant ethereum_client::get_block_by_number(const std::string& block_number,
                                                 bool               full_transaction_data) {
   fc::variants params{block_number, full_transaction_data};
   return execute("eth_getBlockByNumber", params);
}

fc::variant ethereum_client::get_block_by_hash(const std::string& block_hash, bool full_transaction_data) {
   fc::variants params{block_hash, full_transaction_data};
   return execute("eth_getBlockByHash", params);
}

fc::variant ethereum_client::get_transaction_by_hash(const std::string& tx_hash) {
   fc::variants params{tx_hash};
   return execute("eth_getTransactionByHash", params);
}

std::string ethereum_client::estimate_gas(const std::string& from, const std::string& to, const std::string& value) {
   fc::mutable_variant_object tx;
   tx("from", from)("to", to)("value", value);
   fc::variants params{fc::variant(tx)};
   auto         resp = execute("eth_estimateGas", params);
   return resp.as_string();
}

std::string ethereum_client::get_gas_price() {
   fc::variants params; // empty
   auto         resp = execute("eth_gasPrice", params);
   return resp.as_string();
}

std::string ethereum_client::send_transaction(const std::string& raw_tx_data) {
   fc::variants params{raw_tx_data};
   auto         resp = execute("eth_sendTransaction", params);
   return resp.as_string();
}

std::string ethereum_client::send_raw_transaction(const std::string& raw_tx_data) {
   fc::variants params{raw_tx_data};
   auto         resp = execute("eth_sendRawTransaction", params);
   return resp.as_string();
}


fc::variant ethereum_client::get_logs(const fc::variant& params) {
   return execute("eth_getLogs", params);
}

fc::variant ethereum_client::get_transaction_receipt(const std::string& tx_hash) {
   fc::variants params{tx_hash};
   return execute("eth_getTransactionReceipt", params);
}

} // namespace sysio::outpost_client::ethereum