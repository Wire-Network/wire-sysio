#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio.opp.common/amm_math.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>
#include <fc/crypto/public_key.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;

namespace {

/// Extract the raw 33-byte compressed pubkey from an EM `public_key` — the
/// `creator_pub_key` form `oncrtreserve` reconstructs via `pubkey_from_raw`
/// (CHAIN_KIND_EVM → EM key). Mirrors the helper in `sysio.dispatch_tests`.
std::vector<char> em_pubkey_bytes(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize();  // std::array<char, 33>
   return std::vector<char>(compressed.begin(), compressed.end());
}

/// Extract the raw 32-byte ed25519 pubkey carried by an SVM ChainAddress.
std::vector<char> ed_pubkey_bytes(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::crypto::ed::public_key_shim>();
   const auto raw = shim.serialize();
   return std::vector<char>(reinterpret_cast<const char*>(raw.data()),
                            reinterpret_cast<const char*>(raw.data()) + raw.size());
}

} // anonymous namespace

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
   /// Stand-in for a swap's winning underwriter — the account the settlement
   /// actions accrue the underwriter half of the fee to.
   static constexpr auto UNDERWRITER_ACCOUNT = "underwriter1"_n;

   sysio_reserve_tester() {
      produce_blocks(2);
      // sysio.authex is pre-created by the tester boot (account linking) —
      // creating it again would collide.
      create_accounts({RESERVE_ACCOUNT, MSGCH_ACCOUNT, UWRIT_ACCOUNT,
                       TOKEN_ACCOUNT, CHAINS_ACCOUNT, "alice"_n,
                       UNDERWRITER_ACCOUNT});
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

      // Normal OPP reserve flows target registered outpost chains. The
      // never-throw inbound handlers soft-skip unregistered chain_code values
      // before they can queue outbound replies through sysio.msgch.
      set_code(CHAINS_ACCOUNT, contracts::chains_wasm());
      set_abi(CHAINS_ACCOUNT, contracts::chains_abi().data());
      set_privileged(CHAINS_ACCOUNT);
      produce_blocks();
      {
         const auto* chains = control->find_account_metadata(CHAINS_ACCOUNT);
         BOOST_REQUIRE(chains != nullptr);
         abi_def chains_abi_def;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(chains->abi, chains_abi_def), true);
         chains_abi_ser.set_abi(std::move(chains_abi_def),
                                abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1));
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOLANA", 2));

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

   action_result regchain(ChainKind kind,
                          std::string_view code,
                          uint32_t external_chain_id) {
      return push_to(CHAINS_ACCOUNT, chains_abi_ser, CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind",              kind)
         ("code",              codename_mvo(code))
         ("external_chain_id", external_chain_id)
         ("name",              std::string("outpost"))
         ("description",       std::string{})
         ("outpost", sysio_system::test_support::no_outpost_mvo()));
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

   /// `swapquote` is a read-only action whose ANSWER is its return value.
   /// `push_action` above discards the transaction trace and yields only
   /// `success()`, so a test written against it passes even when the quote
   /// ignores every fee. Decode the action return value instead.
   uint64_t swapquote_value(std::string_view from_chain, std::string_view from_token,
                            std::string_view from_reserve, uint64_t from_amount,
                            std::string_view to_chain, std::string_view to_token,
                            std::string_view to_reserve) {
      auto trace = tester::push_action(RESERVE_ACCOUNT, "swapquote"_n, RESERVE_ACCOUNT, mvo()
         ("from_chain_code",   codename_mvo(from_chain))
         ("from_token_code",   codename_mvo(from_token))
         ("from_reserve_code", codename_mvo(from_reserve))
         ("from_amount",       from_amount)
         ("to_chain_code",     codename_mvo(to_chain))
         ("to_token_code",     codename_mvo(to_token))
         ("to_reserve_code",   codename_mvo(to_reserve)));
      BOOST_REQUIRE(trace && !trace->action_traces.empty());
      return fc::raw::unpack<uint64_t>(trace->action_traces[0].return_value);
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
                            const std::string& description = "",
                            uint32_t source_token_precision = 9) {
      return push_action(RESERVE_ACCOUNT, "regreserve"_n, mvo()
         ("chain_code",            codename_mvo(chain_code))
         ("token_code",            codename_mvo(token_code))
         ("reserve_code",          codename_mvo(reserve_code))
         ("name",                  name_str)
         ("description",           description)
         ("initial_chain_amount",  initial_chain_amount)
         ("initial_wire_amount",   initial_wire_amount)
         ("source_token_precision", source_token_precision)
         ("connector_weight_bps",  weight)
         ("is_private",            is_private)
         ("owner",                 owner));
   }

   /// The contract's real WIRE token balance (raw units, 9 decimals).
   int64_t wire_balance(name account) {
      auto bal = get_currency_balance(TOKEN_ACCOUNT, symbol(9, "WIRE"), account);
      return bal.get_amount();
   }

   /// Read `underwriter`'s `uwfees` accrual row. Null when they have never
   /// earned a swap fee (no row is created until the first accrual).
   fc::variant get_uwfees(name underwriter) {
      auto data = get_row_by_account(RESERVE_ACCOUNT, RESERVE_ACCOUNT, "uwfees"_n, underwriter);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "uw_fee_row", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Read the `rewards_bucket` singleton (kv::global). Null when never accrued.
   fc::variant get_rewardbkt() {
      auto data = get_row_by_account(RESERVE_ACCOUNT, RESERVE_ACCOUNT, "rewardbkt"_n, "rewardbkt"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "rewards_bucket", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Claimable WIRE credited by `paywire` / `refundwire`. Null when the account is owed
   /// nothing. `paywire` and `refundwire` credit rather than transfer, because both run on
   /// never-throw paths where a recipient's transfer-notify handler could otherwise abort the
   /// enclosing transaction.
   fc::variant get_wireclaim(name account) {
      auto data = get_row_by_account(RESERVE_ACCOUNT, RESERVE_ACCOUNT, "wireclaims"_n, account);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "wire_claim", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Claimable balance owed to `account`, or 0 when there is no row.
   uint64_t wire_claimable(name account) {
      auto c = get_wireclaim(account);
      return c.is_null() ? 0u : c["balance"].as_uint64();
   }

   action_result claimwire(name account) {
      return push_action(account, "claimwire"_n, mvo()("account", account));
   }

   /// Mirrors `sysio.reserv::MAX_CLAIM_SWEEP_PER_EPOCH` — the budget
   /// `sysio.epoch::advance` hands the inline retention sweep.
   static constexpr uint32_t reserve_max_claim_sweep_per_epoch = 32;

   /// Drive the retention sweep directly, as the epoch tick would. Signed by the contract itself
   /// (`sweepclaims` accepts `sysio.epoch` or self), so a test can age rows out without standing
   /// up the epoch machinery.
   action_result sweepclaims(uint32_t max_rows) {
      return push_action(RESERVE_ACCOUNT, "sweepclaims"_n, mvo()("max_rows", max_rows));
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

   // ── authex link seeding ──
   // `oncrtreserve` gates a create on the creator being authex-linked (it
   // probes `sysio.authex::links.bypubkey`). The base fixture deploys no
   // authex code, so every creator reads as unlinked; tests that exercise the
   // LINKED path (e.g. CANCELLED-row reclaim) deploy authex on demand and seed
   // a link with `recordlink`.

   /// Deploy sysio.authex (account is pre-created by the tester boot) and load
   /// its ABI so `recordlink` can be pushed and the cross-contract
   /// `links.bypubkey` read in oncrtreserve resolves.
   void deploy_authex() {
      set_code(AUTHEX_ACCOUNT, contracts::authex_wasm());
      set_abi(AUTHEX_ACCOUNT, contracts::authex_abi().data());
      set_privileged(AUTHEX_ACCOUNT);
      produce_blocks();
      const auto* a = control->find_account_metadata(AUTHEX_ACCOUNT);
      BOOST_REQUIRE(a != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(a->abi, abi), true);
      authex_abi_ser.set_abi(std::move(abi),
                             abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Seed an authex link for `pub` on `chain_kind` via the depot-only
   /// `recordlink` (signed by sysio.authex itself). After this, a creator
   /// presenting the matching raw pubkey reads as linked in oncrtreserve.
   action_result recordlink(name account, ChainKind chain_kind,
                            const fc::crypto::public_key& pub) {
      return push_to(AUTHEX_ACCOUNT, authex_abi_ser, AUTHEX_ACCOUNT, "recordlink"_n, mvo()
         ("account",    account)
         ("chain_kind", chain_kind)
         ("pub_key",    pub));
   }

   /// Deploy sysio.msgch so inline queueout actions execute in tests that
   /// need to prove a reserve handler cannot trip msgch's hard chain guard.
   void deploy_msgch() {
      set_code(MSGCH_ACCOUNT, contracts::msgch_wasm());
      set_abi(MSGCH_ACCOUNT, contracts::msgch_abi().data());
      set_privileged(MSGCH_ACCOUNT);
      produce_blocks();
   }

   abi_serializer abi_ser;
   abi_serializer token_abi_ser;
   abi_serializer authex_abi_ser;
   abi_serializer chains_abi_ser;
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
   // Reserve self-describes its precision (default 9 here); never assumed.
   BOOST_REQUIRE_EQUAL(9u, r["source_token_precision"].as_uint64());

   // Custody invariant: the row's WIRE side is REAL — drained from the
   // treasury into sysio.reserv's token balance.
   BOOST_REQUIRE_EQUAL(2'000'000, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE_EQUAL(treasury_before - 2'000'000, wire_balance(SYSIO_ACCOUNT));
} FC_LOG_AND_RETHROW() }

// A token whose depot-frame precision is below 9 (e.g. a 6-decimal stablecoin)
// is recorded as-is on the reserve: precision is carried, never assumed to be 9.
BOOST_FIXTURE_TEST_CASE(regreserve_records_non_default_precision, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "USDC", "PRIMARY",
                 /*chain_amount*/ 1'000'000, /*wire_amount*/ 2'000'000,
                 /*weight*/ 5000, /*is_private*/ false, name{},
                 "usdc reserve", "", /*source_token_precision*/ 6u));

   auto r = find_reserve("ETH", "USDC", "PRIMARY");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL(6u, r["source_token_precision"].as_uint64());
} FC_LOG_AND_RETHROW() }

// source_token_precision above the depot frame (9) is rejected — the outpost must
// downscale to min(native, 9) at its boundary, so a higher value is malformed.
BOOST_FIXTURE_TEST_CASE(regreserve_rejects_precision_over_frame, sysio_reserve_tester) { try {
   BOOST_REQUIRE(
      regreserve("ETH", "ETH", "PRIMARY",
                 1'000'000, 2'000'000, 5000, false, name{}, "", "", /*source_token_precision*/ 18u)
      .find("source_token_precision exceeds the depot frame") != std::string::npos);
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

   // R10: cw == 10000 (== WEIGHT_TOTAL_BPS) makes the token-side weight zero, so the weighted
   // curve returns 0 for every swap — a permanently dead reserve. It must be rejected (max is 9999).
   BOOST_REQUIRE(
      regreserve("ETH", "ETH", "PRIMARY3", 100, 100, 10000)
         .find("connector_weight_bps") != std::string::npos);

   // The boundary value just below the total is accepted (token side keeps 1 bps).
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY4", 100, 100, 9999));
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

// `name` and `description` persist into a reserve row billed to `ram_payer = sysio` — the
// shared system pool — so both are bounded before emplace, via the same
// `sysio::opp::registry::check_metadata` the chains and tokens registries use. CertiK WNS-10
// raised the unbounded pair on `sysio.tokens::regtoken`; it was identical here.
//
// `regreserve` is privileged and abort-safe, so it enforces the bound with `check_metadata`.
// The post-bootstrap `oncrtreserve` path must never abort and instead routes an over-bound
// row into the existing cancel/refund flow — see `oncrtreserve_oversized_metadata_is_cancelled`.
BOOST_FIXTURE_TEST_CASE(regreserve_bounds_metadata, sysio_reserve_tester) { try {
   BOOST_REQUIRE(regreserve("ETH", "ETH", "PRIMARY", 100, 100, 5000, false, name{},
                            std::string(129, 'x'))
      .find("label exceeds 128 bytes") != std::string::npos);

   BOOST_REQUIRE(regreserve("ETH", "ETH", "PRIMARY", 100, 100, 5000, false, name{},
                            "ok", std::string(257, 'x'))
      .find("description exceeds 256 bytes") != std::string::npos);

   // The bounds are inclusive, and neither rejection claimed the reserve key.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 100, 100, 5000, false, name{},
                 std::string(128, 'x'), std::string(256, 'x')));
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
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       std::vector<char>(33, '\x02'))
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(oncrtreserve_unregistered_chain_soft_skips_before_queueout, sysio_reserve_tester) { try {
   // Deploy msgch so the inline RESERVE_CREATE_CANCELLED queueout would execute.
   // Pre-guard, the unlinked-creator cancel path inserted a CANCELLED row and
   // then aborted inside sysio.msgch::queueout's hard chain registration check.
   deploy_msgch();

   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("NOCHAIN"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "user reserve")
      ("description",           "")
      ("external_token_amount", 1000)
      ("requested_wire_amount", 1000)
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       std::vector<char>(33, '\x02'))));

   BOOST_REQUIRE(find_reserve("NOCHAIN", "ETH", "USERRES").is_null());
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
      ("source_token_precision", 9u)
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

// WNS-28: `chain_code` determines the authoritative ChainKind. Before the fix,
// a payload could name ETH while supplying SVM plus an authex-linked ED key;
// oncrtreserve trusted the payload kind and created a PENDING row that
// matchreserve could never match using ETH's registered EVM kind. The handler
// must reject through its non-throwing cancel/refund path instead.
BOOST_FIXTURE_TEST_CASE(oncrtreserve_creator_chain_kind_mismatch_is_cancelled,
                        sysio_reserve_tester) { try {
   deploy_authex();
   deploy_msgch();

   auto creator_pub = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::ed).get_public_key();
   BOOST_REQUIRE_EQUAL(success(),
      recordlink("alice"_n, ChainKind::CHAIN_KIND_SVM, creator_pub));
   const auto creator_key = ed_pubkey_bytes(creator_pub);

   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "mismatched creator")
      ("description",           "")
      ("external_token_amount", 1000)
      ("requested_wire_amount", 1000)
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_SVM)
      ("creator_chain_addr",    creator_key)
      ("is_private",            false)
      ("creator_pub_key",       creator_key)));

   auto r = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r["status"].as_string());
   BOOST_REQUIRE(r["creator_pub_key"].as_string().empty());

   // The mismatch must take the refund path, not merely write the tombstone.
   // `queueout` starts attestation ids at 1 (id 0 is its sequence singleton).
   auto queued = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, 1);
   BOOST_REQUIRE(!queued.empty());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(oncrtreserve_invalid_creator_address_is_cancelled,
                        sysio_reserve_tester) { try {
   deploy_authex();

   auto creator_pub = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key();
   BOOST_REQUIRE_EQUAL(success(),
      recordlink("alice"_n, ChainKind::CHAIN_KIND_EVM, creator_pub));
   const auto creator_key = em_pubkey_bytes(creator_pub);

   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("BADADDR"))
      ("name",                  "malformed creator address")
      ("description",           "")
      ("external_token_amount", 1000)
      ("requested_wire_amount", 1000)
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(32, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       creator_key)));

   auto r = find_reserve("ETH", "ETH", "BADADDR");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r["status"].as_string());
   BOOST_REQUIRE(r["creator_addr"]["address"].as_string().empty());
} FC_LOG_AND_RETHROW() }

