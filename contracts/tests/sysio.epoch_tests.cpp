#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

/// v6: `regoutpost` is gone; `sysio.chains::regchain` is its replacement. The
/// tests still focus on epoch lifecycle, so they only depend on a `sysio.chains`
/// row existing for downstream epoch lookups.
class sysio_epoch_tester : public tester {
public:
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;

   sysio_epoch_tester() {
      produce_blocks(2);

      create_accounts({
         EPOCH_ACCOUNT, CHALG_ACCOUNT, MSGCH_ACCOUNT, CHAINS_ACCOUNT,
         "operator1"_n, "operator2"_n, "operator3"_n,
         "operator4"_n,
      });
      produce_blocks(2);

      set_code(EPOCH_ACCOUNT, contracts::epoch_wasm());
      set_abi(EPOCH_ACCOUNT, contracts::epoch_abi().data());
      set_privileged(EPOCH_ACCOUNT);

      set_code(CHAINS_ACCOUNT, contracts::chains_wasm());
      set_abi(CHAINS_ACCOUNT, contracts::chains_abi().data());
      set_privileged(CHAINS_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(EPOCH_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));

      const auto* chains_accnt = control->find_account_metadata(CHAINS_ACCOUNT);
      BOOST_REQUIRE(chains_accnt != nullptr);
      abi_def chains_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(chains_accnt->abi, chains_abi), true);
      chains_abi_ser.set_abi(std::move(chains_abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_epoch_action(name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(EPOCH_ACCOUNT, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result push_chains_action(name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(CHAINS_ACCOUNT, action_name, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result setconfig(uint32_t duration = 360, uint32_t ops_per = 7,
                           uint32_t total = 21, uint32_t grps = 3,
                           uint32_t retention = 200) {
      return push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("epoch_duration_sec", duration)
         ("operators_per_epoch", ops_per)
         ("batch_operator_minimum_active", total)
         ("batch_op_groups", grps)
         ("epoch_retention_envelope_log_count", retention)
      );
   }

   action_result advance() {
      return push_epoch_action(EPOCH_ACCOUNT, "advance"_n, mvo());
   }

   action_result schbatchgps() {
      return push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo());
   }

   /// v6 replacement for `regoutpost`: register a chain row in `sysio.chains`.
   /// Codenames stand in for the old `ChainKind` per-chain identity.
   action_result regchain(ChainKind kind, const std::string& code_str,
                          uint32_t external_chain_id,
                          const std::string& name_str = "test outpost",
                          const std::string& description = "") {
      auto code_v = fc::slug_name{code_str};
      return push_chains_action(CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", kind)
         ("code", mvo()("value", code_v.value))
         ("external_chain_id", external_chain_id)
         ("name", name_str)
         ("description", description)
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

   /// Read a chain row from sysio.chains by code (slug_name PK).
   fc::variant get_chain(const std::string& code_str) {
      auto code_v = fc::slug_name{code_str};
      auto data = get_row_by_id(CHAINS_ACCOUNT, CHAINS_ACCOUNT, "chains"_n, code_v.value);
      return data.empty() ? fc::variant() : chains_abi_ser.binary_to_variant(
         "chain_row",
         data,
         abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   abi_serializer abi_ser;
   abi_serializer chains_abi_ser;
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
   BOOST_REQUIRE_EQUAL(200, cfg["epoch_retention_envelope_log_count"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_validates_total, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups"),
      setconfig(360, 7, 20, 3)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_excess_batch_op_groups, sysio_epoch_tester) { try {
   // 255 groups (indices 0..254) stay clear of the batch_operator_plugin uint8
   // group sentinel (255) and are accepted; 256 is rejected. minimum_active must
   // equal operators_per_epoch * batch_op_groups, so 1 * 255 == 255 here.
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 1, 255, 255));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_op_groups exceeds the uint8 group-index ceiling (255)"),
      setconfig(360, 1, 256, 256)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_total_check_survives_uint32_wrap, sysio_epoch_tester) { try {
   // 16'843'010 * 255 == 4'294'967'550, which wraps a uint32 multiply to 254. The
   // widened uint64 product must reject a minimum_active of 254 rather than
   // spuriously matching the wrapped value.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups"),
      setconfig(360, 16'843'010u, 254, 255)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_oversized_operators_per_epoch, sysio_epoch_tester) { try {
   // Group size drives advance()'s per-epoch inline fanout and vector reserves,
   // so it carries a magnitude bound independent of the product equality: a
   // window of UINT32_MAX operators in one group is internally consistent
   // (UINT32_MAX * 1 == UINT32_MAX) yet must be rejected. 100 is the ceiling.
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 100, 100, 1));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operators_per_epoch exceeds the per-epoch schedule ceiling (100)"),
      setconfig(360, 101, 101, 1)
   );
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operators_per_epoch exceeds the per-epoch schedule ceiling (100)"),
      setconfig(360, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 1)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_oversized_schedule_window, sysio_epoch_tester) { try {
   // The window total (batch_operator_minimum_active == operators_per_epoch *
   // batch_op_groups) is carried in the epochstate row and in every per-outpost
   // BatchOperatorGroups attestation, so it has its own ceiling (1000) even when
   // the per-group size and group count are individually acceptable.
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 100, 1000, 10));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_operator_minimum_active exceeds the schedule-window ceiling (1000)"),
      setconfig(360, 100, 1100, 11)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regchain_basic, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1));
   produce_blocks();

   // Verify chain row written to sysio.chains
   auto row = get_chain("ETH");
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE(ChainKind::CHAIN_KIND_EVM == row["kind"].as<ChainKind>());
   BOOST_REQUIRE_EQUAL(1, row["external_chain_id"].as_uint64());

   // Duplicate code: should fail.
   BOOST_REQUIRE(
      regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1).find("already") != std::string::npos);
   produce_blocks();

   // Register a second chain with a distinct code.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 1));
   produce_blocks();

   auto row2 = get_chain("SOL");
   BOOST_REQUIRE(!row2.is_null());
   BOOST_REQUIRE(ChainKind::CHAIN_KIND_SVM == row2["kind"].as<ChainKind>());
} FC_LOG_AND_RETHROW() }

/// EVM rows must not share an `external_chain_id`: the pair (kind, external_chain_id) is the
/// outbound envelope's destination binding (`sysio.msgch::buildenv` stamps it into the route
/// endpoints, and EVM outposts verify it against their own block.chainid), so it must stay
/// injective across EVM rows. SVM rows are exempt — Solana clusters have no numeric chain id.
BOOST_FIXTURE_TEST_CASE(regchain_evm_external_id_unique, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1));
   produce_blocks();

   // A second EVM chain cannot reuse a registered EVM external_chain_id.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "POLYGON", 1)
      .find("an EVM chain with this external_chain_id already exists") != std::string::npos);

   // A distinct id registers fine.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "POLYGON", 137));
   produce_blocks();

   // SVM rows may share numeric ids with anything, including each other.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 1));
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOLDEV", 1));
   produce_blocks();
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

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: epoch advancement is paused"),
      advance()
   );

   BOOST_REQUIRE_EQUAL(success(), unpause());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(initgroups_no_opreg, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());

   // schbatchgps reads from sysio.opreg — which is not deployed in this fixture.
   // Should fail because there are no AVAILABLE batch operators.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: not enough available batch operators for group assignment"),
      schbatchgps()
   );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
