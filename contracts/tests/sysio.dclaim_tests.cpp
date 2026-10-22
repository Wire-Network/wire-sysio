#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

/// Test fixture for funded, non-expiring DClaim imports and AuthX linking.
class sysio_dclaim_tester : public tester {
public:
   static constexpr auto DCLAIM_ACCOUNT    = "sysio.dclaim"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr uint32_t YEARS_WITHOUT_CLAIM_SEC = 3u * 365u * 24u * 60u * 60u;
   inline static const symbol WIRE_SYMBOL{9, "WIRE"};
   static constexpr int64_t CLAIM_FUNDING = 1'000'000'000;

   sysio_dclaim_tester() {
      produce_blocks(2);
      // sysio.authex is created by the tester bootstrap (account-linking
      // system account); it is signed for directly to drive linkswept, never
      // re-created here.
      create_accounts({
         DCLAIM_ACCOUNT, TOKEN_ACCOUNT, "alice"_n, "bob"_n,
      });
      produce_blocks(2);

      set_code(DCLAIM_ACCOUNT, contracts::dclaim_wasm());
      set_abi(DCLAIM_ACCOUNT, contracts::dclaim_abi().data());
      set_privileged(DCLAIM_ACCOUNT);
      produce_blocks();

      dclaim_abi_ser.set_abi(load_abi(DCLAIM_ACCOUNT),
                          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_def load_abi(name account) {
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      return abi;
   }

   action_result push(name code, abi_serializer& ser, name signer,
                       name action_name, const variant_object& data) {
      // Close a block per applied action: each OPP inbound / crank is its own
      // production transaction, and distinct TaPoS lets replay tests reach the
      // contract instead of failing chain-side as duplicate transactions.
      return sysio_system::test_support::push_contract_action_and_produce_block(
         *this, code, ser, signer, action_name, data);
   }

   action_result push_dclaim(name signer, name action_name, const variant_object& data) {
      return push(DCLAIM_ACCOUNT, dclaim_abi_ser, signer, action_name, data);
   }

   /// Seed a pre-launch credit, optionally sweeping it into the linked account.
   action_result seed_credit(const std::string& wire_account, const std::vector<char>& address, int64_t amount) {
      auto result = push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo()
         ("chain", ChainKind::CHAIN_KIND_EVM)
         ("credits", fc::variants{mvo()("native_address", address)("wire_atomic", amount)}));
      if (result != success() || wire_account.empty()) return result;
      return push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
         ("wire_account", wire_account)("chain", ChainKind::CHAIN_KIND_EVM)("native_pubkey", address));
   }

   fc::variant get_kv(name table, const char* type, uint64_t id) {
      auto data = get_row_by_id(DCLAIM_ACCOUNT, DCLAIM_ACCOUNT, table, id);
      return data.empty() ? fc::variant()
         : dclaim_abi_ser.binary_to_variant(
              type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   fc::variant pending_of(name acct)  { return get_kv("pclaims"_n,     "pending_claim", acct.to_uint64_t()); }
   fc::variant unmapped_row(uint64_t id) { return get_kv("unmapped"_n, "unmapped_token", id); }

   /// Fund DClaim with real WIRE so delayed claims exercise transfers and row erasure.
   void fund_claims() {
      const auto funding = asset{CLAIM_FUNDING, WIRE_SYMBOL};
      set_code(TOKEN_ACCOUNT, contracts::token_wasm());
      set_abi(TOKEN_ACCOUNT, contracts::token_abi().data());
      set_privileged(TOKEN_ACCOUNT);
      produce_blocks();
      base_tester::push_action(TOKEN_ACCOUNT, "create"_n, TOKEN_ACCOUNT,
         mvo()("issuer", "sysio")("maximum_supply", "1000000000.000000000 WIRE"));
      base_tester::push_action(TOKEN_ACCOUNT, "issue"_n, config::system_account_name,
         mvo()("to", "sysio")("quantity", funding)("memo", "seed"));
      base_tester::push_action(TOKEN_ACCOUNT, "transfer"_n, config::system_account_name,
         mvo()("from", "sysio")("to", DCLAIM_ACCOUNT)
              ("quantity", funding)("memo", "fund claims"));
   }

   /// Claim once after arbitrary inactivity, checking exact payment and replay rejection.
   void check_claim(name account, int64_t amount) {
      const auto before = get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, account);
      const auto funding_before = get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, DCLAIM_ACCOUNT);
      BOOST_REQUIRE_EQUAL(push_dclaim(account, "claim"_n, mvo()("wire_account", account)), success());
      BOOST_REQUIRE(pending_of(account).is_null());
      BOOST_REQUIRE_EQUAL(get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, account).get_amount(),
                          before.get_amount() + amount);
      BOOST_REQUIRE_EQUAL(get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, DCLAIM_ACCOUNT).get_amount(),
                          funding_before.get_amount() - amount);
      BOOST_REQUIRE_EQUAL(push_dclaim(account, "claim"_n, mvo()("wire_account", account)),
                          wasm_assert_msg("no pending claim"));
   }

   std::vector<char> addr20{std::vector<char>(20, char(0xA1))};

   abi_serializer dclaim_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_dclaim_tests)