// A re-relay of the same unlinked create must be idempotent — it must NOT
// re-insert the row or queue a second RESERVE_CREATE_CANCELLED refund. The
// CANCELLED marker stays exactly as first written (the outpost refunds per
// (chain,token,reserve_code), so a second refund would be a double spend).
BOOST_FIXTURE_TEST_CASE(oncrtreserve_cancelled_relay_does_not_double_refund, sysio_reserve_tester) { try {
   auto crt = [&]() {
      return push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
         ("chain_code",            codename_mvo("ETH"))
         ("token_code",            codename_mvo("ETH"))
         ("reserve_code",          codename_mvo("USERRES"))
         ("name",                  "user reserve")
         ("description",           "")
         ("external_token_amount", 1000)
         ("requested_wire_amount", 1000)
         ("source_token_precision", 9u)
         ("connector_weight_bps",  5000)
         ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
         ("creator_chain_addr",    std::vector<char>(20, '\x01'))
         ("is_private",            false)
         ("creator_pub_key",       std::vector<char>(33, '\x02')));
   };

   BOOST_REQUIRE_EQUAL(success(), crt());
   auto r1 = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!r1.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r1["status"].as_string());
   const auto registered_at = r1["registered_at_ms"].as_uint64();

   // Advance time, then re-relay the identical create.
   produce_blocks(4);
   BOOST_REQUIRE_EQUAL(success(), crt());

   auto r2 = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!r2.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r2["status"].as_string());
   // The row is untouched: same registered_at_ms despite the advanced clock —
   // proving the second relay did NOT re-insert (and therefore did not refund).
   BOOST_REQUIRE_EQUAL(registered_at, r2["registered_at_ms"].as_uint64());
} FC_LOG_AND_RETHROW() }

