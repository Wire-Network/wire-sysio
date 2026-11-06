#pragma once

#include <fc/io/json.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * The execution-API request rules go-ethereum enforces, as a checker for test doubles.
 *
 * Every argument a node parses as `hexutil.Bytes`, `hexutil.Big`, `hexutil.Uint64`,
 * `common.Address`, `common.Hash` or `rpc.BlockNumberOrHash` is accepted by anvil in forms
 * geth refuses: calldata without its `0x` prefix, a quantity with leading zero digits, a block
 * number as a bare hex string. A client that only ever met anvil carries those forms into
 * production unnoticed, where geth answers `-32602 invalid argument`. The test doubles that
 * stand in for a node run every request through `require_conformant` so the mismatch fails a
 * unit test instead of a deployment.
 *
 * Rule sources (go-ethereum): `common/hexutil/json.go` (bytes and quantities),
 * `common/types.go` (fixed-length hex for addresses and hashes), `rpc/types.go`
 * (`BlockNumber` / `BlockNumberOrHash`), `internal/ethapi/transaction_args.go`
 * (`TransactionArgs` field types and the `data` / `input` and fee-field exclusions), and
 * `rpc/service.go` (positional argument counts).
 */
namespace fc::test::execution_api {

/// One way a request departs from what geth parses.
struct violation {
   std::string method;
   std::string field;
   std::string reason;
};

/// Raised by `require_conformant`; `what()` lists every violation.
class violation_exception : public std::runtime_error {
public:
   explicit violation_exception(std::vector<violation> violations, std::string description)
      : std::runtime_error(std::move(description))
      , _violations(std::move(violations)) {}

