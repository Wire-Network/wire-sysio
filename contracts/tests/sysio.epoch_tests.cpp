#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

/// `regoutpost` is gone; `sysio.chains::regchain` is its replacement. The
/// tests still focus on epoch lifecycle, so they only depend on a `sysio.chains`
/// row existing for downstream epoch lookups.
class sysio_epoch_tester : public tester {
public:
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;

   sysio_epoch_tester() {
      produce_blocks(2);

      // sysio.opreg stays codeless: an epoch close's delivery accounting is sent inline to it.
      create_accounts({
         EPOCH_ACCOUNT, CHALG_ACCOUNT, MSGCH_ACCOUNT, CHAINS_ACCOUNT, OPREG_ACCOUNT,
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
      abi_ser.set_abi(std::move(abi), fixture_abi_yield_function());

      const auto* chains_accnt = control->find_account_metadata(CHAINS_ACCOUNT);
      BOOST_REQUIRE(chains_accnt != nullptr);
      abi_def chains_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(chains_accnt->abi, chains_abi), true);
      chains_abi_ser.set_abi(std::move(chains_abi), fixture_abi_yield_function());
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

   /// `advance`'s refusals.
   const action_result unauthorized_advance =
      error("assertion failure with message: only sysio.msgch or sysio.epoch may advance a chain's epoch");
   const action_result not_active_outpost =
      error("assertion failure with message: a chain's genesis advance requires the chain to be an active outpost");
   const action_result no_schedule_window = error(
      "assertion failure with message: a chain's genesis advance requires its schedule window: run schbatchgps for "
      "the chain first");

   /// The outpost chain the single-chain cases configure.
   static constexpr std::string_view ETH = "ETH";
   /// A second outpost chain, for the per-chain cases.
   static constexpr std::string_view SOL = "SOL";

   action_result setconfig(uint32_t duration = 360, uint32_t ops_per = 7,
                           uint32_t total = 21, uint32_t grps = 3,
                           uint32_t retention = 200, std::string_view chain = ETH) {
      return push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("chain_code", chain)
         ("epoch_duration_sec", duration)
         ("operators_per_epoch", ops_per)
         ("batch_operator_minimum_active", total)
         ("batch_op_groups", grps)
         ("epoch_retention_envelope_log_count", retention)
      );
   }

   action_result advance(std::string_view chain = ETH, name signer = EPOCH_ACCOUNT) {
      return push_epoch_action(signer, "advance"_n, mvo()("chain_code", chain));
   }

   /// The placeholder members `seat_window` seats, one per group. The fixture deploys no sysio.opreg, so
   /// none of them delivers.
   static constexpr std::array WINDOW_MEMBERS{"operator1"_n, "operator2"_n, "operator3"_n, "operator4"_n};

   /// Seat `chain`'s schedule window with `groups` one-member groups, the window a genesis advance needs.
   void seat_window(std::string_view chain = ETH, uint32_t groups = 3) {
      BOOST_REQUIRE_LE(groups, WINDOW_MEMBERS.size());
      std::vector<std::vector<name>> window;
      for (uint32_t g = 0; g < groups; ++g) window.push_back({WINDOW_MEMBERS[g]});
      sysio_system::test_support::epoch_state::seat_window(*this, abi_ser, chain, window);
   }

   /// Make `chain` ready for its genesis advance: an active outpost of `kind` (registered inside the
   /// bootstrap window) with its schedule window seated.
   void ready_genesis(std::string_view chain, ChainKind kind) {
      BOOST_REQUIRE_EQUAL(success(), regchain(kind, std::string(chain), 1));
      seat_window(chain);
   }

   action_result activchain(std::string_view chain) {
      return push_chains_action(CHAINS_ACCOUNT, "activchain"_n, mvo()("code", chain));
   }

   action_result seteconomy(uint32_t duration, time_point_sec start, name signer = EPOCH_ACCOUNT) {
      return push_epoch_action(signer, "seteconomy"_n, mvo()
         ("epoch_duration_sec", duration)("start_time", start));
   }

   action_result tickeconomy(uint32_t epoch_index) {
      return push_epoch_action(EPOCH_ACCOUNT, "tickeconomy"_n, mvo()("epoch_index", epoch_index));
   }

   fc::variant get_economic_config() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "economycfg"_n, "economycfg"_n);
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("economic_config", data, fixture_abi_yield_function());
   }

   action_result schbatchgps(std::string_view chain = ETH) {
      return push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo()("chain_code", chain));
   }

   /// Replacement for `regoutpost`: register a chain row in `sysio.chains`.
   /// Codenames stand in for the old `ChainKind` per-chain identity.
   action_result regchain(ChainKind kind, const std::string& code_str,
                          uint32_t external_chain_id,
                          const std::string& name_str = "test outpost",
                          const std::string& description = "") {
      return push_chains_action(CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", kind)
         ("code", code_str)
         ("external_chain_id", external_chain_id)
         ("name", name_str)
         ("description", description)
         ("outpost", sysio_system::test_support::no_outpost_mvo())
      );
   }

   action_result pause(std::string_view chain = ETH) {
      return push_epoch_action(CHALG_ACCOUNT, "pause"_n, mvo()("chain_code", chain));
   }

   action_result unpause(std::string_view chain = ETH) {
      return push_epoch_action(CHALG_ACCOUNT, "unpause"_n, mvo()("chain_code", chain));
   }

   /// `chain`'s `epochcfg` row; null variant when absent.
   fc::variant get_epoch_config(std::string_view chain = ETH) {
      auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochcfg"_n, fc::slug_name{chain}.value);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "epoch_config",
         data,
         fixture_abi_yield_function() );
   }

   /// `chain`'s `epochstate` row; null variant when absent.
   fc::variant get_epoch_state(std::string_view chain = ETH) {
      auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, fc::slug_name{chain}.value);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "epoch_state",
         data,
         fixture_abi_yield_function() );
   }

   /// The `netgenesis` marker; null variant before the network genesis.
   fc::variant get_network_genesis() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "netgenesis"_n, "netgenesis"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "network_genesis",
         data,
         fixture_abi_yield_function() );
   }

   /// Length of `chain`'s current epoch, in seconds.
   int64_t epoch_length_sec(std::string_view chain) {
      const auto state = get_epoch_state(chain);
      return (state["next_epoch_start"].as<time_point>() - state["current_epoch_start"].as<time_point>())
         .to_seconds();
   }

   /// Read a chain row from sysio.chains by code (slug_name PK).
   fc::variant get_chain(const std::string& code_str) {
      auto code_v = fc::slug_name{code_str};
      auto data = get_row_by_id(CHAINS_ACCOUNT, CHAINS_ACCOUNT, "chains"_n, code_v.value);
      return data.empty() ? fc::variant() : chains_abi_ser.binary_to_variant(
         "chain_row",
         data,
         fixture_abi_yield_function() );
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

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_fewer_than_two_batch_op_groups, sysio_epoch_tester) { try {
   // Each envelope announces the next epoch's group, so the group the slide appends must sit behind
   // another: two groups is the smallest window.
   const action_result below_floor =
      error("assertion failure with message: batch_op_groups must be >= MIN_BATCH_OP_GROUPS (2)");
   BOOST_REQUIRE_EQUAL(below_floor, setconfig(360, 7, 7, 1));
   BOOST_REQUIRE_EQUAL(below_floor, setconfig(360, 7, 0, 0));
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 7, 14, 2));
   BOOST_REQUIRE_EQUAL(2u, get_epoch_config()["batch_op_groups"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_excess_batch_op_groups, sysio_epoch_tester) { try {
   // Solana seats at most 8 groups. minimum_active must equal
   // operators_per_epoch * batch_op_groups, so 1 * 8 == 8 here.
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 1, 8, 8));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_op_groups exceeds the outpost group ceiling (8)"),
      setconfig(360, 1, 9, 9)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_total_check_survives_uint32_wrap, sysio_epoch_tester) { try {
   // 536'870'913 * 8 == 4'294'967'304, which wraps a uint32 multiply to 8. The
   // widened uint64 product must reject a minimum_active of 8 rather than
   // spuriously matching the wrapped value.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups"),
      setconfig(360, 536'870'913u, 8, 8)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_oversized_operators_per_epoch, sysio_epoch_tester) { try {
   // Group size drives advance()'s per-epoch inline fanout and vector reserves,
   // and Ethereum is sized for 7 per group, so it carries a bound independent of
   // the product equality: a window of two groups of 0x7FFF'FFFF operators is
   // internally consistent (0x7FFF'FFFF * 2 == 0xFFFF'FFFE) yet must be rejected.
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 7, 14, 2));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operators_per_epoch exceeds the outpost group-size ceiling (7)"),
      setconfig(360, 8, 16, 2)
   );
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operators_per_epoch exceeds the outpost group-size ceiling (7)"),
      setconfig(360, 0x7FFF'FFFFu, 0xFFFF'FFFEu, 2)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_accepts_the_largest_schedule_window, sysio_epoch_tester) { try {
   // 8 groups of 7 is the largest window the outposts take. The two per-factor ceilings
   // bound the window, so it needs none of its own.
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 7, 56, 8));
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

