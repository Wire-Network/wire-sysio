
#include <atomic>
#include <boost/test/unit_test.hpp>

#include <fc-test/build_info.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>
#include <format>
#include <gsl-lite/gsl-lite.hpp>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using namespace std::literals;
using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;

namespace eth = fc::network::ethereum;
namespace fs = std::filesystem;
namespace bfs = boost::filesystem;

/* RLP encoding test data 01 */
std::pair<std::string, std::string> test_str_01{"test123", "c88774657374313233"};

/* RLP vector of encoding tests */
std::vector<std::pair<std::string, std::string>> test_str_pairs{test_str_01};

std::string test_tx_01_sig{"setNumber(uint256)"};
std::vector<fc::variant> test_tx_01_sig_params{"60"};
std::string test_tx_01_sig_encoded{"3fb5c1cb000000000000000000000000000000000000000000000000000000000000003c"};

/* RLP tx 01 */
eip1559_tx test_tx_01{.chain_id = 31337,
                      .nonce = 13,
                      .max_priority_fee_per_gas = 2000000000,
                      .max_fee_per_gas = 2000101504,
                      .gas_limit = 0x18c80,
                      .to = to_address("5FbDB2315678afecb367f032d93F642f64180aa3"),
                      .value = 0,
                      .data = fc::from_hex(test_tx_01_sig_encoded),
                      .access_list = {}};

/* RLP Encoded result of `test_tx_01` */
std::vector<std::uint8_t> test_tx_01_unsigned_result{
   0x02, 0xf8, 0x4e, 0x82, 0x7a, 0x69, 0x0d, 0x84, 0x77, 0x35, 0x94, 0x00, 0x84, 0x77, 0x37, 0x20, 0x80,
   0x83, 0x01, 0x8c, 0x80, 0x94, 0x5f, 0xbd, 0xb2, 0x31, 0x56, 0x78, 0xaf, 0xec, 0xb3, 0x67, 0xf0, 0x32,
   0xd9, 0x3f, 0x64, 0x2f, 0x64, 0x18, 0x0a, 0xa3, 0x80, 0xa4, 0x3f, 0xb5, 0xc1, 0xcb, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0xc0};


[[maybe_unused]] std::string test_tx_01_r = "93166a3ed10a4050dce7261c4ca8bcba16a1731117c453a326a1742c959b33f0";
[[maybe_unused]] std::string test_tx_01_s = "7c17a232cd69ce93f21a30579a2a94309b2d71918043134b4c5df5788078a0e4";
[[maybe_unused]] fc::uint256 test_tx_01_v = 0;

// noinspection SpellCheckingInspection
std::string test_tx_01_result = "02f84e827a690d8477359400847737208083018c80945fbdb2315678afecb367f032d93f642f64180aa380"
                                "a43fb5c1cb000000000000000000000000000000000000000000000000000000000000003cc0";

constexpr std::string_view test_contract_abi_json_file_01 = "ethereum-abi-complex-types-01.json";
} // namespace

BOOST_AUTO_TEST_SUITE(rlp_encoder)

