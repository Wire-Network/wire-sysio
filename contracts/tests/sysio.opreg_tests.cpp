#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include <limits>

#include "contracts.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;

namespace {

/// Standard opreg prune delay used by focused setconfig tests.
constexpr uint64_t kDefaultPruneDelayMs = 600000;

/// Current production consecutive-miss threshold.
constexpr uint32_t kDefaultMaxConsecutiveMisses = 5;

/// Current production rolling-window miss-percentage threshold.
constexpr uint32_t kDefaultMaxPctMisses24h = 5;

/// Highest accepted miss percentage after SEC-28; 100% is intentionally rejected.
constexpr uint32_t kMaxAcceptedPctMisses24h = 99;

/// Disabling percent threshold rejected because an all-miss window cannot exceed it.
constexpr uint32_t kDisablingPctMisses24h = 100;

/// Compact collateral amount used to activate non-bootstrapped batch operators.
constexpr uint64_t kTestMinBond = 1;

/// Standard 24h rolling-window size used by opreg tests.
constexpr uint64_t kTerminateWindowMs = 24ULL * 60 * 60 * 1000;

} // namespace

/// v6 data-model: per-chain identity has moved from `ChainKind` enums to
/// `sysio::slug_name`-keyed registries (`sysio.chains`, `sysio.tokens`,
/// `sysio.reserv`). The test fixture treats the codenames as opaque uint64
/// values; per-chain spelling ("ETH", "SOL", "WIRE", "LIQETH", ...) maps to
/// the host-side `fc::slug_name` packing algorithm so the bytes match what
/// the contract emplaces under.
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

   // ── SlugName helpers (v6) ──
   //
   // Codenames are 8-byte packed identifiers (`fc::slug_name`). The contract's
   // `sysio::slug_name` and the host-side `fc::slug_name` use the same packing
   // algorithm so values are byte-identical across the boundary.

   static fc::slug_name cn(std::string_view s) { return fc::slug_name{s}; }

   /// Build a slug_name mvo suitable for an action argument:
   /// `{"value": <uint64>}` matches the ABI surface for slug_name fields.
   static fc::mutable_variant_object codename_mvo(std::string_view s) {
      return mvo()("value", fc::slug_name{s}.value);
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

   /// Build a single `chain_min_bond` entry as an fc::variant suitable for
   /// `setconfig`'s `req_*_collat` vector arguments. v6: identity is by
   /// (chain_code, token_code) codenames rather than the old enums.
   static fc::variant make_chain_min_bond(std::string_view chain_code,
                                          std::string_view token_code,
                                          uint64_t min_bond) {
      return fc::variant(mvo()
         ("chain_code",           codename_mvo(chain_code))
         ("token_code",           codename_mvo(token_code))
         ("min_bond",             min_bond)
         ("config_timestamp_ms",  uint64_t{0}));
   }

   /// Push `sysio.opreg::setconfig` with sane defaults.
   action_result setconfig(uint32_t max_prod = 21, uint32_t max_batch = 63,
                           uint32_t max_uw = 21, uint64_t prune_delay = 600000,
                           uint32_t max_consec_misses = 5,
                           uint32_t max_pct_misses_24h = 5,
                           uint64_t terminate_window_ms = 24ULL * 60 * 60 * 1000,
                           std::vector<fc::variant> req_prod_collat    = {},
                           std::vector<fc::variant> req_batchop_collat = {},
                           std::vector<fc::variant> req_uw_collat      = {}) {
      return push_opreg_action(OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("max_available_producers",          max_prod)
         ("max_available_batch_ops",          max_batch)
         ("max_available_underwriters",       max_uw)
         ("terminate_prune_delay_ms",         prune_delay)
         ("terminate_max_consecutive_misses", max_consec_misses)
         ("terminate_max_pct_misses_24h",     max_pct_misses_24h)
         ("terminate_window_ms",              terminate_window_ms)
         ("req_prod_collat",                  req_prod_collat)
         ("req_batchop_collat",               req_batchop_collat)
         ("req_uw_collat",                    req_uw_collat)
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

   /// Record a delivery hit/miss through the same opreg action invoked by
   /// `sysio.epoch::advance`.
   action_result recorddel(name account, uint32_t epoch, bool delivered) {
      return push_opreg_action(EPOCH_ACCOUNT, "recorddel"_n, mvo()
         ("account",   account)
         ("epoch",     epoch)
         ("delivered", delivered));
   }

   /// Run opreg's rolling-window termination check through the epoch-authorized
   /// action surface.
   action_result termcheck(name account) {
      return push_opreg_action(EPOCH_ACCOUNT, "termcheck"_n, mvo()
         ("account", account));
   }

   /// Configure one ETH bond requirement and deposit it so a non-bootstrapped
   /// batch operator becomes ACTIVE through the normal eligibility path.
   void activate_batch_operator(name account,
                                uint32_t max_consec_misses = kDefaultMaxConsecutiveMisses,
                                uint32_t max_pct_misses_24h = kMaxAcceptedPctMisses24h) {
      BOOST_REQUIRE_EQUAL(success(), setconfig(
         /*max_prod=*/21,
         /*max_batch=*/63,
         /*max_uw=*/21,
         /*prune_delay=*/kDefaultPruneDelayMs,
         /*max_consec_misses=*/max_consec_misses,
         /*max_pct_misses_24h=*/max_pct_misses_24h,
         /*terminate_window_ms=*/kTerminateWindowMs,
         /*req_prod_collat=*/{},
         /*req_batchop_collat=*/{
            make_chain_min_bond("ETH", "ETH", kTestMinBond),
         },
         /*req_uw_collat=*/{}));

      BOOST_REQUIRE_EQUAL(success(),
         regoperator(account, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
      BOOST_REQUIRE_EQUAL(success(),
         depositinle(account, "ETH", "ETH", kTestMinBond));
      produce_blocks();

      auto op = get_operator(account);
      BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
      BOOST_REQUIRE_EQUAL(0, op["is_bootstrapped"].as_uint64());
   }

   // ── Collateral-action helpers (msgch-dispatched paths, v6 codenames) ──

   /// `depositinle`: dispatched from sysio.msgch.
   /// v6 signature: `(account, chain_code, token_code, amount,
   ///                actor_chain ChainKind, actor_address bytes,
   ///                original_message_id checksum256)`.
   action_result depositinle(name account,
                             std::string_view chain_code, std::string_view token_code,
                             uint64_t amount,
                             ChainKind actor_chain = ChainKind::CHAIN_KIND_EVM,
                             const std::vector<char>& actor_address = {},
                             const std::string& original_message_id_hex = std::string(64, '0')) {
      return push_opreg_action(OPREG_ACCOUNT, "depositinle"_n, mvo()
         ("account",              account)
         ("chain_code",           codename_mvo(chain_code))
         ("token_code",           codename_mvo(token_code))
         ("amount",               amount)
         ("actor_chain",          actor_chain)
         ("actor_address",        actor_address)
         ("original_message_id",  original_message_id_hex));
   }

   /// `withdrawinle`: same dispatch / auth model as `depositinle`.
   action_result withdrawinle(name account,
                              std::string_view chain_code, std::string_view token_code,
                              uint64_t amount) {
      return push_opreg_action(OPREG_ACCOUNT, "withdrawinle"_n, mvo()
         ("account",     account)
         ("chain_code",  codename_mvo(chain_code))
         ("token_code",  codename_mvo(token_code))
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
                             std::string_view chain_code, std::string_view token_code,
                             uint64_t amount) {
      return push_opreg_action(signer, "releaselock"_n, mvo()
         ("account",     account)
         ("chain_code",  codename_mvo(chain_code))
         ("token_code",  codename_mvo(token_code))
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

   /// Newest entry in the operator's `recent_actions` ring buffer.
   fc::variant latest_action_log(name account) {
      auto op = get_operator(account);
      if (op.is_null()) return fc::variant();
      const auto& log = op["recent_actions"].get_array();
      return log.empty() ? fc::variant() : log.back();
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
   BOOST_REQUIRE_EQUAL(kDefaultMaxConsecutiveMisses,
                       cfg["terminate_max_consecutive_misses"].as_uint64());
   BOOST_REQUIRE_EQUAL(kDefaultMaxPctMisses24h, cfg["terminate_max_pct_misses_24h"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_queue, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: max_available_producers must be positive"),
      setconfig(0, 63, 21, 600000)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_disabling_percent_miss_threshold, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_max_pct_misses_24h must be in [1, 99]"),
      setconfig(21, 63, 21, kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDisablingPctMisses24h, kTerminateWindowMs)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_disabling_consecutive_miss_threshold, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_max_consecutive_misses must be in [1, 5]"),
      setconfig(21, 63, 21, kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses + 1, kMaxAcceptedPctMisses24h, kTerminateWindowMs)
   );

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_max_consecutive_misses must be in [1, 5]"),
      setconfig(21, 63, 21, kDefaultPruneDelayMs,
                std::numeric_limits<uint32_t>::max(), kMaxAcceptedPctMisses24h, kTerminateWindowMs)
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
   BOOST_REQUIRE(OperatorType::OPERATOR_TYPE_BATCH == op["type"].as<OperatorType>());
   // Bootstrapped → immediately ACTIVE (AVAILABLE)
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(1, op["is_bootstrapped"].as_uint64());
   BOOST_REQUIRE(op["available_at"].as_uint64() > 0);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_bootstrapped_producer, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("producer.a"_n, OPERATOR_TYPE_PRODUCER, true));
   produce_blocks();

   auto op = get_operator("producer.a"_n);
   BOOST_REQUIRE(OperatorType::OPERATOR_TYPE_PRODUCER == op["type"].as<OperatorType>());
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
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

   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.a"_n, OPERATOR_TYPE_UNDERWRITER, false));
   produce_blocks();

   auto op = get_operator("uwrit.a"_n);
   BOOST_REQUIRE_EQUAL("uwrit.a", op["account"].as_string());
   BOOST_REQUIRE(OperatorType::OPERATOR_TYPE_UNDERWRITER == op["type"].as<OperatorType>());
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(0, op["is_bootstrapped"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regoperator_duplicate_rejected, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();

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

   BOOST_REQUIRE_EQUAL(success(), slash("batchop.a"_n, "double sign"));
   produce_blocks();

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_SLASHED == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE(op["updated_at"].as_uint64() > 0);

   // Cannot re-register after slash
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator already registered"),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true)
   );
} FC_LOG_AND_RETHROW() }

// ── prune ──

BOOST_FIXTURE_TEST_CASE(prune_requires_config, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: opconfig not initialized"),
      prune()
   );
} FC_LOG_AND_RETHROW() }

// ── Multiple bootstrapped operators for schbatchgps ──

BOOST_FIXTURE_TEST_CASE(multiple_bootstrapped_batch_ops, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.c"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();

   auto op_a = get_operator("batchop.a"_n);
   auto op_b = get_operator("batchop.b"_n);
   auto op_c = get_operator("batchop.c"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op_a["status"].as<OperatorStatus>());
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op_b["status"].as<OperatorStatus>());
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op_c["status"].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
      ("epoch_duration_sec", 90)
      ("operators_per_epoch", 1)
      ("batch_operator_minimum_active", 3)
      ("batch_op_groups", 3)
      ("epoch_retention_envelope_log_count", 200)
   ));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo()));
   produce_blocks();

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
      depositinle("uwrit.alice"_n, "ETH", "ETH", 1'000'000));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());
   BOOST_REQUIRE_EQUAL(cn("ETH").value, balances[0]["chain_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(cn("ETH").value, balances[0]["token_code"]["value"].as_uint64());
   BOOST_REQUIRE_EQUAL(1'000'000,       balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_aggregates_into_existing_balance_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 100));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 50));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());     // single row, NOT two
   BOOST_REQUIRE_EQUAL(150, balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

// SEC-103 (WSA-028 follow-up): operator collateral must never accumulate past
// asset::max_amount (2^62-1). A balance above the asset range would abort the
// WIRE-direct remit path's asset(balance, CORE_SYM); on the never-throw
// depositinle (OPP-inbound) path that abort would stall consensus. depositinle
// gates the RUNNING SUM: a credit that would push the balance over the cap is
// refunded via DEPOSIT_REVERT (the action still succeeds — never throws) and the
// stored balance is left unchanged. The single-value WSA-028 wrap is closed
// upstream in sysio.msgch; this is the accumulation guard.
BOOST_FIXTURE_TEST_CASE(depositinle_credit_over_max_collateral_is_reverted, sysio_opreg_tester) { try {
   // asset::max_amount — the Antelope asset magnitude limit (2^62 - 1), the cap
   // sysio.opreg enforces on a single balance row.
   constexpr uint64_t MAX_COLLATERAL = (uint64_t{1} << 62) - 1;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   // Credit the balance up to EXACTLY the cap — accepted (the boundary is inclusive).
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", MAX_COLLATERAL));
   {
      auto op       = get_operator("uwrit.alice"_n);
      auto balances = op["balances"].get_array();
      BOOST_REQUIRE_EQUAL(1, balances.size());
      BOOST_REQUIRE_EQUAL(MAX_COLLATERAL, balances[0]["balance"].as_uint64());
   }

   // A further +1 would push the sum to 2^62 (one past asset::max_amount). It is
   // refunded via DEPOSIT_REVERT: the action succeeds (never throws) and the
   // stored balance is unchanged — the credit did NOT wrap or saturate it in.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 1));
   {
      auto op       = get_operator("uwrit.alice"_n);
      auto balances = op["balances"].get_array();
      BOOST_REQUIRE_EQUAL(1, balances.size());
      BOOST_REQUIRE_EQUAL(MAX_COLLATERAL, balances[0]["balance"].as_uint64());
   }
} FC_LOG_AND_RETHROW() }

// SEC-103 (PR #449 review): the deposit cap must hold under RE-ENTRANCY. An
// operator account that carries contract code is notified by sysio.token::transfer
// when opreg::deposit moves its SYS collateral, and can re-enter deposit during
// that notification. Were the cap a pre-read-then-credit (not atomic with the
// mutation), two credits could pass against the same stale balance and push the
// WIRE collateral row past asset::max_amount. opreg::deposit performs the cap check
// INSIDE the same modify as the credit (and credits before the transfer), so the
// re-entrant deposit observes the already-committed balance: its own check trips
// and the whole transaction aborts. reenter_deposit (contracts/test_contracts)
// models the malicious operator — it re-enters deposit(+1) on the outgoing transfer.
BOOST_FIXTURE_TEST_CASE(deposit_reentrancy_cannot_exceed_max_collateral, sysio_opreg_tester) { try {
   constexpr uint64_t MAX_COLLATERAL = (uint64_t{1} << 62) - 1;     // asset::max_amount
   const std::string  CAP_SYS        = "461168601842738.7903 SYS";  // (2^62-1) at precision 4
   const auto         OPERATOR       = "uwrit.alice"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, false));

   // Core SYS token (opreg's CORE_SYM = symbol("SYS", 4)); fund the operator with
   // EXACTLY the cap so the outer deposit fills the WIRE row to asset::max_amount
   // and only the re-entrant +1 would exceed it.
   set_code(TOKEN_ACCOUNT, contracts::token_wasm());
   set_abi(TOKEN_ACCOUNT, contracts::token_abi().data());
   set_privileged(TOKEN_ACCOUNT);   // so create/issue can bill the stat/balance RAM
   produce_blocks();
   base_tester::push_action(TOKEN_ACCOUNT, "create"_n, TOKEN_ACCOUNT,
      mvo()("issuer", "sysio")("maximum_supply", CAP_SYS));
   base_tester::push_action(TOKEN_ACCOUNT, "issue"_n, config::system_account_name,
      mvo()("to", "sysio")("quantity", CAP_SYS)("memo", "seed"));
   base_tester::push_action(TOKEN_ACCOUNT, "transfer"_n, config::system_account_name,
      mvo()("from", "sysio")("to", OPERATOR)("quantity", CAP_SYS)("memo", "fund operator"));

   // Turn the operator into a re-entrant contract, and grant its active authority
   // the sysio.code permission so its inline deposit ({operator, active}) authorizes.
   set_code(OPERATOR, contracts::util::reenter_deposit_wasm());
   set_abi(OPERATOR, contracts::util::reenter_deposit_abi().data());
   {
      authority a(get_public_key(OPERATOR, "active"));
      a.accounts.push_back(permission_level_weight{ {OPERATOR, config::sysio_code_name}, 1 });
      set_authority(OPERATOR, config::active_name, a, config::owner_name);
   }
   produce_blocks();

   // Deposit the entire cap: the credit fills the WIRE row to asset::max_amount, the
   // outgoing SYS transfer notifies the operator, and its handler re-enters
   // deposit(+1). The +1 would push the row to 2^62 — its in-modify cap check trips
   // and aborts the whole transaction. No over-credit is possible.
   auto r = push_opreg_action(OPERATOR, "deposit"_n,
                              mvo()("account", OPERATOR)("amount", MAX_COLLATERAL));
   BOOST_REQUIRE_MESSAGE(r != success(), "re-entrant over-cap deposit unexpectedly succeeded");
   BOOST_REQUIRE_MESSAGE(r.find("deposit would exceed max collateral") != std::string::npos,
                         "unexpected failure reason: " + r);

   // The whole transaction reverted — no collateral was credited.
   auto op = get_operator(OPERATOR);
   BOOST_REQUIRE(!op.is_null());
   BOOST_REQUIRE_EQUAL(0u, op["balances"].get_array().size());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_keeps_chain_token_pairs_separate, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 100));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "SOL", "SOL", 200));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(2, balances.size());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(depositinle_logs_failure_when_operator_slashed, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(), slash("uwrit.alice"_n, "test slash"));

   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 100));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string("operator not in a deposit-eligible state"),
                       entry["error_message"].as_string());
} FC_LOG_AND_RETHROW() }

