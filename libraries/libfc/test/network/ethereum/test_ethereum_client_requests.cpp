/**
 * The shape of every JSON-RPC request `ethereum_client` puts on the wire, held to what
 * go-ethereum parses.
 *
 * The client's other tests answer at the `execute` boundary and never serialize a request,
 * which is how calldata without its `0x` prefix -- accepted by anvil, refused by geth with
 * `-32602 invalid argument` -- shipped unnoticed. Here a real client dials a loopback
 * server that records the HTTP body, and each request the relay issues is checked both
 * for the exact field that was wrong and against the full `execution_api` rule set.
 */
#include <boost/test/unit_test.hpp>

#include <fc-test/capture_http_server.hpp>
#include <fc-test/execution_api_strictness.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/io/json.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_transaction_policy.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace fc::crypto;
using namespace fc::network::ethereum;
namespace eth = fc::network::ethereum;
namespace api = fc::test::execution_api;
using fc::test::capture_http_server;

constexpr uint32_t         test_chain_id      = 31337;
constexpr std::string_view test_contract      = "0x5FbDB2315678afecb367f032d93F642f64180aa3";
constexpr std::string_view test_tx_hash       = "0x" "a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1";
constexpr std::chrono::milliseconds request_wait{5000};
/// `setNumber(uint256)` with argument 60, as the ABI encoder emits it (no prefix).
constexpr std::string_view set_number_calldata =
   "3fb5c1cb000000000000000000000000000000000000000000000000000000000000003c";
constexpr std::string_view total_supply_name = "totalSupply";
constexpr std::string_view total_supply_selector = "18160ddd";
constexpr std::string_view rpc_hex_prefix = "0x";
constexpr std::string_view ping_event_name = "Ping";
/// The largest log and transaction index, and 2^32 + 5, which truncates to 5.
constexpr std::string_view max_uint32_quantity  = "0xffffffff";
constexpr std::string_view past_uint32_quantity = "0x100000005";
/// QUANTITY forms the execution API does not allow: a leading zero, and no `0x` prefix.
constexpr std::string_view leading_zero_quantity = "\"0x0010\"";
constexpr std::string_view unprefixed_quantity   = "\"16\"";
/// The Sepolia network id, which `net_version` reports in decimal.
constexpr std::string_view sepolia_network_version = "\"11155111\"";
constexpr uint64_t         sepolia_network_id      = 11155111;
/// The diagnostic field names the client reports a refused quantity under.
constexpr std::string_view block_number_field      = "block_number";
constexpr std::string_view log_index_field         = "log_index";
constexpr std::string_view transaction_index_field = "transaction_index";

/// One JSON-RPC result body echoing the client's first request id.
std::string rpc_result(std::string_view result_json, int64_t id = 1) {
   return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":)" + std::string(result_json) + "}";
}

capture_http_server::scripted_response reply(std::string_view result_json, int64_t id = 1) {
   capture_http_server::scripted_response response;
   response.body = rpc_result(result_json, id);
   return response;
}

/// A local Ethereum signer over a fresh secp256k1 key.
signature_provider_ptr local_signer() {
   const auto key      = private_key::generate(private_key::key_type::em);
   auto       provider = std::make_shared<signature_provider_t>();
   provider->target_chain = chain_kind_ethereum;
   provider->key_type     = chain_key_type_ethereum;
   provider->key_name     = "request-shape-signer";
   provider->public_key   = key.get_public_key();
   provider->private_key  = key;
   provider->sign         = [key](const fc::sha256& digest) { return key.sign(digest); };
   return provider;
}

ethereum_transaction_policy wide_policy() {
   return ethereum_transaction_policy{
      .client_id                = "request-shape-client",
      .chain_id                 = test_chain_id,
      .max_priority_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_fee_per_gas          = maximum_ethereum_transaction_policy_value(),
      .max_gas_limit            = maximum_ethereum_transaction_policy_value(),
      .max_total_native_cost    = maximum_ethereum_transaction_policy_value(),
   };
}

