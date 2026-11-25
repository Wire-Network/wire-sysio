#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include <algorithm>
#include <limits>
#include <random>
#include <utility>

#include "contracts.hpp"
#include "external_chain_simulator.hpp"
#include "contract_test_support.hpp"
#include "shadow_yield_reference.hpp"
#include <sysio/opp/opp.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace sysio::opp::types;

using mvo = fc::mutable_variant_object;
using sysio_system::test_support::codename_mvo;
namespace served = sysio_system::test_support::served_chains;

namespace {

/// Standard opreg prune delay used by focused setconfig tests.
constexpr uint64_t kDefaultPruneDelayMs = 600000;

/// Current production consecutive-miss threshold.
constexpr uint32_t kDefaultMaxConsecutiveMisses = 5;

/// Focused threshold that terminates after a second consecutive miss.
constexpr uint32_t kSingleAllowedConsecutiveMiss = 1;

/// Successful active delivery that seeds the pre-withdrawal percentage sample.
constexpr uint32_t kPreWithdrawalDeliveredEpoch = 1;

/// Missed active delivery immediately before the withdrawal-ineligibility interval.
constexpr uint32_t kPreWithdrawalMissedEpoch = kPreWithdrawalDeliveredEpoch + 1;

/// First duty epoch that closes while a queued withdrawal holds the operator UNKNOWN.
constexpr uint32_t kFirstIneligibleDeliveryEpoch = kPreWithdrawalMissedEpoch + 1;

/// Second duty epoch that closes while a queued withdrawal holds the operator UNKNOWN.
constexpr uint32_t kSecondIneligibleDeliveryEpoch = kFirstIneligibleDeliveryEpoch + 1;

/// Delivered duty epoch of a registration that later ends in termination.
constexpr uint32_t kPriorRegistrationDeliveredEpoch = 1;

/// First missed duty epoch of a registration that later ends in termination.
constexpr uint32_t kPriorRegistrationFirstMissEpoch = kPriorRegistrationDeliveredEpoch + 1;

/// Second missed duty epoch of a registration that later ends in termination.
constexpr uint32_t kPriorRegistrationSecondMissEpoch = kPriorRegistrationFirstMissEpoch + 1;

/// First duty epoch, delivered, after the terminated account registers again.
constexpr uint32_t kReRegistrationDeliveredEpoch = kPriorRegistrationSecondMissEpoch + 1;

/// First epoch in the healthy history used by the percent-rail regression.
constexpr uint32_t kFirstPercentRailHistoryEpoch = 1;

/// Healthy observations required so one later miss is exactly five percent.
constexpr uint32_t kPercentRailHealthyHistoryRows = 19;

/// Delivery-log identifier of the final healthy percent-rail observation.
constexpr uint64_t kLastPercentRailHistoryLogId = kPercentRailHealthyHistoryRows;

/// First missed epoch after percent-rail reactivation.
constexpr uint32_t kPercentRailPostReactivationMissEpoch =
   kFirstPercentRailHistoryEpoch + kPercentRailHealthyHistoryRows;

/// Current production rolling-window miss-percentage threshold.
constexpr uint32_t kDefaultMaxPctMisses24h = 5;

/// Highest accepted miss percentage after SEC-28; 100% is intentionally rejected.
constexpr uint32_t kMaxAcceptedPctMisses24h = 99;

/// Disabling percent threshold rejected because an all-miss window cannot exceed it.
constexpr uint32_t kDisablingPctMisses24h = 100;

/// Mirrors opreg's PERCENT_SCALE: the denominator of the whole-percent miss rate.
constexpr uint32_t kPercentScale = 100;

/// Mirrors opreg's DEFAULT_TERMINATE_MIN_PCT_SAMPLES: duty epochs before the percent rail applies.
constexpr uint32_t kDefaultMinPctSamples = kPercentScale / kDefaultMaxPctMisses24h;

/// Percent-rail sample that applies the rail from the first duty epoch, the lowest accepted.
constexpr uint32_t kPercentRailFromFirstDuty = 1;

/// Highest accepted percent-rail sample (opreg's MAX_TERMINATE_MIN_PCT_SAMPLES).
constexpr uint32_t kMaxAcceptedMinPctSamples = kPercentScale;

/// Compact collateral amount used to activate non-bootstrapped batch operators.
constexpr uint64_t kTestMinBond = 1;

/// Collateral request that exceeds the compact test balance.
constexpr uint64_t kInsufficientTestBond = kTestMinBond + 1;

/// First identifier assigned by an empty withdrawal-request table.
constexpr uint64_t kFirstWithdrawalRequestId = 1;

/// Second identifier assigned by the withdrawal-request table.
constexpr uint64_t kSecondWithdrawalRequestId = kFirstWithdrawalRequestId + 1;

/// First identifier assigned by an empty delivery-log table.
constexpr uint64_t kFirstDeliveryLogId = 1;

/// Second identifier assigned by an empty delivery-log table.
constexpr uint64_t kSecondDeliveryLogId = kFirstDeliveryLogId + 1;

/// Third identifier assigned by an empty delivery-log table.
constexpr uint64_t kThirdDeliveryLogId = kSecondDeliveryLogId + 1;

/// Minimum wall-clock separation that gives delivery rows distinct millisecond stamps.
constexpr uint32_t kDeliveryTimestampSeparationSeconds = 2;

/// Block-time units per second, for aligning a block to the start of a `current_time_ms()` second.
constexpr int64_t kMicrosecondsPerSecond = 1'000'000;

/// Epoch value that matures every queued withdrawal in focused flush tests.
constexpr uint32_t kFlushAllMaturedEpoch = std::numeric_limits<uint32_t>::max();

/// Batch operator account used by focused eligibility tests.
constexpr auto kEligibilityBatchOperator = "batchop.a"_n;

/// Producer account used by focused eligibility tests.
constexpr auto kEligibilityProducer = "producer.a"_n;


/// Native WIRE chain and token codename used by focused collateral tests.
constexpr std::string_view kWireCodename = "WIRE";

/// WIRE asset used by depot-side collateral custody and remits.
const symbol kWireSymbol{9, "WIRE"};

/// One whole WIRE expressed in atomic 9-decimal units.
constexpr uint64_t kWireUnit = 1'000'000'000;

/// WIRE quantity used to fund direct-deposit regression operators.
constexpr std::string_view kWireFundingQuantity = "10.000000000 WIRE";

/// Maximum WIRE quantity representable by an Antelope asset.
constexpr std::string_view kWireMaximumSupply = "4611686018.427387903 WIRE";

/// Liq token whose `sysio.liq` shadow symbol backs depot-native shadow collateral. The
/// collateral row's `token_code` is this registry code, not the symbol code.
constexpr std::string_view kLiqEthCodename = "LIQETH";

/// Shadow symbol `sysio.liq` mints for kLiqEthCodename, at the token's depot precision.
const symbol kLiqEthSymbol{9, "LIQETH"};

/// Token code with no `sysio.liq` shadow symbol and no WIRE identity.
constexpr std::string_view kUnknownTokenCodename = "NOPE";

/// External chain id the shadow fixture registers its ETH outpost under.
constexpr uint32_t kEthExternalChainId = 1;

/// Byte width of an EVM contract address.
constexpr size_t kEvmAddressBytes = 20;

/// Filler byte of the placeholder LIQETH contract address on the ETH outpost.
constexpr char kPlaceholderAddressByte = 0x5a;

/// Shadow minted to each shadow-collateral operator, in atomic LIQETH units.
constexpr uint64_t kShadowFunding = 10 * kWireUnit;

/// Error raised by the depot-native token resolver for a token it cannot custody.
constexpr std::string_view kUnsupportedTokenError =
   "assertion failure with message: unsupported depot-native collateral token";

/// Error raised by an operator-scoped action for an account with no operator row.
constexpr std::string_view kOperatorNotFoundError = "assertion failure with message: operator not found";

/// Withdraw amount `withdraw` rejects as not positive.
constexpr uint64_t kZeroWithdrawAmount = 0;

/// `recent_actions` error a registered operator's zero-amount withdraw records.
constexpr std::string_view kZeroWithdrawLogError = "amount must be positive";

/// Error raised by `claimremit` when no claim row exists for the requested token.
constexpr std::string_view kNoClaimableRemitError =
   "assertion failure with message: no claimable remit for this account";

/// Punishment reason used by terminal-status eligibility regressions.
constexpr std::string_view kTestSlashReason = "test slash";

/// Administrative-removal reason used by terminal-status eligibility regressions.
constexpr std::string_view kTestTerminationReason = "test termination";

/// `termcheck` reason once a run exceeds `kSingleAllowedConsecutiveMiss`.
constexpr std::string_view kSingleAllowedConsecutiveMissReason = "rolling-window: >1 consecutive misses";

/// `termcheck` reason once the miss rate exceeds `kDefaultMaxPctMisses24h`.
constexpr std::string_view kDefaultMaxPctMissesReason = "rolling-window: >5% miss rate";

/// Contract action identifiers used by withdrawal lifecycle helpers.
namespace withdrawal_action {
constexpr auto withdraw = "withdraw"_n;
constexpr auto cancel   = "cancelwtdw"_n;
constexpr auto flush    = "flushwtdw"_n;
} // namespace withdrawal_action

/// Contract action identifiers used by direct collateral deposit and claim helpers.
namespace collateral_action {
constexpr auto deposit    = "deposit"_n;
constexpr auto claimremit = "claimremit"_n;
constexpr auto available  = "available"_n;
} // namespace collateral_action

/// Contract field identifiers used by withdrawal lifecycle helpers.
namespace withdrawal_field {
constexpr char account[]       = "account";
constexpr char amount[]        = "amount";
constexpr char request_id[]    = "request_id";
constexpr char economic_epoch[]  = "economic_epoch";
constexpr char chain_code[]    = "chain_code";
constexpr char token_code[]    = "token_code";
} // namespace withdrawal_field

/// Registry and shadow-token actions the shadow-collateral fixture drives.
namespace shadow_action {
constexpr auto regchain = "regchain"_n;
constexpr auto regtoken = "regtoken"_n;
constexpr auto regctok  = "regctok"_n;
constexpr auto create   = "create"_n;
constexpr auto mint     = "mint"_n;
constexpr auto queueout = "queueout"_n;
} // namespace shadow_action

/// Result field identifiers inspected by focused eligibility tests.
namespace eligibility_field {
constexpr char status[]  = "status";
constexpr char success[] = "success";
} // namespace eligibility_field

/// Rejected collateral minimum that would make eligibility checks vacuous.
constexpr uint64_t kRejectedZeroMinBond = 0;

/// Standard 24h rolling-window size used by opreg tests.
constexpr uint64_t kTerminateWindowMs = 24ULL * 60 * 60 * 1000;

/// Epoch duration of the rotation schedule the window tests install (seconds).
constexpr uint32_t kRotationEpochDurationSec = 360;

/// Rotation-group count of the rotation schedule the window tests install. A resident
/// operator is on duty (and accrues a delivery record) once per this many epochs.
constexpr uint32_t kRotationGroups = 3;

/// One duty rotation at the rotation schedule, in milliseconds: the wall-clock gap
/// between a resident operator's consecutive records.
constexpr uint64_t kDutyRotationMs = uint64_t{kRotationGroups} * kRotationEpochDurationSec * 1000;

/// Wall clock a run evicting on the production consecutive-miss threshold spans at the
/// rotation schedule: (5 + 1) duty rotations.
constexpr uint64_t kEvictingRunSpanMs = (uint64_t{kDefaultMaxConsecutiveMisses} + 1) * kDutyRotationMs;

/// Mirrors opreg's MAX_DELLOG_PRUNE_PER_WRITE (the contract header is not
/// includable from native test code).
constexpr uint32_t kDellogPrunePerWrite = 4;

/// Mirrors opreg's MAX_DELLOG_PRUNE_PER_CRANK.
constexpr uint32_t kDellogPrunePerCrank = 64;

/// Mirrors opreg's MAX_DELLOG_PRUNE_PER_CHECK.
constexpr uint32_t kDellogPrunePerCheck = 16;

/// Clock jump that ages every row recorded before it out of the 24 h window.
constexpr auto kPastTheWindow = fc::hours(25);

/// Shadow-yield forwarding: actions opreg and sysio.liq expose for it.
namespace yield_action {
constexpr auto sweepyield = "sweepyield"_n;
constexpr auto claimyield = "claimyield"_n;
constexpr auto addyield   = "addyield"_n;
} // namespace yield_action

/// First shadow-yield bonder: holds a quarter of the bonded shadow.
constexpr auto kYieldBonderA = "uwrit.alice"_n;

/// Second shadow-yield bonder: holds three quarters of the bonded shadow.
constexpr auto kYieldBonderB = "uwrit.bob"_n;

/// Shadow holder that never bonds, so the registry holds only part of the supply and the
/// registry's share of a distribution is not a round number.
constexpr auto kYieldBystander = "batchop.b"_n;

/// WIRE holder that funds `sysio.liq::addyield` distributions.
constexpr auto kYieldDonor = "batchop.c"_n;

/// Account that cranks the permissionless `sweepyield`: neither a bonder nor the registry.
constexpr auto kYieldCranker = "producer.a"_n;

/// WIRE issued to kYieldDonor, enough for every distribution a yield case makes.
constexpr std::string_view kYieldDonorFunding = "1000.000000000 WIRE";

/// Shadow kYieldBonderA bonds, in atomic LIQETH units.
constexpr uint64_t kYieldBondA = 1 * kWireUnit;

/// Shadow kYieldBonderB bonds: three times kYieldBondA.
constexpr uint64_t kYieldBondB = 3 * kWireUnit;

/// Shadow kYieldBystander holds unbonded: an uneven third of a unit.
constexpr uint64_t kYieldBystanderHolding = kWireUnit / 3;

/// WIRE distributed to the LIQETH holders by one `addyield`, in atomic units.
constexpr int64_t kYieldDistribution = 40 * static_cast<int64_t>(kWireUnit);

/// Error raised by `sweepyield` when the registry's `sysio.liq` row is owed nothing.
constexpr std::string_view kNoYieldToSweepError = "assertion failure with message: no yield to sweep";

/// Error raised by `claimyield` when the operator's row has earned nothing.
constexpr std::string_view kNoYieldOwedError = "assertion failure with message: no yield owed";

/// Error raised by `claimyield` when the row is owed yield the registry's pool cannot yet cover.
constexpr std::string_view kYieldNotCoveredError =
   "assertion failure with message: owed yield is rounding dust the WIRE received from sysio.liq does not "
   "cover; only later slack can cover it";


/// Account with no stake in the yield cases that cranks `claimyield` for someone else.
constexpr auto kYieldKeeper = "uwrit.a"_n;

/// Operator holding WIRE collateral in the dust cases, so the registry's `sysio.token` balance mixes
/// collateral principal with swept yield.
constexpr auto kYieldPrincipalHolder = "batchop.a"_n;

/// WIRE collateral kYieldPrincipalHolder bonds in the dust cases.
constexpr uint64_t kYieldPrincipal = 5 * kWireUnit;

/// Distribute-and-sweep rounds in the dust cases: enough for the registry row's per-settle flooring
/// to fall at least one atomic WIRE behind the operator row's single floor.
constexpr int kDustRounds = 3;

/// Error raised by `sweepyield` for WIRE, which earns no shadow yield.
constexpr std::string_view kWireEarnsNoYieldError =
   "assertion failure with message: WIRE collateral earns no shadow yield";

/// Refusals of `regoperator`, `joinchain` and `leavechain` the served-chain cases assert.
constexpr std::string_view kMissingOutpostLinkError =
   "assertion failure with message: missing authex link for outpost chain";
constexpr std::string_view kOnlyBatchServesChainsError =
   "assertion failure with message: only a batch operator serves chains";
constexpr std::string_view kTerminalOperatorJoinError =
   "assertion failure with message: a slashed or terminated operator cannot serve a chain";
constexpr std::string_view kNotActiveOutpostError = "assertion failure with message: chain is not an active outpost";
constexpr std::string_view kMissingChainLinkError =
   "assertion failure with message: missing authex link for the chain's kind";
constexpr std::string_view kAlreadyServingError = "assertion failure with message: operator already serves this chain";
constexpr std::string_view kNotServingError = "assertion failure with message: operator does not serve this chain";
constexpr std::string_view kNotEnoughBatchOperatorsError =
   "assertion failure with message: not enough available batch operators for group assignment";

/// An outpost the served-chain cases never register.
constexpr std::string_view kUnregisteredChain = "SOL";

/// A second EVM outpost for the per-chain eviction cases, and its external chain id.
constexpr std::string_view kSecondEvmChain     = "BASE";
constexpr uint32_t         kSecondEvmChainId   = 8453;

/// Blocks that move `current_time_ms()`, which has second resolution, to a later second.
constexpr uint32_t kBlocksPastOneSecond = 3;

} // namespace

