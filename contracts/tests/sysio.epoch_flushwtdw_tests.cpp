/// Cross-contract tests for the `sysio.epoch::advance` ↔ `sysio.opreg::
/// flushwtdw` integration (Task 9 of the operator-collateral plan).
///
/// This fixture is the smaller cousin of `sysio.dispatch_tests.cpp` — it
/// only deploys epoch + opreg + msgch (no uwrit / reserve), since the
/// flushwtdw path doesn't go through dispatch_attestation:
///
///   epoch::advance
///     ↳ inline action(permission_level{epoch, owner}, opreg, "flushwtdw"_n,
///                    {current_epoch_index})
///         ↳ opreg::flushwtdw walks `wtdwqueue` for matured rows; each row
///           either drops silently (slashed / unfunded) or subtracts from
///           `operators[].balances` and emits an OPERATOR_ACTION
///           (WITHDRAW_REMIT) attestation via `msgch::queueout`.
///
/// Auth: opreg::flushwtdw uses `require_auth(EPOCH_ACCOUNT)`. epoch
/// declares `permission_level{get_self()=epoch, owner}` on the inline
/// action, so EPOCH_ACCOUNT is in opreg's auth list — Pattern A, no
/// `updateauth` delegation needed.
///
/// Time advancement: epoch::advance silently no-ops if
/// `current_time < state.next_epoch_start`. The fixture pins
/// `epoch_duration_sec` to a small value and uses `produce_block(seconds)`
/// to step the wall clock past each epoch boundary before re-calling
/// advance.

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

using mvo = fc::mutable_variant_object;

class sysio_epoch_flushwtdw_tester : public tester {
public:
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto OPREG_ACCOUNT = "sysio.opreg"_n;
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto UWRIT_OP       = "uwrit.alice"_n;

   /// epoch_duration small enough that one `produce_block(1s)` between
   /// advances reliably crosses the boundary; large enough that intermediate
   /// helper calls in a single test case don't accidentally trip multiple
   /// boundaries.
   static constexpr uint32_t EPOCH_DURATION_SEC = 1;

   sysio_epoch_flushwtdw_tester() {
      produce_blocks(2);

      // sysio.authex is auto-created by base_tester (see tester.cpp), so
      // it must NOT be re-listed here — duplicating it throws
      // `account_name_exists_exception`. The other system accounts in this
      // list (epoch / opreg / msgch / chalg) are not pre-created.
      create_accounts({
         EPOCH_ACCOUNT, OPREG_ACCOUNT, MSGCH_ACCOUNT, CHALG_ACCOUNT,
         BATCHOP, UWRIT_OP
      });
      produce_blocks(2);

      deploy(EPOCH_ACCOUNT, contracts::epoch_wasm(), contracts::epoch_abi(), epoch_abi);
      deploy(OPREG_ACCOUNT, contracts::opreg_wasm(), contracts::opreg_abi(), opreg_abi);
      deploy(MSGCH_ACCOUNT, contracts::msgch_wasm(), contracts::msgch_abi(), msgch_abi);

      produce_blocks();
   }