/// `function setNumber(uint256)`.
eth::abi::contract set_number_abi() {
   eth::abi::component_type value;
   value.name = "value";
   value.type = eth::abi::data_type::uint256;
   return eth::abi::contract{
      .name = "setNumber",
      .type = eth::abi::invoke_target_type::function,
      .inputs = {value},
      .outputs = {},
   };
}

/// `event Ping()`: no fields, so a log of it needs nothing past its position.
eth::abi::contract ping_event_abi() {
   return eth::abi::contract{
      .name = std::string(ping_event_name),
      .type = eth::abi::invoke_target_type::event,
      .inputs = {},
      .outputs = {},
   };
}

/// An `eth_getLogs` result of one log at the given position.
std::string one_log(std::string_view block_number, std::string_view log_index, std::string_view transaction_index) {
   const fc::mutable_variant_object log = fc::mutable_variant_object("blockNumber", std::string(block_number))(
      "logIndex", std::string(log_index))("transactionIndex", std::string(transaction_index));
   return fc::json::to_string(fc::variants{log}, fc::time_point::maximum());
}

/// A predicate matching the rejection of the response field `field` for `reason`.
auto refuses(ethereum_transaction_policy_reason reason, std::string_view field) {
   return [reason, field](const ethereum_transaction_policy_exception& rejection) {
      return rejection.reason() == reason && rejection.field() == field;
   };
}

/// A client dialing `server`, and the parsed body of the request at `index`.
struct recorded_client {
   explicit recorded_client(std::vector<capture_http_server::scripted_response> script)
      : server(std::move(script))
      , client(std::make_shared<ethereum_client>(local_signer(), server.url(), wide_policy())) {}

   fc::variant_object request(std::size_t index) {
      BOOST_REQUIRE(server.wait_for_requests(index + 1, request_wait));
      return fc::json::from_string(server.request(index).body).get_object();
   }

   /// The request's `params`, after asserting it conforms to what geth parses.
   fc::variants conformant_params(std::size_t index, std::string_view method) {
      const auto body       = server.request(index).body;
      const auto violations = api::check_request_body(body);
      BOOST_CHECK_MESSAGE(violations.empty(), method << ": " << api::describe(violations) << " in " << body);
      const auto object = request(index);
      BOOST_CHECK_EQUAL(object["method"].as_string(), std::string(method));
      return object["params"].get_array();
   }

   capture_http_server              server;
   std::shared_ptr<ethereum_client> client;
};

} // namespace

BOOST_AUTO_TEST_SUITE(ethereum_client_requests)