// A CANCELLED row must NOT permanently burn the (chain,token,reserve_code)
// identity. A later authex-LINKED creator reclaims the same triple — the row
// flips CANCELLED → PENDING with the new creator's fields, rather than the
// create being skipped by the existence guard. This is the namespace-squat fix.
BOOST_FIXTURE_TEST_CASE(oncrtreserve_cancelled_is_reclaimable_by_linked_creator, sysio_reserve_tester) { try {
   deploy_authex();

   // 1) An UNLINKED creator squats the triple → CANCELLED.
   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "squatter")
      ("description",           "squat")
      ("external_token_amount", 1000)
      ("requested_wire_amount", 1000)
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x09'))
      ("is_private",            false)
      ("creator_pub_key",       std::vector<char>(33, '\x07'))));   // unlinked key
   {
      auto r = find_reserve("ETH", "ETH", "USERRES");
      BOOST_REQUIRE(!r.is_null());
      BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r["status"].as_string());
   }

   // 2) The rightful owner is authex-linked. Seed the link for their EM key,
   //    then register the SAME triple with the matching raw pubkey.
   auto creator_priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   auto creator_pub  = creator_priv.get_public_key();
   BOOST_REQUIRE_EQUAL(success(),
      recordlink("alice"_n, ChainKind::CHAIN_KIND_EVM, creator_pub));

   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "rightful owner")
      ("description",           "reclaimed")
      ("external_token_amount", 5000)
      ("requested_wire_amount", 4000)
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       em_pubkey_bytes(creator_pub))));   // linked key

   // The squat is gone: the row is PENDING and carries the reclaiming creator's
   // create (proving overwrite, not the existence-guard skip).
   auto r = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_PENDING", r["status"].as_string());
   BOOST_REQUIRE_EQUAL("reclaimed", r["description"].as_string());
   BOOST_REQUIRE_EQUAL(5000u, r["external_token_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(5000u, r["reserve_chain_amount"].as_uint64());
   // The canonical creator pubkey is now stored (empty on the prior CANCELLED row).
   BOOST_REQUIRE(!r["creator_pub_key"].as_string().empty());
} FC_LOG_AND_RETHROW() }

// WSA-028: a reserve-create whose external amount is invalid — zero, which is
// what sysio.msgch's to_depot_amount clamp produces for a negative or
// out-of-range inbound TokenAmount — must NOT be silently dropped. The creator's
// outpost escrow has to be released, so the request is routed into the
// cancel/refund flow: a CANCELLED row is inserted (the same branch that queues
// RESERVE_CREATE_CANCELLED back to the outpost). Pre-fix, oncrtreserve returned
// at the zero-amount guard before any cancel, stranding the escrow. The creator
// here is properly authex-LINKED, so the amount is the sole rejection reason.
BOOST_FIXTURE_TEST_CASE(oncrtreserve_invalid_amount_is_cancelled, sysio_reserve_tester) { try {
   deploy_authex();

   auto creator_priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   auto creator_pub  = creator_priv.get_public_key();
   BOOST_REQUIRE_EQUAL(success(),
      recordlink("alice"_n, ChainKind::CHAIN_KIND_EVM, creator_pub));

   // Linked creator, but external_token_amount == 0 (the clamp result for an
   // invalid inbound amount). The link is valid, so the amount alone forces the
   // cancel/refund — proving the amount path no longer drops silently.
   BOOST_REQUIRE_EQUAL(success(), push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
      ("chain_code",            codename_mvo("ETH"))
      ("token_code",            codename_mvo("ETH"))
      ("reserve_code",          codename_mvo("USERRES"))
      ("name",                  "invalid amount")
      ("description",           "")
      ("external_token_amount", 0)
      ("requested_wire_amount", 1000)
      ("source_token_precision", 9u)
      ("connector_weight_bps",  5000)
      ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
      ("creator_chain_addr",    std::vector<char>(20, '\x01'))
      ("is_private",            false)
      ("creator_pub_key",       em_pubkey_bytes(creator_pub))));   // linked key

   // The escrow is released via the cancel/refund flow: a CANCELLED row stands
   // (inserted in the same branch that queues RESERVE_CREATE_CANCELLED).
   auto r = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!r.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", r["status"].as_string());
} FC_LOG_AND_RETHROW() }

