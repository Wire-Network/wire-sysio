/**
 * The Ethereum client against a real go-ethereum node.
 *
 * Every other test in this binary answers in place of a node. This one starts `geth --dev`
 * and drives the client's RPC surface through it: what geth parses, what it answers, and the
 * shape of its refusals — the things anvil is lenient about and a production endpoint is not.
 *
 * The node is `geth` on `PATH`, or the binary (or a wrapper that execs one, such as
 * `docker run --rm --network host ethereum/client-go:stable "$@"`) that `WIRE_GETH_BIN` names.
 * Neither being present FAILS the case: a conformance test that can quietly stop running is
 * the gap that let a geth-refused request ship. The CI platform images install geth
 * (`.cicd/platforms/*.Dockerfile`), the macOS job through Homebrew, and the devcontainer
 * alongside Foundry. The node's HTTP port comes from this process's registry lease.
 */
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/test/unit_test.hpp>

#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_transaction_policy.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include "startup_rpc_server.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace bp  = boost::process::v1;
namespace eth = fc::network::ethereum;
using namespace fc::crypto;
using namespace fc::network::ethereum;
using sysio::outpost_ethereum_client_detail::classify_epoch_in_revert;
using sysio::outpost_ethereum_client_detail::epoch_in_revert;

constexpr auto geth_binary_variable = "WIRE_GETH_BIN";
constexpr auto geth_binary_name     = "geth";
/// `geth --dev`'s chain id.
constexpr uint32_t geth_dev_chain_id = 1337;
/// Blocks every second, so a receipt wait is short and the chain reaches finality during the case.
constexpr auto geth_dev_period_seconds = "1";
/// How long the node may take to answer its first RPC (a container image starts slowly).
constexpr std::chrono::seconds node_startup_budget{120};
constexpr std::chrono::milliseconds node_poll_interval{250};
/// One receipt wait: generous, the chain mines every second.
constexpr fc::microseconds receipt_wait = fc::seconds(60);
/// What the dev account hands the test signer: one ether.
constexpr auto funding_wei_hex = "0xde0b6b3a7640000";
/// The refusal `OPPInbound.epochIn` raises for an epoch out of order, which the relay classifies
/// (`outpost_ethereum_client.cpp`). The contract below raises it from every call.
constexpr auto non_sequential_epoch_signature = "OPP_NonSequentialEpoch(uint32,uint32)";
/// Execution APIs' code for a call that reverted (geth's `3`), the shape the relay keys on.
constexpr int execution_reverted_code = 3;
constexpr size_t evm_word_hex_chars = 64;
constexpr size_t selector_hex_chars = 8;

/**
 * EVM bytecode deploying a contract whose every call reverts with `signature`'s selector followed
 * by two zero words — the exact shape of `OPP_NonSequentialEpoch(uint32,uint32)`.
 *
 * Runtime (16 bytes): `PUSH4 selector; PUSH1 0xe0; SHL; PUSH1 0; MSTORE` leaves the selector in
 * the top four bytes of memory word 0 with words 1 and 2 zero; `PUSH1 0x44; PUSH1 0; REVERT`
 * returns those 68 bytes. Initcode (12 bytes) copies the runtime after itself into memory and
 * returns it.
 */
std::string always_reverting_contract_initcode(std::string_view signature) {
   const std::string selector =
      fc::crypto::keccak256::hash(std::string(signature)).str().substr(0, selector_hex_chars);
   const std::string runtime = "63" + selector + "60e01b60005260446000fd";
   BOOST_REQUIRE_EQUAL(runtime.size(), 2 * 16);
   const std::string initcode = "6010600c600039" "60106000f3";
   BOOST_REQUIRE_EQUAL(initcode.size(), 2 * 12);
   return "0x" + initcode + runtime;
}

/// `function ping()`: calldata for a call that carries nothing.
eth::abi::contract ping_abi() {
   return eth::abi::contract{
      .name = "ping", .type = eth::abi::invoke_target_type::function, .inputs = {}, .outputs = {}};
}

signature_provider_ptr local_signer() {
   const auto key      = private_key::generate(private_key::key_type::em);
   auto       provider = std::make_shared<signature_provider_t>();
   provider->target_chain = chain_kind_ethereum;
   provider->key_type     = chain_key_type_ethereum;
   provider->key_name     = "geth-dev-signer";
   provider->public_key   = key.get_public_key();
   provider->private_key  = key;
   provider->sign         = [key](const fc::sha256& digest) { return key.sign(digest); };
   return provider;
}

ethereum_transaction_policy wide_policy() {
   return ethereum_transaction_policy{
      .client_id                = "geth-dev",
      .chain_id                 = geth_dev_chain_id,
      .max_priority_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_fee_per_gas          = maximum_ethereum_transaction_policy_value(),
      .max_gas_limit            = maximum_ethereum_transaction_policy_value(),
      .max_total_native_cost    = maximum_ethereum_transaction_policy_value(),
   };
}