   void deploy(name account, std::vector<uint8_t> wasm, std::vector<char> abi,
               abi_serializer& out_ser) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      set_privileged(account);
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def parsed_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, parsed_abi), true);
      out_ser.set_abi(std::move(parsed_abi),
                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Push an action against any deployed contract.
   action_result push(name contract, abi_serializer& ser, name signer,
                      name action_name, const fc::variant_object& data) {
      try {
         std::string action_type = ser.get_action_type(action_name);
         action act;
         act.account = contract;
         act.name    = action_name;
         act.data    = ser.variant_to_binary(action_type, data,
                        abi_serializer::create_yield_function(abi_serializer_max_time));
         act.authorization = std::vector<permission_level>{{signer, config::active_name}};
         signed_transaction trx;
         trx.actions.emplace_back(std::move(act));
         set_transaction_headers(trx);
         trx.sign(get_private_key(signer, "active"), control->get_chain_id());
         push_transaction(trx);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   /// One-shot bootstrap: epoch config + opreg config + a bootstrapped
   /// batch op + a pending underwriter + an Ethereum outpost + initgroups
   /// + the genesis advance.
   ///
   /// Note: `find_outpost_id_for_chain` in opreg returns 0 as the "no
   /// outpost" sentinel AND the first registered outpost gets id=0. To
   /// keep WITHDRAW_REMIT attestations from being silently dropped when
   /// targeting our chain, we register a placeholder SOLANA outpost first
   /// (id=0) and the real ETHEREUM outpost second (id=1). The placeholder
   /// also satisfies the contract's chain-uniqueness check (one outpost
   /// per chain_kind+chain_id pair).
   void bootstrap_for_flushwtdw() {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "setconfig"_n, mvo()
            ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
            ("operators_per_epoch",                 1)
            ("batch_operator_minimum_active",       1)
            ("batch_op_groups",                     1)
            ("epoch_retention_envelope_log_count",  200)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "setconfig"_n, mvo()
            ("max_available_producers",   21)
            ("max_available_batch_ops",   63)
            ("max_available_underwriters",21)
            ("terminate_prune_delay_ms",  600000)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",          BATCHOP.to_string())
            ("type",             OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped",  true)));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",          UWRIT_OP.to_string())
            ("type",             OperatorType::OPERATOR_TYPE_UNDERWRITER)
            ("is_bootstrapped",  false)));

      // Placeholder SOLANA outpost — soaks up id=0 so the real ETH outpost
      // sits at id=1 and `find_outpost_id_for_chain(ETHEREUM)` returns a
      // non-sentinel id that emit_withdraw_remit will accept.
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "regoutpost"_n, mvo()
            ("chain_kind", ChainKind::CHAIN_KIND_SOLANA)
            ("chain_id",   1)));
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "regoutpost"_n, mvo()
            ("chain_kind", ChainKind::CHAIN_KIND_ETHEREUM)
            ("chain_id",   31337)));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "initgroups"_n, mvo()));

      // Genesis advance — permissionless until current_epoch_index moves
      // off zero. Sets up the next_epoch_start wall-clock so subsequent
      // advances must wait out epoch_duration_sec.
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "advance"_n, mvo()));

      produce_blocks();
   }

   /// Step the wall clock past `next_epoch_start` and call advance once.
   /// Caller signs as msgch (post-genesis advance requires msgch or self
   /// auth). Returns the resulting `current_epoch_index` so the test can
   /// assert on it.
   uint32_t advance_one_epoch() {
      // Push wall-clock forward one full epoch_duration; produce_block's
      // default delta is 500 ms, so 2*EPOCH_DURATION_SEC blocks comfortably
      // crosses the boundary even at EPOCH_DURATION_SEC=1.
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT,
         "advance"_n, mvo()));
      produce_blocks();
      return current_epoch();
   }

   /// Read the running WIRE epoch index from epoch_state.
   uint32_t current_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT,
                                     "epochstate"_n, "epochstate"_n);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["current_epoch_index"].as<uint32_t>();
   }

   /// Build a TokenAmount mvo from a typed (TokenKind, amount) pair. Action
   /// signatures take the proto-struct `TokenAmount` rather than separate
   /// kind+scalar parameters — this packs the two for the ABI serializer.
   static fc::mutable_variant_object token_amount_mvo(TokenKind kind, uint64_t amount) {
      return mvo()("kind", kind)("amount", amount);
   }

   /// Direct opreg::depositinle, signed as opreg itself (require_auth(self)
   /// passes when self signs). Bypasses the msgch dispatch path tested
   /// separately in sysio.dispatch_tests.cpp — deposit semantics are the
   /// same either way once the auth gate is past. `actor` defaults to a
   /// well-formed Ethereum-shaped placeholder; tests here don't exercise
   /// the DEPOSIT_REVERT correlation path.
   action_result depositinle(name account, ChainKind chain, TokenKind token, uint64_t amount) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "depositinle"_n, mvo()
         ("account",              account.to_string())
         ("chain",                chain)
         ("amount",               token_amount_mvo(token, amount))
         ("actor",                mvo()
            ("kind",    ChainKind::CHAIN_KIND_ETHEREUM)
            ("address", std::vector<char>{}))
         ("original_message_id",  std::string(64, '0')));
   }

   /// Direct opreg::withdrawinle, signed as opreg.
   action_result withdrawinle(name account, ChainKind chain, TokenKind token, uint64_t amount) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "withdrawinle"_n, mvo()
         ("account",  account.to_string())
         ("chain",    chain)
         ("amount",   token_amount_mvo(token, amount)));
   }

   /// chalg-authorized slash hook (mirrors the production chalg→opreg path
   /// for testing slashed-during-the-wait drops).
   action_result slash(name account, std::string reason) {
      return push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n, mvo()
         ("account", account.to_string())
         ("reason",  reason));
   }

   /// Look up an opreg operator row by account name.
   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                     "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "operator_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Look up a wtdwqueue row by request_id.
   fc::variant get_wtdw(uint64_t request_id) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT,
                                "wtdwqueue"_n, request_id);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "withdraw_request", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Locate an operator's balance entry for a (chain, token_kind) pair.
   /// Reads each row's typed enum out of the variant via `as<EnumT>()`
   /// (FC_REFLECT_ENUM provides the from_variant overload), so the equality
   /// check operates on enum values, not their stringified names.
   uint64_t balance_of(name account, ChainKind chain, TokenKind token_kind) {
      auto op = get_operator(account);
      if (op.is_null()) return 0;
      const auto& arr = op["balances"].get_array();
      for (const auto& b : arr) {
         if (b["chain"].as<ChainKind>() == chain &&
             b["token_kind"].as<TokenKind>() == token_kind) {
            return b["balance"].as_uint64();
         }
      }
      return 0;
   }

   /// Count attestations of a given type currently sitting in
   /// `msgch.attestations`. The flushwtdw path never erases its emits
   /// (those are READY for the next buildenv), so a bounded scan from
   /// id=0 over the live keyspace is enough.
   uint32_t count_attestations(sysio::opp::types::AttestationType expected,
                               uint64_t scan_until = 32) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT,
                                   "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant("attestation_entry", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["type"].as<sysio::opp::types::AttestationType>() == expected) ++n;
      }
      return n;
   }

   abi_serializer epoch_abi, opreg_abi, msgch_abi;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_epoch_flushwtdw_tests)