/// The rule set itself: what it accepts is what geth accepts, field by field.
BOOST_AUTO_TEST_CASE(strictness_rules_match_geth) try {
   const auto violations_for = [](std::string_view method, const fc::variant& params) {
      return api::describe(api::check_request(method, params));
   };
   const auto call = [](fc::variant data, fc::variant block) {
      return fc::variants{fc::mutable_variant_object("to", std::string(test_contract))("data", std::move(data)),
                          std::move(block)};
   };
   // Conformant forms.
   BOOST_CHECK_EQUAL(violations_for("eth_call", call("0x3fb5c1cb", "latest")), "");
   BOOST_CHECK_EQUAL(violations_for("eth_call", call("0x", "0x1a")), "");
   BOOST_CHECK_EQUAL(violations_for("eth_call", call("0x", "0x0")), "");
   BOOST_CHECK_EQUAL(violations_for("eth_call", call("0x", fc::mutable_variant_object("blockNumber", "0x1a"))), "");
   BOOST_CHECK_EQUAL(violations_for("eth_sendRawTransaction", fc::variants{"0x02f8"}), "");
   BOOST_CHECK_EQUAL(violations_for("eth_getTransactionReceipt", fc::variants{std::string(test_tx_hash)}), "");
   BOOST_CHECK_EQUAL(violations_for("eth_blockNumber", fc::variants{}), "");
   BOOST_CHECK_EQUAL(violations_for("eth_someFutureMethod", fc::variants{"anything"}), "");

   // What anvil tolerates and geth does not.
   BOOST_CHECK_NE(violations_for("eth_call", call("3fb5c1cb", "latest")).find("without 0x prefix"), std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_call", call("0x3fb", "latest")).find("odd length"), std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_call", call("0x", "0x01")).find("leading zero"), std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_call", call("0x", "1a")).find("without 0x prefix"), std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_call", call("0x", "0x")).find("no digits"), std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_call", call("0x", "newest")).find("without 0x prefix"), std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_sendRawTransaction", fc::variants{"02f8"}).find("without 0x prefix"),
                  std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_getTransactionReceipt", fc::variants{"0xabcd"}).find("want 64"),
                  std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_getTransactionCount", fc::variants{"0xabcd", "pending"}).find("want 40"),
                  std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_blockNumber", fc::variants{"extra"}).find("too many arguments"),
                  std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_getBlockByNumber", fc::variants{"latest"}).find("missing value"),
                  std::string::npos);

   // The transaction-argument exclusions geth applies before executing.
   const auto args = [](fc::mutable_variant_object fields) { return fc::variants{std::move(fields), "latest"}; };
   BOOST_CHECK_NE(violations_for("eth_estimateGas", args(fc::mutable_variant_object("data", "0x01")("input", "0x02")))
                     .find("not equal"),
                  std::string::npos);
   BOOST_CHECK_EQUAL(
      violations_for("eth_estimateGas", args(fc::mutable_variant_object("data", "0x01")("input", "0x01"))), "");
   BOOST_CHECK_NE(violations_for("eth_estimateGas", args(fc::mutable_variant_object("gasPrice", "0x1")(
                                                       "maxFeePerGas", "0x2")))
                     .find("both gasPrice"),
                  std::string::npos);
   BOOST_CHECK_NE(violations_for("eth_estimateGas", args(fc::mutable_variant_object("maxFeePerGas", "0xff")(
                                                       "maxPriorityFeePerGas", "0x100")))
                     .find("maxFeePerGas (0xff) < maxPriorityFeePerGas (0x100)"),
                  std::string::npos);
   BOOST_CHECK_EQUAL(violations_for("eth_estimateGas", args(fc::mutable_variant_object("maxFeePerGas", "0x100")(
                                                          "maxPriorityFeePerGas", "0xff"))),
                     "");

   // The envelope: a request without an id is a notification nobody answers.
   BOOST_CHECK_NE(api::describe(api::check_request_body(R"({"jsonrpc":"2.0","method":"eth_blockNumber","params":[]})"))
                     .find("notification"),
                  std::string::npos);
   BOOST_CHECK_EQUAL(
      api::describe(api::check_request_body(R"({"jsonrpc":"2.0","id":7,"method":"eth_blockNumber","params":[]})")),
      "");
   BOOST_CHECK_THROW(api::require_conformant("eth_call", call("3fb5c1cb", "latest")), api::violation_exception);
} FC_LOG_AND_RETHROW();

/// `eth_call` sends `0x`-prefixed calldata -- the field hoodi's geth refused -- at the block asked for.
BOOST_AUTO_TEST_CASE(contract_view_call_sends_prefixed_calldata_at_the_block_asked_for) try {
   recorded_client recorded({reply("\"0x\""), reply("\"0x\"", 2)});
   const auto      contract = set_number_abi();
   const auto      target   = fc::crypto::ethereum::to_address(std::string(test_contract));

   const contract_invoke_data_items sixty{fc::variant(uint64_t{60})};

   (void) recorded.client->execute_contract_view_fn(target, contract, block_tag_t::finalized, sixty);
   const auto at_tag = recorded.conformant_params(0, "eth_call");
   BOOST_REQUIRE_EQUAL(at_tag.size(), 2u);
   BOOST_CHECK_EQUAL(at_tag[0].get_object()["data"].as_string(), "0x" + std::string(set_number_calldata));
   BOOST_CHECK_EQUAL(at_tag[0].get_object()["to"].as_string(), fc::to_hex(target, true));
   BOOST_CHECK_EQUAL(at_tag[1].as_string(), "finalized");

   (void) recorded.client->execute_contract_view_fn(target, contract, format_rpc_quantity(fc::uint256{26}), sixty);
   const auto pinned = recorded.conformant_params(1, "eth_call");
   BOOST_CHECK_EQUAL(pinned[1].as_string(), "0x1a");
} FC_LOG_AND_RETHROW();