// The same `sysio`-billed metadata bound `regreserve` enforces, applied to the inbound
// creation path. `oncrtreserve` is an OPP dispatch handler, so it must NOT `check()` —
// aborting here rolls back the consensus-tipping delivery and stalls epoch advancement
// chain-wide (feedback_opp_handlers_never_throw / epoch-stall-is-fatal). An over-bound
// string therefore joins `invalid_amount` and the unlinked-creator case on the existing
// reject predicate, releasing the creator's escrow via RESERVE_CREATE_CANCELLED.
//
// The creator here is properly LINKED with a valid amount, so the metadata is the sole
// rejection reason. The tombstone is itself a `sysio`-billed row, so the over-bound strings
// are replaced by a fixed marker rather than carried onto it. Both an ASCII and a MULTIBYTE
// over-bound label are covered: because nothing is truncated there is no code-point boundary
// to split, so neither can leave malformed text in state. A third case drives the OTHER half
// of `metadata_exceeds_bounds` — an over-bound `description` behind an in-bound name — so
// each side of the predicate is exercised independently rather than only via the label.
BOOST_FIXTURE_TEST_CASE(oncrtreserve_oversized_metadata_is_cancelled, sysio_reserve_tester) { try {
   deploy_authex();

   auto creator_priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   auto creator_pub  = creator_priv.get_public_key();
   BOOST_REQUIRE_EQUAL(success(),
      recordlink("alice"_n, ChainKind::CHAIN_KIND_EVM, creator_pub));

   auto create_with_metadata = [&](std::string_view reserve_code,
                                   const std::string& name,
                                   const std::string& description) {
      return push_action(MSGCH_ACCOUNT, "oncrtreserve"_n, mvo()
         ("chain_code",            codename_mvo("ETH"))
         ("token_code",            codename_mvo("ETH"))
         ("reserve_code",          codename_mvo(reserve_code))
         ("name",                  name)
         ("description",           description)
         ("external_token_amount", 1000)
         ("requested_wire_amount", 1000)
         ("source_token_precision", 9u)
         ("connector_weight_bps",  5000)
         ("creator_chain_kind",    ChainKind::CHAIN_KIND_EVM)
         ("creator_chain_addr",    std::vector<char>(20, '\x01'))
         ("is_private",            false)
         ("creator_pub_key",       em_pubkey_bytes(creator_pub)));   // linked key
   };

   // 129 ASCII bytes — one over label_max_bytes.
   BOOST_REQUIRE_EQUAL(success(), create_with_metadata("USERRES", std::string(129, 'x'), ""));
   auto ascii_row = find_reserve("ETH", "ETH", "USERRES");
   BOOST_REQUIRE(!ascii_row.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", ascii_row["status"].as_string());
   BOOST_REQUIRE_EQUAL("<rejected>", ascii_row["name"].as_string());

   // 129 bytes as 127 ASCII + `é` (0xC3 0xA9) — a byte-wise truncation at the bound would
   // have split the final character and persisted a lone lead byte. The marker sidesteps
   // that entirely: nothing is truncated, so there is no code-point boundary to get wrong.
   const std::string multibyte_name = std::string(127, 'x') + "\xC3\xA9";
   BOOST_REQUIRE_EQUAL(129u, multibyte_name.size());
   BOOST_REQUIRE_EQUAL(success(), create_with_metadata("USERRES2", multibyte_name, ""));
   auto multibyte_row = find_reserve("ETH", "ETH", "USERRES2");
   BOOST_REQUIRE(!multibyte_row.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", multibyte_row["status"].as_string());
   BOOST_REQUIRE_EQUAL("<rejected>", multibyte_row["name"].as_string());

   // 257 bytes — one over description_max_bytes — behind an in-bound name. This is the
   // `description` half of `metadata_exceeds_bounds`: the label alone would have passed, so
   // the description is the sole rejection reason. The tombstone drops the over-bound text
   // rather than carrying it onto a `sysio`-billed row, so the stored description is empty
   // while the name still reads as the rejection marker.
   const std::string oversized_description(257, 'd');
   BOOST_REQUIRE_EQUAL(success(),
      create_with_metadata("USERRES3", "in-bound name", oversized_description));
   auto description_row = find_reserve("ETH", "ETH", "USERRES3");
   BOOST_REQUIRE(!description_row.is_null());
   BOOST_REQUIRE_EQUAL("RESERVE_STATUS_CANCELLED", description_row["status"].as_string());
   BOOST_REQUIRE_EQUAL("<rejected>", description_row["name"].as_string());
   BOOST_REQUIRE_EQUAL("", description_row["description"].as_string());
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
      ("underwriter",      "underwriter1")
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
      ("dst_amount",       50)
      ("underwriter",      "underwriter1")));

   auto src = find_reserve("ETH", "ETH", "PRIMARY");
   auto dst = find_reserve("SOLANA", "SOL", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1100, src["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(910,  src["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1090, dst["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(950,  dst["reserve_chain_amount"].as_uint64());
   // Σ reserve_wire_amount unchanged (910 + 1090 == 2000) — at these tiny
   // amounts the 0.1% fee floors to 0, so the w hop stays fully internal.
} FC_LOG_AND_RETHROW() }

// [P0] WNS-02: applyswap must refuse a destination debit larger than the
// destination curve's output for the post-fee WIRE it is receiving. The audited
// vulnerability paid out a caller-chosen `dst_amount` verbatim, so the reserve
// enforces its own floor instead of trusting `sysio.uwrit` to have derived it.
BOOST_FIXTURE_TEST_CASE(applyswap_rejects_debit_above_curve_output, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1000, 1000));

   // w_gross = 90 (see applyswap_applies_four_legs), fee floors to 0, so the
   // destination receives 90 WIRE and its curve yields
   // floor(1000*90/1090) = 82 token. 83 is one subunit past what the AMM owes.
   constexpr int64_t CURVE_OUT = 82;
   auto apply = [&](int64_t dst_amount) {
      return push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
         ("src_chain_code",   codename_mvo("ETH"))
         ("src_token_code",   codename_mvo("ETH"))
         ("src_reserve_code", codename_mvo("PRIMARY"))
         ("src_amount",       100)
         ("dst_chain_code",   codename_mvo("SOLANA"))
         ("dst_token_code",   codename_mvo("SOL"))
         ("dst_reserve_code", codename_mvo("PRIMARY"))
         ("dst_amount",       dst_amount)
         ("underwriter",      "underwriter1"));
   };

   // One subunit over — refused. A grossly-inflated amount (the drain shape)
   // takes the same path.
   const auto expected = error("assertion failure with message: applyswap: "
                               "destination amount exceeds the curve output for the post-fee WIRE");
   BOOST_REQUIRE_EQUAL(expected, apply(CURVE_OUT + 1));
   BOOST_REQUIRE_EQUAL(expected, apply(900));

   // Exactly the curve output settles — the amount `sysio.uwrit` derives.
   BOOST_REQUIRE_EQUAL(success(), apply(CURVE_OUT));
   BOOST_REQUIRE_EQUAL(1000 - CURVE_OUT,
                       find_reserve("SOLANA", "SOL", "PRIMARY")["reserve_chain_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(applyswap_charges_fee_and_routes_50_50, sysio_reserve_tester) { try {
   // Large amounts so the default 0.1% (10 bps) fee is non-zero and routes.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));

   const int64_t sysio_before = wire_balance(SYSIO_ACCOUNT);
   const int64_t resv_before  = wire_balance(RESERVE_ACCOUNT);

   // w_gross = cp_output(1e12, 1e12, 1e9) = 999'000'999 (50/50 = constant product).
   // fee = 999'000'999 * 10 / 10000 = 999'000 ; underwriter = reward = 499'500 ;
   // net = 999'000'999 - 999'000 = 998'001'999.
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       100'000'000ULL)
      ("underwriter",      "underwriter1")));

   auto src = find_reserve("ETH", "ETH", "PRIMARY");
   auto dst = find_reserve("SOLANA", "SOL", "PRIMARY");
   // Source gives up the full gross WIRE; destination receives only the net.
   BOOST_REQUIRE_EQUAL(1'000'000'000'000ULL - 999'000'999ULL, src["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1'000'000'000'000ULL + 998'001'999ULL, dst["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1'000'000'000'000ULL + 1'000'000'000ULL, src["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1'000'000'000'000ULL - 100'000'000ULL,   dst["reserve_chain_amount"].as_uint64());

   // Fee split 50/50: half accrues to the winning underwriter's claimable row,
   // half to the batch-operator rewards bucket.
   auto uwf = get_uwfees(UNDERWRITER_ACCOUNT);
   BOOST_REQUIRE_EQUAL(499'500ULL, uwf["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(499'500ULL, uwf["lifetime_accrued"].as_uint64());
   BOOST_REQUIRE_EQUAL(0ULL,       uwf["lifetime_claimed"].as_uint64());

   auto bkt = get_rewardbkt();
   BOOST_REQUIRE_EQUAL(499'500ULL, bkt["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(499'500ULL, bkt["lifetime_accrued"].as_uint64());

   // BOTH halves stay in reserv custody — no part of a swap fee reaches the
   // emissions treasury, so neither real balance moves at settlement.
   BOOST_REQUIRE_EQUAL(sysio_before, wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(resv_before,  wire_balance(RESERVE_ACCOUNT));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_emissions_share_routes_pool_to_treasury, sysio_reserve_tester) { try {
   // Stage 2 of the split is a governance dial: a non-zero
   // `fee_emissions_share_bps` diverts that share of the REWARDS POOL (the half
   // left after the underwriter's cut) out of custody to the `sysio` treasury.
   BOOST_REQUIRE(push_action("alice"_n, "setconfig"_n, mvo()("fee_emissions_share_bps", 5000))
      .find("missing authority of sysio.reserv") != std::string::npos);
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: setconfig: fee_emissions_share_bps must be <= 10000 (100% of the rewards pool)"),
      push_action(RESERVE_ACCOUNT, "setconfig"_n, mvo()("fee_emissions_share_bps", 10001)));

   // Send the WHOLE rewards pool to the treasury so the split is unambiguous.
   BOOST_REQUIRE_EQUAL(success(), push_action(RESERVE_ACCOUNT, "setconfig"_n,
      mvo()("fee_emissions_share_bps", 10000)));

   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));

   const int64_t sysio_before = wire_balance(SYSIO_ACCOUNT);
   const int64_t resv_before  = wire_balance(RESERVE_ACCOUNT);

   // Same swap as the 50/50 test: fee 999'000, underwriter 499'500, pool 499'500.
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       100'000'000ULL)
      ("underwriter",      "underwriter1")));

   // The underwriter's half is untouched by this dial and stays in custody.
   BOOST_REQUIRE_EQUAL(499'500ULL, get_uwfees(UNDERWRITER_ACCOUNT)["balance"].as_uint64());
   // The whole pool went to the treasury instead of the rewards bucket, and it
   // is the ONLY part of the fee that left custody.
   BOOST_REQUIRE(get_rewardbkt().is_null());
   BOOST_REQUIRE_EQUAL(sysio_before + 499'500, wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(resv_before  - 499'500, wire_balance(RESERVE_ACCOUNT));
} FC_LOG_AND_RETHROW() }

