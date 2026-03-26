#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;

using mvo = fc::mutable_variant_object;

class sysio_epoch_tester : public tester {
public:
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;

   sysio_epoch_tester() {
      produce_blocks(2);

      create_accounts({
         EPOCH_ACCOUNT, CHALG_ACCOUNT, MSGCH_ACCOUNT,
         "operator1"_n, "operator2"_n, "operator3"_n,
         "operator4"_n, "operator5"_n, "operator6"_n,
         "operator7"_n, "op8"_n, "op9"_n,
         "op10"_n, "op11"_n, "op12"_n,
         "op13"_n, "op14"_n, "op15"_n,
         "op16"_n, "op17"_n, "op18"_n,
         "op19"_n, "op20"_n, "op21"_n,
         "newop"_n
      });
      produce_blocks(2);

      set_code(EPOCH_ACCOUNT, contracts::epoch_wasm());
      set_abi(EPOCH_ACCOUNT, contracts::epoch_abi().data());
      set_privileged(EPOCH_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(EPOCH_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_epoch_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);

      action act;
      act.account = EPOCH_ACCOUNT;
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

   action_result setconfig(uint32_t duration = 360, uint32_t ops_per = 7,
                           uint32_t total = 21, uint32_t grps = 3,
                           uint32_t warmup = 1, uint32_t cooldown = 1) {
      return push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("epoch_duration_sec", duration)
         ("operators_per_epoch", ops_per)
         ("batch_operator_minimum_active", total)
         ("batch_op_groups", grps)
         ("warmup_epochs", warmup)
         ("cooldown_epochs", cooldown)
      );
   }

   action_result regoperator(name account, uint8_t type = 2 /* BATCH */) {
      return push_epoch_action(EPOCH_ACCOUNT, "regoperator"_n, mvo()
         ("account", account)
         ("type", type)
      );
   }

   action_result unregoper(name account) {
      return push_epoch_action(account, "unregoper"_n, mvo()
         ("account", account)
      );
   }

   action_result advance() {
      return push_epoch_action(EPOCH_ACCOUNT, "advance"_n, mvo());
   }

   action_result initgroups() {
      return push_epoch_action(EPOCH_ACCOUNT, "initgroups"_n, mvo());
   }

   action_result replaceop(name old_op, name new_op) {
      return push_epoch_action(EPOCH_ACCOUNT, "replaceop"_n, mvo()
         ("old_op", old_op)
         ("new_op", new_op)
      );
   }

   action_result regoutpost(uint8_t chain_kind, uint32_t chain_id) {
      return push_epoch_action(EPOCH_ACCOUNT, "regoutpost"_n, mvo()
         ("chain_kind", chain_kind)
         ("chain_id", chain_id)
      );
   }

   action_result pause() {
      return push_epoch_action(CHALG_ACCOUNT, "pause"_n, mvo());
   }

   action_result unpause() {
      return push_epoch_action(CHALG_ACCOUNT, "unpause"_n, mvo());
   }

   fc::variant get_epoch_config() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochcfg"_n, "epochcfg"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "epoch_config",
         data,
         abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   fc::variant get_epoch_state() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, "epochstate"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "epoch_state",
         data,
         abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   fc::variant get_operator(name account) {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "operators"_n, account);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "operator_info",
         data,
         abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_epoch_tests)

BOOST_FIXTURE_TEST_CASE(setconfig_basic, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());

   auto cfg = get_epoch_config();
   BOOST_REQUIRE_EQUAL(360, cfg["epoch_duration_sec"].as_uint64());
   BOOST_REQUIRE_EQUAL(7, cfg["operators_per_epoch"].as_uint64());
   BOOST_REQUIRE_EQUAL(21, cfg["batch_operator_minimum_active"].as_uint64());
   BOOST_REQUIRE_EQUAL(3, cfg["batch_op_groups"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_validates_total, sysio_epoch_tester) { try {
   // batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups"),
      setconfig(360, 7, 20, 3)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_basic, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("operator1"_n, 2));

   auto op = get_operator("operator1"_n);
   BOOST_REQUIRE_EQUAL("operator1", op["account"].as_string());
   BOOST_REQUIRE_EQUAL(2, op["type"].as_uint64());  // BATCH
   BOOST_REQUIRE_EQUAL(1, op["status"].as_uint64()); // WARMUP
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_invalid_type, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: invalid operator type"),
      regoperator("operator1"_n, 99)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoutpost_basic, sysio_epoch_tester) { try {
   // ChainKind: ETHEREUM=2, chain_id=1
   BOOST_REQUIRE_EQUAL(success(), regoutpost(2, 1));

   // Duplicate should fail
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: outpost already registered"),
      regoutpost(2, 1)
   );

   // Different chain should succeed
   BOOST_REQUIRE_EQUAL(success(), regoutpost(3, 1)); // SOLANA
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(advance_before_config, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: epoch config not initialized"),
      advance()
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(pause_unpause, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), pause());

   // advance should fail while paused
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: epoch advancement is paused"),
      advance()
   );

   BOOST_REQUIRE_EQUAL(success(), unpause());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(initgroups_not_enough_operators, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());

   // Register only 5 operators — not enough for 21
   for (int i = 1; i <= 5; ++i) {
      std::string name_str = "operator" + std::to_string(i);
      BOOST_REQUIRE_EQUAL(success(), regoperator(name(name_str), 2));
   }

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: not enough active batch operators for group assignment"),
      initgroups()
   );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
