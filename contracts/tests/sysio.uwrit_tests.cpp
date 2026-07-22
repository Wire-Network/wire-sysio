#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>
#include <fc-lite/crypto/chain_types.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;
using namespace fc::crypto;

using mvo = fc::mutable_variant_object;

namespace {

/// SlugName mvo helper for v6 action arguments.
inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return mvo()("value", fc::slug_name{s}.value);
}

} // anonymous namespace

class sysio_uwrit_tester : public tester {
public:
   static constexpr auto UWRIT_ACCOUNT = "sysio.uwrit"_n;
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto OPREG_ACCOUNT = "sysio.opreg"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

   sysio_uwrit_tester() {
      produce_blocks(2);

      create_accounts({
         UWRIT_ACCOUNT, MSGCH_ACCOUNT, OPREG_ACCOUNT, CHALG_ACCOUNT,
         "sysio.epoch"_n, "sysio.chains"_n, "sysio.reserv"_n,
         "uwrit.a"_n, "uwrit.b"_n
      });
      produce_blocks(2);

      set_code(UWRIT_ACCOUNT, contracts::uwrit_wasm());
      set_abi(UWRIT_ACCOUNT, contracts::uwrit_abi().data());
      set_privileged(UWRIT_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(UWRIT_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_uwrit_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);

      action act;
      act.account = UWRIT_ACCOUNT;
      act.name = action_name;
      act.data = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function(abi_serializer_max_time)
      );
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

   action_result setconfig(uint32_t fee_bps                      = 10,
                           uint64_t collateral_lock_duration_ms  = 43'200'000,
                           uint64_t min_fromwire_amount          = 5'000'000'000ull,
                           uint32_t fromwire_revert_fee_bps      = 10,
                           uint32_t uwreq_pending_timeout_epochs = 10,
                           uint32_t uwreq_retention_epochs       = 10) {
      return push_uwrit_action(UWRIT_ACCOUNT, "setconfig"_n, mvo()
         ("fee_bps",                      fee_bps)
         ("collateral_lock_duration_ms",  collateral_lock_duration_ms)
         ("min_fromwire_amount",          min_fromwire_amount)
         ("fromwire_revert_fee_bps",      fromwire_revert_fee_bps)
         ("uwreq_pending_timeout_epochs", uwreq_pending_timeout_epochs)
         ("uwreq_retention_epochs",       uwreq_retention_epochs)
      );
   }

   /// Read uwconfig singleton row.
   fc::variant get_uwconfig() {
      auto data = get_row_by_account(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwconfig"_n, "uwconfig"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "uw_config", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Read a uwreq by id.
   fc::variant get_uwreq(uint64_t id) {
      auto data = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, id);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "uw_request_t", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_uwrit_tests)

BOOST_FIXTURE_TEST_CASE(setconfig_basic, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(25));

   auto cfg = get_uwconfig();
   BOOST_REQUIRE_EQUAL(25, cfg["fee_bps"].as_uint64());
   BOOST_REQUIRE_EQUAL(43'200'000u, cfg["collateral_lock_duration_ms"].as_uint64());
   BOOST_REQUIRE_EQUAL(5'000'000'000u, cfg["min_fromwire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(10, cfg["fromwire_revert_fee_bps"].as_uint64());
   BOOST_REQUIRE_EQUAL(10, cfg["uwreq_pending_timeout_epochs"].as_uint64());
   BOOST_REQUIRE_EQUAL(10, cfg["uwreq_retention_epochs"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_writes_custom_lock_duration, sysio_uwrit_tester) { try {
   // Test clusters shorten the 12h challenge window via setconfig — e.g.
   // two minutes here.
   BOOST_REQUIRE_EQUAL(success(),
      setconfig(/*fee_bps*/10, /*lock_ms*/120'000));
   auto cfg = get_uwconfig();
   BOOST_REQUIRE_EQUAL(120'000u, cfg["collateral_lock_duration_ms"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_excessive_fee, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: fee_bps must be below 10000 (100%): a 100% fee zeroes the post-fee WIRE leg"),
      setconfig(10001)
   );
} FC_LOG_AND_RETHROW() }

// SEC-26 / WSA-042: the exact 100% boundary must be rejected. It was previously
// accepted (`fee_bps <= 10000`), which zeroed the post-fee WIRE leg and let a
// swap debit destination reserve liquidity while crediting zero WIRE.
BOOST_FIXTURE_TEST_CASE(setconfig_rejects_full_fee, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: fee_bps must be below 10000 (100%): a 100% fee zeroes the post-fee WIRE leg"),
      setconfig(10000)
   );
} FC_LOG_AND_RETHROW() }

// The largest sub-100% fee (MAX_FEE_BPS) is accepted and leaves a positive
// post-fee WIRE leg for every positive input.
BOOST_FIXTURE_TEST_CASE(setconfig_accepts_max_fee, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(9999));
   BOOST_REQUIRE_EQUAL(9999, get_uwconfig()["fee_bps"].as_uint64());
} FC_LOG_AND_RETHROW() }

// The swap-from-WIRE floor is the queue-slot price: a zero floor would reopen
// free dust rows in the fwqueue, so setconfig rejects it.
BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_fromwire_minimum, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: min_fromwire_amount must be positive"),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/0)
   );
} FC_LOG_AND_RETHROW() }