// ── Reserve OWNER fee: per-reserve rate, accrual, and owner claim (WIRE-281) ──

BOOST_FIXTURE_TEST_CASE(setrsvfee_guards_owner_status_and_bounds, sysio_reserve_tester) { try {
   // Owner-less (bootstrap-seeded) reserve: nobody can authorize a fee, so no
   // WIRE can ever accrue with no claimant.
   BOOST_REQUIRE_EQUAL(success(), regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: setrsvfee: reserve has no owner"),
      push_action(RESERVE_ACCOUNT, "setrsvfee"_n, mvo()
         ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("ETH"))
         ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 100)));

   // An OWNED reserve: only the owner may set the fee.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1000, 1000, 5000, false, "alice"_n));
   auto setFee = [&](name signer, uint32_t bps) {
      return push_action(signer, "setrsvfee"_n, mvo()
         ("chain_code", codename_mvo("SOLANA"))("token_code", codename_mvo("SOL"))
         ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", bps));
   };
   BOOST_REQUIRE(setFee(UNDERWRITER_ACCOUNT, 100).find("missing authority of alice") != std::string::npos);

   // 0 disables; the [1, 9900] band is accepted at both ends; past 99% rejected.
   BOOST_REQUIRE_EQUAL(success(), setFee("alice"_n, 0));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), setFee("alice"_n, 1));
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), setFee("alice"_n, 9900));
   produce_block();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: setrsvfee: owner_fee_bps must be 0 or within [1, 9900]"),
      setFee("alice"_n, 9901));

   // Unknown reserve fails before any auth work.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: setrsvfee: reserve not found"),
      push_action("alice"_n, "setrsvfee"_n, mvo()
         ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("NOPE"))
         ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 10)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(applyswap_charges_both_reserve_owner_fees, sysio_reserve_tester) { try {
   // Jonathan, 2026-08-04: "per reserve" — a chain-to-chain swap pays 3 fees,
   // two reserve owners plus the network.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL,
                 5000, false, "alice"_n));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL,
                 5000, false, UNDERWRITER_ACCOUNT));

   // src 100 bps (1%), dst 200 bps (2%) on the same WIRE leg.
   BOOST_REQUIRE_EQUAL(success(), push_action("alice"_n, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("ETH"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 100)));
   BOOST_REQUIRE_EQUAL(success(), push_action(UNDERWRITER_ACCOUNT, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("SOLANA"))("token_code", codename_mvo("SOL"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 200)));

   const int64_t resv_before = wire_balance(RESERVE_ACCOUNT);

   // w_gross = 999'000'999. network 10bps = 999'000; src 1% = 9'990'009;
   // dst 2% = 19'980'019; net = w_gross - (999'000 + 9'990'009 + 19'980'019).
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       100'000'000ULL)
      ("underwriter",      "underwriter1")));

   auto src = find_reserve("ETH", "ETH", "PRIMARY");
   auto dst = find_reserve("SOLANA", "SOL", "PRIMARY");
   BOOST_REQUIRE_EQUAL(9'990'009ULL,  src["owner_fee_accrued"].as_uint64());
   BOOST_REQUIRE_EQUAL(9'990'009ULL,  src["owner_fee_lifetime"].as_uint64());
   BOOST_REQUIRE_EQUAL(19'980'019ULL, dst["owner_fee_accrued"].as_uint64());
   BOOST_REQUIRE_EQUAL(19'980'019ULL, dst["owner_fee_lifetime"].as_uint64());

   // Destination liquidity is the leg minus ALL THREE fees.
   const uint64_t total_fee = 999'000ULL + 9'990'009ULL + 19'980'019ULL;
   BOOST_REQUIRE_EQUAL(1'000'000'000'000ULL + (999'000'999ULL - total_fee),
                       dst["reserve_wire_amount"].as_uint64());

   // Every fee stayed in custody — the reserve shares as accruals, the network
   // fee as the underwriter accrual + rewards bucket.
   BOOST_REQUIRE_EQUAL(resv_before, wire_balance(RESERVE_ACCOUNT));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(single_reserve_paths_charge_only_their_own_side, sysio_reserve_tester) { try {
   // A WIRE endpoint has no reserve, so only the one participating reserve
   // charges: paywire → source, applyfromwire → destination.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL,
                 5000, false, "alice"_n));
   BOOST_REQUIRE_EQUAL(success(), push_action("alice"_n, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("ETH"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 100)));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("recipient",        "alice")
      ("wire_out",         100'000'000ULL)
      ("underwriter",      "underwriter1")));

   // 1% of the 999'000'999 gross leg accrued to the source reserve's owner.
   BOOST_REQUIRE_EQUAL(9'990'009ULL,
                       find_reserve("ETH", "ETH", "PRIMARY")["owner_fee_accrued"].as_uint64());

   // applyfromwire: the destination reserve is the only one that charges.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL,
                 5000, false, UNDERWRITER_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), push_action(UNDERWRITER_ACCOUNT, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("SOLANA"))("token_code", codename_mvo("SOL"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 200)));
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyfromwire"_n, mvo()
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("wire_in",          1'000'000'000ULL)
      ("dst_amount",       100'000'000ULL)
      ("underwriter",      "underwriter1")));

   // 2% of the 1e9 escrow. The source reserve is untouched by this swap.
   BOOST_REQUIRE_EQUAL(20'000'000ULL,
                       find_reserve("SOLANA", "SOL", "PRIMARY")["owner_fee_accrued"].as_uint64());
   BOOST_REQUIRE_EQUAL(9'990'009ULL,
                       find_reserve("ETH", "ETH", "PRIMARY")["owner_fee_accrued"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(claimrsvfee_pays_owner_and_guards_auth, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL,
                 5000, false, "alice"_n));

   auto claim = [&](name signer) {
      return push_action(signer, "claimrsvfee"_n, mvo()
         ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("ETH"))
         ("reserve_code", codename_mvo("PRIMARY")));
   };
   // Nothing earned yet.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: claimrsvfee: no unclaimed balance"), claim("alice"_n));

   BOOST_REQUIRE_EQUAL(success(), push_action("alice"_n, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("ETH"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 100)));
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("recipient",        UNDERWRITER_ACCOUNT)
      ("wire_out",         100'000'000ULL)
      ("underwriter",      "underwriter1")));

   const int64_t alice_before = wire_balance("alice"_n),
                 resv_before  = wire_balance(RESERVE_ACCOUNT);
   BOOST_REQUIRE_EQUAL(9'990'009ULL,
                       find_reserve("ETH", "ETH", "PRIMARY")["owner_fee_accrued"].as_uint64());

   // Only the owner may sweep it.
   BOOST_REQUIRE(claim(UNDERWRITER_ACCOUNT).find("missing authority of alice") != std::string::npos);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));

   BOOST_REQUIRE_EQUAL(alice_before + 9'990'009, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(resv_before  - 9'990'009, wire_balance(RESERVE_ACCOUNT));

   auto row = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE_EQUAL(0ULL,          row["owner_fee_accrued"].as_uint64());
   BOOST_REQUIRE_EQUAL(9'990'009ULL,  row["owner_fee_lifetime"].as_uint64()); // audit total survives

   produce_block();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: claimrsvfee: no unclaimed balance"), claim("alice"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(swapquote_prices_the_reserve_owner_fees, sysio_reserve_tester) { try {
   // The read-only quote must price EXACTLY what settlement charges, reserve
   // fees included — otherwise the variance check drifts against the books.
   // So this asserts the DECODED quote against the shared AMM kernel, not just
   // that the action succeeded: a quote that ignored `owner_fee_bps` entirely
   // would still return success.
   constexpr uint64_t POOL      = 1'000'000'000'000ULL;
   constexpr uint32_t WEIGHT    = 5000;
   constexpr uint64_t FROM      = 1'000'000'000ULL;
   constexpr uint32_t OWNER_FEE = 500;   // 5%, charged by EACH side's reserve
   // `sysio.uwrit` exists as a bare account in this fixture (no contract), so
   // the contract's `uwrit_fee_bps()` reads the `uw_config{}` in-struct default.
   constexpr uint32_t NETWORK_FEE_BPS = 10;

   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", POOL, POOL, WEIGHT, false, "alice"_n));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", POOL, POOL, WEIGHT, false, UNDERWRITER_ACCOUNT));

   /// The kernel's answer for these pools at a given pair of reserve fees.
   auto expected = [&](uint32_t src_fee, uint32_t dst_fee) {
      return opp::amm::quote_swap(/*src_is_wire*/false, POOL, POOL, WEIGHT,
                                  /*dst_is_wire*/false, POOL, POOL, WEIGHT,
                                  FROM, NETWORK_FEE_BPS, src_fee, dst_fee);
   };

   // Fee-free baseline: network fee only.
   const uint64_t before = swapquote_value("ETH", "ETH", "PRIMARY", FROM,
                                           "SOLANA", "SOL", "PRIMARY");
   BOOST_REQUIRE_GT(before, 0u);
   BOOST_CHECK_EQUAL(before, expected(0, 0));

   BOOST_REQUIRE_EQUAL(success(), push_action("alice"_n, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("ETH"))("token_code", codename_mvo("ETH"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", OWNER_FEE)));
   BOOST_REQUIRE_EQUAL(success(), push_action(UNDERWRITER_ACCOUNT, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("SOLANA"))("token_code", codename_mvo("SOL"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", OWNER_FEE)));
   produce_block();

   // Both owner fees are now priced in, off the same gross WIRE leg.
   const uint64_t after = swapquote_value("ETH", "ETH", "PRIMARY", FROM,
                                          "SOLANA", "SOL", "PRIMARY");
   BOOST_CHECK_EQUAL(after, expected(OWNER_FEE, OWNER_FEE));
   BOOST_CHECK_LT(after, before);   // the claim this case exists to prove

   // Each side is priced INDEPENDENTLY — clearing one must move the quote back
   // by only that side's share, which a quote summing the wrong reserve's rate
   // (or double-counting one) would not reproduce.
   BOOST_REQUIRE_EQUAL(success(), push_action(UNDERWRITER_ACCOUNT, "setrsvfee"_n, mvo()
      ("chain_code", codename_mvo("SOLANA"))("token_code", codename_mvo("SOL"))
      ("reserve_code", codename_mvo("PRIMARY"))("owner_fee_bps", 0)));
   produce_block();
   const uint64_t source_only = swapquote_value("ETH", "ETH", "PRIMARY", FROM,
                                                "SOLANA", "SOL", "PRIMARY");
   BOOST_CHECK_EQUAL(source_only, expected(OWNER_FEE, 0));
   BOOST_CHECK_LT(source_only, before);
   BOOST_CHECK_GT(source_only, after);
} FC_LOG_AND_RETHROW() }

// ── Underwriter fee accrual + owner-authenticated claim ──

BOOST_FIXTURE_TEST_CASE(claimuwfee_pays_accrual_and_zeroes_balance, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       100'000'000ULL)
      ("underwriter",      "underwriter1")));

   const int64_t resv_before = wire_balance(RESERVE_ACCOUNT);
   BOOST_REQUIRE_EQUAL(0, wire_balance(UNDERWRITER_ACCOUNT));
   BOOST_REQUIRE_EQUAL(499'500ULL, get_uwfees(UNDERWRITER_ACCOUNT)["balance"].as_uint64());

   // The earner claims their own accrual.
   BOOST_REQUIRE_EQUAL(success(), push_action(UNDERWRITER_ACCOUNT, "claimuwfee"_n,
      mvo()("underwriter", "underwriter1")));

   // REAL WIRE moved out of reserv custody to the underwriter.
   BOOST_REQUIRE_EQUAL(499'500, wire_balance(UNDERWRITER_ACCOUNT));
   BOOST_REQUIRE_EQUAL(resv_before - 499'500, wire_balance(RESERVE_ACCOUNT));

   // Row retained at zero balance; lifetime totals record the round trip.
   auto uwf = get_uwfees(UNDERWRITER_ACCOUNT);
   BOOST_REQUIRE_EQUAL(0ULL,       uwf["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(499'500ULL, uwf["lifetime_accrued"].as_uint64());
   BOOST_REQUIRE_EQUAL(499'500ULL, uwf["lifetime_claimed"].as_uint64());

   // A second claim on the drained row is a caller error, not a silent no-op.
   // Produce a block first: an identical action re-pushed against the same TAPOS
   // reference block serializes to the same tx id and is rejected as a duplicate
   // before it ever reaches the contract.
   produce_block();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: claimuwfee: no unclaimed balance"),
      push_action(UNDERWRITER_ACCOUNT, "claimuwfee"_n, mvo()("underwriter", "underwriter1")));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(claimuwfee_requires_earner_auth_and_a_row, sysio_reserve_tester) { try {
   // Nobody else may sweep an underwriter's accrual.
   BOOST_REQUIRE(push_action("alice"_n, "claimuwfee"_n, mvo()("underwriter", "underwriter1"))
      .find("missing authority of underwriter1") != std::string::npos);

   // An underwriter that never earned has no row at all.
   BOOST_REQUIRE(get_uwfees(UNDERWRITER_ACCOUNT).is_null());
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: claimuwfee: no accrued swap fees for this underwriter"),
      push_action(UNDERWRITER_ACCOUNT, "claimuwfee"_n, mvo()("underwriter", "underwriter1")));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(applyswap_accrues_per_underwriter_and_accumulates, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));

   auto swap_won_by = [&](const char* underwriter) {
      return push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
         ("src_chain_code",   codename_mvo("ETH"))
         ("src_token_code",   codename_mvo("ETH"))
         ("src_reserve_code", codename_mvo("PRIMARY"))
         ("src_amount",       1'000'000'000ULL)
         ("dst_chain_code",   codename_mvo("SOLANA"))
         ("dst_token_code",   codename_mvo("SOL"))
         ("dst_reserve_code", codename_mvo("PRIMARY"))
         ("dst_amount",       100'000'000ULL)
         ("underwriter",      underwriter));
   };

   // Two swaps won by the same underwriter accumulate on one row. Each repeat of
   // an identical action needs its own block — same TAPOS reference + same bytes
   // is one tx id, rejected as a duplicate before reaching the contract.
   BOOST_REQUIRE_EQUAL(success(), swap_won_by("underwriter1"));
   const uint64_t first = get_uwfees(UNDERWRITER_ACCOUNT)["balance"].as_uint64();
   BOOST_REQUIRE_GT(first, 0u);
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), swap_won_by("underwriter1"));
   auto after_two = get_uwfees(UNDERWRITER_ACCOUNT);
   BOOST_REQUIRE_GT(after_two["balance"].as_uint64(), first);
   BOOST_REQUIRE_EQUAL(after_two["balance"].as_uint64(),
                       after_two["lifetime_accrued"].as_uint64());

   // A different winner accrues to their OWN row, leaving the first untouched.
   // (Different `underwriter` bytes, so no duplicate-tx risk here.)
   const uint64_t before_other = after_two["balance"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), swap_won_by("alice"));
   BOOST_REQUIRE_GT(get_uwfees("alice"_n)["balance"].as_uint64(), 0u);
   BOOST_REQUIRE_EQUAL(before_other, get_uwfees(UNDERWRITER_ACCOUNT)["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

// ── drainrewards: sweep the accrued batch-operator share to the emissions treasury ──
// payepoch (sysio.system) calls this inline to fold swap fees into the per-epoch
// batch-operator distribution.

BOOST_FIXTURE_TEST_CASE(drainrewards_sweeps_bucket_to_treasury, sysio_reserve_tester) { try {
   // Seed the rewards bucket with a swap fee (same setup as the 50/50 routing test:
   // reward half = 499'500 accrues into the bucket, staying in reserv custody).
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1'000'000'000'000ULL, 1'000'000'000'000ULL));
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyswap"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       1'000'000'000ULL)
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("dst_amount",       100'000'000ULL)
      ("underwriter",      "underwriter1")));

   auto bkt = get_rewardbkt();
   const uint64_t reward   = bkt["balance"].as_uint64();
   const uint64_t lifetime = bkt["lifetime_accrued"].as_uint64();
   BOOST_REQUIRE_EQUAL(499'500ULL, reward);

   const int64_t sysio_before = wire_balance(SYSIO_ACCOUNT);
   const int64_t resv_before  = wire_balance(RESERVE_ACCOUNT);

   // Sweep the whole bucket to the treasury (auth = sysio).
   BOOST_REQUIRE_EQUAL(success(), push_action(SYSIO_ACCOUNT, "drainrewards"_n,
      mvo()("amount", static_cast<int64_t>(reward))));

   auto bkt_after = get_rewardbkt();
   BOOST_REQUIRE_EQUAL(0ULL, bkt_after["balance"].as_uint64());                  // balance drained
   BOOST_REQUIRE_EQUAL(lifetime, bkt_after["lifetime_accrued"].as_uint64());     // audit total untouched
   BOOST_REQUIRE_EQUAL(sysio_before + static_cast<int64_t>(reward),
                       wire_balance(SYSIO_ACCOUNT));                             // WIRE moved to treasury
   BOOST_REQUIRE_EQUAL(resv_before - static_cast<int64_t>(reward),
                       wire_balance(RESERVE_ACCOUNT));                          // left reserv custody
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(drainrewards_auth_and_overdrain_guarded, sysio_reserve_tester) { try {
   // Only `sysio` (the treasury / system account) may drain.
   BOOST_REQUIRE(push_action("alice"_n, "drainrewards"_n, mvo()("amount", int64_t(1)))
      .find("missing authority of sysio") != std::string::npos);

   // Draining more than the live balance (here: an empty bucket) is rejected,
   // not silently clamped -- this only fires on a caller bug.
   BOOST_REQUIRE(push_action(SYSIO_ACCOUNT, "drainrewards"_n, mvo()("amount", int64_t(1)))
      .find("amount exceeds rewards bucket balance") != std::string::npos);

   // Non-positive amounts fail loudly (internal sweep -> a non-positive amount
   // signals a caller bug, not a no-op).
   BOOST_REQUIRE(push_action(SYSIO_ACCOUNT, "drainrewards"_n, mvo()("amount", int64_t(0)))
      .find("amount must be positive") != std::string::npos);
   BOOST_REQUIRE(push_action(SYSIO_ACCOUNT, "drainrewards"_n, mvo()("amount", int64_t(-5)))
      .find("amount must be positive") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(applyfromwire_credits_wire_and_debits_chain, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "applyfromwire"_n, mvo()
      ("dst_chain_code",   codename_mvo("SOLANA"))
      ("dst_token_code",   codename_mvo("SOL"))
      ("dst_reserve_code", codename_mvo("PRIMARY"))
      ("wire_in",          200)
      ("dst_amount",       100)
      ("underwriter",      "underwriter1")));

   auto r = find_reserve("SOLANA", "SOL", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1200, r["reserve_wire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(900,  r["reserve_chain_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

// [P0] WNS-02, from-WIRE shape — see applyswap_rejects_debit_above_curve_output.
// The user's escrowed WIRE feeds the WIRE leg directly, so the bound is the
// destination curve's output for the post-fee remainder.
BOOST_FIXTURE_TEST_CASE(applyfromwire_rejects_debit_above_curve_output,
                        sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("SOLANA", "SOL", "PRIMARY", 1000, 1000));

   // 200 WIRE in, 0.1% fee floors to 0, so the curve yields
   // floor(1000*200/1200) = 166 token.
   constexpr int64_t CURVE_OUT = 166;
   auto apply = [&](int64_t dst_amount) {
      return push_action(UWRIT_ACCOUNT, "applyfromwire"_n, mvo()
         ("dst_chain_code",   codename_mvo("SOLANA"))
         ("dst_token_code",   codename_mvo("SOL"))
         ("dst_reserve_code", codename_mvo("PRIMARY"))
         ("wire_in",          200)
         ("dst_amount",       dst_amount)
         ("underwriter",      "underwriter1"));
   };

   const auto expected = error("assertion failure with message: applyfromwire: "
                               "destination amount exceeds the curve output for the post-fee WIRE");
   BOOST_REQUIRE_EQUAL(expected, apply(CURVE_OUT + 1));
   BOOST_REQUIRE_EQUAL(expected, apply(900));

   BOOST_REQUIRE_EQUAL(success(), apply(CURVE_OUT));
   BOOST_REQUIRE_EQUAL(1000 - CURVE_OUT,
                       find_reserve("SOLANA", "SOL", "PRIMARY")["reserve_chain_amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(paywire_pays_real_wire_from_custody, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));
   BOOST_REQUIRE_EQUAL(1000, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0,    wire_balance("alice"_n));

   // The curve's output for this leg: equal weights reduce to constant product,
   // so `token_to_wire(1000, 1000, 100)` = floor(1000*100/1100) = 90, and the
   // 0.1% fee floors to 0 at that size, leaving a post-fee 90. `sysio.uwrit`
   // passes exactly this (its `swap_quote` returns the post-fee WIRE leg for a
   // WIRE destination), and paywire now refuses anything above it (WNS-02).
   constexpr int64_t CURVE_OUT = 90;

   // Swap-to-WIRE settlement: source books move + alice is paid REAL WIRE.
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       100)
      ("recipient",        "alice")
      ("wire_out",         CURVE_OUT)
      ("underwriter",      "underwriter1")));

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1100, r["reserve_chain_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(910,  r["reserve_wire_amount"].as_uint64());

   // Custody invariant: `Σ reserve_wire_amount` drops by the payout immediately, but the
   // WIRE itself does NOT leave -- it moves sideways into `Σ wireclaims.balance`, still in this
   // contract's custody, until the recipient pulls it. paywire credits rather than transfers
   // because it settles inside the never-throw consensus dispatch chain.
   BOOST_REQUIRE_EQUAL(90u,  wire_claimable("alice"_n));
   BOOST_REQUIRE_EQUAL(1000, wire_balance(RESERVE_ACCOUNT));   // unchanged: 910 booked + 90 claimable
   BOOST_REQUIRE_EQUAL(0,    wire_balance("alice"_n));

   // The pull completes the settlement and restores the original end state.
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(910, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE_EQUAL(90,  wire_balance("alice"_n));
} FC_LOG_AND_RETHROW() }

// [P0] WNS-02: paywire must refuse a payout larger than the WIRE its own curve
// produces for `src_amount`. `wire_out` is a caller-supplied parameter, and the
// audited vulnerability was precisely a caller-chosen destination amount being
// paid out verbatim — so the reserve enforces its own floor rather than trusting
// `sysio.uwrit` to have derived it.
//
// This bound also subsumes the older "insufficient source reserve WIRE for
// payout + fee" guard: `w_gross` is capped at the reserve's own WIRE balance by
// `out_given_in`, so `wire_out + fee <= w_gross <= reserve_wire_amount` holds
// by construction once the payout is bounded. That guard stays in the contract
// as defense-in-depth but is no longer constructible through this action.
BOOST_FIXTURE_TEST_CASE(paywire_rejects_payout_above_curve_output, sysio_reserve_tester) { try {
   // 1000 token / 100 WIRE: the curve yields floor(100*100/1100) = 9 WIRE for a
   // 100-token source leg, so a 200 payout is ~22x the reserve's own price.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 100));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: paywire: payout exceeds the post-fee WIRE the source leg produced"),
      push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
         ("src_chain_code",   codename_mvo("ETH"))
         ("src_token_code",   codename_mvo("ETH"))
         ("src_reserve_code", codename_mvo("PRIMARY"))
         ("src_amount",       100)
         ("recipient",        "alice")
         ("wire_out",         200)
         ("underwriter",      "underwriter1")));

   // The curve's own output settles cleanly against the same reserve.
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "paywire"_n, mvo()
      ("src_chain_code",   codename_mvo("ETH"))
      ("src_token_code",   codename_mvo("ETH"))
      ("src_reserve_code", codename_mvo("PRIMARY"))
      ("src_amount",       100)
      ("recipient",        "alice")
      ("wire_out",         9)
      ("underwriter",      "underwriter1")));
   BOOST_REQUIRE_EQUAL(9u, wire_claimable("alice"_n));   // credited, not pushed
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(9, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(refundwire_returns_escrow, sysio_reserve_tester) { try {
   // Seed custody via a bootstrap reserve (the refund itself touches no
   // reserve row — it returns in-flight escrow). Zero revert fee: a no-fault
   // refund returns the full escrow.
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",      "alice")
      ("wire_amount",    150)
      ("revert_fee_bps", 0)));

   // The refund is CREDITED, not pushed. Until alice pulls it the WIRE stays in this
   // contract's custody, so the escrow is owed but has not moved.
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));
   BOOST_REQUIRE_EQUAL(0,    wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(1000, wire_balance(RESERVE_ACCOUNT));

   // Pulling it produces the original settlement: alice holds the escrow, custody drops by it.
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(150, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(850, wire_balance(RESERVE_ACCOUNT));
   BOOST_REQUIRE(get_wireclaim("alice"_n).is_null());   // row consumed by the payout

   auto r = find_reserve("ETH", "ETH", "PRIMARY");
   BOOST_REQUIRE_EQUAL(1000, r["reserve_wire_amount"].as_uint64());   // untouched

   BOOST_REQUIRE(get_rewardbkt().is_null());   // no fee — nothing accrued
} FC_LOG_AND_RETHROW() }

// The retention deadline has to hold WITHOUT further settlement traffic. `credit_wire_claim`
// sweeps opportunistically, so it only fires while swaps keep arriving; if traffic stops, nothing
// revisits an aged-out row and both the system-funded table and the WIRE it reserves stay
// outstanding forever. `sweepclaims` — inlined every epoch by `sysio.epoch::advance` — is the
// trigger that makes the window real, and `claimwire` refuses an expired row so the deadline means
// "claimable until" rather than "swept eventually".
BOOST_FIXTURE_TEST_CASE(expired_wire_claim_is_swept_without_further_traffic, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",      "alice")
      ("wire_amount",    150)
      ("revert_fee_bps", 0)));
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));

   const int64_t treasury_before = wire_balance(SYSIO_ACCOUNT);
   const int64_t custody_before  = wire_balance(RESERVE_ACCOUNT);

   // Age past the one-year window with NO further credits — the opportunistic sweep never runs.
   produce_block();
   produce_block(fc::days(366));
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));   // still sitting there

   // The deadline is enforced at the claim, so the forfeit balance cannot be pulled.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: claimwire: this claim expired and is pending sweep to the treasury"),
      claimwire("alice"_n));

   // A zero budget is a no-op even with an expired row waiting.
   BOOST_REQUIRE_EQUAL(success(), sweepclaims(0));
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));

   // The epoch-driven sweep reclaims it with no settlement traffic involved: the row is gone and
   // its WIRE has left custody for the treasury.
   BOOST_REQUIRE_EQUAL(success(), sweepclaims(reserve_max_claim_sweep_per_epoch));
   BOOST_REQUIRE(get_wireclaim("alice"_n).is_null());
   BOOST_REQUIRE_EQUAL(0u, wire_claimable("alice"_n));
   BOOST_REQUIRE_EQUAL(treasury_before + 150, wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(custody_before  - 150, wire_balance(RESERVE_ACCOUNT));

   // Nothing to claim afterwards — the balance is forfeit, not merely deferred.
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: no claimable WIRE for this account"),
                       claimwire("alice"_n));
} FC_LOG_AND_RETHROW() }