/// Data model: per-chain identity has moved from `ChainKind` enums to
/// `sysio::slug_name`-keyed registries (`sysio.chains`, `sysio.tokens`).
/// The test fixture treats the codenames as opaque uint64
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
   static constexpr auto LIQ_ACCOUNT    = "sysio.liq"_n;
   /// The shadow ledger's only minter; an account is enough to sign as it here.
   static constexpr auto SYND_ACCOUNT   = "sysio.synd"_n;
   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto DCLAIM_ACCOUNT = "sysio.dclaim"_n;

   sysio_opreg_tester() {
      produce_blocks(2);

      create_accounts({
         OPREG_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT, MSGCH_ACCOUNT, TOKEN_ACCOUNT,
         LIQ_ACCOUNT, SYND_ACCOUNT, TOKENS_ACCOUNT, CHAINS_ACCOUNT, DCLAIM_ACCOUNT,
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
      opreg_abi_ser.set_abi(std::move(abi), fixture_abi_yield_function());

      // Load epoch ABI serializer
      const auto* epoch_accnt = control->find_account_metadata(EPOCH_ACCOUNT);
      BOOST_REQUIRE(epoch_accnt != nullptr);
      abi_def epoch_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(epoch_accnt->abi, epoch_abi), true);
      epoch_abi_ser.set_abi(std::move(epoch_abi), fixture_abi_yield_function());
   }

   // ── SlugName helpers ──
   //
   // Codenames are 8-byte packed identifiers (`fc::slug_name`). The contract's
   // `sysio::slug_name` and the host-side `fc::slug_name` use the same packing
   // algorithm so values are byte-identical across the boundary.

   static fc::slug_name cn(std::string_view s) { return fc::slug_name{s}; }

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

   /// The outpost chain whose epoch schedule the fixture configures by default.
   static constexpr std::string_view EPOCH_CHAIN = "ETH";

   /// Push `sysio.epoch::setconfig` for `chain` with a 7-operators-per-group schedule of
   /// the given duration and group count.
   action_result set_epoch_config(uint32_t epoch_duration_sec,
                                  uint32_t batch_op_groups = kRotationGroups,
                                  std::string_view chain = EPOCH_CHAIN) {
      return push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("chain_code",                         chain)
         ("epoch_duration_sec",                 epoch_duration_sec)
         ("traffic_wait_sec",                   0)
         ("operators_per_epoch",                7)
         ("batch_operator_minimum_active",      7 * batch_op_groups)
         ("batch_op_groups",                    batch_op_groups)
         ("epoch_retention_envelope_log_count", 200));
   }

   /// Build a single `chain_min_bond` entry as an fc::variant suitable for
   /// `setconfig`'s `req_*_collat` vector arguments. identity is by
   /// (chain_code, token_code) codenames rather than the old enums.
   static fc::variant make_chain_min_bond(std::string_view chain_code,
                                          std::string_view token_code,
                                          uint64_t min_bond) {
      return fc::variant(mvo()
         ("chain_code",           chain_code)
         ("token_code",           token_code)
         ("min_bond",             min_bond)
         ("config_timestamp_ms",  uint64_t{0}));
   }

   /// Push `sysio.opreg::setconfig` with sane defaults.
   action_result setconfig(uint64_t prune_delay = 600000,
                           uint32_t max_consec_misses = 5,
                           uint32_t max_pct_misses_24h = 5,
                           uint32_t min_pct_samples = kDefaultMinPctSamples,
                           uint64_t terminate_window_ms = 24ULL * 60 * 60 * 1000,
                           std::vector<fc::variant> req_prod_collat    = {},
                           std::vector<fc::variant> req_batchop_collat = {},
                           std::vector<fc::variant> req_uw_collat      = {}) {
      return push_opreg_action(OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("terminate_prune_delay_ms",         prune_delay)
         ("terminate_max_consecutive_misses", max_consec_misses)
         ("terminate_max_pct_misses_24h",     max_pct_misses_24h)
         ("terminate_min_pct_samples",        min_pct_samples)
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

   /// Record a delivery hit/miss on `chain` through the same opreg action invoked by
   /// `sysio.epoch::advance(chain)`.
   action_result recorddel(name account, uint32_t epoch, bool delivered, std::string_view chain = EPOCH_CHAIN) {
      return push_opreg_action(EPOCH_ACCOUNT, "recorddel"_n, mvo()
         ("account",    account)
         ("chain_code", chain)
         ("epoch",      epoch)
         ("delivered",  delivered));
   }

   /// Run opreg's rolling-window eviction check for `chain` through the epoch-authorized
   /// action surface.
   action_result termcheck(name account, std::string_view chain = EPOCH_CHAIN) {
      return push_opreg_action(EPOCH_ACCOUNT, "termcheck"_n, mvo()
         ("account",    account)
         ("chain_code", chain));
   }

   /// `account`'s `servchains` row on `chain`; null when it has none.
   fc::variant membership(name account, std::string_view chain = EPOCH_CHAIN) {
      return sysio_system::test_support::served_chains::membership(*this, opreg_abi_ser, account, chain);
   }

   /// `account`'s membership status on `chain`, as the ABI spells it.
   std::string membership_status(name account, std::string_view chain = EPOCH_CHAIN) {
      const auto row = membership(account, chain);
      BOOST_REQUIRE(!row.is_null());
      return row["status"].as_string();
   }

   /// Require `account` EVICTED from `chain` for `reason`, its global status still `status`.
   void require_evicted(name account, std::string_view reason,
                        OperatorStatus status = OperatorStatus::OPERATOR_STATUS_ACTIVE,
                        std::string_view chain = EPOCH_CHAIN) {
      const auto row = membership(account, chain);
      BOOST_REQUIRE(!row.is_null());
      BOOST_REQUIRE_EQUAL(std::string(sysio_system::test_support::served_chains::status::evicted),
                          row["status"].as_string());
      BOOST_REQUIRE_EQUAL(std::string(reason), row["status_reason"].as_string());
      BOOST_REQUIRE(status == get_operator(account)["status"].as<OperatorStatus>());
   }

   /// `account`'s duty tally on `chain`.
   fc::variant tally(name account, std::string_view chain = EPOCH_CHAIN) {
      const auto row = membership(account, chain);
      BOOST_REQUIRE(!row.is_null());
      return row["tally"];
   }

   /// What a walk of `account`'s dellog rows on `chain`, from log id `first_log_id` on, counts: the rails the
   /// tally replaced read the log this way.
   struct log_walk {
      uint64_t duties       = 0;
      uint64_t misses       = 0;
      uint64_t trailing_run = 0;   ///< misses since the last delivery
      uint64_t longest_run  = 0;   ///< the longest run of misses
   };

   log_walk walk_dellog(name account, uint64_t first_log_id, std::string_view chain = EPOCH_CHAIN) {
      std::vector<std::pair<uint64_t, bool>> rows;
      for (const auto& row : sysio_system::test_support::table_rows(*this, OPREG_ACCOUNT, "dellog"_n, opreg_abi_ser,
                                                                     "delivery_log_entry")) {
         if (row["account"].as_string() != account.to_string() || row["chain_code"].as_string() != chain) continue;
         if (row["log_id"].as_uint64() < first_log_id) continue;
         rows.emplace_back(row["log_id"].as_uint64(), row["delivered"].as_bool());
      }
      std::sort(rows.begin(), rows.end());
      log_walk walk;
      for (const auto& [log_id, delivered] : rows) {
         ++walk.duties;
         if (delivered) {
            walk.trailing_run = 0;
         } else {
            ++walk.misses;
            ++walk.trailing_run;
         }
         walk.longest_run = std::max(walk.longest_run, walk.trailing_run);
      }
      return walk;
   }

   /// Require `account`'s tally on `chain` to hold what `walk` counted.
   void require_tally(name account, const log_walk& walk, std::string_view chain = EPOCH_CHAIN) {
      const auto counted = tally(account, chain);
      BOOST_REQUIRE_EQUAL(walk.duties, counted["duties"].as_uint64());
      BOOST_REQUIRE_EQUAL(walk.misses, counted["misses"].as_uint64());
      BOOST_REQUIRE_EQUAL(walk.trailing_run, counted["miss_run"].as_uint64());
   }

   /// Overwrite `account`'s tally on `chain` in the store, leaving its dellog rows as they are.
   void rewrite_tally(name account, const fc::variant& value, std::string_view chain = EPOCH_CHAIN) {
      std::string key;
      const auto append_word = [&](uint64_t word) {
         for (int shift = 56; shift >= 0; shift -= 8) key.push_back(char(word >> shift));
      };
      append_word(cn(chain).value);
      append_word(account.to_uint64_t());
      auto& store = const_cast<kv_store&>(control->kv());
      const auto stored_row = store.find_row(OPREG_ACCOUNT, compute_table_id("servchains"_n.to_uint64_t()), key);
      BOOST_REQUIRE(stored_row);
      const std::vector<char> data(stored_row->value.begin(), stored_row->value.end());
      mvo row(opreg_abi_ser.binary_to_variant("served_chain", data, fixture_abi_yield_function()).get_object());
      row.set("tally", value);
      const auto encoded = opreg_abi_ser.variant_to_binary("served_chain", row, fixture_abi_yield_function());
      store.update_row(*stored_row, stored_row->payer, {encoded.data(), encoded.size()});
   }

   /// Produce blocks until the next block opens a new second, so actions pushed now and the
   /// actions in the block after them share one `current_time_ms()` value.
   void align_next_block_to_second_start() {
      while ((control->head().block_time() + fc::milliseconds(config::block_interval_ms))
                .time_since_epoch().count() % kMicrosecondsPerSecond != 0) {
         produce_block();
      }
   }

   /// Configure one ETH bond requirement and deposit it so a non-bootstrapped
   /// batch operator becomes ACTIVE through the normal eligibility path.
   void activate_batch_operator(name account,
                                uint32_t max_consec_misses = kDefaultMaxConsecutiveMisses,
                                uint32_t max_pct_misses_24h = kMaxAcceptedPctMisses24h,
                                uint32_t min_pct_samples = kDefaultMinPctSamples,
                                uint64_t terminate_window_ms = kTerminateWindowMs) {
      BOOST_REQUIRE_EQUAL(success(), setconfig(
         /*prune_delay=*/kDefaultPruneDelayMs,
         /*max_consec_misses=*/max_consec_misses,
         /*max_pct_misses_24h=*/max_pct_misses_24h,
         /*min_pct_samples=*/min_pct_samples,
         /*terminate_window_ms=*/terminate_window_ms,
         /*req_prod_collat=*/{},
         /*req_batchop_collat=*/{
            make_chain_min_bond("WIRE", "NTA", kTestMinBond),
         },
         /*req_uw_collat=*/{}));

      BOOST_REQUIRE_EQUAL(success(),
         regoperator(account, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
      // Serves EPOCH_CHAIN, whose rows the termination cases record and evaluate.
      setup_served_chain();
      serve_evm_chain({account});
      BOOST_REQUIRE_EQUAL(success(),
         bond_generic(account, "NTA", kTestMinBond));
      produce_blocks();

      auto op = get_operator(account);
      BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
      BOOST_REQUIRE_EQUAL(0, op["is_bootstrapped"].as_uint64());
   }

   /// `withdraw`: operator-authorized request against the depot-native `(WIRE, token_code)` row.
   action_result withdraw(name account, std::string_view token_code, uint64_t amount) {
      return push_opreg_action(account, withdrawal_action::withdraw, mvo()
         (withdrawal_field::account,    account)
         (withdrawal_field::token_code, token_code)
         (withdrawal_field::amount,     amount));
   }

   /// `deposit`: operator-authorized depot-native collateral deposit of `token_code`.
   action_result deposit(name account, std::string_view token_code, uint64_t amount) {
      return push_opreg_action(account, collateral_action::deposit, mvo()
         (withdrawal_field::account,    account)
         (withdrawal_field::token_code, token_code)
         (withdrawal_field::amount,     amount));
   }

   /// Read-only `available(account, chain_code, token_code)`, taken from the action's return value.
   /// A block is produced after each read so the same query can be repeated later in a case
   /// without being rejected as a duplicate transaction.
   uint64_t available(name account, std::string_view chain_code, std::string_view token_code) {
      auto trace = base_tester::push_action(OPREG_ACCOUNT, collateral_action::available, OPREG_ACCOUNT, mvo()
         (withdrawal_field::account,    account)
         (withdrawal_field::chain_code, chain_code)
         (withdrawal_field::token_code, token_code));
      BOOST_REQUIRE(trace && !trace->action_traces.empty());
      produce_blocks();
      return fc::raw::unpack<uint64_t>(trace->action_traces[0].return_value);
   }

   /// Cancel an operator-owned queued withdrawal request.
   action_result cancelwtdw(name signer, name account, uint64_t request_id) {
      return push_opreg_action(signer, withdrawal_action::cancel, mvo()
         (withdrawal_field::account,    account)
         (withdrawal_field::request_id, request_id));
   }

   /// Flush every withdrawal economically matured by `epoch` through the epoch-authorized action.
   action_result flushwtdw(uint32_t epoch) {
      return push_opreg_action(EPOCH_ACCOUNT, withdrawal_action::flush, mvo()
         (withdrawal_field::economic_epoch, epoch));
   }

   action_result terminate(name account, std::string reason) {
      return push_opreg_action(OPREG_ACCOUNT, "terminate"_n, mvo()
         ("account",  account)
         ("reason",   reason));
   }

   /// Invoke the contract-owned batch eligibility transition callback.
   action_result processbatch(name account, bool was_eligible, bool is_eligible) {
      return push_opreg_action(OPREG_ACCOUNT, "processbatch"_n, mvo()
         ("account",      account)
         ("was_eligible", was_eligible)
         ("is_eligible",  is_eligible));
   }

   /// Read a wtdwqueue row by request_id (primary key).
   fc::variant get_wtdw(uint64_t request_id) {
      auto data = get_row_by_id(OPREG_ACCOUNT, OPREG_ACCOUNT, "wtdwqueue"_n, request_id);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "withdraw_request", data,
         fixture_abi_yield_function());
   }

   // ── Table read helpers ──

   fc::variant get_opconfig() {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "opconfig"_n, "opconfig"_n);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "op_config", data,
         fixture_abi_yield_function());
   }

   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "operator_entry", data,
         fixture_abi_yield_function());
   }

   /// The dellog row allocated `log_id`, of any chain; empty variant when the row does not exist.
   fc::variant get_dellog_entry(uint64_t log_id) {
      for (auto& row : sysio_system::test_support::table_rows(*this, OPREG_ACCOUNT, "dellog"_n, opreg_abi_ser,
                                                              "delivery_log_entry")) {
         if (row["log_id"].as_uint64() == log_id) return row;
      }
      return fc::variant();
   }

   /// Newest entry in the operator's `recent_actions` ring buffer.
   fc::variant latest_action_log(name account) {
      auto op = get_operator(account);
      if (op.is_null()) return fc::variant();
      const auto& log = op["recent_actions"].get_array();
      return log.empty() ? fc::variant() : log.back();
   }

   /// Claimable `token_code` row credited by a WIRE-chain remit. Empty variant when absent.
   ///
   /// The row key `{account, token_code}` is two big-endian words, which is exactly the
   /// `[scope][id]` layout `get_row_by_id` probes first — so the account rides as the scope.
   fc::variant get_remitclaim(name account, std::string_view token_code) {
      auto data = get_row_by_id(OPREG_ACCOUNT, account, "remitclaims"_n, cn(token_code).value);
      return data.empty() ? fc::variant() : opreg_abi_ser.binary_to_variant(
         "remit_claim", data,
         fixture_abi_yield_function());
   }

   /// Banked yield retained independently of an operator's registration, or zero when absent.
   fc::uint128_t yield_debt(name account, std::string_view token_code = kLiqEthCodename) {
      const auto data = get_row_by_id(OPREG_ACCOUNT, account, "yielddebts"_n, cn(token_code).value);
      return data.empty() ? fc::uint128_t{0} : opreg_abi_ser.binary_to_variant(
         "yield_debt", data, fixture_abi_yield_function())["owed_wire"].as_uint128();
   }

   /// Seed an existing ledger field at an arithmetic boundary without issuing impossible token
   /// supplies. Used only to verify that rejected claims/archival roll back the whole transaction.
   void rewrite_claim_field(name table, const char* row_type, name account, std::string_view token_code,
                            const char* field, const fc::variant& value) {
      std::string key;
      const auto append_word = [&](uint64_t word) {
         for (int shift = 56; shift >= 0; shift -= 8) key.push_back(char(word >> shift));
      };
      if (table != "yieldpool"_n) append_word(account.to_uint64_t());
      append_word(cn(token_code).value);
      auto& store = const_cast<kv_store&>(control->kv());
      const auto stored_row = store.find_row(OPREG_ACCOUNT, compute_table_id(table.to_uint64_t()), key);
      BOOST_REQUIRE(stored_row);
      const std::vector<char> data(stored_row->value.begin(), stored_row->value.end());
      mvo row(opreg_abi_ser.binary_to_variant(row_type, data,
         fixture_abi_yield_function()).get_object());
      row.set(field, value);
      const auto encoded = opreg_abi_ser.variant_to_binary(row_type, row,
         fixture_abi_yield_function());
      const auto updated = store.update_row(*stored_row, stored_row->payer, {encoded.data(), encoded.size()});
      const std::vector<char> stored(updated.value.begin(), updated.value.end());
      const auto observed = opreg_abi_ser.binary_to_variant(row_type, stored,
         fixture_abi_yield_function());
      BOOST_REQUIRE_EQUAL(value.as_string(), observed[field].as_string());
   }

   /// Operator-authorized pull of the `token_code` claim row.
   action_result claimremit(name account, std::string_view token_code) {
      return push_opreg_action(account, collateral_action::claimremit, mvo()
         (withdrawal_field::account,    account)
         (withdrawal_field::token_code, token_code));
   }

   /// Stand up sysio.token with the opreg WIRE asset and fund `who`, so a
   /// WIRE-chain `deposit` can move real collateral.
   void setup_wire_token_and_fund(name who, std::string_view quantity) {
      set_code(TOKEN_ACCOUNT, contracts::token_wasm());
      set_abi(TOKEN_ACCOUNT, contracts::token_abi().data());
      set_privileged(TOKEN_ACCOUNT);   // so create/issue can bill the stat/balance RAM
      produce_blocks();
      base_tester::push_action(TOKEN_ACCOUNT, "create"_n, TOKEN_ACCOUNT,
         mvo()("issuer", "sysio")("maximum_supply", std::string(kWireMaximumSupply)));
      base_tester::push_action(TOKEN_ACCOUNT, "issue"_n, config::system_account_name,
         mvo()("to", "sysio")("quantity", std::string(quantity))("memo", "seed"));
      base_tester::push_action(TOKEN_ACCOUNT, "transfer"_n, config::system_account_name,
         mvo()("from", "sysio")("to", who)("quantity", std::string(quantity))("memo", "fund operator"));
   }

   /// Read one account's WIRE balance in atomic 9-decimal units.
   int64_t wire_balance(name account) {
      return get_currency_balance(TOKEN_ACCOUNT, kWireSymbol, account).get_amount();
   }

   /// Park the blocking contract on `who`: every INCOMING sysio.token::transfer asserts.
   void make_transfer_blocking(name who) {
      set_code(who, contracts::util::block_transfer_wasm());
      set_abi(who, contracts::util::block_transfer_abi().data());
      produce_blocks();
   }

   bool generic_assets_ready = false;

   /// Mint generic shadow LIQ through sysio.synd and bond it through the public native action.
   action_result bond_generic(name account, std::string_view token, uint64_t amount) {
      if (!generic_assets_ready) {
         setup_shadow_liq({sysio::testing::external::First, sysio::testing::external::Second});
         generic_assets_ready = true;
      }
      BOOST_REQUIRE_EQUAL(success(), push_contract(LIQ_ACCOUNT, liq_abi_ser, SYND_ACCOUNT,
         shadow_action::mint, mvo()("to", account)("token_code", codename_mvo(token))("amount", amount)));
      return deposit(account, token, amount);
   }

   // ── Shadow LIQ collateral (sysio.liq) ──

   /// Deploy a privileged registry or token contract and load its ABI serializer.
   void deploy_privileged(name account, const std::vector<uint8_t>& wasm, const std::vector<char>& abi,
                          abi_serializer& ser) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      set_privileged(account);
      produce_blocks();
      sysio_system::test_support::load_account_abi(*this, account, ser);
   }

   /// Push an ABI-encoded action on a contract other than opreg and produce a block.
   action_result push_contract(name code, abi_serializer& ser, name signer, name action_name,
                               const variant_object& data) {
      return sysio_system::test_support::push_contract_action_and_produce_block(*this, code, ser, signer,
                                                                                action_name, data);
   }

   /// Stand up what a shadow-collateral case needs: the ETH outpost in `sysio.chains`, its
   /// LIQETH liq token in `sysio.tokens`, and the LIQETH shadow symbol in `sysio.liq` (the
   /// same registrations `sysio.liq_tests` seeds). Called per case rather than from the
   /// constructor: an active ETH chain row would turn the ETH-bucket slashes and remits of the
   /// other cases into real `sysio.msgch` queueouts and change what those cases exercise.
   void setup_shadow_liq(const std::vector<sysio::testing::external::asset_spec>& assets =
         {{"ETH", "LIQETH", ChainKind::CHAIN_KIND_EVM, kEthExternalChainId}}) {
      deploy_chain_registry_once();
      deploy_privileged(TOKENS_ACCOUNT, contracts::tokens_wasm(), contracts::tokens_abi(), tokens_abi_ser);
      deploy_privileged(LIQ_ACCOUNT,    contracts::liq_wasm(),    contracts::liq_abi(),    liq_abi_ser);

      for (const auto& a : assets) {
         const auto sym = symbol::from_string(std::string("9,") + a.token);
         // Registered before the network genesis (the bootstrap window), so the rows land ACTIVE.
         BOOST_REQUIRE_EQUAL(success(), push_contract(CHAINS_ACCOUNT, chains_abi_ser, CHAINS_ACCOUNT,
            shadow_action::regchain, mvo()
            ("kind", a.kind)("code", codename_mvo(a.chain))
            ("external_chain_id", a.id)("name", std::string(a.chain))
            ("description", std::string{})("outpost", sysio_system::test_support::no_outpost_mvo())));

         const std::vector<char> address(a.kind == ChainKind::CHAIN_KIND_SVM ? 32 : kEvmAddressBytes, kPlaceholderAddressByte);
         BOOST_REQUIRE_EQUAL(success(), push_contract(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT,
            shadow_action::regtoken, mvo()
            ("kind", TokenKind::TOKEN_KIND_LIQ)("code", codename_mvo(a.token))
            ("symbol_name", std::string(a.token))("description", std::string{})
            ("precision", sym.decimals())
            ("address", mvo()("kind", a.kind)("address", address))));
         BOOST_REQUIRE_EQUAL(success(), push_contract(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT,
            shadow_action::regctok, mvo()
            ("chain_code", codename_mvo(a.chain))("token_code", codename_mvo(a.token))
            ("contract_addr", address)("is_native", false)));

         BOOST_REQUIRE_EQUAL(success(), push_contract(LIQ_ACCOUNT, liq_abi_ser, LIQ_ACCOUNT, shadow_action::create,
            mvo()("sym", sym)("chain_code", codename_mvo(a.chain))
            ("token_code", codename_mvo(a.token))));
      }
   }

   bool chain_registry_deployed = false;

   /// Deploy sysio.chains once per case; a second identical set_code is refused.
   void deploy_chain_registry_once() {
      if (chain_registry_deployed) return;
      deploy_privileged(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi_ser);
      chain_registry_deployed = true;
   }

   bool link_registry_deployed = false;
   std::vector<std::string> served_chains_registered;

   /// Stand up what a served-chain case needs: `chain` registered as an active outpost of `kind`
   /// in sysio.chains, and sysio.authex with a privileged sysio.dclaim so operators can link keys.
   /// Called per case for the same reason as `setup_shadow_liq`; a chain already set up is left as is.
   void setup_served_chain(std::string_view chain = EPOCH_CHAIN, ChainKind kind = ChainKind::CHAIN_KIND_EVM,
                           uint32_t external_chain_id = kEthExternalChainId) {
      if (std::find(served_chains_registered.begin(), served_chains_registered.end(), chain) !=
          served_chains_registered.end()) return;
      served_chains_registered.emplace_back(chain);
      deploy_chain_registry_once();
      if (!link_registry_deployed) {
         deploy_privileged(AUTHEX_ACCOUNT, contracts::authex_wasm(), contracts::authex_abi(), authex_abi_ser);
         deploy_privileged(DCLAIM_ACCOUNT, contracts::dclaim_wasm(), contracts::dclaim_abi(), dclaim_abi_ser);
         link_registry_deployed = true;
      }
      BOOST_REQUIRE_EQUAL(success(), push_contract(CHAINS_ACCOUNT, chains_abi_ser, CHAINS_ACCOUNT,
         shadow_action::regchain, mvo()
         ("kind", kind)("code", codename_mvo(chain))
         ("external_chain_id", external_chain_id)("name", std::string(chain))
         ("description", std::string{})("outpost", sysio_system::test_support::no_outpost_mvo())));
   }

   /// Link each of `ops` to a fresh EVM key and join it to `chain`.
   void serve_evm_chain(const std::vector<name>& ops, std::string_view chain = EPOCH_CHAIN) {
      sysio_system::test_support::served_chains::serve(*this, ops, {{chain, ChainKind::CHAIN_KIND_EVM}});
   }

   /// Put `amount` LIQETH shadow in `holder`'s hands the way the depot mints it: `sysio.synd`, the
   /// ledger's only minter, calling `mint`.
   void mint_shadow(name holder, uint64_t amount) {
      const int64_t before = shadow_balance(holder);
      BOOST_REQUIRE_EQUAL(success(), push_contract(LIQ_ACCOUNT, liq_abi_ser, SYND_ACCOUNT, shadow_action::mint,
         mvo()("to", holder)("token_code", codename_mvo(kLiqEthCodename))("amount", amount)));
      BOOST_REQUIRE_EQUAL(before + static_cast<int64_t>(amount), shadow_balance(holder));
   }

   /// Read one account's LIQETH shadow balance in atomic units.
   int64_t shadow_balance(name account) {
      return get_currency_balance(LIQ_ACCOUNT, kLiqEthSymbol, account).get_amount();
   }

   /// Slash through the chalg-authorized surface and report whether the transaction queued any
   /// `sysio.msgch::queueout`, i.e. whether a slash attestation left for an outpost.
   bool slash_queues_outbound(name account, std::string_view reason) {
      auto trace = base_tester::push_action(OPREG_ACCOUNT, "slash"_n, CHALG_ACCOUNT, mvo()
         ("account", account)
         ("reason",  std::string(reason)));
      BOOST_REQUIRE(trace);
      produce_blocks();
      return std::any_of(trace->action_traces.begin(), trace->action_traces.end(), [](const action_trace& at) {
         return at.act.account == MSGCH_ACCOUNT && at.act.name == shadow_action::queueout;
      });
   }

   // ── Shadow-yield forwarding ──

   /// Register the two underwriters, fund kYieldDonor with WIRE, and mint kYieldBondA / kYieldBondB
   /// LIQETH to the bonders and kYieldBystanderHolding to kYieldBystander -- nothing bonded yet.
   void setup_yield_holders() {
      BOOST_REQUIRE_EQUAL(success(), setconfig());
      BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
      BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderB, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
      setup_wire_token_and_fund(kYieldDonor, kYieldDonorFunding);
      setup_shadow_liq();
      mint_shadow(kYieldBonderA, kYieldBondA);
      mint_shadow(kYieldBonderB, kYieldBondB);
      mint_shadow(kYieldBystander, kYieldBystanderHolding);
   }

   /// `setup_yield_holders`, then both underwriters bond everything they were minted.
   void setup_yield_bonders() {
      setup_yield_holders();
      BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
      BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderB, kLiqEthCodename, kYieldBondB));
   }

   /// `sysio.liq::addyield`: kYieldDonor distributes `amount` WIRE to every LIQETH holder.
   action_result addyield(int64_t amount) {
      return push_contract(LIQ_ACCOUNT, liq_abi_ser, kYieldDonor, yield_action::addyield, mvo()
         ("from", kYieldDonor)("quantity", asset(amount, kWireSymbol))("target", kLiqEthSymbol.to_symbol_code()));
   }

   /// Permissionless `sweepyield(token_code)`, cranked by kYieldCranker.
   action_result sweepyield(std::string_view token_code) {
      return push_opreg_action(kYieldCranker, yield_action::sweepyield, mvo()
         (withdrawal_field::token_code, token_code));
   }

   /// Operator-authorized `claimyield(account, token_code)`.
   action_result claimyield(name account, std::string_view token_code) {
      return push_opreg_action(account, yield_action::claimyield, mvo()
         (withdrawal_field::account,    account)
         (withdrawal_field::token_code, token_code));
   }

   /// `sysio.liq`'s cumulative LIQETH index, 0 before the first distribution. Every bonded unit's
   /// checkpoint tracks this index.
   fc::uint128_t liq_index() {
      const auto data = get_row_by_id(LIQ_ACCOUNT, LIQ_ACCOUNT, "yieldidx"_n, kLiqEthSymbol.to_symbol_code().value);
      return data.empty() ? fc::uint128_t{0}
                          : liq_abi_ser.binary_to_variant("yield_index", data,
                               fixture_abi_yield_function())["index"].as_uint128();
   }

   /// `account`'s depot-native `(WIRE, token_code)` balance row; empty variant when absent.
   fc::variant depot_native_row(name account, std::string_view token_code) {
      const auto op = get_operator(account);   // held: the loop below references into it
      for (const auto& bal : op["balances"].get_array()) {
         if (bal["chain_code"].as<fc::slug_name>().value == cn(kWireCodename).value &&
             bal["token_code"].as<fc::slug_name>().value == cn(token_code).value) {
            return bal;
         }
      }
      return fc::variant();
   }

   /// What `sysio.liq` owes the registry's LIQETH holder row now, by the shadow yield spec.
   int64_t registry_liq_owed() {
      const auto sym_code = kLiqEthSymbol.to_symbol_code().value;
      const auto holder   = liq_abi_ser.binary_to_variant("account",
         get_row_by_id(LIQ_ACCOUNT, OPREG_ACCOUNT, "accounts"_n, sym_code),
         fixture_abi_yield_function());
      return yield_reference::owed(holder["balance"].as<asset>().get_amount(), liq_index(),
                                   holder["index_checkpoint"].as_uint128(), holder["owed_wire"].as_uint64());
   }

   /// Sweep LIQETH and return the WIRE it moved into the registry, asserting that the same
   /// amount left `sysio.liq` and that it equals what the spec says the registry was owed.
   int64_t sweep_liqeth() {
      const int64_t owed          = registry_liq_owed();
      const int64_t opreg_before  = wire_balance(OPREG_ACCOUNT);
      const int64_t liq_before    = wire_balance(LIQ_ACCOUNT);
      BOOST_REQUIRE_EQUAL(success(), sweepyield(kLiqEthCodename));
      const int64_t swept = wire_balance(OPREG_ACCOUNT) - opreg_before;
      BOOST_REQUIRE_EQUAL(owed, swept);
      BOOST_REQUIRE_EQUAL(liq_before - swept, wire_balance(LIQ_ACCOUNT));
      return swept;
   }

   /// The dust setup: kYieldPrincipalHolder bonds kYieldPrincipal WIRE collateral, A is the only
   /// shadow bonder so the registry's `sysio.liq` row holds exactly A's shadow, then kDustRounds
   /// rounds of addyield + sweep. Returns the WIRE the registry received from `sysio.liq`, having
   /// asserted after every sweep that the registry's WIRE is exactly principal plus receipts.
   int64_t setup_yield_dust() {
      setup_yield_holders();
      BOOST_REQUIRE_EQUAL(success(),
                          regoperator(kYieldPrincipalHolder, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
      base_tester::push_action(TOKEN_ACCOUNT, "transfer"_n, kYieldDonor, mvo()
         ("from", kYieldDonor)("to", kYieldPrincipalHolder)
         ("quantity", asset(static_cast<int64_t>(kYieldPrincipal), kWireSymbol))("memo", std::string{}));
      BOOST_REQUIRE_EQUAL(success(), deposit(kYieldPrincipalHolder, kWireCodename, kYieldPrincipal));
      BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));

      int64_t received = 0;
      for (int round = 0; round < kDustRounds; ++round) {
         BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
         received += sweep_liqeth();
         BOOST_REQUIRE_EQUAL(static_cast<int64_t>(kYieldPrincipal) + received, wire_balance(OPREG_ACCOUNT));
      }
      return received;
   }

   /// `claimyield` and return what it credited to the operator's WIRE claim row.
   int64_t claim_yield_credit(name account) {
      const auto before = get_remitclaim(account, kWireCodename);
      const int64_t claimed_before = before.is_null() ? 0 : static_cast<int64_t>(before["balance"].as_uint64());
      BOOST_REQUIRE_EQUAL(success(), claimyield(account, kLiqEthCodename));
      return static_cast<int64_t>(get_remitclaim(account, kWireCodename)["balance"].as_uint64()) - claimed_before;
   }

   abi_serializer opreg_abi_ser;
   abi_serializer epoch_abi_ser;
   abi_serializer chains_abi_ser;
   abi_serializer authex_abi_ser;
   abi_serializer dclaim_abi_ser;
   abi_serializer tokens_abi_ser;
   abi_serializer liq_abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_opreg_tests)