/// The outbound envelope's destination binding is {kind, external_chain_id}
/// (`sysio.msgch::buildenv` stamps it into the route endpoints), so registration must keep it
/// injective per outpost: EVM rows must not share an `external_chain_id` (EVM outposts verify it
/// against their own block.chainid), and only ONE SVM row may exist — Solana clusters have no
/// numeric chain id, so the Solana gate is kind-only and a second row would be indistinguishable.
BOOST_FIXTURE_TEST_CASE(regchain_destination_binding_uniqueness, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1));
   produce_blocks();

   // A second EVM chain cannot reuse a registered EVM external_chain_id.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "POLYGON", 1)
      .find("an EVM chain with this external_chain_id already exists") != std::string::npos);

   // A distinct id registers fine.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "POLYGON", 137));
   produce_blocks();

   // An SVM row may share a numeric id with EVM rows (Solana has no chain id of its own)...
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, "SOL", 1));
   produce_blocks();

   // ...but a second SVM row is rejected outright: the wire binding cannot tell them apart.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_SVM, "SOLDEV", 2)
      .find("an SVM chain is already registered") != std::string::npos);
} FC_LOG_AND_RETHROW() }

/// Bootstrap invariant V3 (docs/platform-bootstrap-config.md) is "exactly one CHAIN_KIND_WIRE
/// chain, code WIRE". Only the cardinality half used to be on-chain; `is_depot` is derived from
/// the KIND alone, so a row could claim depot identity under any code and the code's validity
/// rested entirely on the off-chain config validator. CertiK WNS-11.
BOOST_FIXTURE_TEST_CASE(regchain_wire_requires_canonical_code, sysio_epoch_tester) { try {
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_WIRE, "FAKE", 0)
      .find("a WIRE chain must use the code WIRE") != std::string::npos);

   // The canonical code registers, and is the depot row.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0));
   produce_blocks();

   auto row = get_chain("WIRE");
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE(ChainKind::CHAIN_KIND_WIRE == row["kind"].as<ChainKind>());
   BOOST_REQUIRE_EQUAL(true, row["is_depot"].as<bool>());

   // The cardinality half still holds — and is reported as such, not as a code error.
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0)
      .find("already") != std::string::npos);
} FC_LOG_AND_RETHROW() }

