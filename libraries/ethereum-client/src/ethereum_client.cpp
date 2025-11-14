#include <fc/log/logger.hpp>
#include <iostream>
#include <sysio/ethereum/ethereum_client.hpp>

namespace sysio::ethereum {

ethereum_client::ethereum_client(const std::string& node_url, network_adapter& net_adapter)
    : node_url_(node_url), net_adapter_(net_adapter) {}  // Correct initialization list

std::optional<std::string> ethereum_client::execute(const std::string& method, const Json::Value& params) {
    Json::Value payload;
    payload["jsonrpc"] = "2.0";
    payload["method"] = method;
    payload["params"] = params;
    payload["id"] = 1;

    Json::StreamWriterBuilder writer;
    std::string request_data = Json::writeString(writer, payload);

    auto response = net_adapter_.send_post_request(node_url_, request_data);
    if (!response) {
      elog("Failed to get response for method: " + method);
       
        return std::nullopt;
    }
    return *response;
}

std::optional<Json::Value> ethereum_client::parse_json_response(const std::string& response) {
    Json::Value json_response;
    Json::CharReaderBuilder reader;
    std::string errs;

    std::istringstream stream(response);
    if (!Json::parseFromStream(reader, stream, &json_response, &errs)) {
        elog("Error parsing response: " + errs);
        return std::nullopt;
    }

    return json_response;
}

void ethereum_client::process_result(const Json::Value& result_json) {
   if (result_json.isMember("error")) {
        elog("Error: " + result_json["error"]["message"].asString());
        return;
    }

    if (result_json.isMember("result")) {
        std::string result = result_json["result"].asString();
        ilog("Result: ${result}",("result",result));
    } else {
        elog("Error: 'result' field not found in response.");
    }
}

std::optional<std::string> ethereum_client::get_transaction_count(const std::string& address) {
   Json::Value params;
    params[0] = address;

    auto response = execute("eth_getTransactionCount", params);
    return response;
}

std::optional<std::string> ethereum_client::get_chain_id() {
   Json::Value params;
    auto response = execute("eth_chainId", params);
    return response;
}

std::optional<std::string> ethereum_client::get_network_version() {
   Json::Value params; // Empty params array
    auto response = execute("net_version", params);
    return response;
}

std::optional<std::string> ethereum_client::get_syncing_status() {
   Json::Value params;
    auto response = execute("eth_syncing", params);
    return response;
}

std::optional<std::string> ethereum_client::get_block_number() {
   Json::Value params;
    auto response = execute("eth_blockNumber", params);
    return response;
}

std::optional<Json::Value> ethereum_client::get_block_by_number(const std::string& blockNumber,
                                                                bool               fullTransactionData) {
   Json::Value params;
    params[0] = blockNumber;
    params[1] = fullTransactionData;

    auto response = execute("eth_getBlockByNumber", params);
    if (!response) {
        return std::nullopt;
    }

    return parse_json_response(*response);
}

std::optional<Json::Value> ethereum_client::get_block_by_hash(const std::string& block_hash, bool full_transaction_data) {
    Json::Value params;
    params[0] = block_hash;
    params[1] = full_transaction_data;

    auto response = execute("eth_getBlockByHash", params);
    if (!response) {
        return std::nullopt;
    }

    return parse_json_response(*response);
}

std::optional<Json::Value> ethereum_client::get_transaction_by_hash(const std::string& tx_hash) {
    Json::Value params;
    params[0] = tx_hash;

    auto response = execute("eth_getTransactionByHash", params);
    if (!response) {
        return std::nullopt;
    }

    return parse_json_response(*response);
}

std::optional<std::string> ethereum_client::estimate_gas(const std::string& from, const std::string& to, const std::string& value) {
    Json::Value params;
    Json::Value tx_data;
    tx_data["from"] = from;
    tx_data["to"] = to;
    tx_data["value"] = value;
    params[0] = tx_data;

    auto response = execute("eth_estimateGas", params);
    return response;
}

std::optional<std::string> ethereum_client::get_gas_price() {
    Json::Value params;
    auto        response = execute("eth_gasPrice", params);
    return response;
}

std::optional<std::string> ethereum_client::send_transaction(const std::string& raw_tx_data) {
    Json::Value params;
    params[0] = raw_tx_data;

    auto response = execute("eth_sendTransaction", params);
    return response;
}

std::optional<Json::Value> ethereum_client::get_logs(const Json::Value& params) {
    auto response = execute("eth_getLogs", params);
    if (!response) {
       return std::nullopt;
    }

    return parse_json_response(*response);
}

std::optional<Json::Value> ethereum_client::get_transaction_receipt(const std::string& tx_hash) {
    Json::Value params;
    params[0] = tx_hash;

    auto response = execute("eth_getTransactionReceipt", params);
    if (!response) {
        return std::nullopt;
    }

    return parse_json_response(*response);
}

} // namespace sysio::ethereum