/// Selector-only views carry the same prefix, while the ABI encoder retains raw hex.
BOOST_AUTO_TEST_CASE(selector_only_view_call_sends_prefixed_calldata) try {
   recorded_client recorded({reply("\"0x\"")});
   const eth::abi::contract contract{
      .name = std::string(total_supply_name),
      .type = eth::abi::invoke_target_type::function,
      .inputs = {},
      .outputs = {},
   };
   const auto target = fc::crypto::ethereum::to_address(std::string(test_contract));
   const contract_invoke_data_items no_arguments;
   (void) recorded.client->execute_contract_view_fn(target, contract, block_tag_t::finalized, no_arguments);
   const auto params = recorded.conformant_params(0, "eth_call");
   BOOST_CHECK_EQUAL(params.at(0).get_object()["data"].as_string(),
                     std::string(rpc_hex_prefix) + std::string(total_supply_selector));
   BOOST_CHECK_EQUAL(contract_encode_data(contract, no_arguments), total_supply_selector);
} FC_LOG_AND_RETHROW();

/// Signed and unsigned submission paths carry prefixed bytes without doubling an existing prefix.
BOOST_AUTO_TEST_CASE(raw_transaction_is_sent_with_its_prefix) try {
   recorded_client recorded({reply("\"" + std::string(test_tx_hash) + "\""),
                             reply("\"" + std::string(test_tx_hash) + "\"", 2),
                             reply("\"" + std::string(test_tx_hash) + "\"", 3),
                             reply("\"" + std::string(test_tx_hash) + "\"", 4)});

   // Pre-signed bytes given without a prefix.
   (void) recorded.client->send_raw_transaction("02f8");
   const auto bare = recorded.conformant_params(0, "eth_sendRawTransaction");
   BOOST_CHECK_EQUAL(bare.at(0).as_string(), "0x02f8");
   // And with one: it is not doubled.
   (void) recorded.client->send_raw_transaction("0x02f8");
   BOOST_CHECK_EQUAL(recorded.conformant_params(1, "eth_sendRawTransaction").at(0).as_string(), "0x02f8");

   // What the client signs itself.
   const eip1559_tx tx{.chain_id = test_chain_id,
                       .nonce = 13,
                       .max_priority_fee_per_gas = 2'000'000'000,
                       .max_fee_per_gas = 2'000'101'504,
                       .gas_limit = 0x18c80,
                       .to = fc::crypto::ethereum::to_address(std::string(test_contract)),
                       .value = 0,
                       .data = {},
                       .access_list = {}};
   const auto signed_tx = recorded.client->sign_transaction(tx, "setNumber");
   BOOST_CHECK(signed_tx.raw_hex.starts_with("0x02"));
   (void) recorded.client->execute_contract_tx_fn(tx, set_number_abi(), {fc::variant(uint64_t{60})});
   const auto signed_params = recorded.conformant_params(2, "eth_sendRawTransaction");
   BOOST_CHECK(signed_params.at(0).as_string().starts_with("0x02"));
   (void) recorded.client->execute_contract_tx_fn(tx, set_number_abi(), {fc::variant(uint64_t{60})}, /*sign=*/false);
   const auto unsigned_params = recorded.conformant_params(3, "eth_sendRawTransaction");
   BOOST_CHECK(unsigned_params.at(0).as_string().starts_with("0x02"));
} FC_LOG_AND_RETHROW();