// ── queuewtdw + cancelwtdw ──

BOOST_FIXTURE_TEST_CASE(queuewtdw_creates_request_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 400));

   auto row = get_wtdw(1);   // monotonic id starts at 1
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL("uwrit.alice", row["account"].as_string());
   BOOST_REQUIRE_EQUAL(400,        row["amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(withdrawinle_logs_failure_on_insufficient_available, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 100));

   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 200));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string("insufficient available balance for withdraw"),
                       entry["error_message"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(withdrawinle_subtracts_from_available_on_subsequent_call, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 700));

   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 400));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string("insufficient available balance for withdraw"),
                       entry["error_message"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cancelwtdw_removes_pending_request, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 400));

   BOOST_REQUIRE_EQUAL(success(), cancelwtdw("uwrit.alice"_n, "uwrit.alice"_n, 1));

   auto row = get_wtdw(1);
   BOOST_REQUIRE(row.is_null());

   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 1000));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cancelwtdw_rejects_other_operators_request, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.bob"_n,   OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("uwrit.alice"_n, "ETH", "ETH", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      withdrawinle("uwrit.alice"_n, "ETH", "ETH", 400));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: not your withdraw request"),
      cancelwtdw("uwrit.bob"_n, "uwrit.bob"_n, 1));
} FC_LOG_AND_RETHROW() }