/// A failed inline payout restores the entitlement; funding later permits exactly one full claim.
BOOST_FIXTURE_TEST_CASE(underfunded_claim_preserves_credit_until_full_payment, sysio_dclaim_tester) { try {
   constexpr int64_t funding = CLAIM_FUNDING;
   constexpr int64_t entitlement = funding + 1;
   const auto top_up = asset{entitlement - funding, WIRE_SYMBOL};
   fund_claims();
   BOOST_REQUIRE_EQUAL(success(), seed_credit("alice", addr20, entitlement));
   const auto before = get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, "alice"_n);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"),
      push_dclaim("alice"_n, "claim"_n, mvo()("wire_account", "alice")));
   BOOST_REQUIRE_EQUAL(entitlement, pending_of("alice"_n)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(before.get_amount(),
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, "alice"_n).get_amount());
   BOOST_REQUIRE_EQUAL(funding,
      get_currency_balance(TOKEN_ACCOUNT, WIRE_SYMBOL, DCLAIM_ACCOUNT).get_amount());

   base_tester::push_action(TOKEN_ACCOUNT, "issue"_n, config::system_account_name,
      mvo()("to", "sysio")("quantity", top_up)("memo", "complete funding"));
   base_tester::push_action(TOKEN_ACCOUNT, "transfer"_n, config::system_account_name,
      mvo()("from", "sysio")("to", DCLAIM_ACCOUNT)
           ("quantity", top_up)("memo", "complete funding"));
   check_claim("alice"_n, entitlement);
} FC_LOG_AND_RETHROW() }

/// A malformed later import entry must not leave any earlier credit in the batch, whether it created
/// a row or added to an existing one, nor consume a row identifier.
BOOST_FIXTURE_TEST_CASE(invalid_import_batch_rolls_back_all_prior_credits, sysio_dclaim_tester) { try {
   constexpr int64_t existing_credit = 1000;
   constexpr int64_t credit = 5000;
   auto second_address = addr20;
   second_address.back() = char(0xB2);
   auto third_address = addr20;
   third_address.back() = char(0xC3);
   BOOST_REQUIRE_EQUAL(success(), seed_credit("", addr20, existing_credit));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("negative wire_atomic"),
      push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo()
         ("chain", ChainKind::CHAIN_KIND_EVM)
         ("credits", fc::variants{
            mvo()("native_address", second_address)("wire_atomic", credit),
            mvo()("native_address", third_address)("wire_atomic", credit),
            mvo()("native_address", addr20)("wire_atomic", credit),
            mvo()("native_address", second_address)("wire_atomic", -1)})));
   BOOST_REQUIRE_EQUAL(existing_credit, unmapped_row(1)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE(unmapped_row(2).is_null());
   BOOST_REQUIRE(unmapped_row(3).is_null());
   BOOST_REQUIRE_EQUAL(success(), seed_credit("", second_address, credit));
   BOOST_REQUIRE_EQUAL(credit, unmapped_row(2)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE(unmapped_row(3).is_null());
} FC_LOG_AND_RETHROW() }