// ── setconfig ──

BOOST_FIXTURE_TEST_CASE(setconfig_basic, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   auto cfg = get_opconfig();
   BOOST_REQUIRE_EQUAL(600000, cfg["terminate_prune_delay_ms"].as_uint64());
   BOOST_REQUIRE_EQUAL(kDefaultMaxConsecutiveMisses,
                       cfg["terminate_max_consecutive_misses"].as_uint64());
   BOOST_REQUIRE_EQUAL(kDefaultMaxPctMisses24h, cfg["terminate_max_pct_misses_24h"].as_uint64());
   BOOST_REQUIRE_EQUAL(kDefaultMinPctSamples, cfg["terminate_min_pct_samples"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_bounds_min_pct_samples, sysio_opreg_tester) { try {
   constexpr std::string_view kOutOfBounds =
      "assertion failure with message: terminate_min_pct_samples must be in [1, 100]";
   for (const uint32_t rejected : {kPercentRailFromFirstDuty - 1, kMaxAcceptedMinPctSamples + 1}) {
      BOOST_REQUIRE_EQUAL(error(std::string(kOutOfBounds)),
                          setconfig(kDefaultPruneDelayMs, kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h,
                                    rejected, kTerminateWindowMs));
   }
   for (const uint32_t accepted : {kPercentRailFromFirstDuty, kMaxAcceptedMinPctSamples}) {
      BOOST_REQUIRE_EQUAL(success(),
                          setconfig(kDefaultPruneDelayMs, kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h,
                                    accepted, kTerminateWindowMs));
      BOOST_REQUIRE_EQUAL(accepted, get_opconfig()["terminate_min_pct_samples"].as_uint64());
      produce_blocks();
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_prune_delay, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_prune_delay_ms must be positive"),
      setconfig(/*prune_delay=*/0)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_disabling_percent_miss_threshold, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_max_pct_misses_24h must be in [1, 99]"),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDisablingPctMisses24h, kDefaultMinPctSamples,
                kTerminateWindowMs)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_disabling_consecutive_miss_threshold, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_max_consecutive_misses must be in [1, 5]"),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses + 1, kMaxAcceptedPctMisses24h, kDefaultMinPctSamples,
                kTerminateWindowMs)
   );

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: terminate_max_consecutive_misses must be in [1, 5]"),
      setconfig(kDefaultPruneDelayMs,
                std::numeric_limits<uint32_t>::max(), kMaxAcceptedPctMisses24h, kDefaultMinPctSamples,
                kTerminateWindowMs)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_zero_min_bond, sysio_opreg_tester) { try {
   // A zero min_bond entry makes the `available >= min_bond` eligibility gate
   // vacuously true, so an operator could reach ACTIVE with no collateral posted.
   // The sane "no requirement" form is an empty vector, which stays accepted.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: req_uw_collat: min_bond must be positive "
            "(an empty requirement set imposes no bond)"),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h, kDefaultMinPctSamples, kTerminateWindowMs,
                {}, {}, { make_chain_min_bond("WIRE", "NTA", kRejectedZeroMinBond) })
   );
   // The identical shape with a positive min_bond is accepted.
   BOOST_REQUIRE_EQUAL(
      success(),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h, kDefaultMinPctSamples, kTerminateWindowMs,
                {}, {}, { make_chain_min_bond("WIRE", "NTA", kTestMinBond) })
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_uncanonical_collateral_code, sysio_opreg_tester) { try {
   // A `slug_name` reaches action JSON either as its canonical STRING or through the
   // transitional object form `{"value": N}`, and only the string arm validates. These
   // entries persist on the config row, and rendering is TOTAL so an uncanonical code
   // never announces itself — it can decode to "" or to a valid spelling that re-parses
   // as a DIFFERENT code. `setconfig` is a privileged top-level action and refuses.
   constexpr uint64_t uncanonical = 7;   // decodes to "", packs back to 0 — not a code
   BOOST_REQUIRE(!fc::slug_name{uncanonical}.is_canonical());

   const auto bad_chain = fc::variant(mvo()
      ("chain_code",          mvo()("value", uncanonical))
      ("token_code",          "ETH")
      ("min_bond",            kTestMinBond)
      ("config_timestamp_ms", uint64_t{0}));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: req_uw_collat: code 7 has no canonical "
            "slug_name spelling"),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h, kDefaultMinPctSamples, kTerminateWindowMs,
                {}, {}, { bad_chain })
   );

   // The identical shape with a spellable code is accepted.
   BOOST_REQUIRE_EQUAL(
      success(),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h, kDefaultMinPctSamples, kTerminateWindowMs,
                {}, {}, { make_chain_min_bond("WIRE", "NTA", kTestMinBond) })
   );
} FC_LOG_AND_RETHROW() }

/// `setconfig` takes any positive window, whatever every chain's schedule: one narrower than a chain's
/// evicting run, down to a millisecond.
BOOST_FIXTURE_TEST_CASE(setconfig_accepts_a_window_narrower_than_every_chains_run, sysio_opreg_tester) { try {
   constexpr std::string_view kSlowChain = "SOL";
   BOOST_REQUIRE_EQUAL(success(), set_epoch_config(kRotationEpochDurationSec));
   BOOST_REQUIRE_EQUAL(success(), set_epoch_config(2 * kRotationEpochDurationSec, kRotationGroups, kSlowChain));
   produce_blocks();
   for (const uint64_t window_ms : {kEvictingRunSpanMs - 1, kDutyRotationMs, uint64_t{1}}) {
      BOOST_REQUIRE_EQUAL(success(),
         setconfig(kDefaultPruneDelayMs,
                   kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h, kDefaultMinPctSamples, window_ms));
      produce_blocks();
      BOOST_REQUIRE_EQUAL(window_ms, get_opconfig()["terminate_window_ms"].as_uint64());
   }
} FC_LOG_AND_RETHROW() }

/// `sysio.epoch::setconfig` takes any schedule whatever opreg's stored window: a longer duration, or more
/// groups before the rotation is materialized.
BOOST_FIXTURE_TEST_CASE(epoch_setconfig_ignores_the_stored_opreg_window, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(),
      setconfig(kDefaultPruneDelayMs,
                kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h, kDefaultMinPctSamples, kDutyRotationMs));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), set_epoch_config(kRotationEpochDurationSec));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), set_epoch_config(2 * kRotationEpochDurationSec));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), set_epoch_config(kRotationEpochDurationSec, kRotationGroups + 1));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_evicts_a_run_whose_rows_aged_out, sysio_opreg_tester) { try {
   // A window of one duty rotation keeps only the latest record, yet the consecutive rail counts every
   // missed duty epoch since the last delivery: a run recorded one rotation apart evicts at the duty that
   // makes it too long, the delivered anchor and the earlier misses long erased.
   BOOST_REQUIRE_EQUAL(success(), set_epoch_config(kRotationEpochDurationSec));
   produce_blocks();
   activate_batch_operator("batchop.a"_n,
                           kDefaultMaxConsecutiveMisses,
                           kMaxAcceptedPctMisses24h,
                           kDefaultMinPctSamples,
                           kDutyRotationMs);
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/true));
   produce_blocks();

   // Five missed duty epochs, one full rotation apart: still serving. Each push is finalized before the
   // next rotation jump so the pending transaction cannot expire across it.
   for (uint32_t duty = 1; duty <= kDefaultMaxConsecutiveMisses; ++duty) {
      produce_block(fc::milliseconds(kDutyRotationMs));
      BOOST_REQUIRE_EQUAL(success(),
         recorddel("batchop.a"_n, 1 + duty * kRotationGroups, /*delivered=*/false));
      BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
      produce_blocks();
      BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
   }
   BOOST_REQUIRE(get_dellog_entry(kFirstDeliveryLogId).is_null());
   BOOST_REQUIRE_EQUAL(uint64_t{kDefaultMaxConsecutiveMisses}, tally("batchop.a"_n)["miss_run"].as_uint64());
   BOOST_REQUIRE_LT(tally("batchop.a"_n)["duties"].as_uint64(), uint64_t{kDefaultMaxConsecutiveMisses});

   // The sixth missed rotation makes the run too long.
   produce_block(fc::milliseconds(kDutyRotationMs));
   BOOST_REQUIRE_EQUAL(success(),
      recorddel("batchop.a"_n, 1 + (kDefaultMaxConsecutiveMisses + 1) * kRotationGroups,
                /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();

   require_evicted("batchop.a"_n, "rolling-window: >5 consecutive misses");
} FC_LOG_AND_RETHROW() }

// Schedule drift: the live rotation (epoch_state.batch_op_groups) is sized from
// batch_op_groups once at schbatchgps and advance() then preserves its length, so
// the config count must stay pinned to the live rotation: epoch::setconfig rejects
// any group-count change once the schedule is materialized, while an unchanged
// count still passes.
BOOST_FIXTURE_TEST_CASE(epoch_setconfig_locks_batch_op_groups_after_schedule, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.c"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   setup_served_chain();
   serve_evm_chain({"batchop.a"_n, "batchop.b"_n, "batchop.c"_n});

   auto epoch_setconfig = [&](uint32_t groups, uint32_t min_active) {
      return push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("chain_code",                         EPOCH_CHAIN)
         ("epoch_duration_sec",                 90)
         ("traffic_wait_sec",                   0)
         ("operators_per_epoch",                1)
         ("batch_operator_minimum_active",      min_active)
         ("batch_op_groups",                    groups)
         ("epoch_retention_envelope_log_count", 200));
   };

   // Before the schedule exists the group count is free; land on 3 and materialize.
   BOOST_REQUIRE_EQUAL(success(), epoch_setconfig(3, 3));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo()("chain_code", EPOCH_CHAIN)));
   produce_blocks();

   // Decreasing the group count is now rejected.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_op_groups cannot change once "
            "the rotation schedule is materialized"),
      epoch_setconfig(2, 2));

   // Increasing it is rejected the same way (no fourth operator needed -- the guard
   // fires before schbatchgps would re-materialize).
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: batch_op_groups cannot change once "
            "the rotation schedule is materialized"),
      epoch_setconfig(4, 4));

   // Re-issuing the same group count still succeeds.
   BOOST_REQUIRE_EQUAL(success(), epoch_setconfig(3, 3));
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
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op[eligibility_field::status].as<OperatorStatus>());
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

/// WNS-70: `sysio.epoch::advance` reaches `slash` inline, so an operator with nothing left to
/// slash (no row, already slashed, terminated) is left as it is instead of aborting the caller.
BOOST_FIXTURE_TEST_CASE(slash_leaves_removed_operator_unchanged, sysio_opreg_tester) { try {
   constexpr const char* kSlashReason       = "first slash";
   constexpr const char* kRepeatSlashReason = "repeat slash";
   constexpr const char* kTerminationReason = "administrative removal";
   BOOST_REQUIRE_EQUAL(success(), setconfig());

   BOOST_REQUIRE(get_operator("batchop.c"_n).is_null());
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.c"_n, kSlashReason));
   BOOST_REQUIRE(get_operator("batchop.c"_n).is_null());

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.a"_n, kSlashReason));
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.a"_n, kRepeatSlashReason));
   BOOST_REQUIRE_EQUAL(kSlashReason, get_operator("batchop.a"_n)["status_reason"].as_string());

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.b"_n, kTerminationReason));
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.b"_n, kSlashReason));
   auto terminated = get_operator("batchop.b"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED == terminated["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(kTerminationReason, terminated["status_reason"].as_string());
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
   setup_served_chain();
   serve_evm_chain({"batchop.a"_n, "batchop.b"_n, "batchop.c"_n});

   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
      ("chain_code", EPOCH_CHAIN)
      ("epoch_duration_sec", 90)
      ("traffic_wait_sec", 0)
      ("operators_per_epoch", 1)
      ("batch_operator_minimum_active", 3)
      ("batch_op_groups", 3)
      ("epoch_retention_envelope_log_count", 200)
   ));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo()("chain_code", EPOCH_CHAIN)));
   produce_blocks();

   auto epoch_state_data =
      get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, fc::slug_name{EPOCH_CHAIN}.value);
   BOOST_REQUIRE(!epoch_state_data.empty());
   auto epoch_state = epoch_abi_ser.binary_to_variant(
      "epoch_state", epoch_state_data,
      fixture_abi_yield_function());
   auto groups = epoch_state["batch_op_groups"].get_array();
   BOOST_REQUIRE_EQUAL(3, groups.size());
} FC_LOG_AND_RETHROW() }

// ── Served chains (joinchain / leavechain) ──

/// A batch operator registering itself needs no outpost link: `joinchain` checks the links chain by
/// chain. Every other type registering itself still needs one for every active outpost.
BOOST_FIXTURE_TEST_CASE(regoperator_leaves_batch_links_to_joinchain, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   setup_served_chain();

   const auto self_register = [&](name account, OperatorType type) {
      return push_opreg_action(account, "regoperator"_n, mvo()
         ("account", account)("type", type)("is_bootstrapped", false));
   };
   BOOST_REQUIRE_EQUAL(error(std::string(kMissingOutpostLinkError)),
                       self_register("uwrit.a"_n, OPERATOR_TYPE_UNDERWRITER));
   BOOST_REQUIRE_EQUAL(success(), self_register("batchop.a"_n, OPERATOR_TYPE_BATCH));
   BOOST_REQUIRE_EQUAL(error(std::string(kMissingChainLinkError)),
                       served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.a"_n));

   served::link_fresh_key(*this, "batchop.a"_n, ChainKind::CHAIN_KIND_EVM);
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.a"_n));
} FC_LOG_AND_RETHROW() }

/// `joinchain` signed by the operator or by sysio.opreg records a SERVING membership stamped with the
/// join time; any other signer is refused.
BOOST_FIXTURE_TEST_CASE(joinchain_records_a_serving_membership, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   setup_served_chain();
   for (const name op : {"batchop.a"_n, "batchop.b"_n}) served::link_fresh_key(*this, op, ChainKind::CHAIN_KIND_EVM);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(error("missing authority of batchop.a"),
                       served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.b"_n));
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.a"_n));
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.b"_n, EPOCH_CHAIN));
   produce_blocks();

   const uint64_t now_ms = static_cast<uint64_t>(control->head().block_time().sec_since_epoch()) * 1000;
   for (const name op : {"batchop.a"_n, "batchop.b"_n}) {
      const auto row = served::membership(*this, opreg_abi_ser, op, EPOCH_CHAIN);
      BOOST_REQUIRE(!row.is_null());
      BOOST_REQUIRE_EQUAL(op, row["account"].as<name>());
      BOOST_REQUIRE_EQUAL(std::string(EPOCH_CHAIN), row["chain_code"].as_string());
      BOOST_REQUIRE_EQUAL(std::string(served::status::serving), row["status"].as_string());
      BOOST_REQUIRE_GT(row["joined_at_ms"].as_uint64(), 0u);
      BOOST_REQUIRE_LE(row["joined_at_ms"].as_uint64(), now_ms);
      BOOST_REQUIRE_EQUAL(row["joined_at_ms"].as_uint64(), row["updated_at_ms"].as_uint64());
      BOOST_REQUIRE(row["status_reason"].as_string().empty());
   }
   BOOST_REQUIRE(served::membership(*this, opreg_abi_ser, "batchop.a"_n, kUnregisteredChain).is_null());
} FC_LOG_AND_RETHROW() }

