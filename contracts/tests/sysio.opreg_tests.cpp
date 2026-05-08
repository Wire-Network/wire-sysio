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

class sysio_opreg_tester : public tester {
public:
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;

   sysio_opreg_tester() {
      produce_blocks(2);

      create_accounts({
         OPREG_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT, MSGCH_ACCOUNT, TOKEN_ACCOUNT,
         "batchop.a"_n, "batchop.b"_n, "batchop.c"_n,
         "uwrit.a"_n, "producer.a"_n,
         "uwrit.alice"_n, "uwrit.bob"_n,         // for Task 2 deposit/withdraw/cancel tests
      });
      produce_blocks(2);

      // Deploy opreg
      set_code(OPREG_ACCOUNT, contracts::opreg_wasm());
      set_abi(OPREG_ACCOUNT, contracts::opreg_abi().data());
      set_privileged(OPREG_ACCOUNT);

      // Deploy epoch (opreg reads outposts table from it)
      set_code(EPOCH_ACCOUNT, contracts::epoch_wasm());
      set_abi(EPOCH_ACCOUNT, contracts::epoch_abi().data());
      set_privileged(EPOCH_ACCOUNT);

      produce_blocks();

      // Load opreg ABI serializer
      const auto* accnt = control->find_account_metadata(OPREG_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      opreg_abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));