/// `flushwtdw` must require sysio.epoch's authorization. A direct call
/// from any other actor (including opreg itself) is rejected — locks the
/// "only the epoch loop drives drains" invariant in.
BOOST_FIXTURE_TEST_CASE(flushwtdw_requires_epoch_auth, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   // OPREG signing its own flushwtdw — should be rejected since opreg is
   // NOT EPOCH. Any non-epoch actor produces the same error.
   auto r = push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "flushwtdw"_n, mvo()
      ("current_epoch", 999));
   BOOST_REQUIRE(r.find("missing authority of sysio.epoch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

/// End-to-end happy path: epoch::advance triggers opreg::flushwtdw which
/// drains the matured row and debits the balance. The remit attestation
/// emitted by emit_withdraw_remit is queued into msgch.attestations and
/// then immediately consumed by msgch::buildenv (also called inline by
/// advance), so this test stops at the on-chain state changes; the
/// remit-to-msgch.attestations side-effect is covered in isolation by
/// `flushwtdw_direct_emits_withdraw_remit_attestation` below.
BOOST_FIXTURE_TEST_CASE(advance_drains_matured_eth_withdraw, sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 5'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT = 2'000'000;
   const ChainKind   ETH_CHAIN  = ChainKind::CHAIN_KIND_ETHEREUM;
   const TokenKind   ETH_TOKEN  = TokenKind::TOKEN_KIND_ETH;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, WITHDRAW_AMOUNT));

   // Sanity: row exists pre-flush, balance not yet debited.
   BOOST_REQUIRE(!get_wtdw(/*request_id=*/1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT,
                       balance_of(UWRIT_OP, ETH_CHAIN, ETH_TOKEN));

   // WITHDRAW_WAIT_EPOCHS = 2 — two boundary crossings to mature.
   advance_one_epoch();
   advance_one_epoch();

   // Row drained, balance debited.
   BOOST_REQUIRE(get_wtdw(/*request_id=*/1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT,
                       balance_of(UWRIT_OP, ETH_CHAIN, ETH_TOKEN));
} FC_LOG_AND_RETHROW() }

/// Direct flushwtdw probe — bypasses epoch::advance (and the buildenv
/// it triggers, which would empty msgch.attestations) to verify the
/// `emit_withdraw_remit` side-effect lands as an
/// `ATTESTATION_TYPE_OPERATOR_ACTION` row in `msgch.attestations`.
/// The `current_epoch` argument is passed explicitly so we don't need
/// to crank the chain's wall clock past WITHDRAW_WAIT_EPOCHS — the
/// helper just compares the queued row's `eligible_at_epoch` against
/// the supplied value.
BOOST_FIXTURE_TEST_CASE(flushwtdw_direct_emits_withdraw_remit_attestation,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;
   const ChainKind   ETH_CHAIN  = ChainKind::CHAIN_KIND_ETHEREUM;
   const TokenKind   ETH_TOKEN  = TokenKind::TOKEN_KIND_ETH;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, WITHDRAW_AMOUNT));

   // Pass a `current_epoch` value comfortably past `eligible_at_epoch`
   // (which is queue-time epoch + WITHDRAW_WAIT_EPOCHS=2). Signing as
   // epoch satisfies opreg::flushwtdw's `require_auth(EPOCH_ACCOUNT)`.
   constexpr uint32_t FUTURE_EPOCH = 100;
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, EPOCH_ACCOUNT,
      "flushwtdw"_n, mvo()("current_epoch", FUTURE_EPOCH)));

   // wtdw row drained + balance debited (same invariants as the
   // end-to-end test, re-checked here so a regression in either path
   // surfaces in this isolated test too).
   BOOST_REQUIRE(get_wtdw(/*request_id=*/1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT - WITHDRAW_AMOUNT,
                       balance_of(UWRIT_OP, ETH_CHAIN, ETH_TOKEN));

   // emit_withdraw_remit's `msgch::queueout` call landed an attestation.
   // The OPERATORS / BATCH_OPERATOR_GROUPS attestations from epoch::
   // advance have NOT been queued (we never advanced post-genesis here),
   // so a single OPERATOR_ACTION row is the only thing in the table.
   BOOST_REQUIRE_GE(count_attestations(
      sysio::opp::types::AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION), 1u);
} FC_LOG_AND_RETHROW() }