// Crediting an account whose row already expired must not resurrect the forfeited balance.
// `claimable::credit` upserts, so without settling first the old amount would be added to and its
// stamp refreshed — making a balance `claimwire` has been refusing claimable again, and letting a
// trickle of small credits keep a system-funded row alive forever. The new credit legitimately
// starts a fresh window (an account being credited is not abandoned); only the old balance is
// forfeit.
BOOST_FIXTURE_TEST_CASE(recredit_after_expiry_does_not_revive_the_forfeited_balance,
                        sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",      "alice")
      ("wire_amount",    150)
      ("revert_fee_bps", 0)));
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));

   // Age past the window WITHOUT sweeping, so the forfeited row is still sitting there.
   produce_block();
   produce_block(fc::days(366));
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));

   const int64_t treasury_before = wire_balance(SYSIO_ACCOUNT);

   // A later refund for the same account credits 40.
   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",      "alice")
      ("wire_amount",    40)
      ("revert_fee_bps", 0)));

   // Only the NEW amount is claimable — the forfeited 150 went to the treasury, it did not
   // accumulate into a 190 balance with a fresh window.
   BOOST_REQUIRE_EQUAL(40u, wire_claimable("alice"_n));
   BOOST_REQUIRE_EQUAL(treasury_before + 150, wire_balance(SYSIO_ACCOUNT));

   // And the fresh claim is live, so the recipient is not penalised for the new payout.
   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(40, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW() }