/// The reservation is BIDIRECTIONAL. Enforcing only "kind WIRE => code WIRE" still admits
/// `regchain(EVM, "WIRE", ...)`, and because chain codes are unique with NO erase action that
/// row would permanently squat the depot's identity — leaving the canonical self-row
/// unregisterable and bootstrap bricked with no on-chain recovery. Ordering is the whole point
/// of this case: the squat is attempted BEFORE the depot row exists.
BOOST_FIXTURE_TEST_CASE(regchain_wire_code_reserved_from_other_kinds, sysio_epoch_tester) { try {
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "WIRE", 1)
      .find("the code WIRE is reserved for the depot self-row") != std::string::npos);
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_SVM, "WIRE", 0)
      .find("the code WIRE is reserved for the depot self-row") != std::string::npos);

   // Nothing squatted the code, so the canonical depot row still registers.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_WIRE, "WIRE", 0));
   produce_blocks();

   auto row = get_chain("WIRE");
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(true, row["is_depot"].as<bool>());

   // Non-WIRE codes are unaffected by the reservation.
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1));
} FC_LOG_AND_RETHROW() }

/// `name` and `description` persist into a row billed to the shared `sysio` RAM pool, so both
/// are bounded before emplace. CertiK WNS-10 raised this for `sysio.tokens::regtoken`; the same
/// unbounded pair existed on `sysio.chains::regchain`, and both now share
/// `sysio::opp::registry::check_metadata`.
BOOST_FIXTURE_TEST_CASE(regchain_bounds_metadata, sysio_epoch_tester) { try {
   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, std::string(129, 'x'))
      .find("label exceeds 128 bytes") != std::string::npos);

   BOOST_REQUIRE(regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, "ok", std::string(257, 'x'))
      .find("description exceeds 256 bytes") != std::string::npos);

   // The bounds are inclusive.
   BOOST_REQUIRE_EQUAL(success(),
      regchain(ChainKind::CHAIN_KIND_EVM, "ETH", 1, std::string(128, 'x'), std::string(256, 'x')));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(advance_before_config, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: epoch config not initialized"),
      advance()
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(seteconomy_validates_and_stores, sysio_epoch_tester) { try {
   constexpr uint32_t duration = 360;
   const time_point_sec start{control->head().block_time() + fc::seconds(duration)};
   constexpr uint32_t thirty_days = 30u * 24u * 60u * 60u;

   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.epoch"), seteconomy(duration, start, "operator1"_n));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: economic epoch_duration_sec must be >= MIN_ECONOMIC_EPOCH_DURATION_SEC"),
      seteconomy(59, start));
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: economic epoch_duration_sec exceeds 30-day ceiling"),
                       seteconomy(thirty_days + 1, start));
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: economic start_time must be set"),
                       seteconomy(duration, time_point_sec{}));

   // The bounds are inclusive. While no boundary has passed the grid may move, into the past too: the clock then
   // catches up through the elapsed epochs.
   BOOST_REQUIRE_EQUAL(success(), seteconomy(thirty_days, start));
   const time_point_sec past{start.sec_since_epoch() - 7u * 24u * 60u * 60u};
   BOOST_REQUIRE_EQUAL(success(), seteconomy(duration, past));
   const auto cfg = get_economic_config();
   BOOST_REQUIRE_EQUAL(duration, cfg["epoch_duration_sec"].as<uint32_t>());
   BOOST_REQUIRE(past == cfg["start_time"].as<time_point_sec>());

   // Boundaries have now passed without their ticks: the grid stays as it is until the crank catches up, though
   // restating it is accepted.
   const std::string behind = "assertion failure with message: economic clock is behind: tick every passed boundary "
                              "before changing it";
   BOOST_REQUIRE_EQUAL(error(behind), seteconomy(duration * 2, past));
   BOOST_REQUIRE_EQUAL(error(behind), seteconomy(duration, start));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), seteconomy(duration, past));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(tickeconomy_waits_for_config_and_consensus_genesis, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: economic clock not configured"), tickeconomy(1));

   BOOST_REQUIRE_EQUAL(success(), seteconomy(360, time_point_sec{control->head().block_time()}));
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: the economic clock starts at the network genesis"),
                       tickeconomy(1));
} FC_LOG_AND_RETHROW() }