/// Negative path: a single advance — only one boundary crossed —
/// leaves the queue row alone. This proves WITHDRAW_WAIT_EPOCHS=2 is
/// actually enforced and not bypassed by the first matured-eligible
/// advance.
BOOST_FIXTURE_TEST_CASE(single_advance_leaves_immature_row_intact,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;
   const ChainKind   ETH_CHAIN  = ChainKind::CHAIN_KIND_ETHEREUM;
   const TokenKind   ETH_TOKEN  = TokenKind::TOKEN_KIND_ETH;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, WITHDRAW_AMOUNT));

   advance_one_epoch();   // only one boundary — eligible_at_epoch is +2

   // Row should still be present, balance unchanged.
   BOOST_REQUIRE(!get_wtdw(/*request_id=*/1).is_null());
   BOOST_REQUIRE_EQUAL(INITIAL_DEPOSIT,
                       balance_of(UWRIT_OP, ETH_CHAIN, ETH_TOKEN));
} FC_LOG_AND_RETHROW() }

/// Slashed-during-the-wait: queue a withdraw, slash the operator, advance
/// past maturity. The flush helper must drop the row silently and NOT
/// credit the balance back — slashed funds were already routed to LP via
/// the slash flow, returning them via flush would double-spend.
BOOST_FIXTURE_TEST_CASE(slashed_operator_withdraw_drops_silently,
                        sysio_epoch_flushwtdw_tester) { try {
   bootstrap_for_flushwtdw();

   constexpr uint64_t INITIAL_DEPOSIT = 1'000'000;
   constexpr uint64_t WITHDRAW_AMOUNT =   400'000;
   const ChainKind   ETH_CHAIN  = ChainKind::CHAIN_KIND_ETHEREUM;
   const TokenKind   ETH_TOKEN  = TokenKind::TOKEN_KIND_ETH;

   BOOST_REQUIRE_EQUAL(success(),
      depositinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, INITIAL_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle(UWRIT_OP, ETH_CHAIN, ETH_TOKEN, WITHDRAW_AMOUNT));

   // Capture the post-slash balance — the slash routes the operator's
   // entire balance to the LP, so balance_of after slash is the value
   // we expect to see UNCHANGED across the flush.
   BOOST_REQUIRE_EQUAL(success(), slash(UWRIT_OP, "test slash"));
   uint64_t balance_after_slash = balance_of(UWRIT_OP, ETH_CHAIN, ETH_TOKEN);

   advance_one_epoch();
   advance_one_epoch();

   // Withdraw row gone (dropped silently).
   BOOST_REQUIRE(get_wtdw(/*request_id=*/1).is_null());
   // Balance unchanged from the post-slash state — flush did NOT credit
   // anything back, did NOT double-debit, did NOT emit a WITHDRAW_REMIT.
   BOOST_REQUIRE_EQUAL(balance_after_slash,
                       balance_of(UWRIT_OP, ETH_CHAIN, ETH_TOKEN));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