// A LIVE row is untouched by the sweep and still claims normally — the budget walks the
// expiry-ordered index and stops at the first row whose window is open.
BOOST_FIXTURE_TEST_CASE(sweepclaims_leaves_live_rows_alone, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",      "alice")
      ("wire_amount",    150)
      ("revert_fee_bps", 0)));

   BOOST_REQUIRE_EQUAL(success(), sweepclaims(reserve_max_claim_sweep_per_epoch));
   BOOST_REQUIRE_EQUAL(150u, wire_claimable("alice"_n));

   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(150, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW() }

// A nonzero revert fee (caller-fault drain revert) is taken out of the refund and
// routed exactly like a settlement fee, through `route_wire_fee`. Arithmetic here:
// 10% of 150 = 15, and a revert has no winning underwriter, so the whole 15 is the
// rewards pool. This fixture never calls `setconfig`, so `fee_emissions_share_bps`
// is the default 0 and all 15 accrues to `rewards_bucket` — custody-internal, with
// nothing transferred to the emissions treasury.
BOOST_FIXTURE_TEST_CASE(refundwire_routes_revert_fee, sysio_reserve_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      regreserve("ETH", "ETH", "PRIMARY", 1000, 1000));

   BOOST_REQUIRE_EQUAL(success(), push_action(UWRIT_ACCOUNT, "refundwire"_n, mvo()
      ("recipient",      "alice")
      ("wire_amount",    150)
      ("revert_fee_bps", 1000)));   // 10%

   // Credited net of the fee, and the refund waits to be pulled. With the default zero
   // emissions dial the whole fee stays in custody as rewards, so nothing leaves at all.
   BOOST_REQUIRE_EQUAL(135u, wire_claimable("alice"_n));         // 150 - 15 fee
   BOOST_REQUIRE_EQUAL(0,    wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(1000, wire_balance(RESERVE_ACCOUNT));     // nothing transferred out yet

   BOOST_REQUIRE_EQUAL(success(), claimwire("alice"_n));
   BOOST_REQUIRE_EQUAL(135, wire_balance("alice"_n));            // 150 - 15 fee
   BOOST_REQUIRE_EQUAL(865, wire_balance(RESERVE_ACCOUNT));      // 1000 - 135; the fee stays in custody

   // The whole fee, per the header: no underwriter share to carve out, and a
   // zero emissions dial leaves the pool intact. A configured dial would divert
   // its share to the emissions treasury instead.
   auto bkt = get_rewardbkt();
   BOOST_REQUIRE(!bkt.is_null());
   BOOST_REQUIRE_EQUAL(15u, bkt["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(15u, bkt["lifetime_accrued"].as_uint64());
   BOOST_REQUIRE(get_uwfees(UNDERWRITER_ACCOUNT).is_null());     // nobody underwrote it

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