      // Load epoch ABI serializer
      const auto* epoch_accnt = control->find_account_metadata(EPOCH_ACCOUNT);
      BOOST_REQUIRE(epoch_accnt != nullptr);
      abi_def epoch_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(epoch_accnt->abi, epoch_abi), true);
      epoch_abi_ser.set_abi(std::move(epoch_abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // ── Action helpers ──

   action_result push_opreg_action(name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(OPREG_ACCOUNT, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result push_epoch_action(name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(EPOCH_ACCOUNT, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result setconfig(uint32_t max_prod = 21, uint32_t max_batch = 63,
                           uint32_t max_uw = 21, uint64_t prune_delay = 600000) {
      return push_opreg_action(OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("max_available_producers", max_prod)
         ("max_available_batch_ops", max_batch)
         ("max_available_underwriters", max_uw)
         ("terminate_prune_delay_ms", prune_delay)
      );
   }

   action_result regoperator(name account, OperatorType type, bool is_bootstrapped) {
      return push_opreg_action(OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", account)
         ("type", type)
         ("is_bootstrapped", is_bootstrapped)
      );
   }

   action_result slash(name account, std::string reason) {
      return push_opreg_action(CHALG_ACCOUNT, "slash"_n, mvo()
         ("account", account)
         ("reason", reason)
      );
   }

   action_result prune() {
      return push_opreg_action(OPREG_ACCOUNT, "prune"_n, mvo());
   }

   // ── New-action helpers (Task 2 refactor) ──

   /// Internal-only deposit dispatched from sysio.msgch (require_auth(get_self())).
   /// Used by tests that need to seed an operator's balance without going
   /// through the WIRE-direct wirestake (which requires sysio.token).
   action_result deposit(name account, const std::string& chain, const std::string& token,
                         uint64_t amount, std::string tx_hash = "") {
      return push_opreg_action(OPREG_ACCOUNT, "deposit"_n, mvo()
         ("account",         account)
         ("chain",           chain)
         ("token_kind",      token)
         ("amount",          amount)
         ("outpost_tx_hash", tx_hash));
   }

   /// Operator-driven cross-chain withdraw (msgch dispatch path).
   action_result queuewtdw(name account, const std::string& chain, const std::string& token,
                           uint64_t amount) {
      return push_opreg_action(OPREG_ACCOUNT, "queuewtdw"_n, mvo()
         ("account",     account)
         ("chain",       chain)
         ("token_kind",  token)
         ("amount",      amount));
   }

   action_result cancelwtdw(name signer, name account, uint64_t request_id) {
      return push_opreg_action(signer, "cancelwtdw"_n, mvo()
         ("account",     account)
         ("request_id",  request_id));
   }

   action_result terminate(name account, std::string reason) {
      return push_opreg_action(OPREG_ACCOUNT, "terminate"_n, mvo()
         ("account",  account)
         ("reason",   reason));
   }

   action_result releaselock(name signer, name account,
                             const std::string& chain, const std::string& token,
                             uint64_t amount) {
      return push_opreg_action(signer, "releaselock"_n, mvo()
         ("account",     account)
         ("chain",       chain)
         ("token_kind",  token)
         ("amount",      amount));
   }

   /// Read a wtdwqueue row by request_id (primary key).
   fc::variant get_wtdw(uint64_t request_id) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT, "wtdwqueue"_n, request_id);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "withdraw_request", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // ── Table read helpers ──

   fc::variant get_opconfig() {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "opconfig"_n, "opconfig"_n);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "op_config", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "operator_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_serializer opreg_abi_ser;
   abi_serializer epoch_abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_opreg_tests)

// ── setconfig ──

BOOST_FIXTURE_TEST_CASE(setconfig_basic, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   auto cfg = get_opconfig();
   BOOST_REQUIRE_EQUAL(21, cfg["max_available_producers"].as_uint64());
   BOOST_REQUIRE_EQUAL(63, cfg["max_available_batch_ops"].as_uint64());
   BOOST_REQUIRE_EQUAL(21, cfg["max_available_underwriters"].as_uint64());
   BOOST_REQUIRE_EQUAL(600000, cfg["terminate_prune_delay_ms"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_queue, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: max_available_producers must be positive"),
      setconfig(0, 63, 21, 600000)
   );
} FC_LOG_AND_RETHROW() }

// ── regoperator ──

BOOST_FIXTURE_TEST_CASE(regoperator_bootstrapped_batch, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE_EQUAL("batchop.a", op["account"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_TYPE_BATCH", op["type"].as_string());
   // Bootstrapped → immediately ACTIVE (AVAILABLE)
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op["status"].as_string());
   BOOST_REQUIRE_EQUAL(1, op["is_bootstrapped"].as_uint64());
   BOOST_REQUIRE(op["available_at"].as_uint64() > 0);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_bootstrapped_producer, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("producer.a"_n, OPERATOR_TYPE_PRODUCER, true));
   produce_blocks();

   auto op = get_operator("producer.a"_n);
   BOOST_REQUIRE_EQUAL("OPERATOR_TYPE_PRODUCER", op["type"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op["status"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_uw_rejects_bootstrap, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   // Underwriters can NEVER be bootstrapped
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: underwriter type cannot be bootstrapped"),
      regoperator("uwrit.a"_n, OPERATOR_TYPE_UNDERWRITER, true)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_non_bootstrapped_pending, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   // Non-bootstrapped registration — status should be PENDING (UNKNOWN=0)
   // Note: this succeeds because opreg is the privileged caller (skips authex check)
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.a"_n, OPERATOR_TYPE_UNDERWRITER, false));
   produce_blocks();

   auto op = get_operator("uwrit.a"_n);
   BOOST_REQUIRE_EQUAL("uwrit.a", op["account"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_TYPE_UNDERWRITER", op["type"].as_string());
   // Non-bootstrapped without staking → PENDING (UNKNOWN)
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_UNKNOWN", op["status"].as_string());
   BOOST_REQUIRE_EQUAL(0, op["is_bootstrapped"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_duplicate_rejected, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();

   // Duplicate registration should fail
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator already registered"),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true)
   );
} FC_LOG_AND_RETHROW() }

// ── slash ──

BOOST_FIXTURE_TEST_CASE(slash_permanent, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();

   // Slash requires sysio.chalg auth
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.a"_n, "double sign"));
   produce_blocks();

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_SLASHED", op["status"].as_string());
   BOOST_REQUIRE(op["slashed_at"].as_uint64() > 0);

   // Cannot re-register after slash
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator already registered"),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true)
   );
} FC_LOG_AND_RETHROW() }

// ── prune ──

BOOST_FIXTURE_TEST_CASE(prune_requires_config, sysio_opreg_tester) { try {
   // prune without config should fail
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: opconfig not initialized"),
      prune()
   );
} FC_LOG_AND_RETHROW() }

// ── Multiple bootstrapped operators for initgroups ──

BOOST_FIXTURE_TEST_CASE(multiple_bootstrapped_batch_ops, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.c"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();

   // All three should be AVAILABLE
   auto op_a = get_operator("batchop.a"_n);
   auto op_b = get_operator("batchop.b"_n);
   auto op_c = get_operator("batchop.c"_n);
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op_a["status"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op_b["status"].as_string());
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op_c["status"].as_string());

   // Now configure epoch and run initgroups which reads from opreg
   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
      ("epoch_duration_sec", 90)
      ("operators_per_epoch", 1)
      ("batch_operator_minimum_active", 3)
      ("batch_op_groups", 3)
      ("epoch_retention_envelope_log_count", 200)
   ));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "initgroups"_n, mvo()));
   produce_blocks();

   // Verify epoch state has groups populated
   auto epoch_state_data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, "epochstate"_n);
   BOOST_REQUIRE(!epoch_state_data.empty());
   auto epoch_state = epoch_abi_ser.binary_to_variant(
      "epoch_state", epoch_state_data,
      abi_serializer::create_yield_function(abi_serializer_max_time));
   auto groups = epoch_state["batch_op_groups"].get_array();
   BOOST_REQUIRE_EQUAL(3, groups.size());
} FC_LOG_AND_RETHROW() }