/// `joinchain` refuses an account with no operator row, a non-batch operator, a slashed or a terminated
/// operator, a chain that is not an active outpost, an operator with no link for the chain's kind, and
/// an operator already serving the chain.
BOOST_FIXTURE_TEST_CASE(joinchain_refusals, sysio_opreg_tester) { try {
   constexpr auto kSlashed    = "batchop.b"_n;
   constexpr auto kTerminated = "batchop.c"_n;
   constexpr auto kUnlinked   = "batchop.d"_n;
   create_accounts({kUnlinked});
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   for (const name op : {"batchop.a"_n, kSlashed, kTerminated, kUnlinked}) {
      BOOST_REQUIRE_EQUAL(success(), regoperator(op, OPERATOR_TYPE_BATCH, true));
   }
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.a"_n, OPERATOR_TYPE_UNDERWRITER, false));
   setup_served_chain();
   for (const name op : {"batchop.a"_n, kSlashed, kTerminated, "uwrit.a"_n, "producer.a"_n}) {
      served::link_fresh_key(*this, op, ChainKind::CHAIN_KIND_EVM);
   }
   BOOST_REQUIRE_EQUAL(success(), slash(kSlashed, "slashed before joining"));
   BOOST_REQUIRE_EQUAL(success(), terminate(kTerminated, "terminated before joining"));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(error(std::string(kOperatorNotFoundError)), served::join(*this, "producer.a"_n, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(error(std::string(kOnlyBatchServesChainsError)), served::join(*this, "uwrit.a"_n, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(error(std::string(kTerminalOperatorJoinError)), served::join(*this, kSlashed, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(error(std::string(kTerminalOperatorJoinError)), served::join(*this, kTerminated, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(error(std::string(kNotActiveOutpostError)),
                       served::join(*this, "batchop.a"_n, kUnregisteredChain));
   BOOST_REQUIRE_EQUAL(error(std::string(kMissingChainLinkError)), served::join(*this, kUnlinked, EPOCH_CHAIN));

   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, EPOCH_CHAIN));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(error(std::string(kAlreadyServingError)), served::join(*this, "batchop.a"_n, EPOCH_CHAIN));
} FC_LOG_AND_RETHROW() }

/// `leavechain` moves a SERVING membership to LEFT under the operator's own signature and refuses any
/// other state. With no seat held on the chain the operator rejoins at once, with a new join time.
BOOST_FIXTURE_TEST_CASE(leavechain_then_rejoin, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   setup_served_chain();
   serve_evm_chain({"batchop.a"_n});
   produce_blocks();
   const uint64_t first_join = served::membership(*this, opreg_abi_ser, "batchop.a"_n, EPOCH_CHAIN)["joined_at_ms"]
                                  .as_uint64();

   BOOST_REQUIRE_EQUAL(error(std::string(kNotServingError)), served::leave(*this, "batchop.b"_n, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(error("missing authority of batchop.a"),
                       push_opreg_action(OPREG_ACCOUNT, "leavechain"_n,
                                         mvo()("account", "batchop.a"_n)("chain_code", EPOCH_CHAIN)));
   produce_blocks(kBlocksPastOneSecond);
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, "batchop.a"_n, EPOCH_CHAIN));
   produce_blocks();

   auto row = served::membership(*this, opreg_abi_ser, "batchop.a"_n, EPOCH_CHAIN);
   BOOST_REQUIRE_EQUAL(std::string(served::status::left), row["status"].as_string());
   BOOST_REQUIRE_EQUAL(first_join, row["joined_at_ms"].as_uint64());
   BOOST_REQUIRE_GT(row["updated_at_ms"].as_uint64(), first_join);
   BOOST_REQUIRE_EQUAL(error(std::string(kNotServingError)), served::leave(*this, "batchop.a"_n, EPOCH_CHAIN));

   produce_blocks(kBlocksPastOneSecond);
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.a"_n));
   produce_blocks();
   row = served::membership(*this, opreg_abi_ser, "batchop.a"_n, EPOCH_CHAIN);
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), row["status"].as_string());
   BOOST_REQUIRE_GT(row["joined_at_ms"].as_uint64(), first_join);
} FC_LOG_AND_RETHROW() }

/// The schedule seats only ACTIVE batch operators SERVING the chain: an operator that never joined and
/// one that left are passed over, and too few servers leave `schbatchgps` short.
BOOST_FIXTURE_TEST_CASE(schbatchgps_seats_only_operators_serving_the_chain, sysio_opreg_tester) { try {
   constexpr auto kLeaver     = "batchop.d"_n;
   constexpr auto kNeverJoins = "batchop.e"_n;
   constexpr uint32_t kGroups = 3;
   create_accounts({kLeaver, kNeverJoins});
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   for (const name op : {"batchop.a"_n, "batchop.b"_n, "batchop.c"_n, kLeaver, kNeverJoins}) {
      BOOST_REQUIRE_EQUAL(success(), regoperator(op, OPERATOR_TYPE_BATCH, true));
   }
   setup_served_chain();
   serve_evm_chain({"batchop.a"_n, "batchop.b"_n, "batchop.c"_n, kLeaver});
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, kLeaver, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, "batchop.c"_n, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "setconfig"_n, mvo()
      ("chain_code", EPOCH_CHAIN)("epoch_duration_sec", kRotationEpochDurationSec)("traffic_wait_sec", 0)
      ("operators_per_epoch", 1)
      ("batch_operator_minimum_active", kGroups)("batch_op_groups", kGroups)
      ("epoch_retention_envelope_log_count", 200)));
   produce_blocks();

   // Two servers for a three-seat window.
   BOOST_REQUIRE_EQUAL(error(std::string(kNotEnoughBatchOperatorsError)),
                       push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo()("chain_code", EPOCH_CHAIN)));

   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.c"_n, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), push_epoch_action(EPOCH_ACCOUNT, "schbatchgps"_n, mvo()("chain_code", EPOCH_CHAIN)));
   produce_blocks();

   const auto state = epoch_abi_ser.binary_to_variant("epoch_state",
      get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, cn(EPOCH_CHAIN).value),
      fixture_abi_yield_function());
   std::vector<name> seated;
   for (const auto& group : state["batch_op_groups"].get_array()) {
      for (const auto& member : group.get_array()) seated.push_back(member.as<name>());
   }
   std::sort(seated.begin(), seated.end());
   BOOST_REQUIRE(seated == (std::vector<name>{"batchop.a"_n, "batchop.b"_n, "batchop.c"_n}));
} FC_LOG_AND_RETHROW() }

/// A registration ends its memberships: an account registering again after termination serves no
/// chain, and `prune` erases a pruned operator's memberships with its row.
BOOST_FIXTURE_TEST_CASE(memberships_end_with_the_registration, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.b"_n, OPERATOR_TYPE_BATCH, true));
   setup_served_chain();
   serve_evm_chain({"batchop.a"_n, "batchop.b"_n});
   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "registers again"));
   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.b"_n, "is pruned"));
   produce_blocks();
   BOOST_REQUIRE(!served::membership(*this, opreg_abi_ser, "batchop.a"_n, EPOCH_CHAIN).is_null());

   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   produce_blocks();
   BOOST_REQUIRE(served::membership(*this, opreg_abi_ser, "batchop.a"_n, EPOCH_CHAIN).is_null());

   produce_block(fc::milliseconds(kDefaultPruneDelayMs));
   BOOST_REQUIRE(!served::membership(*this, opreg_abi_ser, "batchop.b"_n, EPOCH_CHAIN).is_null());
   BOOST_REQUIRE_EQUAL(success(), prune());
   produce_blocks();
   BOOST_REQUIRE(get_operator("batchop.b"_n).is_null());
   BOOST_REQUIRE(served::membership(*this, opreg_abi_ser, "batchop.b"_n, EPOCH_CHAIN).is_null());
} FC_LOG_AND_RETHROW() }


// ── deposit (Task 2: msgch-dispatched outpost-driven deposit) ──

BOOST_FIXTURE_TEST_CASE(deposit_credits_balance_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1'000'000));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());
   BOOST_REQUIRE_EQUAL(cn("WIRE").value, balances[0]["chain_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(cn("NTA").value, balances[0]["token_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(1'000'000,       balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_aggregates_into_existing_balance_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));

   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 100));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 50));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());     // single row, NOT two
   BOOST_REQUIRE_EQUAL(150, balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// WIRE-375 / WNS-40: direct depot deposits must custody the same 9-decimal WIRE
/// asset that the `(WIRE, WIRE)` collateral bucket denotes. Crediting raw
/// 4-decimal SYS units into that bucket made challenge-bond valuation interpret
/// one SYS atomic unit as one WIRE atomic unit, understating the bond by 10^5.
BOOST_FIXTURE_TEST_CASE(deposit_custodies_and_credits_wire_units, sysio_opreg_tester) { try {
   constexpr uint64_t DEPOSIT = 2 * kWireUnit;
   const auto OPERATOR = "uwrit.alice"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*prune_delay=*/kDefaultPruneDelayMs,
      /*max_consec_misses=*/kDefaultMaxConsecutiveMisses,
      /*max_pct_misses_24h=*/kDefaultMaxPctMisses24h,
      /*min_pct_samples=*/kDefaultMinPctSamples,
      /*terminate_window_ms=*/kTerminateWindowMs,
      /*req_prod_collat=*/{},
      /*req_batchop_collat=*/{},
      /*req_uw_collat=*/{
         make_chain_min_bond(kWireCodename, kWireCodename, DEPOSIT),
      }));
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);

   const int64_t operator_before = wire_balance(OPERATOR);
   const int64_t opreg_before = wire_balance(OPREG_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kWireCodename, DEPOSIT));

   BOOST_REQUIRE_EQUAL(operator_before - static_cast<int64_t>(DEPOSIT), wire_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(opreg_before + static_cast<int64_t>(DEPOSIT), wire_balance(OPREG_ACCOUNT));

   auto op = get_operator(OPERATOR);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1u, balances.size());
   BOOST_REQUIRE_EQUAL(cn(kWireCodename).value, balances[0]["chain_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(cn(kWireCodename).value, balances[0]["token_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(DEPOSIT, balances[0]["balance"].as_uint64());
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
} FC_LOG_AND_RETHROW() }

// SEC-103 (PR #449 review): the deposit cap must hold under RE-ENTRANCY. An
// operator account that carries contract code is notified by sysio.token::transfer
// when opreg::deposit moves its WIRE collateral, and can re-enter deposit during
// that notification. Were the cap a pre-read-then-credit (not atomic with the
// mutation), two credits could pass against the same stale balance and push the
// WIRE collateral row past asset::max_amount. opreg::deposit performs the cap check
// INSIDE the same modify as the credit (and credits before the transfer), so the
// re-entrant deposit observes the already-committed balance: its own check trips
// and the whole transaction aborts. reenter_deposit (contracts/test_contracts)
// models the malicious operator — it re-enters deposit(+1) on the outgoing transfer.
BOOST_FIXTURE_TEST_CASE(deposit_reentrancy_cannot_exceed_max_collateral, sysio_opreg_tester) { try {
   constexpr uint64_t MAX_COLLATERAL = (uint64_t{1} << 62) - 1; // asset::max_amount
   const auto OPERATOR = "uwrit.alice"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, false));

   // Fund the operator with EXACTLY the WIRE asset cap so the outer deposit fills
   // the WIRE row to asset::max_amount
   // and only the re-entrant +1 would exceed it.
   setup_wire_token_and_fund(OPERATOR, kWireMaximumSupply);

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
   // outgoing WIRE transfer notifies the operator, and its handler re-enters
   // deposit(+1). The +1 would push the row to 2^62 — its in-modify cap check trips
   // and aborts the whole transaction. No over-credit is possible.
   auto r = deposit(OPERATOR, kWireCodename, MAX_COLLATERAL);
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
      bond_generic("uwrit.alice"_n, "NTA", 100));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTB", 200));

   auto op = get_operator("uwrit.alice"_n);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(2, balances.size());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(deposit_rejects_slashed_operator, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(), slash("uwrit.alice"_n, "test slash"));
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: operator not in a deposit-eligible state"),
                       bond_generic("uwrit.alice"_n, "NTA", 100));
   BOOST_CHECK(get_operator("uwrit.alice"_n)["balances"].get_array().empty());
   BOOST_CHECK_EQUAL(100, get_currency_balance(LIQ_ACCOUNT, symbol(9, "NTA"), "uwrit.alice"_n).get_amount());
} FC_LOG_AND_RETHROW() }

// ── queuewtdw + cancelwtdw ──

BOOST_FIXTURE_TEST_CASE(queuewtdw_creates_request_row, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 400));

   auto row = get_wtdw(1);   // monotonic id starts at 1
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL("uwrit.alice", row["account"].as_string());
   BOOST_REQUIRE_EQUAL(400,        row["amount"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(withdraw_logs_failure_on_insufficient_available, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 100));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 200));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string("insufficient available balance for withdraw"),
                       entry["error_message"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(withdraw_subtracts_from_available_on_subsequent_call,
                        sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 700));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 400));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string("insufficient available balance for withdraw"),
                       entry["error_message"].as_string());
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE(get_wtdw(2).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(withdraw_allows_requests_across_collateral_buckets,
                        sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTB", 1000));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 400));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTB", 400));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(true, entry["success"].as_bool());
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE(!get_wtdw(2).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(withdraw_rejects_when_request_is_outstanding_for_same_bucket,
                        sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   setup_wire_token_and_fund("uwrit.alice"_n, kWireFundingQuantity);
   BOOST_REQUIRE_EQUAL(success(), deposit("uwrit.alice"_n, kWireCodename, 1000));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "WIRE", 400));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), withdraw("uwrit.alice"_n, kWireCodename, 400));

   auto entry = latest_action_log("uwrit.alice"_n);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(
      std::string("operator already has an outstanding withdraw request for this collateral bucket"),
      entry["error_message"].as_string());
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE(get_wtdw(2).is_null());
} FC_LOG_AND_RETHROW() }

/// A successful outpost withdrawal reservation immediately removes an
/// undercollateralized batch operator from the active set.
BOOST_FIXTURE_TEST_CASE(withdraw_rechecks_batch_eligibility_after_enqueue, sysio_opreg_tester) { try {
   activate_batch_operator(kEligibilityBatchOperator);

   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));

   auto op = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE(!get_wtdw(kFirstWithdrawalRequestId).is_null());
} FC_LOG_AND_RETHROW() }

/// A direct WIRE withdrawal reservation applies the same immediate eligibility
/// transition to a non-bootstrapped producer.
BOOST_FIXTURE_TEST_CASE(withdraw_rechecks_eligibility_after_enqueue, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*prune_delay=*/kDefaultPruneDelayMs,
      /*max_consec_misses=*/kDefaultMaxConsecutiveMisses,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h,
      /*min_pct_samples=*/kDefaultMinPctSamples,
      /*terminate_window_ms=*/kTerminateWindowMs,
      /*req_prod_collat=*/{
         make_chain_min_bond(kWireCodename, kWireCodename, kTestMinBond),
      },
      /*req_batchop_collat=*/{},
      /*req_uw_collat=*/{}));
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(kEligibilityProducer, OPERATOR_TYPE_PRODUCER, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(kEligibilityProducer, kWireFundingQuantity);
   BOOST_REQUIRE_EQUAL(success(), deposit(kEligibilityProducer, kWireCodename, kTestMinBond));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(kEligibilityProducer)[eligibility_field::status].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(), withdraw(kEligibilityProducer, kWireCodename, kTestMinBond));

   auto op = get_operator(kEligibilityProducer);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE(!get_wtdw(kFirstWithdrawalRequestId).is_null());
} FC_LOG_AND_RETHROW() }

/// A rejected outpost withdrawal has no reservation side effect and therefore
/// cannot change an otherwise eligible operator's status.
BOOST_FIXTURE_TEST_CASE(withdraw_rejection_preserves_eligibility, sysio_opreg_tester) { try {
   activate_batch_operator(kEligibilityBatchOperator);

   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kInsufficientTestBond));

   auto op = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
   auto entry = latest_action_log(kEligibilityBatchOperator);
   BOOST_REQUIRE_EQUAL(false, entry[eligibility_field::success].as_bool());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cancelwtdw_removes_pending_request, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 400));

   BOOST_REQUIRE_EQUAL(success(), cancelwtdw("uwrit.alice"_n, "uwrit.alice"_n, 1));

   auto row = get_wtdw(1);
   BOOST_REQUIRE(row.is_null());

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 1000));
   BOOST_REQUIRE(!get_wtdw(2).is_null());
} FC_LOG_AND_RETHROW() }

/// Canceling the only pending reservation restores the operator immediately
/// once its available collateral again covers the configured minimum.
BOOST_FIXTURE_TEST_CASE(cancelwtdw_rechecks_eligibility_after_erase, sysio_opreg_tester) { try {
   activate_batch_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(), cancelwtdw(
      kEligibilityBatchOperator, kEligibilityBatchOperator, kFirstWithdrawalRequestId));

   auto op = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
} FC_LOG_AND_RETHROW() }

/// A stale eligibility callback after punishment must not reactivate a
/// bootstrapped operator, even though bootstrapped operators bypass collateral
/// minimums.
BOOST_FIXTURE_TEST_CASE(processbatch_preserves_slashed_bootstrapped_status, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(kEligibilityBatchOperator, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/true));
   BOOST_REQUIRE_EQUAL(success(), slash(kEligibilityBatchOperator, std::string{kTestSlashReason}));

   BOOST_REQUIRE_EQUAL(success(), processbatch(
      kEligibilityBatchOperator, /*was_eligible=*/false, /*is_eligible=*/true));

   auto op = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_SLASHED ==
                 op[eligibility_field::status].as<OperatorStatus>());
} FC_LOG_AND_RETHROW() }

/// A stale eligibility callback after administrative removal must likewise
/// leave a bootstrapped operator permanently terminated.
BOOST_FIXTURE_TEST_CASE(processbatch_preserves_terminated_bootstrapped_status, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(kEligibilityBatchOperator, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/true));
   BOOST_REQUIRE_EQUAL(success(),
      terminate(kEligibilityBatchOperator, std::string{kTestTerminationReason}));

   BOOST_REQUIRE_EQUAL(success(), processbatch(
      kEligibilityBatchOperator, /*was_eligible=*/false, /*is_eligible=*/true));

   auto op = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED ==
                 op[eligibility_field::status].as<OperatorStatus>());
} FC_LOG_AND_RETHROW() }