/// The byte-oriented ledger resolves prefix collisions and isolates identical payloads by chain kind.
/// This drives linkswept as AuthEx; authenticated, chain-valid wallet linking is covered separately.
BOOST_FIXTURE_TEST_CASE(import_sweep_matches_full_identity_and_chain, sysio_dclaim_tester) { try {
   constexpr int64_t first_credit = 1000;
   constexpr int64_t second_credit = 2000;
   constexpr int64_t svm_credit = 3000;
   auto second_address = addr20;
   second_address.back() = char(0xB2);
   BOOST_REQUIRE_EQUAL(success(), seed_credit("", addr20, first_credit));
   BOOST_REQUIRE_EQUAL(success(), seed_credit("", second_address, second_credit));
   BOOST_REQUIRE_EQUAL(success(), seed_credit("", second_address, second_credit));
   BOOST_REQUIRE_EQUAL(success(), push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo()
      ("chain", ChainKind::CHAIN_KIND_SVM)
      ("credits", fc::variants{mvo()("native_address", addr20)("wire_atomic", svm_credit)})));
   BOOST_REQUIRE_EQUAL(success(), push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "bob")("chain", ChainKind::CHAIN_KIND_EVM)("native_pubkey", second_address)));
   BOOST_REQUIRE_EQUAL(first_credit, unmapped_row(1)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE(unmapped_row(2).is_null());
   BOOST_REQUIRE_EQUAL(svm_credit, unmapped_row(3)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(2 * second_credit, pending_of("bob"_n)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(success(), push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "alice")("chain", ChainKind::CHAIN_KIND_SVM)("native_pubkey", addr20)));
   BOOST_REQUIRE_EQUAL(svm_credit, pending_of("alice"_n)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE(unmapped_row(3).is_null());
   BOOST_REQUIRE_EQUAL(success(), push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "alice")("chain", ChainKind::CHAIN_KIND_EVM)("native_pubkey", addr20)));
   BOOST_REQUIRE(unmapped_row(1).is_null());
   fund_claims();
   check_claim("alice"_n, first_credit + svm_credit);
   check_claim("bob"_n, 2 * second_credit);
} FC_LOG_AND_RETHROW() }

// -- config / import surface --

BOOST_FIXTURE_TEST_CASE(setconfig_initializes_singleton, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "setconfig"_n, mvo{}), success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(claim_rejects_empty_ledger, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_NE(push_dclaim("alice"_n, "claim"_n, mvo()("wire_account", "alice")), success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_accepts_credit_batch, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("credits", fc::variants{ mvo()("native_address", addr20)("wire_atomic", int64_t{982953049502}) })),
      success());
   // Pre-launch import lands as an unmapped balance (unlinked by definition).
   auto u = unmapped_row(1);
   BOOST_REQUIRE(!u.is_null());
   BOOST_REQUIRE_EQUAL(u["balance"].as_string().substr(0, 3), std::string("982"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_rejects_negative_atomic, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_NE(push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("credits", fc::variants{ mvo()("native_address", addr20)("wire_atomic", -1) })),
      success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importdone_locks_subsequent_importseed, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "importdone"_n, mvo{}), success());
   BOOST_REQUIRE_NE(push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("credits", fc::variants{ mvo()("native_address", addr20)("wire_atomic", 1) })),
      success());
} FC_LOG_AND_RETHROW() }

// -- pre-launch import routing --