/// The pre-flight estimate names the same calldata in `data` and `input`, prefixed, with
/// canonical fee quantities and the cap as `gas`.
BOOST_AUTO_TEST_CASE(gas_estimate_sends_a_conformant_transaction_object) try {
   recorded_client recorded({reply("\"0x5208\"")});
   const auto      contract = set_number_abi();
   const ethereum_client::gas_config_t fees{.tip = 10, .base_fee = 45, .max_fee_per_gas = 100};

   (void) recorded.client->estimate_gas(fc::crypto::ethereum::to_address(std::string(test_contract)), contract,
                                        fc::variants{fc::variant(uint64_t{60})}, fees, /*gas_limit_cap=*/1'000'000);
   const auto params = recorded.conformant_params(0, "eth_estimateGas");
   const auto& args  = params.at(0).get_object();
   BOOST_CHECK_EQUAL(args["data"].as_string(), "0x" + std::string(set_number_calldata));
   BOOST_CHECK_EQUAL(args["input"].as_string(), args["data"].as_string());
   BOOST_CHECK_EQUAL(args["maxPriorityFeePerGas"].as_string(), "0xa");
   BOOST_CHECK_EQUAL(args["maxFeePerGas"].as_string(), "0x64");
   BOOST_CHECK_EQUAL(args["gas"].as_string(), "0xf4240");
} FC_LOG_AND_RETHROW();

/// The reads around a send: the signer's address at `pending`, blocks by canonical number
/// or tag, and a receipt by its full hash.
BOOST_AUTO_TEST_CASE(chain_reads_send_canonical_addresses_blocks_and_hashes) try {
   recorded_client recorded({reply("\"0x7\""), reply("{\"number\":\"0x0\"}", 2), reply("{\"number\":\"0x1a\"}", 3),
                             reply("null", 4)});

   (void) recorded.client->get_transaction_count(recorded.client->get_signer_address(), block_tag_t::pending);
   const auto count = recorded.conformant_params(0, "eth_getTransactionCount");
   BOOST_CHECK_EQUAL(count.at(0).as_string(), fc::to_hex(recorded.client->get_signer_address(), true));
   BOOST_CHECK_EQUAL(count.at(1).as_string(), "pending");

   (void) recorded.client->get_block_by_number(format_rpc_quantity(fc::uint256{0}));
   BOOST_CHECK_EQUAL(recorded.conformant_params(1, "eth_getBlockByNumber").at(0).as_string(), "0x0");
   (void) recorded.client->get_block_by_number(block_tag_t::finalized);
   BOOST_CHECK_EQUAL(recorded.conformant_params(2, "eth_getBlockByNumber").at(0).as_string(), "finalized");

   (void) recorded.client->get_transaction_receipt(std::string(test_tx_hash));
   BOOST_CHECK_EQUAL(recorded.conformant_params(3, "eth_getTransactionReceipt").at(0).as_string(),
                     std::string(test_tx_hash));
} FC_LOG_AND_RETHROW();

/// A log's index is read whole: the largest uint32 is kept, and one past it is refused rather than
/// truncated to its low 32 bits.
BOOST_AUTO_TEST_CASE(log_indexes_past_uint32_are_refused_not_truncated) try {
   recorded_client recorded({reply(one_log("0x1a", max_uint32_quantity, max_uint32_quantity)),
                             reply(one_log("0x1a", past_uint32_quantity, "0x0"), 2),
                             reply(one_log("0x1a", "0x0", past_uint32_quantity), 3)});
   const auto query = [&] {
      return recorded.client->get_events(std::string(test_contract), {std::string(ping_event_name)},
                                         {ping_event_abi()}, format_rpc_quantity(fc::uint256{0}));
   };

   const auto events = query();
   BOOST_REQUIRE_EQUAL(events.size(), 1u);
   BOOST_CHECK_EQUAL(events.front().block_number, fc::uint256{26});
   BOOST_CHECK_EQUAL(events.front().log_index, std::numeric_limits<uint32_t>::max());
   BOOST_CHECK_EQUAL(events.front().transaction_index, std::numeric_limits<uint32_t>::max());

   BOOST_CHECK_EXCEPTION(query(), ethereum_transaction_policy_exception,
                         refuses(ethereum_transaction_policy_reason::rpc_quantity_out_of_range, log_index_field));
   BOOST_CHECK_EXCEPTION(
      query(), ethereum_transaction_policy_exception,
      refuses(ethereum_transaction_policy_reason::rpc_quantity_out_of_range, transaction_index_field));
} FC_LOG_AND_RETHROW();