/// Flushing a partial withdrawal erases its reservation before eligibility is
/// recomputed, so the matured amount is not subtracted twice.
BOOST_FIXTURE_TEST_CASE(flushwtdw_rechecks_eligibility_after_erase, sysio_opreg_tester) { try {
   activate_batch_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));

   auto op = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(flushwtdw_allows_operator_to_request_again, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 400));

   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
   BOOST_REQUIRE(get_wtdw(1).is_null());

   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 600));
   BOOST_REQUIRE(!get_wtdw(2).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(cancelwtdw_rejects_other_operators_request, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.alice"_n, OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(), regoperator("uwrit.bob"_n,   OPERATOR_TYPE_UNDERWRITER, false));
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("uwrit.alice"_n, "NTA", 1000));
   BOOST_REQUIRE_EQUAL(success(),
      withdraw("uwrit.alice"_n, "NTA", 400));

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
      bond_generic("batchop.a"_n, "NTA", 500));

   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "rolling-24h: >5% miss rate"));

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE(op["terminated_at"].as_uint64() > 0);
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1, balances.size());
   BOOST_REQUIRE_EQUAL(0, balances[0]["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

// An operator cannot block its own termination by refusing the collateral remit.
//
// `sysio.token::transfer` notifies `to`, and the chain runs notified receivers with no exception
// isolation, so a recipient asserting in its transfer-notify handler aborts the WHOLE transaction.
// While the WIRE-chain remit was a pushed transfer, an operator facing termination could park such
// a handler and abort `terminate` -- and because `terminate` is reached from `sysio.epoch::advance`
// via `termcheck`, that aborted epoch advancement chain-wide. Worse, the termination never
// committed, so every later advance retried the same remit and re-aborted: a self-defending
// deadlock with no convergence.
//
// The remit now credits `remitclaims` and transfers nothing, so no notify handler runs on the
// termination path at all. The hostile operator is terminated on schedule and its collateral waits
// in the claim ledger; the ONLY thing its handler can still block is its own `claimremit`.
BOOST_FIXTURE_TEST_CASE(terminate_survives_operator_blocking_its_own_remit, sysio_opreg_tester) { try {
   const auto OPERATOR    = "batchop.a"_n;
   const uint64_t DEPOSIT = 5000;   // atomic WIRE units

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(OPERATOR, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));

   // Real WIRE collateral on the WIRE chain, so termination takes the WIRE-direct remit
   // branch rather than emitting a WITHDRAW_REMIT attestation to an outpost.
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kWireCodename, DEPOSIT));

   // Arm the operator AFTER the deposit: `deposit`'s escrow leg is an OUTGOING transfer, which the
   // blocking contract deliberately ignores, but deploying afterwards keeps the setup honest about
   // which transfer is under test.
   make_transfer_blocking(OPERATOR);

   // Termination must commit despite the hostile handler. Before this change it aborted here.
   BOOST_REQUIRE_EQUAL(success(), terminate(OPERATOR, "rolling-24h: >5% miss rate"));

   auto op = get_operator(OPERATOR);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_TERMINATED == op["status"].as<OperatorStatus>());
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1u, balances.size());
   BOOST_REQUIRE_EQUAL(0u, balances[0]["balance"].as_uint64());   // debited, not stranded

   // The collateral is owed, in full, in the claim ledger.
   auto claim = get_remitclaim(OPERATOR, kWireCodename);
   BOOST_REQUIRE(!claim.is_null());
   BOOST_REQUIRE_EQUAL(DEPOSIT, claim["balance"].as_uint64());

   // ...and the blocker's reach ends there: its own claim is the one thing it breaks.
   auto r = claimremit(OPERATOR, kWireCodename);
   BOOST_REQUIRE_MESSAGE(r != success(), "blocking operator unexpectedly claimed its own remit");
   BOOST_REQUIRE_MESSAGE(r.find("block_transfer: rejecting incoming transfer") != std::string::npos,
                         "unexpected failure reason: " + r);

   // The failed claim rolled back whole -- the balance is still owed, not burned.
   BOOST_REQUIRE_EQUAL(DEPOSIT, get_remitclaim(OPERATOR, kWireCodename)["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// Returned collateral remains claimable after years of inactivity and operator pruning.
BOOST_FIXTURE_TEST_CASE(claimremit_never_expires_and_clears_row_after_payment, sysio_opreg_tester) { try {
   const auto OPERATOR    = "batchop.a"_n;
   const uint64_t DEPOSIT = 5000;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(OPERATOR, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kWireCodename, DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(), terminate(OPERATOR, "rolling-24h miss"));

   BOOST_REQUIRE_EQUAL(DEPOSIT, get_remitclaim(OPERATOR, kWireCodename)["balance"].as_uint64());
   constexpr uint32_t INACTIVE_DAYS = 3 * 365;
   produce_block();
   produce_block(fc::days(INACTIVE_DAYS));
   produce_blocks(2);
   BOOST_REQUIRE_EQUAL(success(), prune());
   BOOST_REQUIRE(get_operator(OPERATOR).is_null());
   BOOST_REQUIRE_EQUAL(DEPOSIT, get_remitclaim(OPERATOR, kWireCodename)["balance"].as_uint64());
   const int64_t balance_before_claim = wire_balance(OPERATOR);
   BOOST_REQUIRE_EQUAL(success(), claimremit(OPERATOR, kWireCodename));
   BOOST_REQUIRE_EQUAL(balance_before_claim + static_cast<int64_t>(DEPOSIT), wire_balance(OPERATOR));
   BOOST_REQUIRE(get_remitclaim(OPERATOR, kWireCodename).is_null());   // row erased by the payout

   // Double-claim finds nothing -- the erase happens before the transfer, closing the re-entrancy
   // window a notify handler would otherwise use to drain the row twice. Advance a block first:
   // an identical action replayed in the same block is rejected as a duplicate transaction before
   // the contract ever runs, which would assert nothing about the contract's own guard.
   produce_blocks();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: no claimable remit for this account"),
      claimremit(OPERATOR, kWireCodename));
} FC_LOG_AND_RETHROW() }

// ── depot-native shadow LIQ collateral (sysio.liq) ──

/// A shadow LIQ deposit custodies the shadow in `sysio.liq` and credits the depot-native
/// `(WIRE, LIQETH)` row: the chain is the depot, the token is the liq token's registry code.
BOOST_FIXTURE_TEST_CASE(deposit_shadow_liq_credits_wire_chain_row, sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;
   constexpr uint64_t DEPOSIT = 2 * kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);
   setup_shadow_liq();
   mint_shadow(OPERATOR, kShadowFunding);

   const int64_t operator_shadow_before = shadow_balance(OPERATOR);
   const int64_t opreg_shadow_before    = shadow_balance(OPREG_ACCOUNT);
   const int64_t operator_wire_before   = wire_balance(OPERATOR);
   const int64_t opreg_wire_before      = wire_balance(OPREG_ACCOUNT);

   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kLiqEthCodename, DEPOSIT));

   BOOST_REQUIRE_EQUAL(operator_shadow_before - static_cast<int64_t>(DEPOSIT), shadow_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(opreg_shadow_before + static_cast<int64_t>(DEPOSIT), shadow_balance(OPREG_ACCOUNT));
   BOOST_REQUIRE_EQUAL(operator_wire_before, wire_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(opreg_wire_before, wire_balance(OPREG_ACCOUNT));

   auto balances = get_operator(OPERATOR)["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1u, balances.size());
   BOOST_REQUIRE_EQUAL(cn(kWireCodename).value, balances[0]["chain_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(cn(kLiqEthCodename).value, balances[0]["token_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(DEPOSIT, balances[0]["balance"].as_uint64());

   // The recent-actions entry names the token actually deposited, not the WIRE constant.
   auto entry = latest_action_log(OPERATOR);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(true, entry[eligibility_field::success].as_bool());
   BOOST_REQUIRE_EQUAL(cn(kLiqEthCodename).value, entry["action"]["amount"]["token_code"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// A token with no `sysio.liq` shadow symbol that is not WIRE cannot be custodied: the
/// deposit reverts with the resolver's message and moves nothing.
BOOST_FIXTURE_TEST_CASE(deposit_unknown_token_is_rejected, sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_shadow_liq();
   mint_shadow(OPERATOR, kShadowFunding);

   const int64_t operator_shadow_before = shadow_balance(OPERATOR);
   BOOST_REQUIRE_EQUAL(error(std::string(kUnsupportedTokenError)),
                       deposit(OPERATOR, kUnknownTokenCodename, kWireUnit));

   BOOST_REQUIRE_EQUAL(0u, get_operator(OPERATOR)["balances"].get_array().size());
   BOOST_REQUIRE_EQUAL(operator_shadow_before, shadow_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(0, shadow_balance(OPREG_ACCOUNT));
} FC_LOG_AND_RETHROW() }

/// A withdraw of a token opreg cannot custody reverts with the resolver's message instead of
/// logging: it queues no request and leaves no `recent_actions` entry, so a request that could
/// never be paid out never exists.
BOOST_FIXTURE_TEST_CASE(withdraw_unknown_token_is_rejected, sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_shadow_liq();
   const auto actions_before = get_operator(OPERATOR)["recent_actions"].get_array().size();

   BOOST_REQUIRE_EQUAL(error(std::string(kUnsupportedTokenError)),
                       withdraw(OPERATOR, kUnknownTokenCodename, kWireUnit));

   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
   BOOST_REQUIRE_EQUAL(actions_before, get_operator(OPERATOR)["recent_actions"].get_array().size());
} FC_LOG_AND_RETHROW() }

/// A withdraw by an account with no operator row reverts, whatever the amount: it has nothing to
/// withdraw and no `recent_actions` row to log into, so succeeding would do nothing at no cost to
/// the caller.
BOOST_FIXTURE_TEST_CASE(withdraw_by_non_operator_is_rejected, sysio_opreg_tester) { try {
   const auto NON_OPERATOR = "uwrit.bob"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE(get_operator(NON_OPERATOR).is_null());

   BOOST_REQUIRE_EQUAL(error(std::string(kOperatorNotFoundError)),
                       withdraw(NON_OPERATOR, kWireCodename, kWireUnit));
   BOOST_REQUIRE_EQUAL(error(std::string(kOperatorNotFoundError)),
                       withdraw(NON_OPERATOR, kWireCodename, kZeroWithdrawAmount));

   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
   BOOST_REQUIRE(get_operator(NON_OPERATOR).is_null());
} FC_LOG_AND_RETHROW() }

/// A registered operator's zero-amount withdraw stays on the logged path: the action succeeds,
/// queues nothing, and records the failure in `recent_actions`.
BOOST_FIXTURE_TEST_CASE(withdraw_zero_amount_by_operator_logs_failure, sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));

   BOOST_REQUIRE_EQUAL(success(), withdraw(OPERATOR, kWireCodename, kZeroWithdrawAmount));

   auto entry = latest_action_log(OPERATOR);
   BOOST_REQUIRE(!entry.is_null());
   BOOST_REQUIRE_EQUAL(false, entry["success"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string(kZeroWithdrawLogError), entry["error_message"].as_string());
   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
} FC_LOG_AND_RETHROW() }

/// WIRE and shadow deposits by one operator land in two rows, and `available()` rolls each up
/// on its own: a pending shadow withdraw reserves shadow only.
BOOST_FIXTURE_TEST_CASE(deposit_keeps_wire_and_shadow_rows_separate, sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;
   constexpr uint64_t WIRE_DEPOSIT   = 3 * kWireUnit;
   constexpr uint64_t SHADOW_DEPOSIT = 2 * kWireUnit;
   constexpr uint64_t SHADOW_WITHDRAW = kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);
   setup_shadow_liq();
   mint_shadow(OPERATOR, kShadowFunding);

   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kWireCodename, WIRE_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kLiqEthCodename, SHADOW_DEPOSIT));

   auto balances = get_operator(OPERATOR)["balances"].get_array();
   BOOST_REQUIRE_EQUAL(2u, balances.size());
   BOOST_REQUIRE_EQUAL(WIRE_DEPOSIT, available(OPERATOR, kWireCodename, kWireCodename));
   BOOST_REQUIRE_EQUAL(SHADOW_DEPOSIT, available(OPERATOR, kWireCodename, kLiqEthCodename));

   BOOST_REQUIRE_EQUAL(success(), withdraw(OPERATOR, kLiqEthCodename, SHADOW_WITHDRAW));
   BOOST_REQUIRE_EQUAL(true, latest_action_log(OPERATOR)[eligibility_field::success].as_bool());
   BOOST_REQUIRE_EQUAL(WIRE_DEPOSIT, available(OPERATOR, kWireCodename, kWireCodename));
   BOOST_REQUIRE_EQUAL(SHADOW_DEPOSIT - SHADOW_WITHDRAW, available(OPERATOR, kWireCodename, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

/// A `(WIRE, LIQETH)` role minimum is met by the shadow row alone: a WIRE deposit of the same
/// amount leaves the batch operator ineligible, and the shadow deposit activates it.
///
/// The fixture registers through the privileged path, like every eligibility case here, so the
/// self-registration AuthX link check never runs; eligibility itself reads only balances.
BOOST_FIXTURE_TEST_CASE(shadow_row_satisfies_role_minimum, sysio_opreg_tester) { try {
   constexpr uint64_t MIN_BOND = kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*prune_delay=*/kDefaultPruneDelayMs,
      /*max_consec_misses=*/kDefaultMaxConsecutiveMisses,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h,
      /*min_pct_samples=*/kDefaultMinPctSamples,
      /*terminate_window_ms=*/kTerminateWindowMs,
      /*req_prod_collat=*/{},
      /*req_batchop_collat=*/{
         make_chain_min_bond(kWireCodename, kLiqEthCodename, MIN_BOND),
      },
      /*req_uw_collat=*/{}));
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(kEligibilityBatchOperator, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(kEligibilityBatchOperator, kWireFundingQuantity);
   setup_shadow_liq();
   mint_shadow(kEligibilityBatchOperator, kShadowFunding);

   BOOST_REQUIRE_EQUAL(success(), deposit(kEligibilityBatchOperator, kWireCodename, MIN_BOND));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(), deposit(kEligibilityBatchOperator, kLiqEthCodename, MIN_BOND));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());
} FC_LOG_AND_RETHROW() }

/// A shadow withdraw matures into a `(operator, LIQETH)` claim, and `claimremit` pays it back
/// through `sysio.liq`; the WIRE claim key stays empty.
BOOST_FIXTURE_TEST_CASE(withdraw_shadow_flush_credits_token_claim_and_claimremit_pays_liq,
                        sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;
   constexpr uint64_t DEPOSIT = 2 * kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_shadow_liq();
   mint_shadow(OPERATOR, kShadowFunding);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kLiqEthCodename, DEPOSIT));

   BOOST_REQUIRE_EQUAL(success(), withdraw(OPERATOR, kLiqEthCodename, DEPOSIT));
   auto request = get_wtdw(kFirstWithdrawalRequestId);
   BOOST_REQUIRE(!request.is_null());
   BOOST_REQUIRE_EQUAL(cn(kWireCodename).value, request["chain_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(cn(kLiqEthCodename).value, request["token_code"].as<fc::slug_name>().value);

   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());

   auto claim = get_remitclaim(OPERATOR, kLiqEthCodename);
   BOOST_REQUIRE(!claim.is_null());
   BOOST_REQUIRE_EQUAL(cn(kLiqEthCodename).value, claim["token_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(DEPOSIT, claim["balance"].as_uint64());
   BOOST_REQUIRE(get_remitclaim(OPERATOR, kWireCodename).is_null());

   constexpr unsigned InactiveYears = 3;
   produce_blocks();
   produce_block(fc::days(InactiveYears * 365));
   produce_blocks();
   const int64_t operator_shadow_before = shadow_balance(OPERATOR);
   const int64_t opreg_shadow_before    = shadow_balance(OPREG_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), claimremit(OPERATOR, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(operator_shadow_before + static_cast<int64_t>(DEPOSIT), shadow_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(opreg_shadow_before - static_cast<int64_t>(DEPOSIT), shadow_balance(OPREG_ACCOUNT));
   BOOST_REQUIRE(get_remitclaim(OPERATOR, kLiqEthCodename).is_null());

   BOOST_REQUIRE_EQUAL(error(std::string(kNoClaimableRemitError)), claimremit(OPERATOR, kWireCodename));
} FC_LOG_AND_RETHROW() }

/// WIRE and shadow claims outstanding at once are independent rows: each `claimremit` pays its
/// own token only and leaves the other claim whole.
BOOST_FIXTURE_TEST_CASE(remit_claims_for_two_tokens_coexist, sysio_opreg_tester) { try {
   const auto OPERATOR = "batchop.a"_n;
   constexpr uint64_t WIRE_DEPOSIT   = 3 * kWireUnit;
   constexpr uint64_t SHADOW_DEPOSIT = 2 * kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);
   setup_shadow_liq();
   mint_shadow(OPERATOR, kShadowFunding);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kWireCodename, WIRE_DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kLiqEthCodename, SHADOW_DEPOSIT));

   // Termination remits both depot-native rows into their own claim rows.
   BOOST_REQUIRE_EQUAL(success(), terminate(OPERATOR, std::string(kTestTerminationReason)));
   BOOST_REQUIRE_EQUAL(WIRE_DEPOSIT, get_remitclaim(OPERATOR, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(SHADOW_DEPOSIT, get_remitclaim(OPERATOR, kLiqEthCodename)["balance"].as_uint64());

   const int64_t wire_before   = wire_balance(OPERATOR);
   const int64_t shadow_before = shadow_balance(OPERATOR);
   BOOST_REQUIRE_EQUAL(success(), claimremit(OPERATOR, kWireCodename));
   BOOST_REQUIRE_EQUAL(wire_before + static_cast<int64_t>(WIRE_DEPOSIT), wire_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(shadow_before, shadow_balance(OPERATOR));
   BOOST_REQUIRE(get_remitclaim(OPERATOR, kWireCodename).is_null());
   BOOST_REQUIRE_EQUAL(SHADOW_DEPOSIT, get_remitclaim(OPERATOR, kLiqEthCodename)["balance"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), claimremit(OPERATOR, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(shadow_before + static_cast<int64_t>(SHADOW_DEPOSIT), shadow_balance(OPERATOR));
   BOOST_REQUIRE_EQUAL(wire_before + static_cast<int64_t>(WIRE_DEPOSIT), wire_balance(OPERATOR));
   BOOST_REQUIRE(get_remitclaim(OPERATOR, kLiqEthCodename).is_null());
} FC_LOG_AND_RETHROW() }

/// Slashing a `(WIRE, LIQETH)` row subtracts the balance and sends nothing to an outpost: the
/// shadow is depot-native, so it stays in `sysio.opreg`'s `sysio.liq` account. The control
/// operator's ETH-bucket slash does queue an attestation, so the trace probe is live.
BOOST_FIXTURE_TEST_CASE(slash_shadow_row_emits_no_attestation, sysio_opreg_tester) { try {
   const auto OPERATOR = "uwrit.alice"_n;
   constexpr uint64_t DEPOSIT = 2 * kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_shadow_liq();
   mint_shadow(OPERATOR, kShadowFunding);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kLiqEthCodename, DEPOSIT));

   // The row under test is the shadow row, custodied as shadow.
   const auto bonded = get_operator(OPERATOR)["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1u, bonded.size());
   BOOST_REQUIRE_EQUAL(cn(kLiqEthCodename).value, bonded[0]["token_code"].as<fc::slug_name>().value);
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(DEPOSIT), shadow_balance(OPREG_ACCOUNT));

   BOOST_REQUIRE(!slash_queues_outbound(OPERATOR, kTestSlashReason));

   auto op = get_operator(OPERATOR);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_SLASHED == op[eligibility_field::status].as<OperatorStatus>());
   auto balances = op["balances"].get_array();
   BOOST_REQUIRE_EQUAL(1u, balances.size());
   BOOST_REQUIRE_EQUAL(0u, balances[0]["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(DEPOSIT), shadow_balance(OPREG_ACCOUNT));

} FC_LOG_AND_RETHROW() }

/// `claimremit` resolves its token before touching any claim row, so a token opreg cannot
/// custody is refused with the resolver's message.
BOOST_FIXTURE_TEST_CASE(claimremit_unknown_token_is_rejected, sysio_opreg_tester) { try {
   setup_shadow_liq();

   BOOST_REQUIRE_EQUAL(error(std::string(kUnsupportedTokenError)),
                       claimremit("uwrit.alice"_n, kUnknownTokenCodename));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(terminate_rejects_already_slashed_operator, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, true));
   BOOST_REQUIRE_EQUAL(success(), slash("batchop.a"_n, "double sign"));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator not in a terminable state"),
      terminate("batchop.a"_n, "post-slash terminate attempt"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_evicts_all_miss_window_at_max_accepted_percent, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kDefaultMaxConsecutiveMisses, kMaxAcceptedPctMisses24h,
                           kPercentRailFromFirstDuty);

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));

   require_evicted("batchop.a"_n, "rolling-window: >99% miss rate");
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_evicts_after_default_consecutive_boundary, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/true));
   produce_blocks();

   for (uint32_t epoch = 2; epoch <= 6; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      produce_blocks();
   }
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 7, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));

   require_evicted("batchop.a"_n, "rolling-window: >5 consecutive misses");
} FC_LOG_AND_RETHROW() }

/// The percent rail waits for `terminate_min_pct_samples` duty epochs: a miss rate above the threshold
/// evicts only once the sample is reached.
BOOST_FIXTURE_TEST_CASE(termcheck_percent_rail_waits_for_min_samples, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kDefaultMaxConsecutiveMisses, kDefaultMaxPctMisses24h);
   constexpr uint32_t kEarlyMisses = 2;

   // An early miss alone, then a second: each is above the threshold but short of the sample.
   uint32_t epoch = 1;
   for (; epoch <= kEarlyMisses; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
      produce_block();
      BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
   }
   // Deliveries up to one short of the sample keep the rate above the threshold.
   for (; epoch < kDefaultMinPctSamples; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/true));
      BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
      produce_block();
   }
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
   BOOST_REQUIRE_GT(uint64_t{kEarlyMisses} * kPercentScale / (kDefaultMinPctSamples - 1), kDefaultMaxPctMisses24h);

   // The duty that reaches the sample applies the rail.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/true));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   require_evicted("batchop.a"_n, kDefaultMaxPctMissesReason);
} FC_LOG_AND_RETHROW() }

/// The sample grace holds back only the percent rail: a consecutive run evicts before it is reached.
BOOST_FIXTURE_TEST_CASE(termcheck_consecutive_rail_applies_during_the_percent_grace, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kDefaultMaxPctMisses24h);
   for (uint32_t epoch = 1; epoch <= kSingleAllowedConsecutiveMiss + 1; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
   }
   BOOST_REQUIRE_LT(kSingleAllowedConsecutiveMiss + 1, kDefaultMinPctSamples);
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   require_evicted("batchop.a"_n, kSingleAllowedConsecutiveMissReason);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_keeps_bootstrapped_operator_exempt, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*prune_delay=*/kDefaultPruneDelayMs,
      /*max_consec_misses=*/1,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h,
      /*min_pct_samples=*/kDefaultMinPctSamples,
      /*terminate_window_ms=*/kTerminateWindowMs));
   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/true));
   setup_served_chain();
   serve_evm_chain({"batchop.a"_n});
   produce_blocks(kBlocksPastOneSecond);

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));

   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(1, op["is_bootstrapped"].as_uint64());
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
} FC_LOG_AND_RETHROW() }

/// A miss is recorded while a queued withdrawal holds the operator UNKNOWN, joins the run of
/// misses from before the withdrawal, and termcheck evaluates the operator in that state: it is
/// evicted from the chain. Eviction leaves the bond alone, so the queued withdrawal flushes as usual.
BOOST_FIXTURE_TEST_CASE(termcheck_counts_misses_while_withdrawal_ineligible, sysio_opreg_tester) { try {
   activate_batch_operator(
      kEligibilityBatchOperator,
      /*max_consec_misses=*/kSingleAllowedConsecutiveMiss,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h);

   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPreWithdrawalDeliveredEpoch, /*delivered=*/true));
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPreWithdrawalMissedEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());

   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kFirstIneligibleDeliveryEpoch, /*delivered=*/false));
   auto ineligible_miss = get_dellog_entry(kThirdDeliveryLogId);
   BOOST_REQUIRE(!ineligible_miss.is_null());
   BOOST_REQUIRE_EQUAL(kFirstIneligibleDeliveryEpoch, ineligible_miss["epoch"].as_uint64());
   BOOST_REQUIRE_EQUAL(false, ineligible_miss["delivered"].as_bool());
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));
   require_evicted(kEligibilityBatchOperator, kSingleAllowedConsecutiveMissReason,
                   OperatorStatus::OPERATOR_STATUS_UNKNOWN);
   BOOST_REQUIRE(get_remitclaim(kEligibilityBatchOperator, "NTA").is_null());

   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
   BOOST_REQUIRE(get_wtdw(kFirstWithdrawalRequestId).is_null());
   BOOST_REQUIRE_EQUAL(kTestMinBond, get_remitclaim(kEligibilityBatchOperator, "NTA")["balance"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// The O1 cycle: queue a withdrawal below the minimum before each duty close and cancel it after.
/// Each skipped seat is a recorded miss, and the cancel's return to ACTIVE does not restart the
/// run, so the second skipped seat evicts the operator from the chain.
BOOST_FIXTURE_TEST_CASE(termcheck_evicts_withdraw_cancel_cycling, sysio_opreg_tester) { try {
   activate_batch_operator(
      kEligibilityBatchOperator,
      /*max_consec_misses=*/kSingleAllowedConsecutiveMiss,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h);

   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPreWithdrawalDeliveredEpoch, /*delivered=*/true));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kFirstIneligibleDeliveryEpoch, /*delivered=*/false));
   BOOST_REQUIRE(!get_dellog_entry(kSecondDeliveryLogId).is_null());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status(kEligibilityBatchOperator));

   // Cancel in a later block, so the reactivation stamp strictly follows the recorded miss.
   produce_block(fc::seconds(kDeliveryTimestampSeparationSeconds));
   BOOST_REQUIRE_EQUAL(success(), cancelwtdw(
      kEligibilityBatchOperator, kEligibilityBatchOperator, kFirstWithdrawalRequestId));
   auto reactivated = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 reactivated[eligibility_field::status].as<OperatorStatus>());
   auto skipped_miss_ts = get_dellog_entry(kSecondDeliveryLogId)["ts_ms"].as_uint64();
   auto reactivated_at  = reactivated["available_at"].as_uint64();
   BOOST_REQUIRE_MESSAGE(skipped_miss_ts < reactivated_at,
                         "skipped-seat miss timestamp " << skipped_miss_ts
                                                        << " must precede reactivation " << reactivated_at);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE(!get_wtdw(kSecondWithdrawalRequestId).is_null());
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kSecondIneligibleDeliveryEpoch, /*delivered=*/false));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));

   require_evicted(kEligibilityBatchOperator, kSingleAllowedConsecutiveMissReason,
                   OperatorStatus::OPERATOR_STATUS_UNKNOWN);
} FC_LOG_AND_RETHROW() }

