#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

/// Test fixture for sysio.cap. Deploys sysio.cap plus sysio.reserv (so the
/// reward leg can price native -> WIRE off its published reserve table) and
/// creates sysio.msgch / sysio.authex as the authorized inbound callers.
class sysio_cap_tester : public tester {
public:
   static constexpr auto CAP_ACCOUNT    = "sysio.cap"_n;
   static constexpr auto RESERV_ACCOUNT = "sysio.reserv"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;

   sysio_cap_tester() {
      produce_blocks(2);
      // sysio.authex is created by the tester bootstrap (account-linking
      // system account); it is signed for directly to drive linkswept, never
      // re-created here.
      create_accounts({
         CAP_ACCOUNT, RESERV_ACCOUNT, MSGCH_ACCOUNT,
         TOKEN_ACCOUNT, "alice"_n, "bob"_n,
      });
      produce_blocks(2);

      set_code(CAP_ACCOUNT, contracts::cap_wasm());
      set_abi(CAP_ACCOUNT, contracts::cap_abi().data());
      set_privileged(CAP_ACCOUNT);

      set_code(RESERV_ACCOUNT, contracts::reserve_wasm());
      set_abi(RESERV_ACCOUNT, contracts::reserve_abi().data());
      set_privileged(RESERV_ACCOUNT);
      produce_blocks();

      cap_abi_ser.set_abi(load_abi(CAP_ACCOUNT),
                          abi_serializer::create_yield_function(abi_serializer_max_time));
      reserv_abi_ser.set_abi(load_abi(RESERV_ACCOUNT),
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
      try {
         string atype = ser.get_action_type(action_name);
         action act;
         act.account = code;
         act.name    = action_name;
         act.data    = ser.variant_to_binary(
            atype, data, abi_serializer::create_yield_function(abi_serializer_max_time));
         act.authorization = vector<permission_level>{{signer, config::active_name}};
         signed_transaction trx;
         trx.actions.emplace_back(std::move(act));
         set_transaction_headers(trx);
         trx.sign(get_private_key(signer, "active"), control->get_chain_id());
         push_transaction(trx);
         // Close a block per applied action: each OPP inbound / crank is its
         // own transaction in production, and distinct TaPoS makes an
         // intentional replay (e.g. the dedupe test) a new transaction that
         // actually reaches the contract instead of being rejected chain-side
         // as a duplicate.
         produce_block();
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result push_cap(name signer, name action_name, const variant_object& data) {
      return push(CAP_ACCOUNT, cap_abi_ser, signer, action_name, data);
   }

   /// Provision a reserve so reserve::quote returns a non-zero WIRE amount.
   /// Field shape mirrors sysio.reserv::setreserve's flat ABI (chain,
   /// outpost_kind, outpost_amount:uint64, wire_amount:uint64,
   /// connector_weight_bps:uint32) — not a packed TokenAmount.
   action_result setreserve(ChainKind chain, TokenKind outpost_kind,
                            int64_t outpost_amount, int64_t wire_amount,
                            uint32_t weight = 5000) {
      return push(RESERV_ACCOUNT, reserv_abi_ser, RESERV_ACCOUNT, "setreserve"_n, mvo()
         ("chain", chain)
         ("outpost_kind", outpost_kind)
         ("outpost_amount", static_cast<uint64_t>(outpost_amount))
         ("wire_amount",    static_cast<uint64_t>(wire_amount))
         ("connector_weight_bps", weight));
   }

   /// Dispatch a STAKING_REWARD per-staker body to cap::onreward.
   action_result onreward(name signer, uint64_t outpost_id,
                          const std::string& wire_account, ChainKind chain,
                          const std::vector<char>& native_addr, TokenKind kind,
                          uint64_t amount, uint32_t epoch_index,
                          uint64_t external_epoch_ref, uint32_t share_bps = 10000) {
      return push_cap(signer, "onreward"_n, mvo()
         ("outpost_id", outpost_id)
         ("staker_wire_account", wire_account)
         ("reward_chain", chain)
         ("staker_native_addr", native_addr)
         ("reward_kind", kind)
         ("reward_amount", amount)
         ("reward_epoch_index", epoch_index)
         ("external_epoch_ref", external_epoch_ref)
         ("share_bps", share_bps));
   }

   fc::variant get_kv(name table, const char* type, uint64_t id) {
      auto data = get_row_by_id(CAP_ACCOUNT, CAP_ACCOUNT, table, id);
      return data.empty() ? fc::variant()
         : cap_abi_ser.binary_to_variant(
              type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   fc::variant pending_of(name acct)  { return get_kv("pclaims"_n,     "pending_claim", acct.to_uint64_t()); }
   fc::variant unmapped_row(uint64_t id) { return get_kv("unmapped"_n, "unmapped_token", id); }
   fc::variant stage_row(uint64_t id)    { return get_kv("nativestage"_n, "native_stage", id); }
   fc::variant cursor_row(uint64_t id)   { return get_kv("rwdcursors"_n, "reward_cursor", id); }

   std::vector<char> addr20{std::vector<char>(20, char(0xA1))};

   abi_serializer cap_abi_ser;
   abi_serializer reserv_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_cap_tests)

// ── existing config / import surface ──

BOOST_FIXTURE_TEST_CASE(setconfig_initializes_singleton, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(push_cap(CAP_ACCOUNT, "setconfig"_n, mvo{}), success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(claim_rejects_empty_ledger, sysio_cap_tester) { try {
   BOOST_REQUIRE_NE(push_cap("alice"_n, "claim"_n, mvo()("wire_account", "alice")), success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_accepts_credit_batch, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(push_cap(CAP_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
      ("credits", fc::variants{ mvo()("native_address", addr20)("wire_atomic", 982953049502) })),
      success());
   // Pre-launch import lands as an unmapped balance (unlinked by definition).
   auto u = unmapped_row(1);
   BOOST_REQUIRE(!u.is_null());
   BOOST_REQUIRE_EQUAL(u["balance"].as_string().substr(0, 3), std::string("982"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importseed_rejects_negative_atomic, sysio_cap_tester) { try {
   BOOST_REQUIRE_NE(push_cap(CAP_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
      ("credits", fc::variants{ mvo()("native_address", addr20)("wire_atomic", -1) })),
      success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(importdone_locks_subsequent_importseed, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(push_cap(CAP_ACCOUNT, "importdone"_n, mvo{}), success());
   BOOST_REQUIRE_NE(push_cap(CAP_ACCOUNT, "importseed"_n, mvo
      ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
      ("credits", fc::variants{ mvo()("native_address", addr20)("wire_atomic", 1) })),
      success());
} FC_LOG_AND_RETHROW() }

// ── setwindow ──

BOOST_FIXTURE_TEST_CASE(setwindow_rejects_zero, sysio_cap_tester) { try {
   BOOST_REQUIRE_NE(push_cap(CAP_ACCOUNT, "setwindow"_n, mvo()("window_sec", 0)), success());
   BOOST_REQUIRE_EQUAL(push_cap(CAP_ACCOUNT, "setwindow"_n, mvo()("window_sec", 3600)), success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setwindow_requires_self_auth, sysio_cap_tester) { try {
   BOOST_REQUIRE_NE(push_cap("alice"_n, "setwindow"_n, mvo()("window_sec", 3600)), success());
} FC_LOG_AND_RETHROW() }

// ── onreward auth + routing ──

BOOST_FIXTURE_TEST_CASE(onreward_requires_msgch_auth, sysio_cap_tester) { try {
   BOOST_REQUIRE_NE(
      onreward("alice"_n, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 7, 100),
      success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_linked_credits_pending_claims, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(
      setreserve(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000000, 1000000),
      success());
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 7, 100),
      success());
   auto p = pending_of("alice"_n);
   BOOST_REQUIRE(!p.is_null());
   BOOST_REQUIRE_GT(p["balance"].as<asset>().get_amount(), 0);
   // No reserve quote needed -> nothing staged.
   BOOST_REQUIRE(stage_row(1).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_unlinked_parks_unmapped_then_linkswept, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(
      setreserve(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000000, 1000000),
      success());
   // Empty wire account -> parked in unmapped by native address.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 5000, 7, 100),
      success());
   BOOST_REQUIRE(pending_of("bob"_n).is_null());
   auto u = unmapped_row(1);
   BOOST_REQUIRE(!u.is_null());
   const int64_t parked = u["balance"].as<asset>().get_amount();
   BOOST_REQUIRE_GT(parked, 0);

   // AuthX link sweeps it into pending_claims for bob.
   BOOST_REQUIRE_EQUAL(
      push_cap(AUTHEX_ACCOUNT, "linkswept"_n, mvo()
         ("wire_account", "bob")
         ("chain", ChainKind::CHAIN_KIND_ETHEREUM)
         ("native_pubkey", addr20)),
      success());
   BOOST_REQUIRE(unmapped_row(1).is_null());
   BOOST_REQUIRE_EQUAL(pending_of("bob"_n)["balance"].as<asset>().get_amount(), parked);
} FC_LOG_AND_RETHROW() }

// ── dedupe cursor ──

BOOST_FIXTURE_TEST_CASE(onreward_dedupes_stale_external_ref, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(
      setreserve(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000000, 1000000),
      success());
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 7, 100),
      success());
   const int64_t after_first = pending_of("alice"_n)["balance"].as<asset>().get_amount();

   // Replay same external_epoch_ref -> admitted=false -> no extra credit.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 7, 100),
      success());
   BOOST_REQUIRE_EQUAL(pending_of("alice"_n)["balance"].as<asset>().get_amount(), after_first);

   // A newer external_epoch_ref is accepted and adds more.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 8, 101),
      success());
   BOOST_REQUIRE_GT(pending_of("alice"_n)["balance"].as<asset>().get_amount(), after_first);
} FC_LOG_AND_RETHROW() }

// ── no-quote staging + retryconvert ──

BOOST_FIXTURE_TEST_CASE(onreward_no_quote_stages_then_retryconvert_promotes, sysio_cap_tester) { try {
   // No reserve provisioned yet -> quote 0 -> staged in native units.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 7, 100),
      success());
   auto s = stage_row(1);
   BOOST_REQUIRE(!s.is_null());
   BOOST_REQUIRE_EQUAL(s["native_amount"].as_uint64(), 1000u);
   BOOST_REQUIRE(pending_of("alice"_n).is_null());

   // retryconvert with still no reserve: leaves the row staged.
   BOOST_REQUIRE_EQUAL(push_cap("alice"_n, "retryconvert"_n, mvo()("max_rows", 10)), success());
   BOOST_REQUIRE(!stage_row(1).is_null());

   // Provision the reserve, then retryconvert promotes it to pending_claims.
   BOOST_REQUIRE_EQUAL(
      setreserve(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000000, 1000000),
      success());
   BOOST_REQUIRE_EQUAL(push_cap("alice"_n, "retryconvert"_n, mvo()("max_rows", 10)), success());
   BOOST_REQUIRE(stage_row(1).is_null());
   BOOST_REQUIRE_GT(pending_of("alice"_n)["balance"].as<asset>().get_amount(), 0);
} FC_LOG_AND_RETHROW() }

// ── claimable window expiry / reversion ──

BOOST_FIXTURE_TEST_CASE(flushexpired_reverts_expired_pending, sysio_cap_tester) { try {
   BOOST_REQUIRE_EQUAL(push_cap(CAP_ACCOUNT, "setwindow"_n, mvo()("window_sec", 1)), success());
   BOOST_REQUIRE_EQUAL(
      setreserve(ChainKind::CHAIN_KIND_ETHEREUM, TokenKind::TOKEN_KIND_ETH, 1000000, 1000000),
      success());
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_ETHEREUM, addr20,
               TokenKind::TOKEN_KIND_ETH, 1000, 7, 100),
      success());
   BOOST_REQUIRE(!pending_of("alice"_n).is_null());

   // Advance chain time well past the 1-second window.
   produce_blocks(10);
   produce_block(fc::seconds(5));

   BOOST_REQUIRE_EQUAL(push_cap("alice"_n, "flushexpired"_n, mvo()("max_rows", 50)), success());
   BOOST_REQUIRE(pending_of("alice"_n).is_null());   // reverted to capital fund
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