// Without emissions state the economic clock still ticks: the gate holds the epoch's emissions and
// records why, and the duty settlement step is skipped, so the tick needs nothing from sysio.system.
BOOST_FIXTURE_TEST_CASE(tickeconomy_runs_without_emissions_state, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), seteconomy(360, time_point_sec{control->head().block_time()}));
   ready_genesis(ETH, ChainKind::CHAIN_KIND_EVM);
   BOOST_REQUIRE_EQUAL(success(), advance());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), tickeconomy(1));

   const auto state = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "economystate"_n, "economystate"_n);
   BOOST_REQUIRE(!state.empty());
   BOOST_REQUIRE_EQUAL(1u, abi_ser.binary_to_variant("economic_state", state, fixture_abi_yield_function())
                              ["current_epoch_index"].as<uint32_t>());
   const auto log = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "blocklog"_n, 1);
   BOOST_REQUIRE(!log.empty());
   BOOST_REQUIRE_EQUAL("EMISSIONS_BLOCK_REASON_CONFIG_MISSING",
                       abi_ser.binary_to_variant("blocklog_entry", log, fixture_abi_yield_function())
                          ["reason"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(pause_unpause, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), pause());

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: epoch advancement is paused"),
      advance()
   );

   BOOST_REQUIRE_EQUAL(success(), unpause());
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: epoch state not initialized"), unpause(SOL));
} FC_LOG_AND_RETHROW() }

