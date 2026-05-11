#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;

class sysio_reserve_tester : public tester {
public:
   static constexpr auto RESERVE_ACCOUNT = "sysio.reserv"_n;
   static constexpr auto MSGCH_ACCOUNT   = "sysio.msgch"_n;

   sysio_reserve_tester() {
      produce_blocks(2);
      create_accounts({RESERVE_ACCOUNT, MSGCH_ACCOUNT});
      produce_blocks(2);

      set_code(RESERVE_ACCOUNT, contracts::reserve_wasm());
      set_abi(RESERVE_ACCOUNT, contracts::reserve_abi().data());
      set_privileged(RESERVE_ACCOUNT);
      produce_blocks();

      const auto* accnt = control->find_account_metadata(RESERVE_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);
      action act;
      act.account = RESERVE_ACCOUNT;
      act.name = action_name;
      act.data = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      act.authorization = vector<permission_level>{{signer, config::active_name}};

      signed_transaction trx;
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      trx.sign(get_private_key(signer, "active"), control->get_chain_id());
      try {
         push_transaction(trx);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   /// Helper: provision an LP via setlp.
   action_result setlp(ChainKind chain, TokenKind token,
                       uint64_t reserve_paired, uint64_t reserve_wire,
                       uint32_t weight = 5000) {
      return push_action(RESERVE_ACCOUNT, "setlp"_n, mvo()
         ("chain", chain)
         ("paired_token", token)
         ("reserve_paired", reserve_paired)
         ("reserve_wire", reserve_wire)
         ("connector_weight_bps", weight));
   }

   action_result creditlp(name signer, ChainKind chain, TokenKind token,
                          uint64_t paired_amount, uint64_t wire_amount) {
      return push_action(signer, "creditlp"_n, mvo()
         ("chain", chain)
         ("paired_token", token)
         ("paired_amount", paired_amount)
         ("wire_amount", wire_amount));
   }

   /// Pack the (chain_kind, paired_token) composite that `lp_key.chain_token`
   /// stores. Mirrors `sysio::reserve::pack_chain_token` so the row lookup
   /// below uses the same key the contract emplaced under.
   ///   - ChainKind::ETHEREUM = 2  -> high 32 bits
   ///   - ChainKind::SOLANA   = 3
   ///   - TokenKind::ETH      = 256 -> low 32 bits
   static uint64_t pack(uint32_t chain_kind, uint32_t token_kind) {
      return (static_cast<uint64_t>(chain_kind) << 32) | static_cast<uint64_t>(token_kind);
   }

   fc::variant get_lp(uint64_t chain_token_key) {
      auto data = get_row_by_id(RESERVE_ACCOUNT, RESERVE_ACCOUNT, "lps"_n, chain_token_key);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "lp_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_serializer abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_reserve_tests)

// ── setlp ──

BOOST_FIXTURE_TEST_CASE(setlp_creates_lp_row, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setlp(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH,
            /*reserve_paired*/ 1'000'000, /*reserve_wire*/ 2'000'000));

   // ChainKind::ETHEREUM = 2; TokenKind::ETH = 256
   auto lp = get_lp(pack(2, 256));
   BOOST_REQUIRE(!lp.is_null());
   BOOST_REQUIRE(ChainKind::CHAIN_KIND_ETHEREUM == lp["chain"].as<ChainKind>());
   BOOST_REQUIRE(TokenKind::TOKEN_KIND_ETH      == lp["paired_token"].as<TokenKind>());
   BOOST_REQUIRE_EQUAL(1'000'000, lp["reserve_paired"].as_uint64());
   BOOST_REQUIRE_EQUAL(2'000'000, lp["reserve_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(5000,      lp["connector_weight_bps"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setlp_updates_existing_row_in_place, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setlp(ChainKind::CHAIN_KIND_SOLANA, TokenKind::TOKEN_KIND_SOL, 100, 200, 5000));

   // Re-call updates the same row (composite key matches).
   BOOST_REQUIRE_EQUAL(success(),
      setlp(ChainKind::CHAIN_KIND_SOLANA, TokenKind::TOKEN_KIND_SOL, 999, 1234, 6000));

   // ChainKind::SOLANA = 3; TokenKind::SOL = 512
   auto lp = get_lp(pack(3, 512));
   BOOST_REQUIRE(!lp.is_null());
   BOOST_REQUIRE_EQUAL(999,  lp["reserve_paired"].as_uint64());
   BOOST_REQUIRE_EQUAL(1234, lp["reserve_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(6000, lp["connector_weight_bps"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setlp_rejects_wire_paired_with_wire, sysio_reserve_tester) { try {
   // The WIRE/WIRE LP is degenerate — every LP is implicitly paired with
   // WIRE on the depot side.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: WIRE/WIRE LP is degenerate; nothing to provision"),
      setlp(ChainKind::CHAIN_KIND_WIRE, TokenKind::TOKEN_KIND_WIRE, 100, 100));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setlp_rejects_invalid_connector_weight, sysio_reserve_tester) { try {
   // weight must be in (0, 10000].
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: connector_weight_bps must be in (0, 10000]"),
      setlp(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 100, 100, 0));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: connector_weight_bps must be in (0, 10000]"),
      setlp(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 100, 100, 10001));
} FC_LOG_AND_RETHROW() }

// ── creditlp ──

BOOST_FIXTURE_TEST_CASE(creditlp_requires_msgch_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setlp(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000, 1000));

   // Credit-LP is auth=msgch (STAKING_REWARD dispatch).
   // A different signer should fail.
   BOOST_REQUIRE(creditlp(RESERVE_ACCOUNT,
      ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 100, 50)
      .find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(creditlp_grows_reserves, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setlp(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000, 1000));
   BOOST_REQUIRE_EQUAL(success(),
      creditlp(MSGCH_ACCOUNT,
               ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 100, 50));

   auto lp = get_lp(pack(2, 256));
   BOOST_REQUIRE_EQUAL(1100, lp["reserve_paired"].as_uint64());
   BOOST_REQUIRE_EQUAL(1050, lp["reserve_wire"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(creditlp_rejects_unknown_lp, sysio_reserve_tester) { try {
   // No setlp first — creditlp should reject because no LP row exists.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: LP not provisioned for this (chain, paired_token)"),
      creditlp(MSGCH_ACCOUNT,
               ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 100, 50));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