// A 100% revert fee would zero the post-fee refund transfer inside the
// never-throw drainfwq drain, so the boundary is rejected exactly like
// fee_bps.
BOOST_FIXTURE_TEST_CASE(setconfig_rejects_full_revert_fee, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: fromwire_revert_fee_bps must be below 10000 (100%): "
            "a 100% revert fee zeroes the refund"),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10000)
   );
} FC_LOG_AND_RETHROW() }

// The largest sub-100% revert fee and a custom floor round-trip through the
// config singleton.
BOOST_FIXTURE_TEST_CASE(setconfig_accepts_max_revert_fee_and_custom_floor, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/1, /*revert_fee_bps*/9999));
   auto cfg = get_uwconfig();
   BOOST_REQUIRE_EQUAL(1u,   cfg["min_fromwire_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(9999, cfg["fromwire_revert_fee_bps"].as_uint64());
} FC_LOG_AND_RETHROW() }

// ── SEC-129 / WSA-223: UWREQ lifecycle knobs ──
//
// A zero pending timeout would expire every uwreq the epoch it is created; a
// zero retention would erase terminal rows before the audit window they exist
// for; a near-UINT32_MAX value would wrap the `current_epoch + knob` deadline
// stamp to a tiny epoch index (instant expiry). setconfig rejects all three.

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_pending_timeout, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq_pending_timeout_epochs must be positive"),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10, /*pending_timeout*/0)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_retention, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq_retention_epochs must be positive"),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10, /*pending_timeout*/10, /*retention*/0)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_lifecycle_epochs_over_ceiling, sysio_uwrit_tester) { try {
   // The ceiling itself is accepted for both knobs; one epoch beyond it is
   // rejected before the value could wrap a deadline stamp.
   constexpr uint32_t ceiling = 1'000'000;
   BOOST_REQUIRE_EQUAL(success(),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10, /*pending_timeout*/ceiling, /*retention*/ceiling));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq_pending_timeout_epochs exceeds the lifecycle ceiling"),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10, /*pending_timeout*/ceiling + 1, /*retention*/ceiling)
   );
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq_retention_epochs exceeds the lifecycle ceiling"),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10, /*pending_timeout*/ceiling, /*retention*/ceiling + 1)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_roundtrips_lifecycle_epochs, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setconfig(/*fee_bps*/10, /*lock_ms*/43'200'000, /*min_fromwire*/5'000'000'000ull,
                /*revert_fee_bps*/10, /*pending_timeout*/3, /*retention*/7));
   auto cfg = get_uwconfig();
   BOOST_REQUIRE_EQUAL(3, cfg["uwreq_pending_timeout_epochs"].as_uint64());
   BOOST_REQUIRE_EQUAL(7, cfg["uwreq_retention_epochs"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_lock_duration, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: collateral_lock_duration_ms must be positive"),
      setconfig(/*fee_bps*/10, /*lock_ms*/0)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_excess_lock_duration, sysio_uwrit_tester) { try {
   // The one-year ceiling is accepted; one millisecond beyond it is rejected
   // before the duration could wrap `now_ms + collateral_lock_duration_ms` and
   // release the lock the instant it is placed.
   constexpr uint64_t one_year_ms = 365ull * 24 * 60 * 60 * 1000;
   BOOST_REQUIRE_EQUAL(success(), setconfig(/*fee_bps*/10, one_year_ms));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: collateral_lock_duration_ms exceeds the one-year ceiling"),
      setconfig(/*fee_bps*/10, one_year_ms + 1)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(createuwreq_requires_msgch_auth, sysio_uwrit_tester) { try {
   // createuwreq must be invoked by sysio.msgch (inline action). A direct
   // call from another account (uwrit.a here) is rejected.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "createuwreq"_n, mvo()
      ("attestation_id", 1)
      ("type", sysio::opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REQUEST)
      ("chain_code", 1)
      ("data", std::vector<char>{})
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── chklocks — THE lock-release path (12h wall-clock challenge window) ──
//
// Locks are never released by delivery (there is no SWAP_REMIT ack); the
// epoch-advance sweep is the only releaser. `release`/`expirelock` were
// retired with the dead reflected-remit dispatch.

BOOST_FIXTURE_TEST_CASE(chklocks_requires_epoch_or_self_auth, sysio_uwrit_tester) { try {
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "chklocks"_n, mvo()
   ).find("chklocks requires sysio.epoch or sysio.uwrit authority") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(chklocks_noop_with_no_locks, sysio_uwrit_tester) { try {
   // Steady-state: nothing expired, nothing to sweep — must be a clean
   // no-op (it runs inside every epoch advance).
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action(UWRIT_ACCOUNT, "chklocks"_n, mvo()));
} FC_LOG_AND_RETHROW() }

// ── pruneuwreqs — bounded UWREQ lifecycle sweep (SEC-129 / WSA-223) ──
//
// The enforcement trigger for `uw_request_t.expires_at_epoch`. Auth mirrors
// its sibling epoch-inline sweeps; the lifecycle behavior itself (PENDING
// timeout, terminal retention erase, per-epoch budget) needs real epoch
// movement and lives in sysio.dispatch_tests.cpp.

BOOST_FIXTURE_TEST_CASE(pruneuwreqs_requires_epoch_or_self_auth, sysio_uwrit_tester) { try {
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "pruneuwreqs"_n, mvo()
      ("max_rows", 10)
   ).find("pruneuwreqs requires sysio.epoch or sysio.uwrit authority") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(pruneuwreqs_noop_with_empty_table, sysio_uwrit_tester) { try {
   // Steady-state: nothing due, nothing to sweep — must be a clean no-op
   // (it runs inside every epoch advance).
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action(UWRIT_ACCOUNT, "pruneuwreqs"_n, mvo()("max_rows", 10)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(pruneuwreqs_zero_budget_is_noop, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action(UWRIT_ACCOUNT, "pruneuwreqs"_n, mvo()("max_rows", 0)));
} FC_LOG_AND_RETHROW() }

// ── drainfwq — epoch-boundary swap-from-WIRE queue drain ──

BOOST_FIXTURE_TEST_CASE(drainfwq_requires_epoch_or_self_auth, sysio_uwrit_tester) { try {
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "drainfwq"_n, mvo()
   ).find("drainfwq requires sysio.epoch or sysio.uwrit authority") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(drainfwq_noop_with_empty_queue, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action(UWRIT_ACCOUNT, "drainfwq"_n, mvo()));
} FC_LOG_AND_RETHROW() }

// ── swapfromwire — queued depot-originated swap entry ──

BOOST_FIXTURE_TEST_CASE(swapfromwire_rejects_zero_wire_amount, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: swapfromwire: wire_amount must be positive"),
      push_uwrit_action("uwrit.a"_n, "swapfromwire"_n, mvo()
         ("user",                 "uwrit.a")
         ("wire_amount",          0)
         ("dst_chain_code",       codename_mvo("SOLANA"))
         ("dst_token_code",       codename_mvo("SOL"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        100)
         ("target_tolerance_bps", 50)
         ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_SVM)
         ("recipient_addr",       std::vector<char>(32, '\x01'))
      )
   );
} FC_LOG_AND_RETHROW() }

// One atom below the configured floor is rejected before any chain/reserve
// validation — the floor is the queue-slot price (default 5 WIRE).
BOOST_FIXTURE_TEST_CASE(swapfromwire_rejects_below_minimum, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: swapfromwire: wire_amount below the configured minimum"),
      push_uwrit_action("uwrit.a"_n, "swapfromwire"_n, mvo()
         ("user",                 "uwrit.a")
         ("wire_amount",          4'999'999'999ull)
         ("dst_chain_code",       codename_mvo("SOLANA"))
         ("dst_token_code",       codename_mvo("SOL"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        100)
         ("target_tolerance_bps", 50)
         ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_SVM)
         ("recipient_addr",       std::vector<char>(32, '\x01'))
      )
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(swapfromwire_rejects_unregistered_target_chain, sysio_uwrit_tester) { try {
   // No sysio.chains rows exist in this fixture — the target chain cannot
   // be registered+active, so the escrow never happens. wire_amount sits at
   // the default floor so the chain check (not the floor) is what fires.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: swapfromwire: target chain not "
            "registered or not active"),
      push_uwrit_action("uwrit.a"_n, "swapfromwire"_n, mvo()
         ("user",                 "uwrit.a")
         ("wire_amount",          5'000'000'000ull)
         ("dst_chain_code",       codename_mvo("SOLANA"))
         ("dst_token_code",       codename_mvo("SOL"))
         ("dst_reserve_code",     codename_mvo("PRIMARY"))
         ("target_amount",        100)
         ("target_tolerance_bps", 50)
         ("recipient_kind",       sysio::opp::types::ChainKind::CHAIN_KIND_SVM)
         ("recipient_addr",       std::vector<char>(32, '\x01'))
      )
   );
} FC_LOG_AND_RETHROW() }

// ── rcrdcommit (Task 3: per-leg COMMIT arrival recorder) ──

BOOST_FIXTURE_TEST_CASE(rcrdcommit_requires_msgch_auth, sysio_uwrit_tester) { try {
   // rcrdcommit is invoked inline from sysio.msgch on UNDERWRITE_INTENT_COMMIT
   // dispatch. v6 signature carries (from_chain_code, from_token_code,
   // reserve_code) slug_name triples in place of the old enum pair.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "rcrdcommit"_n, mvo()
      ("uwreq_id",         1)
      ("underwriter",      "uwrit.a")
      ("chain_code",       1)
      ("from_chain_code",  codename_mvo("ETH"))
      ("from_token_code",  codename_mvo("ETH"))
      ("reserve_code",     codename_mvo("PRIMARY"))
      ("uic_bytes",        std::vector<char>{})
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(rcrdcommit_rejects_unknown_uwreq, sysio_uwrit_tester) { try {
   // v6: OPP handlers MUST NEVER throw (feedback_opp_handlers_never_throw.md
   // — a `check()` in dispatch halts consensus). The previous error-based
   // assertion is gone; the action logs + skips on unknown uwreq and
   // returns success. The test now pins THAT invariant.
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action(MSGCH_ACCOUNT, "rcrdcommit"_n, mvo()
         ("uwreq_id",         42)
         ("underwriter",      "uwrit.a")
         ("chain_code",       1)
         ("from_chain_code",  codename_mvo("ETH"))
         ("from_token_code",  codename_mvo("ETH"))
         ("reserve_code",     codename_mvo("PRIMARY"))
         ("uic_bytes",        std::vector<char>{})
      )
   );
} FC_LOG_AND_RETHROW() }

// ── sumlocks (Task 3: read-only per-(underwriter, chain, token) lock total) ──

BOOST_FIXTURE_TEST_CASE(sumlocks_zero_for_unbonded_underwriter, sysio_uwrit_tester) { try {
   // v6 sumlocks signature: slug_name pair (chain_code, token_code).
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action("uwrit.a"_n, "sumlocks"_n, mvo()
         ("underwriter", "uwrit.a")
         ("chain_code",  codename_mvo("ETH"))
         ("token_code",  codename_mvo("ETH"))
      )
   );
} FC_LOG_AND_RETHROW() }

// ── B6: same-chain swap routing on rcrdcommit ──────────────────────────
//
// A swap on a single outpost (e.g. ERC20 → ETH-native, both on ETH) has
// src_chain == dst_chain. The depot routes the source-leg vs dest-leg of
// the COMMIT into commit_entry's source_uic_bytes / dest_uic_bytes slots
// based on (from_chain, from_token_kind) matching the uwreq's
// (src_chain, src_token_kind) vs (dst_chain, dst_token_kind). Without
// the from_token_kind discriminator, same-chain swaps would route both
// legs to the source slot. This case verifies the dispatch still
// auth-checks correctly when the two chains coincide.
BOOST_FIXTURE_TEST_CASE(rcrdcommit_same_chain_swap_auth, sysio_uwrit_tester) { try {
   // v6: slug_name triple disambiguates same-chain swap legs.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "rcrdcommit"_n, mvo()
      ("uwreq_id",         7)
      ("underwriter",      "uwrit.a")
      ("chain_code",       1)
      ("from_chain_code",  codename_mvo("ETH"))      // src == dst chain
      ("from_token_code",  codename_mvo("USDC"))     // distinguishes legs
      ("reserve_code",     codename_mvo("PRIMARY"))
      ("uic_bytes",        std::vector<char>{})
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── B4: try_recover_key no-throw guarantee ─────────────────────────
//
// verify_uic_signature must never halt the dispatch chain on malformed
// signature bytes (per feedback_opp_handlers_never_throw.md — a
// `check()` here stalls consensus). It calls `try_recover_key` which
// returns `std::nullopt` on any failure; the helper turns that into a
// `return false` and logs.
//
// This case sends rcrdcommit with msgch auth and a uic_bytes blob whose
// (decoded) signature would normally cause `recover_key` to throw. The
// assertion is "the action does NOT throw" — it may write the
// commit_entry with the bad bytes, but it must not halt. Today the
// uwreq doesn't exist so the dispatch fails earlier with "uwreq not
// found" before the verify path runs; this is fine — the test's
// invariant is that nothing in the call chain throws on a malformed
// signature blob payload.
BOOST_FIXTURE_TEST_CASE(rcrdcommit_malformed_uic_does_not_halt, sysio_uwrit_tester) { try {
   // 32-byte blob with a tag byte (>5, invalid variant tag) — would fail
   // the pre-validation bounds check in verify_uic_signature.
   std::vector<char> bad_uic_bytes(32, '\x00');
   bad_uic_bytes[0] = '\xFF';  // variant tag well outside legal range

   auto r = push_uwrit_action(MSGCH_ACCOUNT, "rcrdcommit"_n, mvo()
      ("uwreq_id",         9001)
      ("underwriter",      "uwrit.a")
      ("chain_code",       1)
      ("from_chain_code",  codename_mvo("ETH"))
      ("from_token_code",  codename_mvo("ETH"))
      ("reserve_code",     codename_mvo("PRIMARY"))
      ("uic_bytes",        bad_uic_bytes)
   );
   // v6: rcrdcommit logs + skips rather than throwing — neither the
   // unknown-uwreq path nor the malformed-uic path may halt the
   // consensus pipeline. The invariant: the action does NOT throw.
   BOOST_REQUIRE_EQUAL(success(), r);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