/// Each outpost chain holds its own config row; configuring one leaves the other untouched.
BOOST_FIXTURE_TEST_CASE(setconfig_is_per_chain, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE_EQUAL(success(), setconfig(120, 1, 2, 2, 50, SOL));

   const auto eth = get_epoch_config(ETH);
   BOOST_REQUIRE_EQUAL(fc::slug_name{ETH}.value, eth["chain_code"].as_uint64());
   BOOST_REQUIRE_EQUAL(360u, eth["epoch_duration_sec"].as_uint64());
   BOOST_REQUIRE_EQUAL(3u, eth["batch_op_groups"].as_uint64());
   BOOST_REQUIRE_EQUAL(200u, eth["epoch_retention_envelope_log_count"].as_uint64());
   const auto sol = get_epoch_config(SOL);
   BOOST_REQUIRE_EQUAL(fc::slug_name{SOL}.value, sol["chain_code"].as_uint64());
   BOOST_REQUIRE_EQUAL(120u, sol["epoch_duration_sec"].as_uint64());
   BOOST_REQUIRE_EQUAL(2u, sol["batch_op_groups"].as_uint64());
   BOOST_REQUIRE_EQUAL(50u, sol["epoch_retention_envelope_log_count"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// Each chain's `setconfig` bounds its own duration to 10 s .. 30 days, both inclusive. A refused duration
/// stores nothing on a new chain and leaves a configured chain's row, and the other chain's, as they were. A chain
/// at the floor runs 10 s epochs.
BOOST_FIXTURE_TEST_CASE(setconfig_bounds_the_epoch_duration_per_chain, sysio_epoch_tester) { try {
   constexpr uint32_t kFloorSec        = 10;
   constexpr uint32_t kCeilingSec      = 30u * 24u * 60u * 60u;
   constexpr uint32_t kBlocksPerSecond = 2;
   const action_result below_floor =
      error("assertion failure with message: epoch_duration_sec must be >= MIN_EPOCH_DURATION_SEC");
   const action_result above_ceiling =
      error("assertion failure with message: epoch_duration_sec exceeds 30-day ceiling");
   const auto duration_of = [&](std::string_view chain) {
      return get_epoch_config(chain)["epoch_duration_sec"].as<uint32_t>();
   };

   BOOST_REQUIRE_EQUAL(below_floor, setconfig(kFloorSec - 1, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE(get_epoch_config(ETH).is_null());
   BOOST_REQUIRE_EQUAL(above_ceiling, setconfig(kCeilingSec + 1, 7, 21, 3, 200, SOL));
   BOOST_REQUIRE(get_epoch_config(SOL).is_null());

   BOOST_REQUIRE_EQUAL(success(), setconfig(kFloorSec, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE_EQUAL(success(), setconfig(kCeilingSec, 7, 21, 3, 200, SOL));
   BOOST_REQUIRE_EQUAL(kFloorSec, duration_of(ETH));
   BOOST_REQUIRE_EQUAL(kCeilingSec, duration_of(SOL));

   BOOST_REQUIRE_EQUAL(below_floor, setconfig(kFloorSec - 1, 7, 21, 3, 200, SOL));
   BOOST_REQUIRE_EQUAL(above_ceiling, setconfig(kCeilingSec + 1, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE_EQUAL(kFloorSec, duration_of(ETH));
   BOOST_REQUIRE_EQUAL(kCeilingSec, duration_of(SOL));

   ready_genesis(ETH, ChainKind::CHAIN_KIND_EVM);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(int64_t{kFloorSec}, epoch_length_sec(ETH));
   produce_blocks(kFloorSec * kBlocksPerSecond + 1);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(2u, get_epoch_state(ETH)["current_epoch_index"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(int64_t{kFloorSec}, epoch_length_sec(ETH));
} FC_LOG_AND_RETHROW() }

/// Two chains run their own counters on their own durations: a chain advances on its own boundary,
/// and a paused chain holds only itself.
BOOST_FIXTURE_TEST_CASE(chains_advance_on_their_own_cadence, sysio_epoch_tester) { try {
   constexpr uint32_t kEthDurationSec = 60;
   constexpr uint32_t kSolDurationSec = 120;
   constexpr uint32_t kBlocksPerSecond = 2;
   const auto index = [&](std::string_view chain) {
      return get_epoch_state(chain)["current_epoch_index"].as<uint32_t>();
   };
   BOOST_REQUIRE_EQUAL(success(), setconfig(kEthDurationSec, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE_EQUAL(success(), setconfig(kSolDurationSec, 7, 21, 3, 200, SOL));
   ready_genesis(ETH, ChainKind::CHAIN_KIND_EVM);
   ready_genesis(SOL, ChainKind::CHAIN_KIND_SVM);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   BOOST_REQUIRE_EQUAL(success(), advance(SOL));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, index(ETH));
   BOOST_REQUIRE_EQUAL(1u, index(SOL));
   BOOST_REQUIRE_EQUAL(kEthDurationSec, epoch_length_sec(ETH));
   BOOST_REQUIRE_EQUAL(kSolDurationSec, epoch_length_sec(SOL));

   // Only ETH's boundary has passed: SOL's advance is a no-op.
   produce_blocks(kEthDurationSec * kBlocksPerSecond + 1);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   BOOST_REQUIRE_EQUAL(success(), advance(SOL));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(2u, index(ETH));
   BOOST_REQUIRE_EQUAL(1u, index(SOL));

   // A paused SOL holds only itself.
   BOOST_REQUIRE_EQUAL(success(), pause(SOL));
   produce_blocks(kEthDurationSec * kBlocksPerSecond + 1);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: epoch advancement is paused"), advance(SOL));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(3u, index(ETH));
   BOOST_REQUIRE_EQUAL(1u, index(SOL));
   BOOST_REQUIRE(!get_epoch_state(ETH)["is_paused"].as<bool>());

   BOOST_REQUIRE_EQUAL(success(), unpause(SOL));
   BOOST_REQUIRE_EQUAL(success(), advance(SOL));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(3u, index(ETH));
   BOOST_REQUIRE_EQUAL(2u, index(SOL));
} FC_LOG_AND_RETHROW() }

/// The first chain's genesis advance is the network genesis: it writes the `netgenesis` marker, which
/// closes the bootstrap window (a chain registered after it starts inactive, so it cannot start until
/// it is activated), and a later chain's genesis leaves the marker as it was.
BOOST_FIXTURE_TEST_CASE(first_genesis_advance_is_the_network_genesis, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, std::string(ETH), 1));
   BOOST_REQUIRE(get_chain(std::string(ETH))["active"].as<bool>());
   BOOST_REQUIRE(get_network_genesis().is_null());

   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE_EQUAL(success(), setconfig(360, 7, 21, 3, 200, SOL));
   seat_window(ETH);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   produce_blocks();
   const auto genesis = get_network_genesis();
   BOOST_REQUIRE(!genesis.is_null());
   const auto genesis_at = genesis["genesis_at"].as<time_point>();

   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, std::string(SOL), 1));
   BOOST_REQUIRE(!get_chain(std::string(SOL))["active"].as<bool>());
   seat_window(SOL);
   BOOST_REQUIRE_EQUAL(not_active_outpost, advance(SOL));
   BOOST_REQUIRE_EQUAL(success(), activchain(SOL));

   BOOST_REQUIRE_EQUAL(success(), advance(SOL));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, get_epoch_state(SOL)["current_epoch_index"].as<uint32_t>());
   BOOST_REQUIRE(genesis_at == get_network_genesis()["genesis_at"].as<time_point>());
} FC_LOG_AND_RETHROW() }

/// Only sysio.msgch (bootstrap, bootchain, chkcons) or sysio.epoch advances a chain, its genesis included.
BOOST_FIXTURE_TEST_CASE(advance_requires_msgch_or_epoch_authority, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(60, 7, 21, 3, 200, ETH));
   ready_genesis(ETH, ChainKind::CHAIN_KIND_EVM);
   BOOST_REQUIRE_EQUAL(unauthorized_advance, advance(ETH, "operator1"_n));
   BOOST_REQUIRE(get_network_genesis().is_null());
   BOOST_REQUIRE_EQUAL(0u, get_epoch_state(ETH)["current_epoch_index"].as<uint32_t>());

   BOOST_REQUIRE_EQUAL(success(), advance(ETH, MSGCH_ACCOUNT));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, get_epoch_state(ETH)["current_epoch_index"].as<uint32_t>());

   constexpr uint32_t kBlocksPerSecond = 2;
   produce_blocks(60 * kBlocksPerSecond + 1);
   BOOST_REQUIRE_EQUAL(unauthorized_advance, advance(ETH, "operator1"_n));
   BOOST_REQUIRE_EQUAL(success(), advance(ETH, MSGCH_ACCOUNT));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(2u, get_epoch_state(ETH)["current_epoch_index"].as<uint32_t>());
} FC_LOG_AND_RETHROW() }

/// A chain's genesis advance needs an active outpost whose schedule window is seated; each refusal
/// leaves the chain and the network unstarted.
BOOST_FIXTURE_TEST_CASE(genesis_advance_requires_an_active_outpost_with_a_window, sysio_epoch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(60, 7, 21, 3, 200, ETH));
   BOOST_REQUIRE_EQUAL(not_active_outpost, advance(ETH));

   BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, std::string(ETH), 1));
   BOOST_REQUIRE_EQUAL(no_schedule_window, advance(ETH));
   BOOST_REQUIRE(get_network_genesis().is_null());

   seat_window(ETH);
   BOOST_REQUIRE_EQUAL(success(), advance(ETH));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, get_epoch_state(ETH)["current_epoch_index"].as<uint32_t>());
   BOOST_REQUIRE(!get_network_genesis().is_null());
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