// ── terminate + releaselock ──

BOOST_FIXTURE_TEST_CASE(terminate_marks_status_and_zeros_unlocked_balance, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("batchop.a"_n, "ETH", "ETH", 500));

   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "rolling-24h: >5% miss rate"));

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE(op["terminated_at"].as_uint64() > 0);
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

BOOST_FIXTURE_TEST_CASE(termcheck_terminates_all_miss_window_at_max_accepted_percent, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL("rolling-window: >99% miss rate", op["status_reason"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_terminates_after_default_consecutive_boundary, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/true));
   produce_blocks();

   for (uint32_t epoch = 2; epoch <= 6; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      produce_blocks();
   }
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 7, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));

   op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL("rolling-window: >5 consecutive misses", op["status_reason"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_keeps_bootstrapped_operator_exempt, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*max_prod=*/21,
      /*max_batch=*/63,
      /*max_uw=*/21,
      /*prune_delay=*/kDefaultPruneDelayMs,
      /*max_consec_misses=*/1,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h,
      /*terminate_window_ms=*/kTerminateWindowMs));
   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/true));

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(1, op["is_bootstrapped"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(releaselock_requires_uwrit_authority, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE(
      releaselock(OPREG_ACCOUNT, "uwrit.alice"_n, "ETH", "ETH", 100)
        .find("missing authority of sysio.uwrit") != std::string::npos);
} FC_LOG_AND_RETHROW() }

// ── setconfig: per-(chain_code, token_code) collateral requirements ──

BOOST_FIXTURE_TEST_CASE(setconfig_two_chain_bond_activation, sysio_opreg_tester) { try {
   constexpr uint64_t MIN_BOND = 1'000'000;

   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*max_prod=*/21, /*max_batch=*/63, /*max_uw=*/21, /*prune_delay=*/600000,
      /*max_consec_misses=*/5, /*max_pct_misses_24h=*/5,
      /*terminate_window_ms=*/24ULL * 60 * 60 * 1000,
      /*req_prod_collat=*/{},
      /*req_batchop_collat=*/{
         make_chain_min_bond("ETH", "ETH", MIN_BOND),
         make_chain_min_bond("SOL", "SOL", MIN_BOND),
      },
      /*req_uw_collat=*/{}));

   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));

   // Pre-deposit: no balances → eligibility predicate fails → status UNKNOWN.
   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op["status"].as<OperatorStatus>());

   // After ETH bond: SOL still missing → still UNKNOWN.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("batchop.a"_n, "ETH", "ETH", MIN_BOND));
   op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op["status"].as<OperatorStatus>());

   // After SOL bond: every requirement met → ACTIVE.
   BOOST_REQUIRE_EQUAL(success(),
      depositinle("batchop.a"_n, "SOL", "SOL", MIN_BOND));
   op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE(op["available_at"].as_uint64() > 0);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_duplicate_chain_token_in_collat, sysio_opreg_tester) { try {
   const auto duplicate_vec = std::vector<fc::variant>{
      make_chain_min_bond("ETH", "ETH", 100),
      make_chain_min_bond("ETH", "ETH", 200),
   };

   // The duplicate-detection assertion text refers to "(chain, token_kind)"
   // historically; the contract message may be updated to "(chain_code,
   // token_code)" — match on the stable prefix to tolerate either spelling.
   auto r = setconfig(21, 63, 21, 600000, 5, 5, 24ULL * 60 * 60 * 1000,
                /*req_prod_collat=*/{},
                /*req_batchop_collat=*/duplicate_vec,
                /*req_uw_collat=*/{});
   BOOST_REQUIRE(r.find("duplicate") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_stamps_collat_config_timestamp, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(
      21, 63, 21, 600000, 5, 5, 24ULL * 60 * 60 * 1000,
      /*req_prod_collat=*/{},
      /*req_batchop_collat=*/{
         make_chain_min_bond("ETH", "ETH", 1000),
      },
      /*req_uw_collat=*/{}));

   auto cfg = get_opconfig();
   auto bops = cfg["req_batchop_collat"].get_array();
   BOOST_REQUIRE_EQUAL(1u, bops.size());
   BOOST_REQUIRE(bops[0]["config_timestamp_ms"].as_uint64() > 0);
} FC_LOG_AND_RETHROW() }

