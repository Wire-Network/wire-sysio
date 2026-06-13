#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/opp.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;

/// v6 data-model: reserves are keyed by the triple `(chain_code, token_code,
/// reserve_code)` (each a `sysio::slug_name` packed uint64). The legacy
/// `setreserve` action is gone; `regreserve` is the bootstrap-window
/// equivalent (it works only while `current_epoch_index == 0`, which is the
/// state immediately after deploying the contract in these tests).
///
/// Custody model: `reserve_wire_amount` is REAL — `regreserve` drains the
/// seed from the `sysio` treasury via `sysio.token::transfer` and the
/// settlement actions (`paywire` / `refundwire`) move real WIRE back out, so
/// this fixture deploys `sysio.token` and issues a WIRE supply to `sysio`
/// (mirroring `sysio.epoch_flushwtdw_tests.cpp`'s bootstrap).
class sysio_reserve_tester : public tester {
public:
   static constexpr auto RESERVE_ACCOUNT = "sysio.reserv"_n;
   static constexpr auto MSGCH_ACCOUNT   = "sysio.msgch"_n;
   static constexpr auto UWRIT_ACCOUNT   = "sysio.uwrit"_n;
   static constexpr auto TOKEN_ACCOUNT   = "sysio.token"_n;
   static constexpr auto AUTHEX_ACCOUNT  = "sysio.authex"_n;
   static constexpr auto CHAINS_ACCOUNT  = "sysio.chains"_n;
   static constexpr auto SYSIO_ACCOUNT   = "sysio"_n;