// ── deposit (Task 2: msgch-dispatched outpost-driven deposit) ──

BOOST_FIXTURE_TEST_CASE(deposit_credits_balance_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 1'000'000));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());
   BOOST_REQUIRE_EQUAL("CHAIN_KIND_ETHEREUM", balances[0]["chain"].as_string());
   BOOST_REQUIRE_EQUAL("TOKEN_KIND_ETH",      balances[0]["token_kind"].as_string());
   BOOST_REQUIRE_EQUAL(1'000'000,             balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_aggregates_into_existing_balance_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 100));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 50));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());     // single row, NOT two
   BOOST_REQUIRE_EQUAL(150, balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_keeps_chain_token_pairs_separate, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 100));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_SOLANA",   "TOKEN_KIND_SOL", 200));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(2, balances.size());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_rejects_wire_chain, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   // WIRE-chain deposits MUST go through wirestake (operator-authorized
   // direct token transfer), not via msgch dispatch.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: WIRE-chain deposits go through wirestake (operator-authorized)"),
      deposit("uwrit.alice"_n, "CHAIN_KIND_WIRE", "TOKEN_KIND_WIRE", 100));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_rejects_slashed_operator, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(), slash("uwrit.alice"_n, "test slash"));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator not in a deposit-eligible state"),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 100));
} FC_LOG_AND_RETHROW() }

// ── queuewtdw + cancelwtdw (Task 2: 2-epoch withdraw queue + cancellation) ──

BOOST_FIXTURE_TEST_CASE(queuewtdw_creates_request_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 400));

   auto row = get_wtdw(1);   // monotonic id starts at 1
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL("uwrit.alice", row["account"].as_string());
   BOOST_REQUIRE_EQUAL(400,        row["amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(queuewtdw_rejects_insufficient_available, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 100));

   // Asking for more than the deposited balance fails the available()
   // sufficiency check (no locks / pending withdraws yet, so available
   // == balance).
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: insufficient available balance for withdraw"),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 200));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(queuewtdw_subtracts_from_available_on_subsequent_call, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 700));

   // After queueing 700, available() should reflect the reservation:
   // a second queue for 400 should fail (only 300 actually available).
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: insufficient available balance for withdraw"),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 400));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cancelwtdw_removes_pending_request, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 400));

   BOOST_REQUIRE_EQUAL(success(), cancelwtdw("uwrit.alice"_n, "uwrit.alice"_n, 1));

   // Row gone — get_wtdw returns null.
   auto row = get_wtdw(1);
   BOOST_REQUIRE(row.is_null());

   // Available should reset, so a fresh full-balance withdraw works.
   BOOST_REQUIRE_EQUAL(success(),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 1000));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cancelwtdw_rejects_other_operators_request, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.bob"_n,   OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      queuewtdw("uwrit.alice"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 400));

   // Bob signing tries to cancel Alice's request — must fail.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: not your withdraw request"),
      cancelwtdw("uwrit.bob"_n, "uwrit.bob"_n, 1));
} FC_LOG_AND_RETHROW() }

// ── terminate + releaselock (Task 2: administrative removal + uwrit hook) ──

BOOST_FIXTURE_TEST_CASE(terminate_marks_status_and_zeros_unlocked_balance, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(),
      deposit("batchop.a"_n, "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 500));

   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "rolling-24h: >5% miss rate"));

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_TERMINATED", op["status"].as_string());
   BOOST_REQUIRE(op["terminated_at"].as_uint64() > 0);
   // Unlocked portion (== entire balance, since no underwriter locks here)
   // got debited; balance row remains at 0.
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());
   BOOST_REQUIRE_EQUAL(0, balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(terminate_rejects_already_slashed_operator, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.a"_n, "double sign"));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator not in a terminable state"),
      terminate("batchop.a"_n, "post-slash terminate attempt"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(releaselock_requires_uwrit_authority, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   // Caller must be sysio.uwrit (the only contract that should ever invoke
   // the deferred-slash / deferred-remit path).
   BOOST_REQUIRE(
      releaselock(OPREG_ACCOUNT, "uwrit.alice"_n,
                  "CHAIN_KIND_ETHEREUM", "TOKEN_KIND_ETH", 100)
        .find("missing authority of sysio.uwrit") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