/// The geth to run: `WIRE_GETH_BIN` when set, else `geth` on `PATH`. Empty when there is neither.
std::string geth_binary() {
   if (const char* named = std::getenv(geth_binary_variable); named != nullptr && *named != '\0') {
      return named;
   }
   return bp::search_path(geth_binary_name).string();
}

/// A `geth --dev` node on a registry port, stopped on destruction; its output goes to `log_path`.
struct geth_dev_node {
   explicit geth_dev_node(const std::string& binary)
      : port(startup_test::next_port())
      , workdir(std::filesystem::temp_directory_path() / ("wire-geth-dev-" + std::to_string(port)))
      , log_path(workdir / "geth.log") {
      std::filesystem::create_directories(workdir);
      process = bp::child(binary, "--dev", "--dev.period", geth_dev_period_seconds, "--http", "--http.addr",
                          "127.0.0.1", "--http.port", std::to_string(port), "--http.api", "eth,net,web3",
                          "--nodiscover", "--maxpeers", "0", "--ipcdisable", "--datadir", workdir.string(),
                          bp::std_out > log_path.string(), bp::std_err > log_path.string());
   }
   ~geth_dev_node() {
      if (process.running()) {
         process.terminate();
         process.wait();
      }
      std::error_code ignored;
      std::filesystem::remove_all(workdir, ignored);
   }

   std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

   /// Block until the node answers `eth_chainId` with the dev chain id.
   void await_ready() {
      const auto deadline = std::chrono::steady_clock::now() + node_startup_budget;
      fc::network::json_rpc::json_rpc_client probe{fc::url(url())};
      while (std::chrono::steady_clock::now() < deadline) {
         BOOST_REQUIRE_MESSAGE(process.running(), "geth exited before serving RPC; see " << log_path);
         try {
            const auto chain_id = probe.call("eth_chainId");
            if (chain_id.is_string() &&
                parse_rpc_quantity(chain_id, "chain_id") == fc::uint256{geth_dev_chain_id}) {
               return;
            }
         } catch (const fc::exception&) {
            // Not listening yet.
         }
         std::this_thread::sleep_for(node_poll_interval);
      }
      BOOST_FAIL("geth did not serve RPC within the startup budget; see " << log_path);
   }

   uint16_t              port;
   std::filesystem::path workdir;
   std::filesystem::path log_path;
   bp::child             process;
};

ethereum_confirm_options confirm_within(fc::microseconds budget) {
   ethereum_confirm_options options = ethereum_confirm_option_defaults;
   options.retry.total_timeout      = budget;
   return options;
}

} // namespace

BOOST_AUTO_TEST_SUITE(geth_dev_conformance)