   const std::vector<violation>& violations() const { return _violations; }

private:
   std::vector<violation> _violations;
};

namespace detail {

constexpr std::string_view hex_prefix = "0x";
constexpr std::size_t      address_hex_digits = 40;
constexpr std::size_t      hash_hex_digits    = 64;
constexpr std::size_t      uint256_hex_digits = 64;
constexpr std::size_t      uint64_hex_digits  = 16;
/// `rpc.BlockNumber` is an `int64`; a quantity above this is "block number larger than int64".
constexpr std::string_view max_int64_hex = "7fffffffffffffff";

constexpr std::string_view block_tags[] = {"earliest", "latest", "pending", "safe", "finalized"};

/// `TransactionArgs` fields by the Go type geth decodes them with.
constexpr std::string_view address_fields[]  = {"from", "to"};
constexpr std::string_view uint64_fields[]   = {"gas", "nonce"};
constexpr std::string_view big_fields[]      = {"gasPrice", "maxFeePerGas", "maxPriorityFeePerGas", "value",
                                                "chainId", "maxFeePerBlobGas"};
constexpr std::string_view bytes_fields[]    = {"data", "input"};

inline bool is_hex_digits(std::string_view text) {
   return std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

inline bool has_prefix(std::string_view text) { return text.starts_with(hex_prefix); }

inline std::string_view digits_of(std::string_view text) {
   return has_prefix(text) ? text.substr(hex_prefix.size()) : text;
}

struct checker {
   std::string            method;
   std::vector<violation> out;

   void fail(std::string field, std::string reason) {
      out.push_back(violation{.method = method, .field = std::move(field), .reason = std::move(reason)});
   }

   /// `hexutil.Bytes`: `0x`-prefixed, even-length hex; `0x` alone is the empty byte string.
   void bytes(const fc::variant& value, const std::string& field) {
      if (!value.is_string()) return fail(field, "expected a hex string");
      const auto text = value.as_string();
      if (!has_prefix(text)) return fail(field, "hex string without 0x prefix");
      const auto digits = digits_of(text);
      if (!is_hex_digits(digits)) return fail(field, "invalid hex string");
      if (digits.size() % 2 != 0) fail(field, "hex string of odd length");
   }

   /// `hexutil.Big` / `hexutil.Uint64`: `0x`-prefixed, at least one digit, no leading zero
   /// digit except in `0x0`, and within the type's width.
   void quantity(const fc::variant& value, const std::string& field, std::size_t max_digits) {
      if (!value.is_string()) return fail(field, "expected a hex quantity string");
      const auto text = value.as_string();
      if (!has_prefix(text)) return fail(field, "hex string without 0x prefix");
      const auto digits = digits_of(text);
      if (digits.empty()) return fail(field, "hex number has no digits");
      if (!is_hex_digits(digits)) return fail(field, "invalid hex number");
      if (digits.size() > 1 && digits.front() == '0') return fail(field, "hex number with leading zero digits");
      if (digits.size() > max_digits) {
         fail(field, max_digits == uint64_hex_digits ? "hex number > 64 bits" : "hex number > 256 bits");
      }
   }

   /// `common.Address` / `common.Hash`: `0x`-prefixed hex of exactly `digits` digits.
   void fixed_hex(const fc::variant& value, const std::string& field, std::size_t digits, std::string_view type) {
      if (!value.is_string()) return fail(field, "expected a hex string");
      const auto text = value.as_string();
      if (!has_prefix(text)) return fail(field, "hex string without 0x prefix");
      const auto body = digits_of(text);
      if (!is_hex_digits(body)) return fail(field, "invalid hex string");
      if (body.size() != digits) {
         fail(field, "hex string has length " + std::to_string(body.size()) + ", want " + std::to_string(digits) +
                        " for " + std::string(type));
      }
   }

   void address(const fc::variant& value, const std::string& field) {
      fixed_hex(value, field, address_hex_digits, "common.Address");
   }
   void hash(const fc::variant& value, const std::string& field) {
      fixed_hex(value, field, hash_hex_digits, "common.Hash");
   }

   /// `rpc.BlockNumber`: a tag or an `int64` quantity.
   void block_number(const fc::variant& value, const std::string& field) {
      if (!value.is_string()) return fail(field, "expected a block tag or hex block number");
      const auto text = value.as_string();
      if (std::ranges::find(block_tags, text) != std::ranges::end(block_tags)) return;
      quantity(value, field, uint64_hex_digits);
      const auto digits = digits_of(text);
      if (digits.size() == uint64_hex_digits && digits > max_int64_hex) {
         fail(field, "block number larger than int64");
      }
   }

   /// `rpc.BlockNumberOrHash`: a block number, or an object naming one of
   /// `blockNumber` / `blockHash` with an optional `requireCanonical`.
   void block_number_or_hash(const fc::variant& value, const std::string& field) {
      if (!value.is_object()) return block_number(value, field);
      const auto& object = value.get_object();
      const bool  by_number = object.contains("blockNumber");
      const bool  by_hash   = object.contains("blockHash");
      if (by_number == by_hash) {
         return fail(field, by_number ? "cannot specify both BlockHash and BlockNumber"
                                      : "object names neither blockNumber nor blockHash");
      }
      if (by_number) block_number(object["blockNumber"], field + ".blockNumber");
      if (by_hash) hash(object["blockHash"], field + ".blockHash");
      if (object.contains("requireCanonical") && !object["requireCanonical"].is_bool()) {
         fail(field + ".requireCanonical", "expected a boolean");
      }
   }

   /// `internal/ethapi.TransactionArgs`, field by field, then the exclusions geth applies
   /// before executing: equal `data` and `input` when both are given, no legacy `gasPrice`
   /// next to the EIP-1559 fees, and a priority fee the max fee covers. Unknown keys pass,
   /// as Go's decoder ignores them.
   void transaction_args(const fc::variant& value, const std::string& field) {
      if (!value.is_object()) return fail(field, "expected a transaction object");
      const auto& object = value.get_object();
      for (const auto name : address_fields) {
         if (object.contains(std::string(name).c_str()) && !object[std::string(name)].is_null()) {
            address(object[std::string(name)], field + "." + std::string(name));
         }
      }
      for (const auto name : uint64_fields) {
         if (object.contains(std::string(name).c_str())) {
            quantity(object[std::string(name)], field + "." + std::string(name), uint64_hex_digits);
         }
      }
      for (const auto name : big_fields) {
         if (object.contains(std::string(name).c_str())) {
            quantity(object[std::string(name)], field + "." + std::string(name), uint256_hex_digits);
         }
      }
      for (const auto name : bytes_fields) {
         if (object.contains(std::string(name).c_str())) {
            bytes(object[std::string(name)], field + "." + std::string(name));
         }
      }
      if (object.contains("accessList") && !object["accessList"].is_array()) {
         fail(field + ".accessList", "expected an array");
      }

      const auto text_of = [&](const char* name) -> std::optional<std::string> {
         if (!object.contains(name) || !object[name].is_string()) return std::nullopt;
         return object[name].as_string();
      };
      const auto data  = text_of("data");
      const auto input = text_of("input");
      if (data && input && !same_hex(*data, *input)) {
         fail(field, "both \"data\" and \"input\" are set and not equal");
      }
      const auto gas_price = text_of("gasPrice");
      const auto max_fee   = text_of("maxFeePerGas");
      const auto tip       = text_of("maxPriorityFeePerGas");
      if (gas_price && (max_fee || tip)) {
         fail(field, "both gasPrice and (maxFeePerGas or maxPriorityFeePerGas) specified");
      }
      if (max_fee && tip && canonical_quantity(*max_fee) && canonical_quantity(*tip) && less_than(*max_fee, *tip)) {
         fail(field, "maxFeePerGas (" + *max_fee + ") < maxPriorityFeePerGas (" + *tip + ")");
      }
   }

   /// The `eth_getLogs` filter: a block range or a block hash, addresses, and topics.
   void log_filter(const fc::variant& value, const std::string& field) {
      if (!value.is_object()) return fail(field, "expected a filter object");
      const auto& object = value.get_object();
      const bool  by_hash = object.contains("blockHash");
      if (by_hash) {
         hash(object["blockHash"], field + ".blockHash");
         if (object.contains("fromBlock") || object.contains("toBlock")) {
            fail(field, "cannot specify both BlockHash and FromBlock/ToBlock");
         }
      }
      if (object.contains("fromBlock")) block_number(object["fromBlock"], field + ".fromBlock");
      if (object.contains("toBlock")) block_number(object["toBlock"], field + ".toBlock");
      if (object.contains("address")) {
         const auto& addresses = object["address"];
         if (addresses.is_array()) {
            std::size_t index = 0;
            for (const auto& entry : addresses.get_array()) {
               address(entry, field + ".address[" + std::to_string(index++) + "]");
            }
         } else {
            address(addresses, field + ".address");
         }
      }
      if (object.contains("topics")) {
         const auto& topics = object["topics"];
         if (!topics.is_array()) return fail(field + ".topics", "expected an array");
         std::size_t index = 0;
         for (const auto& topic : topics.get_array()) {
            const auto name = field + ".topics[" + std::to_string(index++) + "]";
            if (topic.is_null()) continue;
            if (topic.is_array()) {
               std::size_t inner = 0;
               for (const auto& alternative : topic.get_array()) {
                  hash(alternative, name + "[" + std::to_string(inner++) + "]");
               }
            } else {
               hash(topic, name);
            }
         }
      }
   }

   /// Positional argument count, as `rpc/service.go` reports it.
   bool arity(const fc::variant& params, std::size_t at_least, std::size_t at_most) {
      if (!params.is_array()) {
         fail("params", "non-array args");
         return false;
      }
      const auto count = params.get_array().size();
      if (count < at_least) {
         fail("params", "missing value for required argument " + std::to_string(count));
         return false;
      }
      if (count > at_most) {
         fail("params", "too many arguments, want at most " + std::to_string(at_most));
         return false;
      }
      return true;
   }

   const fc::variant& param(const fc::variant& params, std::size_t index) const {
      return params.get_array().at(index);
   }
   bool has(const fc::variant& params, std::size_t index) const { return params.get_array().size() > index; }

private:
   static bool same_hex(std::string_view lhs, std::string_view rhs) {
      return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char a, char b) {
         return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      });
   }
   /// Whether `text` is a quantity `quantity()` accepts, so two of them compare by digits.
   static bool canonical_quantity(std::string_view text) {
      const auto digits = digits_of(text);
      return has_prefix(text) && !digits.empty() && is_hex_digits(digits) &&
             (digits.size() == 1 || digits.front() != '0');
   }
   /// Numeric order of two canonical quantities: shorter is smaller, then lexicographic.
   static bool less_than(std::string_view lhs, std::string_view rhs) {
      const auto l = digits_of(lhs);
      const auto r = digits_of(rhs);
      if (l.size() != r.size()) return l.size() < r.size();
      return std::ranges::lexicographical_compare(l, r, [](unsigned char a, unsigned char b) {
         return std::tolower(a) < std::tolower(b);
      });
   }
};

} // namespace detail