// #3/#10: a TERMINATED operator with a still-queued withdraw must not abort flushwtdw. terminate
// remits the operator's full unlocked balance (zeroing it), leaving the matured withdraw row to be
// subtracted from a zero balance — pre-fix that underflowed and aborted the epoch-inline flushwtdw,
// permanently stalling epoch advancement. The TERMINATED branch erases the row without subtracting.
BOOST_FIXTURE_TEST_CASE(flushwtdw_terminated_operator_does_not_abort, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   BOOST_REQUIRE_EQUAL(success(), depositinle("batchop.a"_n, "ETH", "ETH", 500));

   // Queue a withdraw of the full balance, then terminate (remits + zeroes the balance), leaving
   // the queued row matured against a now-zero balance.
   BOOST_REQUIRE_EQUAL(success(), withdrawinle("batchop.a"_n, "ETH", "ETH", 500));
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "rolling-24h miss"));

   // Flush at an epoch well past the withdraw's eligibility. Pre-fix this aborted with a
   // "balance underflow"; the TERMINATED branch erases the matured row instead.
   BOOST_REQUIRE_EQUAL(success(),
      push_opreg_action(EPOCH_ACCOUNT, "flushwtdw"_n, mvo()("current_epoch", 1000000u)));
   BOOST_REQUIRE(get_wtdw(1).is_null());   // matured row erased, not stuck re-throwing every advance
} FC_LOG_AND_RETHROW() }