/// A terminated account that registers again and rejoins the chain starts a new consecutive run, even
/// within the second of the block that terminated it: the previous registration's misses stay in the
/// log but do not count against it.
BOOST_FIXTURE_TEST_CASE(termcheck_starts_a_new_run_for_a_new_registration, sysio_opreg_tester) { try {
   activate_batch_operator(
      kEligibilityBatchOperator,
      /*max_consec_misses=*/kSingleAllowedConsecutiveMiss,
      /*max_pct_misses_24h=*/kMaxAcceptedPctMisses24h);

   // The terminating block opens a second, and the re-registration lands in the next block.
   align_next_block_to_second_start();
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPriorRegistrationDeliveredEpoch, /*delivered=*/true));
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPriorRegistrationFirstMissEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPriorRegistrationSecondMissEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));
   require_evicted(kEligibilityBatchOperator, kSingleAllowedConsecutiveMissReason);
   BOOST_REQUIRE_EQUAL(success(), terminate(kEligibilityBatchOperator, "registers again"));

   produce_block();
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(kEligibilityBatchOperator, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   BOOST_REQUIRE_EQUAL(get_dellog_entry(kThirdDeliveryLogId)["ts_ms"].as_uint64(),
                       get_operator(kEligibilityBatchOperator)["registered_at"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), bond_generic(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, kEligibilityBatchOperator, EPOCH_CHAIN));

   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kReRegistrationDeliveredEpoch, /*delivered=*/true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));

   const auto row = membership(kEligibilityBatchOperator);
   BOOST_REQUIRE_MESSAGE(std::string(served::status::serving) == row["status"].as_string(),
                         row["status_reason"].as_string());
} FC_LOG_AND_RETHROW() }

/// A terminated account that registers again and rejoins the chain starts a new miss-rate sample: the
/// previous registration's misses stay in the log but do not count against it.
BOOST_FIXTURE_TEST_CASE(termcheck_starts_a_new_sample_for_a_new_registration, sysio_opreg_tester) { try {
   activate_batch_operator(
      kEligibilityBatchOperator,
      /*max_consec_misses=*/kDefaultMaxConsecutiveMisses,
      /*max_pct_misses_24h=*/kDefaultMaxPctMisses24h,
      /*min_pct_samples=*/kPercentRailFromFirstDuty);

   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kPriorRegistrationFirstMissEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));
   require_evicted(kEligibilityBatchOperator, kDefaultMaxPctMissesReason);
   BOOST_REQUIRE_EQUAL(success(), terminate(kEligibilityBatchOperator, "registers again"));

   // Register again in a later second than the old row.
   produce_blocks();
   produce_block(fc::seconds(kDeliveryTimestampSeparationSeconds));
   BOOST_REQUIRE_EQUAL(success(),
      regoperator(kEligibilityBatchOperator, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
   BOOST_REQUIRE_EQUAL(success(), bond_generic(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(kEligibilityBatchOperator)[eligibility_field::status].as<OperatorStatus>());
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, kEligibilityBatchOperator, EPOCH_CHAIN));
   BOOST_REQUIRE(!get_dellog_entry(kFirstDeliveryLogId).is_null());

   produce_blocks(kBlocksPastOneSecond);
   BOOST_REQUIRE_EQUAL(success(),
      recorddel(kEligibilityBatchOperator, kReRegistrationDeliveredEpoch, /*delivered=*/true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));

   const auto row = membership(kEligibilityBatchOperator);
   BOOST_REQUIRE_MESSAGE(std::string(served::status::serving) == row["status"].as_string(),
                         row["status_reason"].as_string());
} FC_LOG_AND_RETHROW() }

/// Healthy observations from before a withdrawal and its cancellation remain in
/// the rolling percentage sample, so one miss among twenty rows stays at the
/// production five-percent ceiling rather than becoming a one-row, 100-percent
/// sample.
BOOST_FIXTURE_TEST_CASE(termcheck_preserves_percent_sample_across_withdrawal_ineligibility,
                        sysio_opreg_tester) { try {
   activate_batch_operator(
      kEligibilityBatchOperator,
      /*max_consec_misses=*/kDefaultMaxConsecutiveMisses,
      /*max_pct_misses_24h=*/kDefaultMaxPctMisses24h,
      /*min_pct_samples=*/kPercentRailFromFirstDuty);

   for (uint32_t epoch = kFirstPercentRailHistoryEpoch;
        epoch < kPercentRailPostReactivationMissEpoch;
        ++epoch) {
      BOOST_REQUIRE_EQUAL(success(),
         recorddel(kEligibilityBatchOperator, epoch, /*delivered=*/true));
   }
   produce_blocks();
   produce_block(fc::seconds(kDeliveryTimestampSeparationSeconds));

   BOOST_REQUIRE_EQUAL(success(),
      withdraw(kEligibilityBatchOperator, "NTA", kTestMinBond));
   BOOST_REQUIRE_EQUAL(success(), cancelwtdw(
      kEligibilityBatchOperator, kEligibilityBatchOperator, kFirstWithdrawalRequestId));

   auto reactivated = get_operator(kEligibilityBatchOperator);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 reactivated[eligibility_field::status].as<OperatorStatus>());
   auto healthy_history_end = get_dellog_entry(kLastPercentRailHistoryLogId)["ts_ms"].as_uint64();
   auto reactivated_at      = reactivated["available_at"].as_uint64();
   BOOST_REQUIRE(healthy_history_end < reactivated_at);

   BOOST_REQUIRE_EQUAL(success(), recorddel(
      kEligibilityBatchOperator, kPercentRailPostReactivationMissEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck(kEligibilityBatchOperator));

   const auto row = membership(kEligibilityBatchOperator);
   BOOST_REQUIRE_MESSAGE(std::string(served::status::serving) == row["status"].as_string(),
                         row["status_reason"].as_string());
} FC_LOG_AND_RETHROW() }

// ---- per-chain eviction ----

/// The rails count one chain's rows only: misses on one chain evict the operator from it while its
/// deliveries on the other keep it serving there, and its global status is untouched.
BOOST_FIXTURE_TEST_CASE(termcheck_evicts_from_the_missed_chain_only, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kMaxAcceptedPctMisses24h);
   setup_served_chain(kSecondEvmChain, ChainKind::CHAIN_KIND_EVM, kSecondEvmChainId);
   // The operator already holds the EVM link EPOCH_CHAIN needed.
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, kSecondEvmChain));
   produce_blocks(kBlocksPastOneSecond);

   for (uint32_t epoch = 1; epoch <= kSingleAllowedConsecutiveMiss + 1; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/true));
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false, kSecondEvmChain));
      produce_blocks();
   }
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n, kSecondEvmChain));
   produce_blocks();

   require_evicted("batchop.a"_n, kSingleAllowedConsecutiveMissReason, OperatorStatus::OPERATOR_STATUS_ACTIVE,
                   kSecondEvmChain);
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
   BOOST_REQUIRE(get_remitclaim("batchop.a"_n, "NTA").is_null());
   BOOST_REQUIRE_EQUAL(kTestMinBond, available("batchop.a"_n, kWireCodename, "NTA"));
} FC_LOG_AND_RETHROW() }

/// An EVICTED membership is not evaluated again: later misses on the chain leave its eviction, reason and
/// stamp as they were.
BOOST_FIXTURE_TEST_CASE(termcheck_leaves_an_evicted_member_alone, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kDefaultMaxConsecutiveMisses, kMaxAcceptedPctMisses24h);
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/true));
   for (uint32_t epoch = 2; epoch <= kDefaultMaxConsecutiveMisses + 2; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      produce_blocks();
   }
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   require_evicted("batchop.a"_n, "rolling-window: >5 consecutive misses");
   const uint64_t evicted_at = membership("batchop.a"_n)["updated_at_ms"].as_uint64();

   // A further miss a second later: a second eviction would restamp the row.
   produce_blocks(kBlocksPastOneSecond);
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kDefaultMaxConsecutiveMisses + 3, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();

   require_evicted("batchop.a"_n, "rolling-window: >5 consecutive misses");
   BOOST_REQUIRE_EQUAL(evicted_at, membership("batchop.a"_n)["updated_at_ms"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// An evicted operator rejoins once it holds no seat on the chain, and its rails restart at the rejoin:
/// the misses that evicted it stay in the log but no longer count.
BOOST_FIXTURE_TEST_CASE(termcheck_starts_a_new_run_for_a_new_membership, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kMaxAcceptedPctMisses24h);

   // The rows and the rejoin share one second: the rails start after it.
   align_next_block_to_second_start();
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kPriorRegistrationDeliveredEpoch, /*delivered=*/true));
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kPriorRegistrationFirstMissEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kPriorRegistrationSecondMissEpoch, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   require_evicted("batchop.a"_n, kSingleAllowedConsecutiveMissReason);
   produce_block();

   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.a"_n));
   produce_blocks();
   const auto rejoined = membership("batchop.a"_n);
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), rejoined["status"].as_string());
   BOOST_REQUIRE(rejoined["status_reason"].as_string().empty());
   BOOST_REQUIRE_EQUAL(get_dellog_entry(kThirdDeliveryLogId)["ts_ms"].as_uint64(),
                       rejoined["joined_at_ms"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kReRegistrationDeliveredEpoch, /*delivered=*/true));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
} FC_LOG_AND_RETHROW() }

/// `termcheck` evaluates a LEFT member's remaining seats like a SERVING one's, and evicts it.
BOOST_FIXTURE_TEST_CASE(termcheck_evicts_a_member_that_left, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kMaxAcceptedPctMisses24h);
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, "batchop.a"_n, EPOCH_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/true));
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 2, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 3, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   require_evicted("batchop.a"_n, kSingleAllowedConsecutiveMissReason);
} FC_LOG_AND_RETHROW() }

/// An operator with no membership of the chain is not evaluated there.
BOOST_FIXTURE_TEST_CASE(termcheck_skips_a_chain_the_operator_never_joined, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kMaxAcceptedPctMisses24h);
   for (uint32_t epoch = 1; epoch <= kSingleAllowedConsecutiveMiss + 1; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false, kSecondEvmChain));
   }
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n, kSecondEvmChain));
   BOOST_REQUIRE(membership("batchop.a"_n, kSecondEvmChain).is_null());
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == get_operator("batchop.a"_n)["status"].as<OperatorStatus>());
} FC_LOG_AND_RETHROW() }

/// The retention sweep runs across chains in age order: a write on one chain prunes another chain's
/// aged-out rows, and `termcheck` still reads each chain's own rows only.
BOOST_FIXTURE_TEST_CASE(recorddel_prunes_aged_rows_of_every_chain, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);
   constexpr uint32_t kAgedRows = 2;
   for (uint32_t epoch = 1; epoch <= kAgedRows; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false, kSecondEvmChain));
      produce_blocks();
   }
   for (uint64_t id = 1; id <= kAgedRows; ++id) {
      BOOST_REQUIRE_EQUAL(std::string(kSecondEvmChain), get_dellog_entry(id)["chain_code"].as_string());
   }
   produce_block(fc::hours(25));

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kAgedRows + 1, /*delivered=*/true));
   produce_blocks();
   for (uint64_t id = 1; id <= kAgedRows; ++id) BOOST_REQUIRE(get_dellog_entry(id).is_null());
   const auto row = get_dellog_entry(kAgedRows + 1);
   BOOST_REQUIRE_EQUAL(std::string(EPOCH_CHAIN), row["chain_code"].as_string());
   BOOST_REQUIRE_EQUAL(kAgedRows + 1, row["epoch"].as_uint64());
} FC_LOG_AND_RETHROW() }

// ---- dellog retention: bounded pruning of rows outside the rolling window ----

BOOST_FIXTURE_TEST_CASE(recorddel_prunes_rows_that_aged_out_of_window, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   for (uint32_t epoch = 1; epoch <= 3; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/true));
      produce_blocks();
   }
   for (uint64_t id = 1; id <= 3; ++id)
      BOOST_REQUIRE(!get_dellog_entry(id).is_null());

   // Push chain time past the 24h window so rows 1..3 age out.
   produce_block(fc::hours(25));

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 4, /*delivered=*/true));
   produce_blocks();

   for (uint64_t id = 1; id <= 3; ++id)
      BOOST_REQUIRE(get_dellog_entry(id).is_null());
   auto row = get_dellog_entry(4);
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL("batchop.a", row["account"].as_string());
   BOOST_REQUIRE_EQUAL(4, row["epoch"].as_uint64());
   BOOST_REQUIRE_EQUAL(true, row["delivered"].as_bool());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(recorddel_prune_is_bounded_per_write, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   for (uint32_t epoch = 1; epoch <= 6; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/true));
      produce_blocks();
   }
   produce_block(fc::hours(25));

   // First write past the window sweeps at most kDellogPrunePerWrite rows.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 7, /*delivered=*/true));
   produce_blocks();
   for (uint64_t id = 1; id <= kDellogPrunePerWrite; ++id)
      BOOST_REQUIRE(get_dellog_entry(id).is_null());
   BOOST_REQUIRE(!get_dellog_entry(5).is_null());
   BOOST_REQUIRE(!get_dellog_entry(6).is_null());
   BOOST_REQUIRE(!get_dellog_entry(7).is_null());

   // Second write clears the remaining two and stops at the in-window row.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 8, /*delivered=*/true));
   produce_blocks();
   BOOST_REQUIRE(get_dellog_entry(5).is_null());
   BOOST_REQUIRE(get_dellog_entry(6).is_null());
   BOOST_REQUIRE(!get_dellog_entry(7).is_null());
   BOOST_REQUIRE(!get_dellog_entry(8).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(prune_sweeps_expired_dellog_rows, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   // One more expired row than a single crank may remove.
   constexpr uint32_t SEEDED_ROWS = kDellogPrunePerCrank + 1;
   for (uint32_t epoch = 1; epoch <= SEEDED_ROWS; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      produce_blocks();
   }
   produce_block(fc::hours(25));

   // First crank removes exactly kDellogPrunePerCrank rows, oldest first.
   BOOST_REQUIRE_EQUAL(success(), prune());
   produce_blocks();
   for (uint64_t id = 1; id <= kDellogPrunePerCrank; ++id)
      BOOST_REQUIRE(get_dellog_entry(id).is_null());
   BOOST_REQUIRE(!get_dellog_entry(SEEDED_ROWS).is_null());

   // Second crank clears the remainder.
   BOOST_REQUIRE_EQUAL(success(), prune());
   produce_blocks();
   BOOST_REQUIRE(get_dellog_entry(SEEDED_ROWS).is_null());

   // The erased misses still form the consecutive run, so the check evicts.
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   require_evicted("batchop.a"_n, "rolling-window: >5 consecutive misses");
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(termcheck_unaffected_by_on_write_pruning, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);

   // Six misses that age out before they are ever evaluated.
   for (uint32_t epoch = 1; epoch <= 6; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      produce_blocks();
   }
   produce_block(fc::hours(25));

   // Six fresh misses; their writes also sweep the six expired rows.
   for (uint32_t epoch = 7; epoch <= 12; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
      produce_blocks();
   }
   for (uint64_t id = 1; id <= 6; ++id)
      BOOST_REQUIRE(get_dellog_entry(id).is_null());
   for (uint64_t id = 7; id <= 12; ++id)
      BOOST_REQUIRE(!get_dellog_entry(id).is_null());

   // The run counts all twelve misses, erased or not.
   BOOST_REQUIRE_EQUAL(uint64_t{12}, tally("batchop.a"_n)["miss_run"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   require_evicted("batchop.a"_n, "rolling-window: >5 consecutive misses");
} FC_LOG_AND_RETHROW() }

// ---- the duty tally termcheck reads ----

/// The tally holds what a walk of the log counts, and the rails evict at the same duty the walk's rails
/// would: a seeded mix of deliveries and misses, ended by the consecutive rail for one operator and by the
/// percent rail for another.
BOOST_FIXTURE_TEST_CASE(tally_matches_the_log_walk_over_mixed_duties, sysio_opreg_tester) { try {
   struct mixed_case {
      name        account;
      uint32_t    max_consecutive;
      uint32_t    max_pct;
      uint32_t    min_samples;
      uint32_t    miss_pct;   ///< chance of a miss per duty, in percent
      std::string reason;
   };
   constexpr uint32_t kMaxDuties = 200;
   const std::vector<mixed_case> cases = {
      {"batchop.a"_n, 3, kMaxAcceptedPctMisses24h, kMaxAcceptedMinPctSamples, 40,
       "rolling-window: >3 consecutive misses"},
      {"batchop.b"_n, kDefaultMaxConsecutiveMisses, 20, 10, 25, "rolling-window: >20% miss rate"},
   };
   std::minstd_rand duty_outcomes(20261124);
   for (const auto& c : cases) {
      activate_batch_operator(c.account, c.max_consecutive, c.max_pct, c.min_samples);
      bool evicted = false;
      for (uint32_t epoch = 1; epoch <= kMaxDuties && !evicted; ++epoch) {
         const bool delivered = duty_outcomes() % kPercentScale >= c.miss_pct;
         BOOST_REQUIRE_EQUAL(success(), recorddel(c.account, epoch, delivered));
         BOOST_REQUIRE_EQUAL(success(), termcheck(c.account));
         produce_block();

         const log_walk walk = walk_dellog(c.account, 0);
         require_tally(c.account, walk);
         evicted = walk.longest_run > c.max_consecutive ||
                   (walk.duties >= c.min_samples && walk.misses * kPercentScale / walk.duties > c.max_pct);
         if (evicted) require_evicted(c.account, c.reason);
         else BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status(c.account));
      }
      BOOST_REQUIRE_MESSAGE(evicted, c.account.to_string() << " was never evicted");
   }
} FC_LOG_AND_RETHROW() }

/// Both retention sweeps take what they erase out of the tally: the on-write sweep erases the oldest
/// aged rows of any member, and `termcheck` erases the rest of its own member's before reading it.
BOOST_FIXTURE_TEST_CASE(retention_sweeps_take_aged_duties_out_of_the_tally, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);
   uint32_t epoch = 1;
   for (const bool delivered : {false, false, false, true, true, true, false, false, false}) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch++, delivered));
   }
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 9, .misses = 6, .trailing_run = 3});
   produce_block(kPastTheWindow);

   // The write sweeps the four oldest (three misses and a delivery) before counting its own delivery.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch++, /*delivered=*/true));
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 6, .misses = 3, .trailing_run = 0});
   require_tally("batchop.a"_n, walk_dellog("batchop.a"_n, 0));

   // termcheck erases the five aged rows left, leaving the duty recorded in the window.
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 1, .misses = 0, .trailing_run = 0});
   require_tally("batchop.a"_n, walk_dellog("batchop.a"_n, 0));
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
} FC_LOG_AND_RETHROW() }

/// A run of misses that aged out of the window still counts: the crank takes its rows out of the tally's duties and
/// misses, but the consecutive run keeps them, so a later miss extends it.
BOOST_FIXTURE_TEST_CASE(aged_run_of_misses_stays_in_the_run, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kMaxAcceptedPctMisses24h);
   for (uint32_t epoch = 1; epoch <= kSingleAllowedConsecutiveMiss; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
   }
   produce_blocks();
   produce_block(kPastTheWindow);
   BOOST_REQUIRE_EQUAL(success(), prune());
   produce_blocks();
   require_tally("batchop.a"_n, {.trailing_run = kSingleAllowedConsecutiveMiss});
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kSingleAllowedConsecutiveMiss + 1, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 1, .misses = 1, .trailing_run = kSingleAllowedConsecutiveMiss + 1});
   require_evicted("batchop.a"_n, kSingleAllowedConsecutiveMissReason);
} FC_LOG_AND_RETHROW() }

/// While the member's aged rows outnumber what one check erases, the percent rail waits rather than rate
/// duties from outside the window; once they are gone, it applies again.
BOOST_FIXTURE_TEST_CASE(percent_rail_waits_while_its_own_aged_rows_remain, sysio_opreg_tester) { try {
   constexpr uint32_t         kMaxPercentMisses = 40;
   constexpr std::string_view kPercentReason    = "rolling-window: >40% miss rate";
   activate_batch_operator("batchop.a"_n, kDefaultMaxConsecutiveMisses, kMaxPercentMisses,
                           /*min_pct_samples=*/1);
   constexpr uint32_t kAgedRows = kDellogPrunePerWrite + kDellogPrunePerCheck + 1;
   uint32_t epoch = 1;
   for (; epoch <= kAgedRows; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/true));
   }
   produce_blocks();
   produce_block(kPastTheWindow);

   // The write sweeps four and the check sixteen; one aged delivery is left beside the new miss, a rate of
   // 50% the window does not hold.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch++, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 2, .misses = 1, .trailing_run = 1});
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));

   // The next write sweeps the last aged row; the window's duties are all misses.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch++, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 2, .misses = 2, .trailing_run = 2});
   require_evicted("batchop.a"_n, kPercentReason);
} FC_LOG_AND_RETHROW() }