BOOST_FIXTURE_TEST_CASE(importseed_linked_credits_pending_claims, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(
      seed_credit("alice", addr20, 1000),
      success());
   // Imported WIRE is credited verbatim.
   auto p = pending_of("alice"_n);
   BOOST_REQUIRE(!p.is_null());
   BOOST_REQUIRE_EQUAL(p["balance"].as<asset>().get_amount(), 1000);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_unlinked_parks_unmapped_then_linkswept, sysio_dclaim_tester) { try {
   // Empty wire account -> parked in unmapped by native address.
   BOOST_REQUIRE_EQUAL(
      seed_credit("", addr20, 5000),
      success());
   BOOST_REQUIRE(pending_of("bob"_n).is_null());
   auto u = unmapped_row(1);
   BOOST_REQUIRE(!u.is_null());
   BOOST_REQUIRE_EQUAL(u["balance"].as<asset>().get_amount(), 5000);

   // AuthX link sweeps it into pending_claims for bob.
   BOOST_REQUIRE_EQUAL(
      push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
         ("wire_account", "bob")
         ("chain", ChainKind::CHAIN_KIND_EVM)
         ("native_pubkey", addr20)),
      success());
   BOOST_REQUIRE(unmapped_row(1).is_null());
   BOOST_REQUIRE_EQUAL(pending_of("bob"_n)["balance"].as<asset>().get_amount(), 5000);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(linkswept_preserves_old_unmapped_imports, sysio_dclaim_tester) { try {
   fund_claims();
   BOOST_REQUIRE_EQUAL(seed_credit("", addr20, 5000), success());
   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   BOOST_REQUIRE_EQUAL(push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "bob")
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("native_pubkey", addr20)), success());

   const auto pending = pending_of("bob"_n);
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 5000);
   BOOST_REQUIRE(unmapped_row(1).is_null());
   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   check_claim("bob"_n, 5000);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(linkswept_adds_old_imports_to_newer_pending_balance, sysio_dclaim_tester) { try {
   fund_claims();
   const std::vector<char> newer_addr(20, char(0xB2));

   BOOST_REQUIRE_EQUAL(seed_credit("", addr20, 5000), success());
   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   BOOST_REQUIRE_EQUAL(seed_credit("bob", newer_addr, 2000), success());

   BOOST_REQUIRE_EQUAL(push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "bob")
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("native_pubkey", addr20)), success());

   const auto pending = pending_of("bob"_n);
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 7000);
   BOOST_REQUIRE(unmapped_row(1).is_null());
   check_claim("bob"_n, 7000);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(linkswept_adds_newer_imports_to_old_pending_balance, sysio_dclaim_tester) { try {
   fund_claims();
   const std::vector<char> newer_addr(20, char(0xB2));

   BOOST_REQUIRE_EQUAL(seed_credit("bob", addr20, 2000), success());
   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   BOOST_REQUIRE_EQUAL(seed_credit("", newer_addr, 5000), success());

   BOOST_REQUIRE_EQUAL(push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "bob")
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("native_pubkey", newer_addr)), success());

   const auto pending = pending_of("bob"_n);
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 7000);
   BOOST_REQUIRE(unmapped_row(2).is_null());
   check_claim("bob"_n, 7000);
} FC_LOG_AND_RETHROW() }

/// Sweeping multiple imported identities into one account saturates without aborting its OPP parent.
BOOST_FIXTURE_TEST_CASE(linkswept_caps_combined_imports_without_aborting, sysio_dclaim_tester) { try {
   constexpr int64_t headroom = 5;
   const std::vector<char> second_address(20, char(0xB2));
   BOOST_REQUIRE_EQUAL(success(), seed_credit("bob", addr20, asset::max_amount - headroom));
   BOOST_REQUIRE_EQUAL(success(), seed_credit("", second_address, headroom + 1));
   BOOST_REQUIRE_EQUAL(success(), push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "bob")("chain", ChainKind::CHAIN_KIND_EVM)("native_pubkey", second_address)));
   BOOST_REQUIRE_EQUAL(asset::max_amount, pending_of("bob"_n)["balance"].as<asset>().get_amount());
   BOOST_REQUIRE(unmapped_row(2).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(imported_balances_remain_claimable_after_years, sysio_dclaim_tester) { try {
   fund_claims();
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "importseed"_n, mvo()
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("credits", fc::variants{mvo()("native_address", addr20)("wire_atomic", int64_t{5000})})), success());
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "importdone"_n, mvo{}), success());
   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   BOOST_REQUIRE_EQUAL(push_dclaim(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
      ("wire_account", "bob")
      ("chain", ChainKind::CHAIN_KIND_EVM)
      ("native_pubkey", addr20)), success());
   BOOST_REQUIRE(unmapped_row(1).is_null());
   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   check_claim("bob"_n, 5000);
} FC_LOG_AND_RETHROW() }



// -- indefinitely claimable balances --

BOOST_FIXTURE_TEST_CASE(pending_imports_remain_claimable_after_years, sysio_dclaim_tester) { try {
   fund_claims();
   BOOST_REQUIRE_EQUAL(
      seed_credit("alice", addr20, 1000),
      success());
   BOOST_REQUIRE(!pending_of("alice"_n).is_null());

   produce_block(fc::seconds(YEARS_WITHOUT_CLAIM_SEC));
   BOOST_REQUIRE_EQUAL(pending_of("alice"_n)["balance"].as<asset>().get_amount(), 1000);
   BOOST_REQUIRE_NE(push_dclaim("bob"_n, "claim"_n, mvo()("wire_account", "alice")), success());
   check_claim("alice"_n, 1000);
} FC_LOG_AND_RETHROW() }


BOOST_AUTO_TEST_SUITE_END()