// SEC-78 / WSA-166: flushwtdw drains at most MAX_WTDW_FLUSH_PER_EPOCH matured rows per advance, so an
// operator cannot split collateral into enough queued withdraws to blow the transaction CPU deadline
// advance shares with the rest of its fan-out and stall epoch progress chain-wide. The remainder
// flushes on the next advance. This drives a TERMINATED operator so every matured row takes the
// erase-without-remit branch (no outpost/token infra needed) -- the bound lives at the top of the
// loop, above the per-row branches, so it holds regardless of which branch each row takes.
BOOST_FIXTURE_TEST_CASE(flushwtdw_bounds_rows_per_epoch, sysio_opreg_tester) { try {
   // Mirror of the contract-internal cap (contract headers are not host-compilable, same convention
   // as the msgch size-cap tests). Keep in sync with sysio.opreg.hpp::MAX_WTDW_FLUSH_PER_EPOCH.
   constexpr uint32_t MAX_WTDW_FLUSH_PER_EPOCH = 32;
   constexpr uint32_t N = MAX_WTDW_FLUSH_PER_EPOCH + 8;   // 40 > one epoch's flush budget

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   BOOST_REQUIRE_EQUAL(success(), depositinle("batchop.a"_n, "ETH", "ETH", 100'000));

   // Queue N one-*ish*-unit withdraws. Amounts vary (i+1) only so each is a distinct transaction
   // (identical actions in one block would be rejected as duplicates before the contract runs); the
   // per-row amount is irrelevant to the bound. Sum stays well under the deposited balance.
   for (uint32_t i = 0; i < N; ++i) {
      BOOST_REQUIRE_EQUAL(success(),
         withdrawinle("batchop.a"_n, "ETH", "ETH", i + 1));
   }
   // Terminate so every matured row takes flushwtdw's erase-without-remit branch.
   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "rolling-24h miss"));

   // Count remaining queue rows by probing the monotonic ids 1..N (order-independent).
   auto count_pending = [&]() {
      uint32_t n = 0;
      for (uint64_t id = 1; id <= N; ++id) if (!get_wtdw(id).is_null()) ++n;
      return n;
   };
   BOOST_REQUIRE_EQUAL(N, count_pending());

   // First flush drains exactly MAX_WTDW_FLUSH_PER_EPOCH rows; the remainder stays queued.
   BOOST_REQUIRE_EQUAL(success(),
      push_opreg_action(EPOCH_ACCOUNT, "flushwtdw"_n, mvo()("current_epoch", 1'000'000u)));
   BOOST_REQUIRE_EQUAL(N - MAX_WTDW_FLUSH_PER_EPOCH, count_pending());

   // Cross a block boundary so the second flush is a distinct transaction — an identical action in
   // the same block is rejected as a duplicate before the contract runs, masking the guard under test.
   produce_blocks();
   // Second flush drains the rest -- progress resumes where the first stopped.
   BOOST_REQUIRE_EQUAL(success(),
      push_opreg_action(EPOCH_ACCOUNT, "flushwtdw"_n, mvo()("current_epoch", 1'000'000u)));
   BOOST_REQUIRE_EQUAL(0u, count_pending());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
