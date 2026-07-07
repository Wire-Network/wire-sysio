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

/// Test fixture for sysio.dclaim. Deploys sysio.dclaim and creates sysio.msgch /
/// sysio.authex as the authorized inbound callers. The v6 staking-reward path
/// credits a WIRE-denominated amount directly (native -> WIRE conversion is
/// outpost-side), so no sysio.reserv deployment is needed.
class sysio_dclaim_tester : public tester {
public:
   static constexpr auto DCLAIM_ACCOUNT    = "sysio.dclaim"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;

   sysio_dclaim_tester() {
      produce_blocks(2);
      // sysio.authex is created by the tester bootstrap (account-linking
      // system account); it is signed for directly to drive linkswept, never
      // re-created here.
      create_accounts({
         DCLAIM_ACCOUNT, MSGCH_ACCOUNT, TOKEN_ACCOUNT, "alice"_n, "bob"_n,
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

   action_result push_dclaim(name signer, name action_name, const variant_object& data) {
      return push(DCLAIM_ACCOUNT, dclaim_abi_ser, signer, action_name, data);
   }

   /// Dispatch a STAKING_REWARD per-staker body to dclaim::onreward. `amount` is
   /// the WIRE-denominated reward (native -> WIRE conversion is outpost-side).
   action_result onreward(name signer, uint64_t chain_code,
                          const std::string& wire_account, ChainKind chain,
                          const std::vector<char>& native_addr,
                          uint64_t amount, uint32_t epoch_index,
                          uint64_t external_epoch_ref, uint32_t share_bps = 10000) {
      return push_dclaim(signer, "onreward"_n, mvo()
         ("chain_code", chain_code)
         ("staker_wire_account", wire_account)
         ("reward_chain", chain)
         ("staker_native_addr", native_addr)
         ("reward_amount", amount)
         ("reward_epoch_index", epoch_index)
         ("external_epoch_ref", external_epoch_ref)
         ("share_bps", share_bps));
   }

   fc::variant get_kv(name table, const char* type, uint64_t id) {
      auto data = get_row_by_id(DCLAIM_ACCOUNT, DCLAIM_ACCOUNT, table, id);
      return data.empty() ? fc::variant()
         : dclaim_abi_ser.binary_to_variant(
              type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   fc::variant pending_of(name acct)  { return get_kv("pclaims"_n,     "pending_claim", acct.to_uint64_t()); }
   fc::variant unmapped_row(uint64_t id) { return get_kv("unmapped"_n, "unmapped_token", id); }
   fc::variant cursor_row(uint64_t id)   { return get_kv("rwdcursors"_n, "reward_cursor", id); }

   std::vector<char> addr20{std::vector<char>(20, char(0xA1))};

   abi_serializer dclaim_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_dclaim_tests)

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

// -- setclmwindow --

BOOST_FIXTURE_TEST_CASE(setclmwindow_rejects_zero, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_NE(push_dclaim(DCLAIM_ACCOUNT, "setclmwindow"_n, mvo()("window_sec", 0)), success());
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "setclmwindow"_n, mvo()("window_sec", 3600)), success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setclmwindow_rejects_excess_window, sysio_dclaim_tester) { try {
   // The ten-year ceiling is accepted; one second beyond it is rejected before
   // the window could overflow `now_sec() + window_sec` and mark fresh claims
   // expired the moment they are written.
   constexpr uint32_t ten_years_sec = 10u * 365 * 24 * 60 * 60;
   BOOST_REQUIRE_EQUAL(success(),
      push_dclaim(DCLAIM_ACCOUNT, "setclmwindow"_n, mvo()("window_sec", ten_years_sec)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("window_sec exceeds the ten-year ceiling"),
      push_dclaim(DCLAIM_ACCOUNT, "setclmwindow"_n, mvo()("window_sec", ten_years_sec + 1)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setclmwindow_requires_self_auth, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_NE(push_dclaim("alice"_n, "setclmwindow"_n, mvo()("window_sec", 3600)), success());
} FC_LOG_AND_RETHROW() }

// -- onreward auth + routing --

BOOST_FIXTURE_TEST_CASE(onreward_requires_msgch_auth, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_NE(
      onreward("alice"_n, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 7, 100),
      success());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_linked_credits_pending_claims, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 7, 100),
      success());
   // reward_amount is already WIRE-denominated -> credited verbatim.
   auto p = pending_of("alice"_n);
   BOOST_REQUIRE(!p.is_null());
   BOOST_REQUIRE_EQUAL(p["balance"].as<asset>().get_amount(), 1000);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_unlinked_parks_unmapped_then_linkswept, sysio_dclaim_tester) { try {
   // Empty wire account -> parked in unmapped by native address.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "", ChainKind::CHAIN_KIND_EVM, addr20, 5000, 7, 100),
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

// -- dedupe cursor --

BOOST_FIXTURE_TEST_CASE(onreward_dedupes_stale_external_ref, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 7, 100),
      success());
   BOOST_REQUIRE_EQUAL(pending_of("alice"_n)["balance"].as<asset>().get_amount(), 1000);

   // Replay same external_epoch_ref -> admitted=false -> no extra credit.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 7, 100),
      success());
   BOOST_REQUIRE_EQUAL(pending_of("alice"_n)["balance"].as<asset>().get_amount(), 1000);

   // A newer external_epoch_ref is accepted and adds more.
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 8, 101),
      success());
   BOOST_REQUIRE_EQUAL(pending_of("alice"_n)["balance"].as<asset>().get_amount(), 2000);
} FC_LOG_AND_RETHROW() }

// -- claimable window expiry / reversion --

BOOST_FIXTURE_TEST_CASE(flushexpired_reverts_expired_pending, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(push_dclaim(DCLAIM_ACCOUNT, "setclmwindow"_n, mvo()("window_sec", 1)), success());
   BOOST_REQUIRE_EQUAL(
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 7, 100),
      success());
   BOOST_REQUIRE(!pending_of("alice"_n).is_null());