BOOST_AUTO_TEST_CASE(the_client_speaks_geth) try {
   const std::string binary = geth_binary();
   BOOST_REQUIRE_MESSAGE(!binary.empty(), "no geth: install go-ethereum so `geth` is on PATH, or set "
                                              << geth_binary_variable << " to a geth binary or a wrapper");

   geth_dev_node node{binary};
   node.await_ready();
   auto client = std::make_shared<ethereum_client>(local_signer(), node.url(), wide_policy());
   const std::string signer_hex = fc::to_hex(client->get_signer_address(), true);

   // The dev account funds the signer. `eth_sendTransaction` is the node's own signing path,
   // used only to seed; everything after goes through the client's.
   const auto accounts = client->execute("eth_accounts", fc::variants{});
   BOOST_REQUIRE(accounts.is_array() && !accounts.get_array().empty());
   const std::string dev_account = accounts.get_array().front().as_string();
   const auto funding_hash = client->execute(
      "eth_sendTransaction",
      fc::variants{fc::mutable_variant_object("from", dev_account)("to", signer_hex)("value", funding_wei_hex)});
   (void) client->wait_for_receipt(funding_hash.as_string(), confirm_within(receipt_wait));

   // A contract that reverts every call with the relay's non-sequential-epoch error.
   const auto deploy_hash = client->execute(
      "eth_sendTransaction",
      fc::variants{fc::mutable_variant_object("from", dev_account)(
         "data", always_reverting_contract_initcode(non_sequential_epoch_signature))});
   const auto deploy_receipt = client->wait_for_receipt(deploy_hash.as_string(), confirm_within(receipt_wait));
   const std::string reverter = deploy_receipt.get_object()["contractAddress"].as_string();
   BOOST_REQUIRE(!reverter.empty());
   const auto reverter_address = fc::crypto::ethereum::to_address(reverter);

   // eth_call: calldata the node parses, a call it executes, and a revert in the shape the
   // relay classifies — code 3 with the error bytes in `data`.
   {
      const auto quiet = client->execute_contract_view_fn(client->get_signer_address(), ping_abi(),
                                                          block_tag_t::latest, {});
      BOOST_CHECK_EQUAL(quiet.as_string(), "0x");   // an account without code returns nothing

      bool refused = false;
      try {
         (void) client->execute_contract_view_fn(reverter_address, ping_abi(), block_tag_t::latest, {});
      } catch (const fc::network::json_rpc::json_rpc_error& refusal) {
         refused = true;
         BOOST_CHECK_EQUAL(refusal.code, execution_reverted_code);
         BOOST_REQUIRE(refusal.data.is_string());
         BOOST_CHECK_EQUAL(refusal.data.as_string().size(), 2 + selector_hex_chars + 2 * evm_word_hex_chars);
         const auto classified = classify_epoch_in_revert(refusal.code, refusal.data.as_string());
         BOOST_REQUIRE(classified.has_value());
         BOOST_CHECK(*classified == epoch_in_revert::non_sequential_epoch);
      }
      BOOST_CHECK(refused);
   }

   // eth_estimateGas: the pre-flight the relay funds `epochIn` from — refused the same way for a
   // reverting call, answered for one that runs, and bounded by a `gas` cap the node honours.
   {
      bool refused = false;
      try {
         (void) client->estimate_gas(reverter_address, ping_abi(), fc::variants{});
      } catch (const fc::network::json_rpc::json_rpc_error& refusal) {
         refused = true;
         BOOST_CHECK_EQUAL(refusal.code, execution_reverted_code);
         BOOST_CHECK(classify_epoch_in_revert(refusal.code, refusal.data.as_string()) ==
                     epoch_in_revert::non_sequential_epoch);
      }
      BOOST_CHECK(refused);
      const auto estimate = client->estimate_gas(client->get_signer_address(), ping_abi(), fc::variants{});
      BOOST_CHECK_GE(estimate, fc::uint256{21'000});
      constexpr uint64_t gas_cap = 100'000;
      BOOST_CHECK_LE(client->estimate_gas(client->get_signer_address(), ping_abi(), fc::variants{}, std::nullopt,
                                          gas_cap),
                     fc::uint256{gas_cap});
   }

   // The signed path end to end: fees from the node, the nonce at `pending`, a transaction the
   // node accepts as sent, and a receipt the client reads back.
   {
      BOOST_CHECK_EQUAL(client->get_transaction_count(client->get_signer_address(), block_tag_t::pending),
                        fc::uint256{0});
      const auto fees = client->get_gas_config();
      BOOST_CHECK_GE(fees.max_fee_per_gas, fees.tip);
      auto tx = client->create_default_tx(client->get_signer_address(), ping_abi(), fc::variants{}, 0, 0);
      BOOST_CHECK_EQUAL(tx.chain_id, fc::uint256{geth_dev_chain_id});
      const auto signed_tx = client->sign_transaction(tx, "ping");
      BOOST_CHECK(signed_tx.raw_hex.starts_with("0x02"));
      const auto reported = client->send_raw_transaction(signed_tx.raw_hex);
      // The node names the transaction by the hash the client computed before sending.
      BOOST_CHECK_EQUAL(reported, signed_tx.tx_hash);
      const auto receipt = client->wait_for_receipt(reported, confirm_within(receipt_wait));
      BOOST_CHECK_EQUAL(receipt.get_object()["status"].as_string(), "0x1");
      BOOST_CHECK_EQUAL(client->get_transaction_count(client->get_signer_address(), block_tag_t::latest),
                        fc::uint256{1});
      // Reads pinned to the receipt's block, as the relay makes them, are served.
      const auto at_receipt = client->get_block_by_number(
         format_rpc_quantity(parse_rpc_quantity(receipt.get_object()["blockNumber"], "block")));
      BOOST_CHECK(at_receipt.contains("number"));
   }

   // The read confirmation: `finalized` and `safe` are served and trail the head, and the
   // default confirmation resolves to a block the node has.
   {
      const auto head      = client->get_block_number();
      const auto finalized = client->get_block_by_number(block_tag_t::finalized);
      const auto safe      = client->get_block_by_number(block_tag_t::safe);
      BOOST_CHECK_LE(parse_rpc_quantity(finalized["number"], "finalized"), head);
      BOOST_CHECK_LE(parse_rpc_quantity(safe["number"], "safe"), head);
      const auto read = client->resolve_read_block(ethereum_read_confirmation_defaults);
      const auto at   = client->get_block_by_number(read.block);
      BOOST_CHECK_LE(parse_rpc_quantity(at["number"], "read"), head);
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