/// The consecutive rail counts missed duty epochs whatever their age, so it never waits for the member's aged
/// rows: a run of aged misses the check cannot all erase evicts at the next miss.
BOOST_FIXTURE_TEST_CASE(consecutive_rail_does_not_wait_for_aged_rows, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n, kSingleAllowedConsecutiveMiss, kMaxAcceptedPctMisses24h);
   constexpr uint32_t kAgedRows = kDellogPrunePerWrite + kDellogPrunePerCheck + 1;
   uint32_t epoch = 1;
   for (; epoch <= kAgedRows; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
   }
   produce_blocks();
   produce_block(kPastTheWindow);

   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch++, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   BOOST_REQUIRE(!get_dellog_entry(kAgedRows).is_null());
   require_tally("batchop.a"_n, {.duties = 2, .misses = 2, .trailing_run = kAgedRows + 1});
   require_evicted("batchop.a"_n, kSingleAllowedConsecutiveMissReason);
} FC_LOG_AND_RETHROW() }

/// A rejoin starts a new tally at the next log id: the rows of the earlier membership are not counted
/// while they stay in the window, and erasing them later leaves the new tally as it is.
BOOST_FIXTURE_TEST_CASE(rejoin_starts_a_tally_earlier_rows_never_touch, sysio_opreg_tester) { try {
   activate_batch_operator("batchop.a"_n);
   constexpr uint32_t kEarlierRows = 3;
   for (uint32_t epoch = 1; epoch <= kEarlierRows; ++epoch) {
      BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, epoch, /*delivered=*/false));
   }
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, "batchop.a"_n, EPOCH_CHAIN));
   produce_blocks();
   produce_block(fc::hours(23));

   BOOST_REQUIRE_EQUAL(success(), served::join(*this, "batchop.a"_n, EPOCH_CHAIN, "batchop.a"_n));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(kEarlierRows + 1, tally("batchop.a"_n)["first_log_id"].as_uint64());
   require_tally("batchop.a"_n, {});
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kEarlierRows + 1, /*delivered=*/true));
   produce_blocks();
   require_tally("batchop.a"_n, {.duties = 1});

   // Two hours on, the earlier rows have aged out: the write sweeps them without touching the tally.
   produce_block(fc::hours(2));
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, kEarlierRows + 2, /*delivered=*/false));
   BOOST_REQUIRE_EQUAL(success(), termcheck("batchop.a"_n));
   produce_blocks();
   for (uint64_t log_id = kFirstDeliveryLogId; log_id <= kEarlierRows; ++log_id) {
      BOOST_REQUIRE(get_dellog_entry(log_id).is_null());
   }
   require_tally("batchop.a"_n, {.duties = 2, .misses = 1, .trailing_run = 1});
   require_tally("batchop.a"_n, walk_dellog("batchop.a"_n, kEarlierRows + 1));
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status("batchop.a"_n));
} FC_LOG_AND_RETHROW() }

/// `termcheck` decides from the member's tally alone, one row whatever the log holds: among members with
/// long, interleaved logs of deliveries, the one whose tally is rewritten to a breaching run is evicted,
/// though its log shows no miss.
BOOST_FIXTURE_TEST_CASE(termcheck_reads_one_tally_however_long_the_log, sysio_opreg_tester) { try {
   const std::vector<name> members = {"batchop.a"_n, "batchop.b"_n, "batchop.c"_n};
   for (const name member : members) activate_batch_operator(member);
   constexpr uint32_t kDutiesEach = 60;
   for (uint32_t epoch = 1; epoch <= kDutiesEach; ++epoch) {
      for (const name member : members) {
         BOOST_REQUIRE_EQUAL(success(), recorddel(member, epoch, /*delivered=*/true));
      }
      produce_block();
   }
   for (const name member : members) {
      require_tally(member, {.duties = kDutiesEach});
      require_tally(member, walk_dellog(member, 0));
   }

   rewrite_tally(members.front(), fc::variant(mvo()
      ("first_log_id", tally(members.front())["first_log_id"])
      ("duties",       kDutiesEach)
      ("misses",       kDefaultMaxConsecutiveMisses + 1)
      ("miss_run",     kDefaultMaxConsecutiveMisses + 1)));
   for (const name member : members) BOOST_REQUIRE_EQUAL(success(), termcheck(member));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(0u, walk_dellog(members.front(), 0).misses);
   require_evicted(members.front(), "rolling-window: >5 consecutive misses");
   for (size_t i = 1; i < members.size(); ++i) {
      BOOST_REQUIRE_EQUAL(std::string(served::status::serving), membership_status(members[i]));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(recorddel_succeeds_without_opconfig, sysio_opreg_tester) { try {
   // No setconfig installed: the on-write sweep falls back to default
   // thresholds via get_or_default instead of asserting.
   BOOST_REQUIRE_EQUAL(success(), recorddel("batchop.a"_n, 1, /*delivered=*/true));
   produce_blocks();
   BOOST_REQUIRE(!get_dellog_entry(1).is_null());
} FC_LOG_AND_RETHROW() }

// ── setconfig: per-(chain_code, token_code) collateral requirements ──

BOOST_FIXTURE_TEST_CASE(setconfig_two_native_asset_bond_activation, sysio_opreg_tester) { try {
   constexpr uint64_t MIN_BOND = 1'000'000;

   BOOST_REQUIRE_EQUAL(success(), setconfig(
      /*prune_delay=*/600000,
      /*max_consec_misses=*/5, /*max_pct_misses_24h=*/5,
      /*min_pct_samples=*/kDefaultMinPctSamples,
      /*terminate_window_ms=*/24ULL * 60 * 60 * 1000,
      /*req_prod_collat=*/{},
      /*req_batchop_collat=*/{
         make_chain_min_bond("WIRE", "NTA", MIN_BOND),
         make_chain_min_bond("WIRE", "NTB", MIN_BOND),
      },
      /*req_uw_collat=*/{}));

   BOOST_REQUIRE_EQUAL(success(),
      regoperator("batchop.a"_n, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));

   // Pre-deposit: no balances → eligibility predicate fails → status UNKNOWN.
   auto op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op["status"].as<OperatorStatus>());

   // After ETH bond: SOL still missing → still UNKNOWN.
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("batchop.a"_n, "NTA", MIN_BOND));
   op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_UNKNOWN == op["status"].as<OperatorStatus>());

   // After SOL bond: every requirement met → ACTIVE.
   BOOST_REQUIRE_EQUAL(success(),
      bond_generic("batchop.a"_n, "NTB", MIN_BOND));
   op = get_operator("batchop.a"_n);
   BOOST_REQUIRE(OperatorStatus::OPERATOR_STATUS_ACTIVE == op["status"].as<OperatorStatus>());
   BOOST_REQUIRE(op["available_at"].as_uint64() > 0);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_duplicate_chain_token_in_collat, sysio_opreg_tester) { try {
   const auto duplicate_vec = std::vector<fc::variant>{
      make_chain_min_bond("WIRE", "NTA", 100),
      make_chain_min_bond("WIRE", "NTA", 200),
   };

   // The duplicate-detection assertion text refers to "(chain, token_kind)"
   // historically; the contract message may be updated to "(chain_code,
   // token_code)" — match on the stable prefix to tolerate either spelling.
   auto r = setconfig(600000, 5, 5, kDefaultMinPctSamples, 24ULL * 60 * 60 * 1000,
                /*req_prod_collat=*/{},
                /*req_batchop_collat=*/duplicate_vec,
                /*req_uw_collat=*/{});
   BOOST_REQUIRE(r.find("duplicate") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_stamps_collat_config_timestamp, sysio_opreg_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(
      600000, 5, 5, kDefaultMinPctSamples, 24ULL * 60 * 60 * 1000,
      /*req_prod_collat=*/{},
      /*req_batchop_collat=*/{
         make_chain_min_bond("WIRE", "NTA", 1000),
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
   BOOST_REQUIRE_EQUAL(success(), bond_generic("batchop.a"_n, "NTA", 500));

   // Queue a withdraw of the full balance, then terminate (remits + zeroes the balance), leaving
   // the queued row matured against a now-zero balance.
   BOOST_REQUIRE_EQUAL(success(), withdraw("batchop.a"_n, "NTA", 500));
   BOOST_REQUIRE(!get_wtdw(1).is_null());
   BOOST_REQUIRE_EQUAL(success(), terminate("batchop.a"_n, "rolling-24h miss"));

   // Flush at an epoch well past the withdraw's eligibility. Pre-fix this aborted with a
   // "balance underflow"; the TERMINATED branch erases the matured row instead.
   BOOST_REQUIRE_EQUAL(success(),
      push_opreg_action(EPOCH_ACCOUNT, "flushwtdw"_n, mvo()("economic_epoch", 1000000u)));
   BOOST_REQUIRE(get_wtdw(1).is_null());   // matured row erased, not stuck re-throwing every advance
} FC_LOG_AND_RETHROW() }

// SEC-78 / WSA-166: flushwtdw drains at most MAX_WTDW_FLUSH_PER_EPOCH matured rows per advance.
// WIRE-376 limits each operator to one queued row, so this regression uses distinct operators to
// retain coverage of the global work bound. Every operator is TERMINATED so each matured row takes
// the erase-without-remit branch (no outpost/token infra needed); the bound lives above the per-row
// branches and therefore holds regardless of which branch a row takes.
BOOST_FIXTURE_TEST_CASE(flushwtdw_bounds_rows_per_epoch, sysio_opreg_tester) { try {
   // Mirror of the contract-internal cap (contract headers are not host-compilable, same convention
   // as the msgch size-cap tests). Keep in sync with sysio.opreg.hpp::MAX_WTDW_FLUSH_PER_EPOCH.
   constexpr uint32_t MAX_WTDW_FLUSH_PER_EPOCH = 32;
   constexpr uint32_t N = MAX_WTDW_FLUSH_PER_EPOCH + 8;   // 40 > one epoch's flush budget

   constexpr auto OPERATOR_NAME_BASE = "batchop.a"_n;

   BOOST_REQUIRE_EQUAL(success(), setconfig());

   // Each distinct operator contributes its one permitted queue row. Raw name values remain valid
   // Antelope names and avoid coupling this global-bound regression to a fixed account-name list.
   for (uint32_t i = 0; i < N; ++i) {
      const name account{OPERATOR_NAME_BASE.value + ((uint64_t(i) + 1) << 4)};
      create_accounts({account});
      produce_blocks();
      BOOST_REQUIRE_EQUAL(success(),
         regoperator(account, OPERATOR_TYPE_BATCH, /*is_bootstrapped=*/false));
      BOOST_REQUIRE_EQUAL(success(), bond_generic(account, "NTA", i + 1));
      BOOST_REQUIRE_EQUAL(success(),
         withdraw(account, "NTA", i + 1));
      BOOST_REQUIRE_EQUAL(success(), terminate(account, "rolling-24h miss"));
      produce_blocks();
   }

   // Count remaining queue rows by probing the monotonic ids 1..N (order-independent).
   auto count_pending = [&]() {
      uint32_t n = 0;
      for (uint64_t id = 1; id <= N; ++id) if (!get_wtdw(id).is_null()) ++n;
      return n;
   };
   BOOST_REQUIRE_EQUAL(N, count_pending());

   // First flush drains exactly MAX_WTDW_FLUSH_PER_EPOCH rows; the remainder stays queued.
   BOOST_REQUIRE_EQUAL(success(),
      push_opreg_action(EPOCH_ACCOUNT, "flushwtdw"_n, mvo()("economic_epoch", 1'000'000u)));
   BOOST_REQUIRE_EQUAL(N - MAX_WTDW_FLUSH_PER_EPOCH, count_pending());

   // Cross a block boundary so the second flush is a distinct transaction — an identical action in
   // the same block is rejected as a duplicate before the contract runs, masking the guard under test.
   produce_blocks();
   // Second flush drains the rest -- progress resumes where the first stopped.
   BOOST_REQUIRE_EQUAL(success(),
      push_opreg_action(EPOCH_ACCOUNT, "flushwtdw"_n, mvo()("economic_epoch", 1'000'000u)));
   BOOST_REQUIRE_EQUAL(0u, count_pending());
} FC_LOG_AND_RETHROW() }

// ── shadow-bond yield forwarding ──
//
// A bonded row's checkpoint tracks sysio.liq's own LIQETH index: a bonded unit earns exactly what a
// unit on the registry's liq holder row earns, from the moment it is bonded until it leaves the row.

/// A sweep claims exactly what `sysio.liq` owes the registry's holder row into the registry, and
/// leaves the registry owed nothing.
BOOST_FIXTURE_TEST_CASE(sweepyield_claims_the_registry_yield, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const int64_t swept = sweep_liqeth();
   BOOST_REQUIRE_LT(0, swept);
   BOOST_REQUIRE_LT(swept, kYieldDistribution);   // the bystander's share stayed in sysio.liq
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA + kYieldBondB, liq_index(), 0), swept);
   BOOST_REQUIRE_EQUAL(0, registry_liq_owed());
} FC_LOG_AND_RETHROW() }

/// Each bonder is credited its own units' share of the distribution, even with no sweep first:
/// `claimyield` pulls the registry's owed WIRE itself, so `claimremit(WIRE)` can pay at once. A
/// second claim with nothing new earned is refused.
BOOST_FIXTURE_TEST_CASE(claimyield_credits_pro_rata_and_claimremit_pays_wire, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t index = liq_index();
   const int64_t registry_owed = registry_liq_owed();
   const int64_t opreg_before  = wire_balance(OPREG_ACCOUNT);

   const int64_t a_credit = claim_yield_credit(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA, index, 0), a_credit);
   BOOST_REQUIRE_EQUAL(opreg_before + registry_owed, wire_balance(OPREG_ACCOUNT));   // pulled in by the claim
   BOOST_REQUIRE_EQUAL(0, registry_liq_owed());

   const int64_t b_credit = claim_yield_credit(kYieldBonderB);
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondB, index, 0), b_credit);
   BOOST_REQUIRE_LE(a_credit + b_credit, registry_owed);   // attribution never exceeds the registry's claim

   const auto a_row = depot_native_row(kYieldBonderA, kLiqEthCodename);
   BOOST_REQUIRE_EQUAL(0u, a_row["shadow_yield"]["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(index, a_row["shadow_yield"]["index_checkpoint"].as_uint128());

   const int64_t a_wire_before = wire_balance(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(a_wire_before + a_credit, wire_balance(kYieldBonderA));
   BOOST_REQUIRE(get_remitclaim(kYieldBonderA, kWireCodename).is_null());

   produce_blocks();
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderA, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

/// A balance change settles the row first: A's claim is its first-distribution share at its old
/// balance plus its second-distribution share at the new one, and B is paid on one balance throughout.
BOOST_FIXTURE_TEST_CASE(yield_settles_before_a_balance_change, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t first_index = liq_index();

   mint_shadow(kYieldBonderA, kYieldBondA);
   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   const auto a_row = depot_native_row(kYieldBonderA, kLiqEthCodename);
   BOOST_REQUIRE_EQUAL(first_index, a_row["shadow_yield"]["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(yield_reference::owed(kYieldBondA, first_index, 0)),
                       a_row["shadow_yield"]["owed_wire"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t second_index = liq_index();

   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA, first_index, 0) +
                          yield_reference::owed(2 * kYieldBondA, second_index, first_index),
                       claim_yield_credit(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondB, second_index, 0), claim_yield_credit(kYieldBonderB));
} FC_LOG_AND_RETHROW() }

/// A sweep with nothing owed is refused -- before any distribution, and again right after a sweep
/// has drained the registry's row. WIRE, which earns no shadow yield, is refused too.
BOOST_FIXTURE_TEST_CASE(sweepyield_with_nothing_owed_is_rejected, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldToSweepError)), sweepyield(kLiqEthCodename));

   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   sweep_liqeth();
   produce_blocks();
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldToSweepError)), sweepyield(kLiqEthCodename));

   BOOST_REQUIRE_EQUAL(error(std::string(kWireEarnsNoYieldError)), sweepyield(kWireCodename));
   BOOST_REQUIRE_EQUAL(error(std::string(kUnsupportedTokenError)), sweepyield(kUnknownTokenCodename));
} FC_LOG_AND_RETHROW() }

/// Termination pays the yield a bond earned alongside its principal: WIRE yield in the WIRE claim
/// row, the shadow in the LIQETH claim row. Termination cannot pull from sysio.liq, so the yield is
/// swept first and the pool covers it in full.
BOOST_FIXTURE_TEST_CASE(terminate_credits_earned_yield_with_the_principal, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const int64_t a_yield = yield_reference::owed(kYieldBondA, liq_index(), 0);
   sweep_liqeth();

   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(a_yield),
                       get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(kYieldBondA, get_remitclaim(kYieldBonderA, kLiqEthCodename)["balance"].as_uint64());
   const auto a_row = depot_native_row(kYieldBonderA, kLiqEthCodename);
   BOOST_REQUIRE_EQUAL(0u, a_row["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, a_row["shadow_yield"]["owed_wire"].as_uint64());

   const int64_t wire_before   = wire_balance(kYieldBonderA);
   const int64_t shadow_before = shadow_balance(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(wire_before + a_yield, wire_balance(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(shadow_before + static_cast<int64_t>(kYieldBondA), shadow_balance(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderA, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

/// A slash settles before it seizes: the yield earned up to the slash stays claimable, and the
/// seized principal earns the operator nothing after it.
BOOST_FIXTURE_TEST_CASE(slash_keeps_yield_earned_before_the_slash, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t first_index = liq_index();

   BOOST_REQUIRE_EQUAL(success(), slash(kYieldBonderA, std::string(kTestSlashReason)));
   BOOST_REQUIRE_EQUAL(0u, depot_native_row(kYieldBonderA, kLiqEthCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA, first_index, 0), claim_yield_credit(kYieldBonderA));

   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondB, liq_index(), 0), claim_yield_credit(kYieldBonderB));
} FC_LOG_AND_RETHROW() }

/// With every bond slashed the registry still holds the seized shadow and still earns on it: the
/// sweep claims that WIRE into the registry, and no operator is owed any of it.
BOOST_FIXTURE_TEST_CASE(sweepyield_with_no_bonded_balance_keeps_wire_in_registry, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), slash(kYieldBonderA, std::string(kTestSlashReason)));
   BOOST_REQUIRE_EQUAL(success(), slash(kYieldBonderB, std::string(kTestSlashReason)));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(kYieldBondA + kYieldBondB), shadow_balance(OPREG_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   BOOST_REQUIRE_LT(0, sweep_liqeth());
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderB, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

/// WIRE collateral earns no shadow yield: depositing and withdrawing it leaves the row's yield
/// fields at zero, and there is nothing to claim on it.
BOOST_FIXTURE_TEST_CASE(wire_rows_never_earn_shadow_yield, sysio_opreg_tester) { try {
   const auto OPERATOR = kYieldBonderA;
   constexpr uint64_t DEPOSIT = 3 * kWireUnit;

   BOOST_REQUIRE_EQUAL(success(), setconfig());
   BOOST_REQUIRE_EQUAL(success(), regoperator(OPERATOR, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   setup_wire_token_and_fund(OPERATOR, kWireFundingQuantity);
   BOOST_REQUIRE_EQUAL(success(), deposit(OPERATOR, kWireCodename, DEPOSIT));
   BOOST_REQUIRE_EQUAL(success(), withdraw(OPERATOR, kWireCodename, kWireUnit));
   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));

   const auto row = depot_native_row(OPERATOR, kWireCodename);
   BOOST_REQUIRE_EQUAL(DEPOSIT - kWireUnit, row["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{0}, row["shadow_yield"]["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(0u, row["shadow_yield"]["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(OPERATOR, kWireCodename));
} FC_LOG_AND_RETHROW() }

/// A bond made after a distribution starts at the index that distribution left: it cannot capture
/// yield earned before it existed, however the sweep is timed. A, bonded throughout, keeps its
/// whole share -- which is everything the registry earned.
BOOST_FIXTURE_TEST_CASE(late_depositor_cannot_capture_earlier_yield, sysio_opreg_tester) { try {
   setup_yield_holders();
   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t index = liq_index();

   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderB, kLiqEthCodename, kYieldBondB));
   BOOST_REQUIRE_EQUAL(index,
      depot_native_row(kYieldBonderB, kLiqEthCodename)["shadow_yield"]["index_checkpoint"].as_uint128());
   const int64_t swept = sweep_liqeth();

   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderB, kLiqEthCodename));
   const int64_t a_credit = claim_yield_credit(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA, index, 0), a_credit);
   BOOST_REQUIRE_EQUAL(swept, a_credit);
} FC_LOG_AND_RETHROW() }

/// Units withdrawn before any sweep keep the yield they earned while bonded: the flush settles the
/// row before debiting it, so the timing of the sweep changes nothing.
BOOST_FIXTURE_TEST_CASE(withdrawn_units_keep_yield_earned_while_bonded, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t index = liq_index();

   BOOST_REQUIRE_EQUAL(success(), withdraw(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
   BOOST_REQUIRE_EQUAL(0u, depot_native_row(kYieldBonderA, kLiqEthCodename)["balance"].as_uint64());
   sweep_liqeth();

   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA, index, 0), claim_yield_credit(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondB, index, 0), claim_yield_credit(kYieldBonderB));
} FC_LOG_AND_RETHROW() }

/// Shadow the registry holds for no bond -- here A's seized shadow -- earns for the registry, not
/// for the remaining bonders: B is credited its own units' share only, and the rest of the sweep
/// stays in the registry.
BOOST_FIXTURE_TEST_CASE(registry_keeps_yield_on_unbonded_holdings, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), slash(kYieldBonderA, std::string(kTestSlashReason)));
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t index = liq_index();

   const int64_t swept    = sweep_liqeth();
   const int64_t b_credit = claim_yield_credit(kYieldBonderB);
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondB, index, 0), b_credit);

   // What stays in the registry is the seized units' share, up to one unit of per-row flooring.
   const int64_t seized_share = yield_reference::owed(kYieldBondA, index, 0);
   BOOST_REQUIRE_EQUAL(swept, wire_balance(OPREG_ACCOUNT));
   BOOST_REQUIRE_GE(wire_balance(OPREG_ACCOUNT) - b_credit, seized_share);
   BOOST_REQUIRE_LE(wire_balance(OPREG_ACCOUNT) - b_credit, seized_share + 1);
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderA, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