/**
 * Check one JSON-RPC request's positional parameters against geth's parsing of `method`.
 *
 * Methods the relay and its client issue are covered; a method not listed yields no
 * violation, so a double for an unlisted method is not accidentally strict about a shape
 * this file never studied.
 *
 * @param method The JSON-RPC method name.
 * @param params The request's `params` value, as the client would serialize it.
 * @return Every departure from what geth accepts; empty when the request conforms.
 */
inline std::vector<violation> check_request(std::string_view method, const fc::variant& params) {
   detail::checker check{.method = std::string(method)};
   const std::string name(method);

   if (name == "eth_call" || name == "eth_estimateGas") {
      // eth_call(args, block?, stateOverride?, blockOverrides?) / eth_estimateGas(args, block?, stateOverride?)
      if (check.arity(params, 1, name == "eth_call" ? 4 : 3)) {
         check.transaction_args(check.param(params, 0), "params[0]");
         if (check.has(params, 1) && !check.param(params, 1).is_null()) {
            check.block_number_or_hash(check.param(params, 1), "params[1]");
         }
      }
   } else if (name == "eth_sendRawTransaction") {
      if (check.arity(params, 1, 1)) {
         check.bytes(check.param(params, 0), "params[0]");
         if (check.param(params, 0).is_string() && detail::digits_of(check.param(params, 0).as_string()).empty()) {
            check.fail("params[0]", "transaction could not be decoded: empty bytes");
         }
      }
   } else if (name == "eth_sendTransaction") {
      if (check.arity(params, 1, 1)) check.transaction_args(check.param(params, 0), "params[0]");
   } else if (name == "eth_getTransactionCount" || name == "eth_getBalance" || name == "eth_getCode") {
      if (check.arity(params, 2, 2)) {
         check.address(check.param(params, 0), "params[0]");
         check.block_number_or_hash(check.param(params, 1), "params[1]");
      }
   } else if (name == "eth_getStorageAt") {
      if (check.arity(params, 3, 3)) {
         check.address(check.param(params, 0), "params[0]");
         check.bytes(check.param(params, 1), "params[1]");
         check.block_number_or_hash(check.param(params, 2), "params[2]");
      }
   } else if (name == "eth_getBlockByNumber") {
      if (check.arity(params, 2, 2)) {
         check.block_number(check.param(params, 0), "params[0]");
         if (!check.param(params, 1).is_bool()) check.fail("params[1]", "expected a boolean");
      }
   } else if (name == "eth_getBlockByHash") {
      if (check.arity(params, 2, 2)) {
         check.hash(check.param(params, 0), "params[0]");
         if (!check.param(params, 1).is_bool()) check.fail("params[1]", "expected a boolean");
      }
   } else if (name == "eth_getTransactionReceipt" || name == "eth_getTransactionByHash") {
      if (check.arity(params, 1, 1)) check.hash(check.param(params, 0), "params[0]");
   } else if (name == "eth_getLogs") {
      if (check.arity(params, 1, 1)) check.log_filter(check.param(params, 0), "params[0]");
   } else if (name == "eth_blockNumber" || name == "eth_chainId" || name == "eth_gasPrice" ||
              name == "eth_maxPriorityFeePerGas" || name == "eth_syncing") {
      check.arity(params, 0, 0);
   } else if (name == "eth_feeHistory") {
      check.arity(params, 2, 3);
   }
   return std::move(check.out);
}