BOOST_AUTO_TEST_CASE(can_encode_list_of_strings) try {
   for (auto& [input, expected] : test_str_pairs) {
      auto actual = rlp::encode_list({rlp::encode_string(input)});
      auto actual_hex = rlp::to_hex(actual, false);
      BOOST_CHECK_EQUAL(actual_hex, expected);
   }
}
FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(can_load_abi_json_file) try {
   using namespace fc::network::ethereum;
   auto abi_filename = fc::test::get_test_fixtures_path() / bfs::path(test_contract_abi_json_file_01);
   auto contract_abis =
      fc::network::ethereum::abi::parse_contracts(std::filesystem::path(abi_filename.generic_string()));
   BOOST_CHECK(contract_abis.size() >= 2);
}
FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(validate_contract_function_signature_and_selector) try {

   auto abi_filename = fc::test::get_test_fixtures_path() / bfs::path(test_contract_abi_json_file_01);
   auto contract_abis =
      eth::abi::parse_contracts(fs::path(abi_filename.generic_string())) |
      std::views::filter([&](auto& contract) { return contract.type == eth::abi::invoke_target_type::function; }) |
      std::ranges::to<std::vector>();

   BOOST_CHECK(contract_abis.size() >= 2);
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[0]), "submitOrder((address,uint256,bytes32))");
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[1]), "submitOrderTx((address,uint256,bytes32))");
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[2]), "submitOrders((address,uint256,bytes32)[])");
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[3]), "submitTwoOrders(((address,uint256,bytes32),string)[2])");

   auto selector0 = eth::abi::to_contract_function_selector(contract_abis[0]);
   auto selector1 = eth::abi::to_contract_function_selector(contract_abis[1]);
   auto selector2 = eth::abi::to_contract_function_selector(contract_abis[2]);
   auto selector3 = eth::abi::to_contract_function_selector(contract_abis[3]);

   BOOST_CHECK_EQUAL(selector0.str(), "6d8ea6459a37844aecf3275e70c542ca3c48d0beb70bdcd21b7c189278962ba5");
   BOOST_CHECK_EQUAL(selector1.str(), "abf88b141967fe7db2a93a75c696460f1d26e736bb943ef5b96e4fbd0abf6f97");
   BOOST_CHECK_EQUAL(selector2.str(), "034918bde7fbc6a2651f8bca060d7487f2c111355fcf04c1c97e89916b3cff69");
   BOOST_CHECK_EQUAL(selector3.str(), "790d4ee08a8ce53fca175ea82d26288d81d932b988c377e39c879410cf27eb45");

   // Test to_contract_component_signature directly
   BOOST_CHECK_EQUAL(eth::abi::to_contract_component_signature(contract_abis[0].inputs[0]), "(address,uint256,bytes32)");

   // Test with a list (manual component creation)
   eth::abi::component_type list_comp;
   list_comp.type = eth::abi::data_type::uint256;
   list_comp.list_config.is_list = true;
   BOOST_CHECK_EQUAL(eth::abi::to_contract_component_signature(list_comp), "uint256[]");

   list_comp.list_config.size = 10;
   BOOST_CHECK_EQUAL(eth::abi::to_contract_component_signature(list_comp), "uint256[10]");
}
FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(validate_contract_invoke_encode_decode) try {

   auto abi_filename = fc::test::get_test_fixtures_path() / bfs::path(test_contract_abi_json_file_01);
   auto contract_abis =
      eth::abi::parse_contracts(fs::path(abi_filename.generic_string())) |
      std::views::filter([&](auto& contract) { return contract.type == eth::abi::invoke_target_type::function; }) |
      std::ranges::to<std::vector>();

   BOOST_CHECK(contract_abis.size() >= 4);
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[0]), "submitOrder((address,uint256,bytes32))");
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[1]), "submitOrderTx((address,uint256,bytes32))");
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[2]), "submitOrders((address,uint256,bytes32)[])");
   BOOST_CHECK_EQUAL(eth::abi::to_contract_function_signature(contract_abis[3]), "submitTwoOrders(((address,uint256,bytes32),string)[2])");

   // Test 1: submitOrder with a single tuple (address, uint256, bytes32)
   // Order: maker=0x1234567890123456789012345678901234567890, amount=1000, salt=0xabcd...
   {
      fc::mutable_variant_object order;
      order("maker", "0x1234567890123456789012345678901234567890");
      order("amount", 1000);
      order("salt", "0xabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd");

      auto encoded = eth::contract_encode_data(contract_abis[0], std::vector{fc::variant(order)});

      // Expected encoding:
      // 4 bytes: function selector (first 4 bytes of keccak256("submitOrder((address,uint256,bytes32))"))
      // 32 bytes: offset to tuple data (0x20 = 32)
      // 32 bytes: address (left-padded)
      // 32 bytes: amount (1000 = 0x3e8)
      // 32 bytes: salt
      std::string expected =
         "6d8ea645"  // selector (first 4 bytes)
         "0000000000000000000000001234567890123456789012345678901234567890"  // address
         "00000000000000000000000000000000000000000000000000000000000003e8"  // amount = 1000
         "abcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd"; // salt

      BOOST_CHECK_EQUAL(encoded, expected);

      // Decode and verify
      auto decoded = eth::contract_decode_data(contract_abis[0], encoded, true);
      BOOST_REQUIRE(decoded.is_object());
      auto decoded_obj = decoded.get_object();
      BOOST_CHECK_EQUAL(decoded_obj["maker"].as_string(), "0x1234567890123456789012345678901234567890");
      BOOST_CHECK_EQUAL(decoded_obj["amount"].as_string(), "1000");
      BOOST_CHECK_EQUAL(decoded_obj["salt"].as_string(), "0xabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcdabcd");
   }

   // Test 2: submitOrders with dynamic array of tuples
   {
      fc::mutable_variant_object order1;
      order1("maker", "0x1111111111111111111111111111111111111111");
      order1("amount", 100);
      order1("salt", "0x1111111111111111111111111111111111111111111111111111111111111111");

      fc::mutable_variant_object order2;
      order2("maker", "0x2222222222222222222222222222222222222222");
      order2("amount", 200);
      order2("salt", "0x2222222222222222222222222222222222222222222222222222222222222222");

      fc::variants orders = {order1, order2};

      auto encoded = eth::contract_encode_data(contract_abis[2], std::vector{fc::variant(orders)});

      // Expected encoding for dynamic array:
      // 4 bytes: function selector
      // 32 bytes: offset to array data (0x20)
      // 32 bytes: array length (2)
      // 96 bytes: first tuple (address, amount, salt)
      // 96 bytes: second tuple (address, amount, salt)
      std::string expected =
         "034918bd"  // selector
         "0000000000000000000000000000000000000000000000000000000000000020"  // offset to array
         "0000000000000000000000000000000000000000000000000000000000000002"  // array length = 2
         "0000000000000000000000001111111111111111111111111111111111111111"  // order1.maker
         "0000000000000000000000000000000000000000000000000000000000000064"  // order1.amount = 100
         "1111111111111111111111111111111111111111111111111111111111111111"  // order1.salt
         "0000000000000000000000002222222222222222222222222222222222222222"  // order2.maker
         "00000000000000000000000000000000000000000000000000000000000000c8"  // order2.amount = 200
         "2222222222222222222222222222222222222222222222222222222222222222"; // order2.salt

      BOOST_CHECK_EQUAL(encoded, expected);

      // Decode and verify
      auto decoded = eth::contract_decode_data(contract_abis[2], encoded, true);
      BOOST_REQUIRE(decoded.is_array());
      auto decoded_arr = decoded.get_array();
      BOOST_CHECK_EQUAL(decoded_arr.size(), 2);

      auto decoded_order1 = decoded_arr[0].get_object();
      BOOST_CHECK_EQUAL(decoded_order1["maker"].as_string(), "0x1111111111111111111111111111111111111111");
      BOOST_CHECK_EQUAL(decoded_order1["amount"].as_string(), "100");
      BOOST_CHECK_EQUAL(decoded_order1["salt"].as_string(), "0x1111111111111111111111111111111111111111111111111111111111111111");

      auto decoded_order2 = decoded_arr[1].get_object();
      BOOST_CHECK_EQUAL(decoded_order2["maker"].as_string(), "0x2222222222222222222222222222222222222222");
      BOOST_CHECK_EQUAL(decoded_order2["amount"].as_string(), "200");
      BOOST_CHECK_EQUAL(decoded_order2["salt"].as_string(), "0x2222222222222222222222222222222222222222222222222222222222222222");
   }

   // Test 3: submitTwoOrders with fixed array[2] of tuples containing nested tuple and string
   {
      fc::mutable_variant_object inner_order1;
      inner_order1("maker", "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
      inner_order1("amount", 500);
      inner_order1("salt", "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

      fc::mutable_variant_object order_ex1;
      order_ex1("order", fc::variant(inner_order1));
      order_ex1("tag", "first");

      fc::mutable_variant_object inner_order2;
      inner_order2("maker", "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
      inner_order2("amount", 600);
      inner_order2("salt", "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

      fc::mutable_variant_object order_ex2;
      order_ex2("order", fc::variant(inner_order2));
      order_ex2("tag", "second");

      fc::variants orders = {fc::variant(order_ex1), fc::variant(order_ex2)};

      auto encoded = eth::contract_encode_data(contract_abis[3], std::vector{fc::variant(orders)});

      // Fixed array[2] of dynamic tuples (due to string field)
      // Each OrderEx contains: (address,uint256,bytes32) + string
      // Since OrderEx contains a string (dynamic), each element needs head/tail encoding
      std::string expected =
         "790d4ee0"  // selector
         "0000000000000000000000000000000000000000000000000000000000000020"  // offset to array
         "0000000000000000000000000000000000000000000000000000000000000040"  // offset to first OrderEx
         "0000000000000000000000000000000000000000000000000000000000000100"  // offset to second OrderEx
         // First OrderEx
         "000000000000000000000000aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"  // order.maker
         "00000000000000000000000000000000000000000000000000000000000001f4"  // order.amount = 500
         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"  // order.salt
         "0000000000000000000000000000000000000000000000000000000000000080"  // offset to tag string
         "0000000000000000000000000000000000000000000000000000000000000005"  // string length = 5
         "6669727374000000000000000000000000000000000000000000000000000000"  // "first" padded
         // Second OrderEx
         "000000000000000000000000bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"  // order.maker
         "0000000000000000000000000000000000000000000000000000000000000258"  // order.amount = 600
         "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"  // order.salt
         "0000000000000000000000000000000000000000000000000000000000000080"  // offset to tag string
         "0000000000000000000000000000000000000000000000000000000000000006"  // string length = 6
         "7365636f6e640000000000000000000000000000000000000000000000000000"; // "second" padded

      BOOST_CHECK_EQUAL(encoded, expected);

      // Decode and verify
      auto decoded = eth::contract_decode_data(contract_abis[3], encoded, true);
      BOOST_REQUIRE(decoded.is_array());
      auto decoded_arr = decoded.get_array();
      BOOST_CHECK_EQUAL(decoded_arr.size(), 2);

      auto decoded_order_ex1 = decoded_arr[0].get_object();
      BOOST_CHECK_EQUAL(decoded_order_ex1["tag"].as_string(), "first");
      auto decoded_inner_order1 = decoded_order_ex1["order"].get_object();
      BOOST_CHECK_EQUAL(decoded_inner_order1["maker"].as_string(), "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
      BOOST_CHECK_EQUAL(decoded_inner_order1["amount"].as_string(), "500");
      BOOST_CHECK_EQUAL(decoded_inner_order1["salt"].as_string(), "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

      auto decoded_order_ex2 = decoded_arr[1].get_object();
      BOOST_CHECK_EQUAL(decoded_order_ex2["tag"].as_string(), "second");
      auto decoded_inner_order2 = decoded_order_ex2["order"].get_object();
      BOOST_CHECK_EQUAL(decoded_inner_order2["maker"].as_string(), "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
      BOOST_CHECK_EQUAL(decoded_inner_order2["amount"].as_string(), "600");
      BOOST_CHECK_EQUAL(decoded_inner_order2["salt"].as_string(), "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
   }

}
FC_LOG_AND_RETHROW();

// BOOST_AUTO_TEST_CASE(can_encode_call_sig) try {
//    auto encoded_call_sig = contract_invoke_encode(test_tx_01_sig, test_tx_01_sig_params);
//    BOOST_CHECK(encoded_call_sig == test_tx_01_sig_encoded);
// } FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_CASE(can_encode_tx_01) try {
   using namespace fc::crypto;

   auto empty_msg_hash = fc::crypto::ethereum::hash_message(ethereum::to_uint8_span(""));
   auto empty_msg_hash_hex = empty_msg_hash.str();
   BOOST_CHECK("c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470" == empty_msg_hash_hex);

   auto actual_unsigned = rlp::encode_eip1559_unsigned_typed(test_tx_01);

   BOOST_CHECK(std::memcmp(actual_unsigned.data(), test_tx_01_unsigned_result.data(), 81) == 0);
   auto actual_unsigned_hex = rlp::to_hex(actual_unsigned, false);
   BOOST_CHECK_EQUAL(actual_unsigned_hex, test_tx_01_result);
}
FC_LOG_AND_RETHROW();

// Signed EIP-1559 r/s must be encoded as minimal big-endian integers.
// Strict RLP decoders (alloy-rs used by reth/anvil) reject non-minimal
// fixed-width 32-byte string encodings of r/s with "leading zero" when the
// most significant byte happens to be 0x00 — this occurs for ~1/256 signatures.
BOOST_AUTO_TEST_CASE(signed_rlp_strips_leading_zeros_in_r_and_s) try {
   auto minimal_tx = test_tx_01;

   // Craft r and s whose top byte is 0x00 — the bug-triggering case.
   bytes32 r_with_leading_zero{};
   r_with_leading_zero[0] = 0x00;
   for (std::size_t i = 1; i < r_with_leading_zero.size(); ++i)
      r_with_leading_zero[i] = static_cast<std::uint8_t>(i); // 0x01..0x1f
   bytes32 s_with_leading_zero{};
   s_with_leading_zero[0] = 0x00;
   for (std::size_t i = 1; i < s_with_leading_zero.size(); ++i)
      s_with_leading_zero[i] = static_cast<std::uint8_t>(0x20 + i - 1); // 0x20..0x3e

   minimal_tx.r = r_with_leading_zero;
   minimal_tx.s = s_with_leading_zero;
   minimal_tx.v = 1;

   auto encoded = rlp::encode_eip1559_signed_typed(minimal_tx);
   auto encoded_hex = rlp::to_hex(encoded, false);

   // The tail of the encoded tx is: access-list (c0), v (01), r, s.
   // Both r and s must be encoded as 31-byte integers (prefix 0x9f) — not
   // 32-byte strings (prefix 0xa0 00 ...).
   const std::string expected_tail =
      "c001"
      "9f" "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "9f" "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e";
   BOOST_CHECK(encoded_hex.ends_with(expected_tail));
   // And the legacy, non-minimal forms must NOT appear anywhere.
   BOOST_CHECK(encoded_hex.find("a0000102030405") == std::string::npos);
   BOOST_CHECK(encoded_hex.find("a0002021222324") == std::string::npos);
}
FC_LOG_AND_RETHROW();

// Signatures whose top byte is non-zero must still be encoded as full 32-byte
// integers (prefix 0xa0) — the minimal encoding happens to be 32 bytes wide.
BOOST_AUTO_TEST_CASE(signed_rlp_preserves_full_width_when_no_leading_zero) try {
   auto tx = test_tx_01;
   bytes32 r_full;
   bytes32 s_full;
   for (std::size_t i = 0; i < 32; ++i) {
      r_full[i] = static_cast<std::uint8_t>(0x80 + i); // starts at 0x80 — no leading zero
      s_full[i] = static_cast<std::uint8_t>(0xff - i); // starts at 0xff — no leading zero
   }
   tx.r = r_full;
   tx.s = s_full;
   tx.v = 0;

   auto encoded = rlp::encode_eip1559_signed_typed(tx);
   auto encoded_hex = rlp::to_hex(encoded, false);

   // access-list c0, v=0 encoded as empty (0x80), r (a0 + 32 bytes), s (a0 + 32 bytes).
   const std::string expected_tail =
      "c080"
      "a0" "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
      "a0" "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0";
   BOOST_CHECK(encoded_hex.ends_with(expected_tail));
}
FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