/// Strict solvency: an operator's yield is credited only out of WIRE the registry actually received
/// from sysio.liq. With A the only bonder, the registry row floors at each of its settles while A's
/// row floors once, so A is owed a little more than was received: the claim credits exactly what
/// was received, the dust stays on A's row, and paying the claim leaves the WIRE collateral whole.
BOOST_FIXTURE_TEST_CASE(yield_credit_never_exceeds_wire_received_from_liq, sysio_opreg_tester) { try {
   const int64_t received = setup_yield_dust();
   const int64_t a_owed   = yield_reference::owed(kYieldBondA, liq_index(), 0);
   const int64_t dust     = a_owed - received;
   BOOST_REQUIRE_LT(0, dust);   // the case exists only if flooring left A owed more than received

   BOOST_REQUIRE_EQUAL(received, claim_yield_credit(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(dust),
                       depot_native_row(kYieldBonderA, kLiqEthCodename)["shadow_yield"]["owed_wire"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(kYieldPrincipal), wire_balance(OPREG_ACCOUNT));
   produce_blocks();   // a repeat of the same claim in the same block is a duplicate transaction
   BOOST_REQUIRE_EQUAL(error(std::string(kYieldNotCoveredError)), claimyield(kYieldBonderA, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

/// Termination can pay only what the pool covers, since it cannot pull from sysio.liq. The rest --
/// here a whole unswept distribution plus flooring dust -- stays banked on the terminated row, and
/// the operator collects it with `claimyield`, which pulls first. Pruning archives the remaining
/// debt indefinitely without retaining the operator registration or spending WIRE collateral.
BOOST_FIXTURE_TEST_CASE(terminated_operator_yield_survives_prune, sysio_opreg_tester) { try {
   const int64_t received = setup_yield_dust();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));   // not swept before termination
   const int64_t a_owed = yield_reference::owed(kYieldBondA, liq_index(), 0);

   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(received),
                       get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());
   const auto terminated_row = depot_native_row(kYieldBonderA, kLiqEthCodename);
   BOOST_REQUIRE_EQUAL(0u, terminated_row["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(a_owed - received),
                       terminated_row["shadow_yield"]["owed_wire"].as_uint64());

   // The claim pulls the unswept distribution and credits it; flooring dust stays banked.
   const int64_t registry_owed = registry_liq_owed();
   BOOST_REQUIRE_EQUAL(registry_owed, claim_yield_credit(kYieldBonderA));
   const int64_t dust = a_owed - received - registry_owed;
   BOOST_REQUIRE_LT(0, dust);
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(dust),
                       depot_native_row(kYieldBonderA, kLiqEthCodename)["shadow_yield"]["owed_wire"].as_uint64());

   // The dust does not hold the operator row: pruning archives the exact amount owed.
   // Commit the claim first: a transaction left pending across the time jump would expire.
   produce_blocks();
   produce_block(fc::milliseconds(kDefaultPruneDelayMs));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), prune());
   BOOST_REQUIRE(get_operator(kYieldBonderA).is_null());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{static_cast<uint64_t>(dust)}, yield_debt(kYieldBonderA));

   produce_blocks();
   produce_block(fc::days(3 * 365));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(error(std::string(kYieldNotCoveredError)), claimyield(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(fc::uint128_t{static_cast<uint64_t>(dust)}, yield_debt(kYieldBonderA));

   // Returned shadow still held by the registry earns slack; it can cover the debt without an
   // operator record. A keeper can collect it only for the original account.
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   BOOST_REQUIRE_EQUAL(success(), push_opreg_action(kYieldKeeper, yield_action::claimyield, mvo()
      (withdrawal_field::account, kYieldBonderA)(withdrawal_field::token_code, kLiqEthCodename)));
   BOOST_REQUIRE_EQUAL(fc::uint128_t{0}, yield_debt(kYieldBonderA));
   BOOST_REQUIRE(get_remitclaim(kYieldKeeper, kWireCodename).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(a_owed),
                       get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_GE(wire_balance(OPREG_ACCOUNT), static_cast<int64_t>(kYieldPrincipal));
} FC_LOG_AND_RETHROW() }

/// `claimyield` is permissionless: a keeper with no stake cranks it for a TERMINATED operator whose
/// yield termination could not cover, and the credit lands in the operator's own WIRE claim.
BOOST_FIXTURE_TEST_CASE(keeper_claims_terminated_operators_yield, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));   // not swept before termination
   const int64_t a_owed = yield_reference::owed(kYieldBondA, liq_index(), 0);

   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   BOOST_REQUIRE(get_remitclaim(kYieldBonderA, kWireCodename).is_null());   // nothing was covered yet
   BOOST_REQUIRE(get_remitclaim(kYieldKeeper, kWireCodename).is_null());

   BOOST_REQUIRE_EQUAL(success(), push_opreg_action(kYieldKeeper, yield_action::claimyield, mvo()
      (withdrawal_field::account, kYieldBonderA)(withdrawal_field::token_code, kLiqEthCodename)));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(a_owed),
                       get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE(get_remitclaim(kYieldKeeper, kWireCodename).is_null());   // the keeper gains nothing

   const int64_t wire_before = wire_balance(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(wire_before + a_owed, wire_balance(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// A row that withdrew everything earns nothing while empty: re-bonding later settles the gap at a
/// balance of 0, so the claim is the first bond's share plus the re-bond's share, and nothing for
/// the distribution in between.
BOOST_FIXTURE_TEST_CASE(rebond_after_full_withdrawal_earns_nothing_for_the_gap, sysio_opreg_tester) { try {
   setup_yield_bonders();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const fc::uint128_t first_index = liq_index();

   BOOST_REQUIRE_EQUAL(success(), withdraw(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));   // the gap: A holds nothing bonded
   const fc::uint128_t gap_index = liq_index();

   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   const auto rebonded = depot_native_row(kYieldBonderA, kLiqEthCodename);
   BOOST_REQUIRE_EQUAL(gap_index, rebonded["shadow_yield"]["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(yield_reference::owed(kYieldBondA, first_index, 0)),
                       rebonded["shadow_yield"]["owed_wire"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   BOOST_REQUIRE_EQUAL(yield_reference::owed(kYieldBondA, first_index, 0) +
                          yield_reference::owed(kYieldBondA, liq_index(), gap_index),
                       claim_yield_credit(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// Re-registration replaces only operator state: already-earned debt and backed remits remain
/// payable, even though the new registration has no collateral or yield position.
BOOST_FIXTURE_TEST_CASE(reregistration_preserves_banked_yield_and_remit_claims, sysio_opreg_tester) { try {
   // A is the only bonder, so the first sweep's pool covers exactly A's first share and the unswept
   // second distribution stays banked on A's row at termination.
   setup_yield_holders();
   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const int64_t first_swept = sweep_liqeth();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));   // not swept: stays banked on A's row

   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   const uint64_t covered = get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64();
   BOOST_REQUIRE_LT(0u, covered);
   BOOST_REQUIRE_LE(covered, static_cast<uint64_t>(first_swept));
   const uint64_t owed = depot_native_row(kYieldBonderA, kLiqEthCodename)["shadow_yield"]["owed_wire"].as_uint64();
   BOOST_REQUIRE_LT(0u, owed);

   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, /*is_bootstrapped=*/false));
   BOOST_REQUIRE(depot_native_row(kYieldBonderA, kLiqEthCodename).is_null());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{owed}, yield_debt(kYieldBonderA));

   BOOST_REQUIRE_EQUAL(covered, get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(kYieldBondA, get_remitclaim(kYieldBonderA, kLiqEthCodename)["balance"].as_uint64());
   const int64_t wire_before = wire_balance(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(wire_before + static_cast<int64_t>(covered), wire_balance(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(success(), claimyield(kYieldBonderA, kLiqEthCodename));
   const uint64_t paid = get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64();
   BOOST_REQUIRE_EQUAL(fc::uint128_t{owed}, fc::uint128_t{paid} + yield_debt(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// Two retirement cycles accumulate debt under the same token; a fresh position earns only
/// its own interval, and a single claim conserves both archived debt and current yield.
BOOST_FIXTURE_TEST_CASE(repeated_reregistration_preserves_debt_and_live_yield, sysio_opreg_tester) { try {
   setup_yield_holders();
   fc::uint128_t total_debt = 0;
   constexpr unsigned RetirementCycles = 2;
   for (unsigned cycle = 0; cycle < RetirementCycles; ++cycle) {
      BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
      BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
      BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
      total_debt += depot_native_row(kYieldBonderA, kLiqEthCodename)["shadow_yield"]["owed_wire"].as_uint64();
      produce_blocks();
      BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, false));
      BOOST_REQUIRE_EQUAL(total_debt, yield_debt(kYieldBonderA));
      BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kLiqEthCodename));
   }
   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   const auto checkpoint = liq_index();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   const uint64_t new_yield = yield_reference::owed(kYieldBondA, liq_index(), checkpoint);
   const int64_t credited = claim_yield_credit(kYieldBonderA);
   const uint64_t live_owed = depot_native_row(kYieldBonderA, kLiqEthCodename)["shadow_yield"]["owed_wire"].as_uint64();
   BOOST_REQUIRE_EQUAL(total_debt + new_yield,
                       fc::uint128_t{static_cast<uint64_t>(credited)} + yield_debt(kYieldBonderA) + live_owed);
   BOOST_REQUIRE_EQUAL(kYieldBondA, depot_native_row(kYieldBonderA, kLiqEthCodename)["balance"].as_uint64());
   const int64_t before = wire_balance(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(success(), claimremit(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(before + credited, wire_balance(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// A full WIRE remit must reject collection: neither detached debt, the pool, nor
/// the registry's unswept LIQ yield can be consumed by a saturating credit.
BOOST_FIXTURE_TEST_CASE(full_remit_rolls_back_archived_yield_claim, sysio_opreg_tester) { try {
   setup_yield_dust();
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, false));
   const auto debt_before = yield_debt(kYieldBonderA);
   const auto claim_before = get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64();
   const auto registry_before = wire_balance(OPREG_ACCOUNT);
   const auto liq_owed_before = registry_liq_owed();
   rewrite_claim_field("remitclaims"_n, "remit_claim", kYieldBonderA, kWireCodename, "balance",
                       uint64_t{asset::max_amount});
   constexpr auto FullRemitError = "assertion failure with message: claim WIRE remit before collecting more yield";
   BOOST_REQUIRE_EQUAL(error(FullRemitError), claimyield(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(debt_before, yield_debt(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(registry_before, wire_balance(OPREG_ACCOUNT));
   BOOST_REQUIRE_EQUAL(liq_owed_before, registry_liq_owed());
   rewrite_claim_field("remitclaims"_n, "remit_claim", kYieldBonderA, kWireCodename, "balance", claim_before);
   produce_blocks();
   const int64_t credited = claim_yield_credit(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(debt_before, fc::uint128_t{static_cast<uint64_t>(credited)} + yield_debt(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// Archived debt can exceed one asset across registrations. Collection must fill at most one
/// remit, preserve the remainder, and make progress again once that remit has been collected.
BOOST_FIXTURE_TEST_CASE(large_archived_yield_collects_in_bounded_parts, sysio_opreg_tester) { try {
   setup_yield_dust();
   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, false));
   const uint64_t old_credited = get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64();
   const uint64_t capacity = asset::max_amount;
   const fc::uint128_t debt = fc::uint128_t{capacity} + 1;
   // Boundary-only state injection: exercising arithmetic beyond a single token supply does not
   // require minting that supply. No transfer is attempted against the synthetic backing pool.
   rewrite_claim_field("yielddebts"_n, "yield_debt", kYieldBonderA, kLiqEthCodename, "owed_wire", debt);
   rewrite_claim_field("yieldpool"_n, "yield_pool", kYieldBonderA, kLiqEthCodename, "received",
                       old_credited + capacity + 1);
   rewrite_claim_field("remitclaims"_n, "remit_claim", kYieldBonderA, kWireCodename, "balance", uint64_t{0});
   BOOST_REQUIRE_EQUAL(success(), claimyield(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(capacity, get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{1}, yield_debt(kYieldBonderA));
   rewrite_claim_field("remitclaims"_n, "remit_claim", kYieldBonderA, kWireCodename, "balance", uint64_t{0});
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), claimyield(kYieldBonderA, kLiqEthCodename));
   BOOST_REQUIRE_EQUAL(1u, get_remitclaim(kYieldBonderA, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{0}, yield_debt(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// A downstream LIQ claim failure rolls back the predicted pool receipt, archived-debt debit
/// and WIRE remit together; restoring LIQ lets the same entitlement be collected once.
BOOST_FIXTURE_TEST_CASE(failed_liq_pull_preserves_archived_yield, sysio_opreg_tester) { try {
   setup_yield_holders();
   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, false));
   const auto debt_before = yield_debt(kYieldBonderA);
   const auto balance_before = wire_balance(OPREG_ACCOUNT);
   const auto owed_before = registry_liq_owed();
   // The token dispatcher rejects LIQ's claim action. Keep LIQ's tables/ABI so the opreg
   // preflight reads valid state and the queued downstream action is the failing boundary.
   set_code(LIQ_ACCOUNT, contracts::token_wasm());
   produce_blocks();
   BOOST_REQUIRE(claimyield(kYieldBonderA, kLiqEthCodename) != success());
   BOOST_REQUIRE_EQUAL(debt_before, yield_debt(kYieldBonderA));
   BOOST_REQUIRE(get_remitclaim(kYieldBonderA, kWireCodename).is_null());
   BOOST_REQUIRE_EQUAL(balance_before, wire_balance(OPREG_ACCOUNT));
   BOOST_REQUIRE_EQUAL(owed_before, registry_liq_owed());
   set_code(LIQ_ACCOUNT, contracts::liq_wasm());
   produce_blocks();
   const auto credited = claim_yield_credit(kYieldBonderA);
   BOOST_REQUIRE_EQUAL(debt_before, fc::uint128_t{static_cast<uint64_t>(credited)} + yield_debt(kYieldBonderA));
} FC_LOG_AND_RETHROW() }

/// A missing debt key for another token cannot consume the real token's pool or change its debt.
BOOST_FIXTURE_TEST_CASE(archived_yield_keeps_token_identity, sysio_opreg_tester) { try {
   setup_yield_holders();
   BOOST_REQUIRE_EQUAL(success(), deposit(kYieldBonderA, kLiqEthCodename, kYieldBondA));
   BOOST_REQUIRE_EQUAL(success(), addyield(kYieldDistribution));
   BOOST_REQUIRE_EQUAL(success(), terminate(kYieldBonderA, std::string(kTestTerminationReason)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), regoperator(kYieldBonderA, OPERATOR_TYPE_UNDERWRITER, false));
   const auto debt_before = yield_debt(kYieldBonderA);
   const auto registry_before = wire_balance(OPREG_ACCOUNT);
   BOOST_REQUIRE_EQUAL(error(std::string(kNoYieldOwedError)), claimyield(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(debt_before, yield_debt(kYieldBonderA));
   BOOST_REQUIRE_EQUAL(fc::uint128_t{0}, yield_debt(kYieldBonderA, kWireCodename));
   BOOST_REQUIRE_EQUAL(registry_before, wire_balance(OPREG_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), claimyield(kYieldBonderA, kLiqEthCodename));
} FC_LOG_AND_RETHROW() }

// This is the dual-shadow producer flow's depot policy with generic symbols.
// Registration of the Ethereum node NFT itself remains in the ETH-specific suites.
BOOST_FIXTURE_TEST_CASE(two_generic_depot_assets_are_both_required_and_withdrawals_preserve_identity,
                        sysio_opreg_tester) {
   using namespace sysio::testing::external;
   constexpr uint64_t Minimum = 2 * Unit;
   constexpr uint64_t Funding = 4 * Unit;
   const auto account = kEligibilityProducer;
   BOOST_REQUIRE_EQUAL(success(), setconfig(kDefaultPruneDelayMs, kDefaultMaxConsecutiveMisses,
      kMaxAcceptedPctMisses24h, kDefaultMinPctSamples, kTerminateWindowMs,
      {make_chain_min_bond(kWireCodename, First.token, Minimum),
       make_chain_min_bond(kWireCodename, Second.token, Minimum)}, {}, {}));
   BOOST_REQUIRE_EQUAL(success(), regoperator(account, OPERATOR_TYPE_PRODUCER, false));
   setup_shadow_liq({First, Second});
   for (const auto& a : Assets) {
      BOOST_REQUIRE_EQUAL(success(), push_contract(LIQ_ACCOUNT, liq_abi_ser, SYND_ACCOUNT, shadow_action::mint,
         mvo()("to", account)("token_code", codename_mvo(a.token))("amount", Funding)));
   }
   const auto active = [&] {
      return get_operator(account)[eligibility_field::status].as<OperatorStatus>() ==
         OperatorStatus::OPERATOR_STATUS_ACTIVE;
   };
   BOOST_REQUIRE(!active());
   // Surplus of the first token never substitutes for the missing second token.
   BOOST_REQUIRE_EQUAL(success(), deposit(account, First.token, Funding));
   BOOST_REQUIRE(!active());
   BOOST_REQUIRE_EQUAL(success(), deposit(account, Second.token, Minimum - 1));
   BOOST_REQUIRE(!active());
   BOOST_REQUIRE_EQUAL(success(), deposit(account, Second.token, 1));
   BOOST_REQUIRE(active());
   // Exercise either required token independently. Reservations must affect
   // eligibility immediately, before epoch maintenance moves any custody.
   for (const auto& a : Assets) {
      const auto sym = symbol::from_string(std::string("9,") + a.token);
      const auto other = symbol::from_string(std::string("9,") + (a.id == First.id ? Second.token : First.token));
      const auto other_before = get_currency_balance(LIQ_ACCOUNT, other, account).get_amount();
      const uint64_t withdrawn = a.id == First.id ? Funding : Minimum;
      const auto before = get_currency_balance(LIQ_ACCOUNT, sym, account).get_amount();
      BOOST_REQUIRE_EQUAL(success(), withdraw(account, a.token, withdrawn));
      BOOST_REQUIRE(!active());
      BOOST_REQUIRE_EQUAL(before, get_currency_balance(LIQ_ACCOUNT, sym, account).get_amount());
      produce_block(); // Distinct TAPOS for repeated maintenance actions.
      BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
      const auto claim = get_remitclaim(account, a.token);
      BOOST_REQUIRE(!claim.is_null());
      BOOST_REQUIRE_EQUAL(withdrawn, claim["balance"].as_uint64());
      BOOST_REQUIRE_EQUAL(success(), claimremit(account, a.token));
      BOOST_REQUIRE_EQUAL(before + int64_t(withdrawn), get_currency_balance(LIQ_ACCOUNT, sym, account).get_amount());
      BOOST_REQUIRE_EQUAL(other_before, get_currency_balance(LIQ_ACCOUNT, other, account).get_amount());
      BOOST_REQUIRE(get_remitclaim(account, a.token).is_null());
      BOOST_REQUIRE_EQUAL(success(), deposit(account, a.token, Minimum));
      BOOST_REQUIRE(active());
   }
}

BOOST_FIXTURE_TEST_CASE(native_wire_partial_withdrawal_stays_active_and_claims_exactly, sysio_opreg_tester) {
   constexpr uint64_t Minimum = 2 * kWireUnit;
   constexpr uint64_t Bond = 2 * Minimum;
   const auto account = kEligibilityBatchOperator;
   BOOST_REQUIRE_EQUAL(success(), setconfig(kDefaultPruneDelayMs, kDefaultMaxConsecutiveMisses,
      kMaxAcceptedPctMisses24h, kDefaultMinPctSamples, kTerminateWindowMs,
      {}, {make_chain_min_bond(kWireCodename, kWireCodename, Minimum)}, {}));
   BOOST_REQUIRE_EQUAL(success(), regoperator(account, OPERATOR_TYPE_BATCH, false));
   setup_wire_token_and_fund(account, kWireFundingQuantity);
   const auto before = wire_balance(account);
   BOOST_REQUIRE_EQUAL(success(), deposit(account, kWireCodename, Bond));
   BOOST_REQUIRE_EQUAL(before - int64_t(Bond), wire_balance(account));
   BOOST_REQUIRE_EQUAL(success(), withdraw(account, kWireCodename, Minimum));
   BOOST_REQUIRE(get_operator(account)[eligibility_field::status].as<OperatorStatus>() ==
      OperatorStatus::OPERATOR_STATUS_ACTIVE);
   BOOST_REQUIRE_EQUAL(before - int64_t(Bond), wire_balance(account));
   BOOST_REQUIRE_EQUAL(success(), flushwtdw(kFlushAllMaturedEpoch));
   BOOST_REQUIRE_EQUAL(Minimum, get_remitclaim(account, kWireCodename)["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), claimremit(account, kWireCodename));
   BOOST_REQUIRE_EQUAL(before - int64_t(Minimum), wire_balance(account));
   BOOST_REQUIRE(get_remitclaim(account, kWireCodename).is_null());
   BOOST_REQUIRE(get_operator(account)[eligibility_field::status].as<OperatorStatus>() ==
      OperatorStatus::OPERATOR_STATUS_ACTIVE);
}
BOOST_AUTO_TEST_SUITE_END()