   // Advance chain time well past the 1-second window.
   produce_blocks(10);
   produce_block(fc::seconds(5));

   BOOST_REQUIRE_EQUAL(push_dclaim("alice"_n, "flushexpired"_n, mvo()("max_rows", 50)), success());
   BOOST_REQUIRE(pending_of("alice"_n).is_null());   // reverted to capital fund
} FC_LOG_AND_RETHROW() }

// onreward runs inside the OPP inbound dispatch chain (msgch::evalcons), where an abort rolls back
// the consensus-tipping deliver and stalls epoch advancement. A cross-chain-supplied
// staker_wire_account that name() would reject must NOT abort: onreward treats it as unlinked and
// parks the credit in unmapped_tokens by native address. name() has three distinct reject classes,
// and the guard absorbs all three: (1) overlong at > 13 chars, (2) a character outside the
// ".12345a-z" name alphabet such as uppercase or '-', and (3) a length-valid 13-character name whose
// final symbol exceeds 'j' (value 15): the 13th position encodes only 4 bits. Each malformed
// spelling is exercised on a distinct native address so it lands as its own row (unmapped + dedupe
// cursor are keyed by native address), covering the full reject domain the dispatch name guard absorbs.
BOOST_FIXTURE_TEST_CASE(onreward_invalid_wire_account_parks_unmapped, sysio_dclaim_tester) { try {
   const struct { const char* wire_account; char addr_byte; } cases[] = {
      { "thisnameistoolong", char(0xB1) },   // 17 chars: length > 13
      { "BADNAME",           char(0xB2) },   // uppercase: outside the name alphabet
      { "bad-name",          char(0xB3) },   // '-': outside the name alphabet
      { "aaaaaaaaaaaak",     char(0xB4) },   // 13 chars, 13th symbol 'k' (16) > 'j' (15): 4-bit slot overflow
   };

   uint64_t expected_id = 1;   // next_unmapped_id defaults to 1; one new row per distinct address
   for (const auto& c : cases) {
      const std::vector<char> addr(20, c.addr_byte);
      BOOST_REQUIRE_EQUAL(success(),
         onreward(MSGCH_ACCOUNT, 1, c.wire_account, ChainKind::CHAIN_KIND_EVM, addr,
                  1000, 7, 100));
      // Soft-handled: credit parked as unmapped (unlinked), never aborted.
      auto u = unmapped_row(expected_id);
      BOOST_REQUIRE(!u.is_null());
      BOOST_REQUIRE_EQUAL(u["balance"].as<asset>().get_amount(), 1000);
      ++expected_id;
   }
} FC_LOG_AND_RETHROW() }

// A reward_amount above asset::max_amount (2^62-1) would abort the WIRE asset constructor. onreward
// must soft-skip it (return early) rather than abort the inbound dispatch — no credit is created.
BOOST_FIXTURE_TEST_CASE(onreward_oversized_amount_soft_skips, sysio_dclaim_tester) { try {
   const uint64_t oversized = (uint64_t(1) << 62);   // asset::max_amount + 1
   BOOST_REQUIRE_EQUAL(success(),
      onreward(MSGCH_ACCOUNT, 1, "alice", ChainKind::CHAIN_KIND_EVM, addr20,
               oversized, 7, 100));
   // Soft-skipped: neither a pending claim (valid name "alice") nor an unmapped row was created.
   BOOST_REQUIRE(pending_of("alice"_n).is_null());
   BOOST_REQUIRE(unmapped_row(1).is_null());
} FC_LOG_AND_RETHROW() }

// The positive side of the dispatch name guard: a valid account name that is not plain lowercase
// (here one carrying both a dot and a digit) must still resolve to a linked staker and credit
// pending_claims, never fall through to the unmapped native-address parking path.
BOOST_FIXTURE_TEST_CASE(onreward_dotted_digit_name_credits_pending, sysio_dclaim_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      onreward(MSGCH_ACCOUNT, 1, "stak.er1", ChainKind::CHAIN_KIND_EVM, addr20, 1000, 7, 100));
   auto p = pending_of("stak.er1"_n);
   BOOST_REQUIRE(!p.is_null());
   BOOST_REQUIRE_EQUAL(p["balance"].as<asset>().get_amount(), 1000);
   BOOST_REQUIRE(unmapped_row(1).is_null());   // linked directly, not parked as unmapped
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