/**
 * Check a complete JSON-RPC request body as it would reach the node: the envelope, then
 * the method's parameters via `check_request`.
 *
 * @param json_body The HTTP request body.
 * @return Every departure from what geth accepts; empty when the request conforms.
 */
inline std::vector<violation> check_request_body(std::string_view json_body) {
   fc::variant request;
   try {
      request = fc::json::from_string(std::string(json_body));
   } catch (const std::exception& error) {
      return {violation{.method = {}, .field = "body", .reason = std::string("not JSON: ") + error.what()}};
   }
   if (!request.is_object()) return {violation{.method = {}, .field = "body", .reason = "not a JSON object"}};
   const auto& object = request.get_object();
   std::vector<violation> out;
   if (!object.contains("method") || !object["method"].is_string()) {
      out.push_back(violation{.method = {}, .field = "method", .reason = "missing or not a string"});
      return out;
   }
   const auto method = object["method"].as_string();
   // A request without an id is a notification: the node answers nothing, and a client that
   // waits for the result hangs.
   if (!object.contains("id") || object["id"].is_null()) {
      out.push_back(violation{.method = method, .field = "id", .reason = "missing: the request is a notification"});
   }
   if (object.contains("jsonrpc") && !(object["jsonrpc"].is_string() && object["jsonrpc"].as_string() == "2.0")) {
      out.push_back(violation{.method = method, .field = "jsonrpc", .reason = "expected \"2.0\""});
   }
   const fc::variant params = object.contains("params") ? object["params"] : fc::variant(fc::variants{});
   auto              rest   = check_request(method, params);
   out.insert(out.end(), rest.begin(), rest.end());
   return out;
}

/// One line per violation, for an assertion message.
inline std::string describe(const std::vector<violation>& violations) {
   std::string text;
   for (const auto& entry : violations) {
      if (!text.empty()) text += "; ";
      text += entry.method + " " + entry.field + ": " + entry.reason;
   }
   return text;
}

/**
 * Throw `violation_exception` unless `check_request` finds the request conformant.
 *
 * Test doubles that answer in place of a node call this before answering, so a request
 * geth would refuse fails the test that produced it.
 */
inline void require_conformant(std::string_view method, const fc::variant& params) {
   auto violations = check_request(method, params);
   if (violations.empty()) return;
   const auto description = "request geth would refuse: " + describe(violations);
   throw violation_exception(std::move(violations), description);
}

} // namespace fc::test::execution_api