   sysio_reserve_tester() {
      produce_blocks(2);
      // sysio.authex is pre-created by the tester boot (account linking) —
      // creating it again would collide.
      create_accounts({RESERVE_ACCOUNT, MSGCH_ACCOUNT, UWRIT_ACCOUNT,
                       TOKEN_ACCOUNT, CHAINS_ACCOUNT, "alice"_n});
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

      // WIRE custody backing: deploy sysio.token + issue the treasury supply
      // to `sysio` so regreserve's treasury drain and the paywire/refundwire
      // payouts move real tokens.
      set_code(TOKEN_ACCOUNT, contracts::token_wasm());
      set_abi(TOKEN_ACCOUNT, contracts::token_abi().data());
      // Privileged-contract RAM-pool model: sysio.token bills its rows to
      // the sysio pool (same model reserv/uwrit use).
      set_privileged(TOKEN_ACCOUNT);
      produce_blocks();
      {
         const auto* tok = control->find_account_metadata(TOKEN_ACCOUNT);
         BOOST_REQUIRE(tok != nullptr);
         abi_def tok_abi_def;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(tok->abi, tok_abi_def), true);
         token_abi_ser.set_abi(std::move(tok_abi_def),
                               abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      BOOST_REQUIRE_EQUAL(success(), push_token_action(TOKEN_ACCOUNT, "create"_n, mvo()
         ("issuer", SYSIO_ACCOUNT)
         ("maximum_supply", "1000000000.000000000 WIRE")));
      BOOST_REQUIRE_EQUAL(success(), push_token_action(SYSIO_ACCOUNT, "issue"_n, mvo()
         ("to", SYSIO_ACCOUNT)
         ("quantity", "1000000000.000000000 WIRE")
         ("memo", "test bootstrap")));
      produce_blocks();
   }

   action_result push_action(name signer, name action_name, const variant_object& data) {
      return push_to(RESERVE_ACCOUNT, abi_ser, signer, action_name, data);
   }

   action_result push_token_action(name signer, name action_name, const variant_object& data) {
      return push_to(TOKEN_ACCOUNT, token_abi_ser, signer, action_name, data);
   }

   action_result push_to(name account, abi_serializer& ser, name signer,
                         name action_name, const variant_object& data) {
      string action_type_name = ser.get_action_type(action_name);
      action act;
      act.account = account;
      act.name = action_name;
      act.data = ser.variant_to_binary(
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

   // ── SlugName helpers (v6) ──

   static fc::slug_name cn(std::string_view s) { return fc::slug_name{s}; }
   static fc::mutable_variant_object codename_mvo(std::string_view s) {
      return mvo()("value", fc::slug_name{s}.value);
   }

   /// `regreserve` is the v6 bootstrap-window action for inserting a reserve
   /// row with `status=ACTIVE` and REAL WIRE backing drained from the
   /// treasury. Triple-slug_name PK is `(chain_code, token_code,
   /// reserve_code)`; `is_private`/`owner` seed privately-owned reserves.
   action_result regreserve(std::string_view chain_code,
                            std::string_view token_code,
                            std::string_view reserve_code,
                            uint64_t initial_chain_amount,
                            uint64_t initial_wire_amount,
                            uint32_t weight = 5000,
                            bool is_private = false,
                            name owner = name{},
                            const std::string& name_str = "test reserve",
                            const std::string& description = "") {
      return push_action(RESERVE_ACCOUNT, "regreserve"_n, mvo()
         ("chain_code",            codename_mvo(chain_code))
         ("token_code",            codename_mvo(token_code))
         ("reserve_code",          codename_mvo(reserve_code))
         ("name",                  name_str)
         ("description",           description)
         ("initial_chain_amount",  initial_chain_amount)
         ("initial_wire_amount",   initial_wire_amount)
         ("connector_weight_bps",  weight)
         ("is_private",            is_private)
         ("owner",                 owner));
   }

   /// `onreward` v6 signature: `(chain_code, token_code, reserve_code,
   /// outpost_amount)`.
   action_result onreward(name signer,
                          std::string_view chain_code,
                          std::string_view token_code,
                          std::string_view reserve_code,
                          uint64_t outpost_amount) {
      return push_action(signer, "onreward"_n, mvo()
         ("chain_code",     codename_mvo(chain_code))
         ("token_code",     codename_mvo(token_code))
         ("reserve_code",   codename_mvo(reserve_code))
         ("outpost_amount", outpost_amount));
   }

   /// `onreject` v6 signature.
   action_result onreject(name signer,
                          std::string_view chain_code,
                          std::string_view token_code,
                          std::string_view reserve_code,
                          uint64_t unremitted_amount) {
      return push_action(signer, "onreject"_n, mvo()
         ("original_swap_remit_id", std::string(64, '0'))
         ("chain_code",             codename_mvo(chain_code))
         ("token_code",             codename_mvo(token_code))
         ("reserve_code",           codename_mvo(reserve_code))
         ("unremitted_amount",      unremitted_amount)
         ("recipient_address",      std::vector<char>{})
         ("reason",                 "test rejection"));
   }

   /// The contract's real WIRE token balance (raw units, 9 decimals).
   int64_t wire_balance(name account) {
      auto bal = get_currency_balance(TOKEN_ACCOUNT, symbol(9, "WIRE"), account);
      return bal.get_amount();
   }

   /// Walk every row in `sysio.reserv::reserves` (KV-keyed by checksum256)
   /// via the DB index and return the row whose slug_name triple matches.
   /// `get_row_by_id` only supports uint64 keys; this scan is the test-side
   /// workaround.
   fc::variant find_reserve(std::string_view chain_code,
                            std::string_view token_code,
                            std::string_view reserve_code) {
      const auto target_chain   = cn(chain_code).value;
      const auto target_token   = cn(token_code).value;
      const auto target_reserve = cn(reserve_code).value;

      const auto& db = control->db();
      const auto table_id = chain::compute_table_id("reserves"_n.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto itr = kv_idx.lower_bound(boost::make_tuple(RESERVE_ACCOUNT, table_id, std::string_view{}));
      for (; itr != kv_idx.end()
             && itr->code == RESERVE_ACCOUNT
             && itr->table_id == table_id; ++itr) {
         std::vector<char> raw(itr->value.size());
         if (!raw.empty())
            std::memcpy(raw.data(), itr->value.data(), raw.size());
         try {
            auto row = abi_ser.binary_to_variant(
               "reserve_row", raw,
               abi_serializer::create_yield_function(abi_serializer_max_time));
            if (row["chain_code"]["value"].as_uint64()   == target_chain &&
                row["token_code"]["value"].as_uint64()   == target_token &&
                row["reserve_code"]["value"].as_uint64() == target_reserve) {
               return row;
            }
         } catch (...) {
            // skip rows that don't decode
         }
      }
      return fc::variant();
   }

   abi_serializer abi_ser;
   abi_serializer token_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_reserve_tests)

// ── regreserve (v6 bootstrap-window action; real-WIRE treasury drain) ──

BOOST_FIXTURE_TEST_CASE(regreserve_creates_reserve_row, sysio_reserve_tester) { try {
   const int64_t treasury_before = wire_balance(SYSIO_ACCOUNT);

   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY",
                 /*chain_amount*/ 1'000'000, /*wire_amount*/ 2'000'000));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(1'000'000, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(2'000'000, r["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(5000,      r["connector_weight_bps"].as_uint64());
   BOOST_REQUIRE_EQUAL(false,     r["is_private"].as_bool());
   BOOST_REQUIRE_EQUAL("",        r["owner"].as_string());

   // Custody invariant: the row's WIRE side is REAL — drained from the
   // treasury into sysio.reserv's token balance.
   BOOST_REQUIRE_EQUAL(2'000'000, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE_EQUAL(treasury_before - 2'000'000, wire_balance(SYSIO_ACCOUNT));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regreserve_rejects_duplicate, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOL", "SOL", "PRIMARY", 100, 200, 5000));

   // Re-call with the same triple must reject (regreserve only inserts).
   BOOST_REQUIRE(
      regreserve("SOL", "SOL", "PRIMARY", 999, 1234, 6000).find("already") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regreserve_rejects_invalid_connector_weight, sysio_reserve_tester) { try {
   BOOST_REQUIRE(
      regreserve("ETH", "ETH", "PRIMARY", 100, 100, 0)
         .find("connector_weight_bps") != std::string::npos);

   BOOST_REQUIRE(
      regreserve("ETH", "ETH", "PRIMARY2", 100, 100, 10001)
         .find("connector_weight_bps") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regreserve_private_requires_owner, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: a private bootstrap reserve must name an owner"),
      regreserve("ETH", "ETH", "PRIVATE", 100, 100, 5000, /*is_private*/true, name{}));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regreserve_seeds_private_reserve, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIVATE", 100, 100, 5000,
                 /*is_private*/true, "alice"_n));
   auto r = find_reserve("ETH", "ETH", "PRIVATE");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(true,    r["is_private"].as_bool());
   BOOST_REQUIRE_EQUAL("alice", r["owner"].as_string());
} FC_LOG_AND_RETHROW() }

// ── oncrtreserve (create gating) ──

BOOST_FIXTURE_TEST_CASE(oncrtreserve_requires_msgch_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE(push_action(RESERVE_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "user reserve")
      ("description",           "")
      ("external_token_amount", 1000)
      ("requested_wire_amount", 1000)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       std::vector<char>(33, '\x02'))
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(oncrtreserve_unlinked_creator_is_cancelled, sysio_reserve_tester) { try {
   // Create gating: no authex link exists for the creator's pubkey (the
   // authex account carries no links table here), so the request must be
   // rejected by inserting a CANCELLED row (idempotency + audit) and
   // queueing RESERVE_CREATE_CANCELLED back. Never throws.
   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "user reserve")
      ("description",           "")
      ("external_token_amount", 1000)
      ("requested_wire_amount", 1000)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       std::vector<char>(33, '\x02'))
   ));

   auto r = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r["status"].as_string());
} FC_LOG_AND_RETHROW() }

// ── matchreserve (gating preconditions) ──

BOOST_FIXTURE_TEST_CASE(matchreserve_rejects_unknown_reserve, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: matchreserve: reserve not found"),
      push_action("alice"_n, "matchreserve"_n, mvo()
         ("chain_code",   codename_mvo("ETH"))
         ("token_code",   codename_mvo("ETH"))
         ("reserve_code", codename_mvo("NOPE"))
         ("matcher",      "alice")
         ("wire_amount",  100)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(matchreserve_rejects_non_pending, sysio_reserve_tester) { try {
   // Bootstrap rows activate inline — there is nothing to match.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: matchreserve: reserve is not PENDING"),
      push_action("alice"_n, "matchreserve"_n, mvo()
         ("chain_code",   codename_mvo("ETH"))
         ("token_code",   codename_mvo("ETH"))
         ("reserve_code", codename_mvo("PRIMARY"))
         ("matcher",      "alice")
         ("wire_amount",  1000)));
} FC_LOG_AND_RETHROW() }

// ── onreward ──

BOOST_FIXTURE_TEST_CASE(onreward_requires_msgch_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE(onreward(RESERVE_ACCOUNT, "ETH", "ETH", "PRIMARY", 100)
      .find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_grows_outpost_reserve_only, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(success(),
      onreward(MSGCH_ACCOUNT, "ETH", "ETH", "PRIMARY", 100));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(1100, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1000, r["reserve_wire_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreward_silently_skips_unknown_reserve, sysio_reserve_tester) { try {
   // v6: onreward is dispatched from msgch; per
   // feedback_opp_handlers_never_throw.md it MUST NOT throw. An unknown
   // reserve simply logs + skips and the action returns success.
   BOOST_REQUIRE_EQUAL(success(),
      onreward(MSGCH_ACCOUNT, "ETH", "ETH", "MISSING", 100));

   auto r = find_reserve("ETH", "ETH", "MISSING");
   BOOST_REQUIRE(r.is_null());
} FC_LOG_AND_RETHROW() }

// ── onreject ──

BOOST_FIXTURE_TEST_CASE(onreject_requires_msgch_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE(onreject(RESERVE_ACCOUNT, "ETH", "ETH", "PRIMARY", 50)
      .find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(onreject_re_adds_unremitted_amount, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(),
      onreject(MSGCH_ACCOUNT, "ETH", "ETH", "PRIMARY", 50));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(1050, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1000, r["reserve_wire_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

// ── Emit-time settlement actions (auth = sysio.uwrit) ──

BOOST_FIXTURE_TEST_CASE(applyswap_requires_uwrit_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE(push_action("alice"_n, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       100)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       50)
   ).find("missing authority of sysio.uwrit") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(applyswap_applies_four_legs, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1000, 1000));

   // w = cp_output(1000, 1000, 100) = 1000*100 / (1000+100) = 90 (floor).
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       100)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       50)));

   auto src = find_reserve("ETH", "ETH", "PRIMARY");
   auto dst = find_reserve("SOLANA", "SOL", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1100, src["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(910,  src["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1090, dst["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(950,  dst["reserve_chain_amount"].as_uint64());
   // Σ reserve_wire_amount unchanged (910 + 1090 == 2000) — the w hop is
   // internal; no real WIRE moved.
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(applyfromwire_credits_wire_and_debits_chain, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyfromwire"_n, mvo()
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("wire_in",          200)
      ("dst_amount",       100)));

   auto r = find_reserve("SOLANA", "SOL", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1200, r["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(900,  r["reserve_chain_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(paywire_pays_real_wire_from_custody, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(1000, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0,    wire_balance("alice"_n));

   // Swap-to-WIRE settlement: source books move + alice is paid REAL WIRE.
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       100)
      ("recipient",        "alice")
      ("wire_out",         200)));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1100, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(800,  r["reserve_wire_amount"].as_uint64());
   // Custody invariant: Σ reserve_wire_amount dropped by 200 AND the real
   // balance dropped by 200, together.
   BOOST_REQUIRE_EQUAL(800, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE_EQUAL(200, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(paywire_rejects_overdraw, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 100));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: paywire: insufficient source reserve WIRE for payout"),
      push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
         ("src_chain_code",   codename_mvo("ETH"))
         ("src_token_code",   codename_mvo("ETH"))
         ("src_reserve_code", codename_mvo("PRIMARY"))
         ("src_amount",       100)
         ("recipient",        "alice")
         ("wire_out",         200)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(refundwire_returns_escrow, sysio_reserve_tester) { try {
   // Seed custody via a bootstrap reserve (the refund itself touches no
   // reserve row — it returns in-flight escrow).
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",   "alice")
      ("wire_amount", 150)));

   BOOST_REQUIRE_EQUAL(150, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(850, wire_balance(RESERVE_ACCOUNT));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1000, r["reserve_wire_amount"].as_uint64());   // untouched
} FC_LOG_AND_RETHROW() }

// ── swapquote ──

BOOST_FIXTURE_TEST_CASE(swapquote_returns_zero_when_reserve_missing, sysio_reserve_tester) { try {
   // No regreserve — the row simply doesn't exist.
   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE(r.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