/// The block number and log position reads take only a canonical QUANTITY: no leading zero, and the
/// `0x` prefix.
BOOST_AUTO_TEST_CASE(quantity_reads_refuse_non_canonical_forms) try {
   const auto invalid = [](std::string_view field) {
      return refuses(ethereum_transaction_policy_reason::rpc_quantity_invalid, field);
   };

   recorded_client block_numbers({reply(leading_zero_quantity), reply(unprefixed_quantity, 2)});
   BOOST_CHECK_EXCEPTION((void) block_numbers.client->get_block_number(), ethereum_transaction_policy_exception,
                         invalid(block_number_field));
   BOOST_CHECK_EXCEPTION((void) block_numbers.client->get_block_number(), ethereum_transaction_policy_exception,
                         invalid(block_number_field));

   recorded_client logs({reply(one_log("0x01a", "0x0", "0x0")), reply(one_log("0x1a", "7", "0x0"), 2),
                         reply(one_log("0x1a", "0x0", "0x07"), 3)});
   const auto query = [&] {
      return logs.client->get_events(std::string(test_contract), {std::string(ping_event_name)},
                                     {ping_event_abi()}, format_rpc_quantity(fc::uint256{0}));
   };
   BOOST_CHECK_EXCEPTION(query(), ethereum_transaction_policy_exception, invalid(block_number_field));
   BOOST_CHECK_EXCEPTION(query(), ethereum_transaction_policy_exception, invalid(log_index_field));
   BOOST_CHECK_EXCEPTION(query(), ethereum_transaction_policy_exception, invalid(transaction_index_field));

   // A receipt read past its first confirmation counts blocks from its own; the head is never asked.
   const std::string receipt = R"({"status":"0x1","blockNumber":)" + std::string(leading_zero_quantity) + "}";
   recorded_client receipts({reply(receipt), reply("\"0x20\"", 2)});
   ethereum_confirm_options two_deep = ethereum_confirm_option_defaults;
   two_deep.confirmations            = 2;
   BOOST_CHECK_EXCEPTION((void) receipts.client->wait_for_receipt(std::string(test_tx_hash), two_deep),
                         ethereum_transaction_policy_exception, invalid(block_number_field));
   BOOST_CHECK_EQUAL(receipts.server.request_count(), 1u);
} FC_LOG_AND_RETHROW();

/// `net_version` answers in decimal, not as a QUANTITY, even after a leading zero.
BOOST_AUTO_TEST_CASE(network_version_is_read_as_decimal) try {
   recorded_client recorded({reply(sepolia_network_version), reply("\"010\"", 2)});
   BOOST_CHECK_EQUAL(recorded.client->get_network_version(), fc::uint256{sepolia_network_id});
   BOOST_CHECK_EQUAL(recorded.client->get_network_version(), fc::uint256{10});
} FC_LOG_AND_RETHROW();

/// A block number past 2^256 - 1 is refused instead of wrapping.
BOOST_AUTO_TEST_CASE(block_number_past_uint256_is_refused) try {
   const std::string past_max_block_number = "\"0x1" + std::string(64, '0') + "\"";
   recorded_client recorded({reply(past_max_block_number)});
   BOOST_CHECK_EXCEPTION((void) recorded.client->get_block_number(), ethereum_transaction_policy_exception,
                         refuses(ethereum_transaction_policy_reason::rpc_quantity_out_of_range, block_number_field));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
