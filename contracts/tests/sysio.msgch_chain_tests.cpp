/// OPP epoch-envelope chaining tests for the `sysio.msgch` depot.
///
/// Outbound: every envelope `buildenv` emits carries the canonical epoch digest of the previous
/// emit for the same outpost in `previous_envelope_hash` (empty for the first emit), the stored
/// `envelope_hash` IS that canonical digest (keccak256 of the emitted bytes, which are the digest
/// preimage because the in-envelope `envelope_hash` field is empty on the wire), and the emitted
/// bytes are the canonical FIELD-COMPLETE protobuf encoding (every singular field written,
/// including proto3 defaults) that the outpost codecs (`OPPCommon.epochHash`) re-derive.
///
/// Inbound: `apply_consensus` records the accepted envelope's canonical digest as the per-outpost
/// chain tip (`outpcons.envelope_digest`) and verifies each envelope continues the chain. ENFORCED
/// (SEC-107 completion): both outposts self-chain per stream, so once a tip is recorded an empty
/// `previous_envelope_hash` is a chain break (valid only at genesis, before any tip); a non-empty
/// one must be exactly the 32-byte tip. Any other value — empty, wrong length, or non-matching —
/// is rejected at ingress: `deliver` reverts, so nothing is recorded or dispatched. An outpost's
/// FIRST accepted envelope (no tip yet) still bootstraps regardless of its prev-hash.
///
/// The oracle encoder in this file is an independent host-side reimplementation of the canonical
/// encoding (the contract-side implementation is
/// `contracts/sysio.opp.common/include/sysio.opp.common/opp_canonical_codec.hpp`). The golden
/// vectors pin the canonical encoding — including the semantic-header derivation — across
/// languages: the identical hex/digest values are pinned in the wire-solana and wire-ethereum
/// vector suites, where the Rust and Solidity implementations (`OPPCommon.epochHash` via the
/// `OPPEpochHashHelper` trampoline on the Solidity side) must reproduce them independently, so
/// oracle == Solidity/Rust (golden vectors) and contract == oracle (round-trip assertions)
/// together give contract == outposts.
#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

#include <fc/crypto/keccak256.hpp>
#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>

#include "contracts.hpp"
#include "contract_test_support.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;

using mvo = fc::mutable_variant_object;

// Canonical-encoding + header-derivation oracle shared across the contract tests.
#include "opp_envelope_oracle.hpp"

namespace {

using fc::slug_name_literals::operator""_s;

constexpr uint64_t ETH_OUTPOST_ID = "ETH"_s.value;
constexpr uint64_t SOL_OUTPOST_ID = "SOL"_s.value;
constexpr std::string_view ETH_CHAIN_CODE = "ETH";
constexpr std::string_view SOL_CHAIN_CODE = "SOL";
constexpr uint32_t ETH_EXTERNAL_CHAIN_ID = 31337; ///< External chain id the fixtures register ETH under.
constexpr uint32_t SOL_EXTERNAL_CHAIN_ID = 1;     ///< External chain id the fixtures register SOL under.
/// Native collateral used by the operator fixtures.
constexpr std::string_view WIRE_CHAIN_CODE = "WIRE";
constexpr uint64_t BATCH_OPERATOR_MINIMUM_COLLATERAL = 1;
constexpr uint64_t TABLE_SCAN_LIMIT = 64;
constexpr uint32_t ONE_OPERATOR_PER_TIED_VERSION = 1;
constexpr uint32_t THREE_OPERATORS_PER_TIED_VERSION = 3;
constexpr std::string_view ONE_TO_ONE_LEFT_PAYLOAD = "one-to-one-left";
constexpr std::string_view ONE_TO_ONE_RIGHT_PAYLOAD = "one-to-one-right";
constexpr std::string_view THREE_TO_THREE_LEFT_PAYLOAD = "three-to-three-left";
constexpr std::string_view THREE_TO_THREE_RIGHT_PAYLOAD = "three-to-three-right";

/// Mirrors of the inbound `envelopes` retention constants in `sysio.msgch.cpp` (contract headers
/// are not host-compilable).
constexpr uint32_t INBOUND_ENVELOPE_RETENTION_EPOCHS = 2;
constexpr uint32_t ENVELOPE_PRUNE_BUDGET             = 4;

/// Unbonded registrations the roster tests add: `UNBONDED_REGISTRANT_PREFIX` plus one letter each.
constexpr std::string_view UNBONDED_REGISTRANT_PREFIX = "rosterop.";
constexpr uint32_t         UNBONDED_REGISTRANTS       = 16;

/// sysio.opreg action identifiers used by the WNS-16 fixture.
namespace opreg_actions {
constexpr name SET_CONFIG        = "setconfig"_n;
constexpr name RECORD_DELIVERY   = "recorddel"_n;
constexpr name TERMINATION_CHECK = "termcheck"_n;
constexpr name SLASH             = "slash"_n;
constexpr name TERMINATE         = "terminate"_n;
} // namespace opreg_actions

/// sysio.opreg ABI field identifiers used by the WNS-16 fixture.
namespace opreg_fields {
constexpr const char* STATUS         = "status";
constexpr const char* STATUS_REASON  = "status_reason";
constexpr const char* RECENT_ACTIONS = "recent_actions";
constexpr const char* ACTION         = "action";
constexpr const char* ACTION_TYPE    = "action_type";
constexpr const char* SUCCESS        = "success";
constexpr const char* CHAIN_CODE     = "chain_code";
constexpr const char* ACCOUNT        = "account";
constexpr const char* EPOCH          = "epoch";
constexpr const char* DELIVERED      = "delivered";
constexpr const char* REASON         = "reason";
} // namespace opreg_fields

/// sysio.opreg tables read by delivery-log assertions.
namespace opreg_tables {
constexpr name DELIVERY_LOG = "dellog"_n;
} // namespace opreg_tables

/// sysio.opreg ABI type names used to decode table rows.
namespace opreg_abi_types {
constexpr const char* DELIVERY_LOG_ENTRY = "delivery_log_entry";
} // namespace opreg_abi_types

/// `sysio.opreg`'s membership status names.
namespace served_status = sysio_system::test_support::served_chains::status;

/// Eviction reason `termcheck` records when the rotation fixture's consecutive-miss rail fires.
constexpr const char* CONSECUTIVE_MISS_EVICTION_REASON = "rolling-window: >5 consecutive misses";

/// Payload of the envelopes BATCHOP delivers while the rotation fixture walks its duty epochs.
constexpr std::string_view ROTATION_DELIVERY_PAYLOAD = "rotation-duty";

/// Reason passed to the privileged `sysio.opreg::terminate` that removes a still-seated operator.
constexpr std::string_view SEATED_TERMINATION_REASON = "removed while seated";

/// Reason passed to `sysio.opreg::slash` when a test takes a member out of the eligible set mid-epoch.
constexpr std::string_view SLASH_REASON = "eligible set shrinks mid-epoch";

/// `joinchain`'s refusal for a chain that is not an active outpost.
constexpr const char* NOT_ACTIVE_OUTPOST_JOIN_ERROR = "assertion failure with message: chain is not an active outpost";
/// `sysio.epoch::advance`'s refusals of a chain's genesis.
constexpr const char* GENESIS_NOT_ACTIVE_OUTPOST_ERROR =
   "a chain's genesis advance requires the chain to be an active outpost";
constexpr const char* GENESIS_WITHOUT_SCHEDULE_ERROR =
   "a chain's genesis advance requires its schedule window: run schbatchgps for the chain first";

/// `joinchain`'s refusal for an operator the chain's schedule still seats.
constexpr const char* STILL_SEATED_JOIN_ERROR =
   "assertion failure with message: operator still holds a seat in this chain's schedule: it may rejoin once its "
   "seats have closed";

/// `deliver`'s refusal of a delivery to a decided epoch that differs from the accepted envelope.
constexpr const char* DECIDED_EPOCH_DIFFERING_DELIVERY_ERROR =
   "assertion failure with message: deliver: the epoch has reached consensus and the envelope differs from the "
   "accepted one";

/// `deliver`'s refusal of a late delivery that is not the previous epoch's accepted envelope.
constexpr const char* LATE_DELIVERY_DIFFERING_ERROR =
   "assertion failure with message: deliver: late delivery differs from the previous epoch's accepted envelope";

/// `deliver`'s refusal of a late delivery once the chain's late-delivery window has closed.
constexpr const char* LATE_DELIVERY_AFTER_WINDOW_ERROR =
   "assertion failure with message: deliver: the late-delivery window for the previous epoch has closed";

/// `deliver`'s refusal of a late delivery from outside the group that served the previous epoch.
constexpr const char* LATE_DELIVERER_NOT_IN_PREVIOUS_GROUP_ERROR =
   "assertion failure with message: deliver: a late delivery must come from the group that served the previous "
   "epoch";

/// `deliver`'s refusal of a current-epoch delivery from outside the group on duty.
constexpr const char* DELIVERER_NOT_IN_ACTIVE_GROUP_ERROR =
   "assertion failure with message: caller is not in the active batch operator group";

/// `deliver`'s refusal of a deliverer seated in neither the chain's current group nor its previous one.
constexpr const char* DELIVERER_NOT_IN_GROUP_ERROR =
   "assertion failure with message: deliver: caller is in neither the chain's current batch operator group nor "
   "its previous one";

/// Mirror of the `sysio.opreg` status reason sysio.msgch gives the slash of a non-canonical delivery: the
/// prefix, the chain code, the epoch separator, then the chain's epoch index.
constexpr std::string_view NON_CANONICAL_DELIVERY_REASON_PREFIX = "non-canonical OPP envelope delivery, chain ";
constexpr std::string_view NON_CANONICAL_DELIVERY_REASON_EPOCH  = " epoch ";

/// `reportqueue`'s refusals, as the contract raises them.
constexpr const char* REPORT_NOT_ACTIVE_OUTPOST = "reportqueue: chain is not an active outpost";
constexpr const char* REPORT_BEFORE_GENESIS =
   "reportqueue: the chain's epoch has not started; backlogs are reported from its epoch 1";
/// Prefix of the epoch-mismatch refusal: the report's epoch, then " current=" and the chain's current epoch.
constexpr const char* REPORT_EPOCH_MISMATCH = "reportqueue: report is not for the chain's current epoch: report=";
constexpr const char* REPORT_UNDECIDED_EPOCH =
   "reportqueue: the chain's current epoch has not reached consensus; its backlog is reported once it has";
constexpr const char* REPORT_WITHOUT_BACKLOG =
   "reportqueue: pending_count must be positive; an empty queue is not reported";
constexpr const char* REPORTER_NOT_ELIGIBLE =
   "reportqueue: reporter is not ACTIVE in sysio.opreg, or does not serve this chain or was evicted from it";
constexpr const char* REPORTER_NOT_IN_GROUP =
   "reportqueue: reporter is in neither the chain's group on duty nor its next group";

/// Queued messages the backlog reports of the tests carry.
constexpr uint32_t REPORTED_PENDING_COUNT = 3;

/// `deliver`'s refusal for a member that is no longer ACTIVE.
constexpr const char* NOT_ACTIVE_DELIVERY_ERROR =
   "assertion failure with message: delivering operator is not ACTIVE in sysio.opreg";

/// `deliver`'s refusal for a member evicted from the chain.
constexpr const char* EVICTED_DELIVERY_ERROR =
   "assertion failure with message: delivering operator does not serve this chain or was evicted from it";

/// `regoperator`'s refusal for an account the batch schedule still seats.
constexpr const char* STILL_SEATED_REGISTRATION_ERROR =
   "assertion failure with message: operator still holds a batch schedule seat: it may register again "
   "once its seats have closed";

/// sysio.opreg configuration ABI field identifiers used by the WNS-16 fixture.
namespace opreg_config_fields {
constexpr const char* TERMINATE_PRUNE_DELAY_MS         = "terminate_prune_delay_ms";
constexpr const char* TERMINATE_MAX_CONSECUTIVE_MISSES = "terminate_max_consecutive_misses";
constexpr const char* TERMINATE_MAX_PERCENT_MISSES_24H = "terminate_max_pct_misses_24h";
constexpr const char* TERMINATE_MIN_PERCENT_SAMPLES    = "terminate_min_pct_samples";
constexpr const char* TERMINATE_WINDOW_MS              = "terminate_window_ms";
constexpr const char* REQUIRED_PRODUCER_COLLATERAL     = "req_prod_collat";
constexpr const char* REQUIRED_BATCH_OPERATOR_COLLATERAL = "req_batchop_collat";
constexpr const char* REQUIRED_UNDERWRITER_COLLATERAL  = "req_uw_collat";
} // namespace opreg_config_fields

/// sysio.msgch action identifiers used by the WNS-16 fixture.
namespace msgch_actions {
constexpr name CHECK_CONSENSUS    = "chkcons"_n;
constexpr name BOOT_CHAIN         = "bootchain"_n;
constexpr name DELIVER            = "deliver"_n;
constexpr name EVALUATE_CONSENSUS = "evalcons"_n;
constexpr name QUEUE_OUT          = "queueout"_n;
constexpr name REPORT_QUEUE       = "reportqueue"_n;
} // namespace msgch_actions

/// Most attestations one outbound envelope carries: a mirror of `sysio.msgch::MAX_ENVELOPE_ATTESTATIONS`.
constexpr uint32_t MAX_ENVELOPE_ATTESTATIONS = 64;

/// sysio.epoch action identifiers the per-chain epoch fixtures push.
namespace epoch_actions {
constexpr name SET_CONFIG    = "setconfig"_n;
constexpr name SCHEDULE      = "schbatchgps"_n;
constexpr name ADVANCE       = "advance"_n;
constexpr name FINISH_ADVANCE = "finishadv"_n;
constexpr name TICK_ECONOMY  = "tickeconomy"_n;
constexpr name NOTE_TRAFFIC  = "notetraffic"_n;
constexpr name RESEND_ROSTER = "resendroster"_n;
constexpr name CLOSE_BOOKS   = "closebooks"_n;
constexpr name PAUSE         = "pause"_n;
constexpr name UNPAUSE       = "unpause"_n;
} // namespace epoch_actions

/// `sysio.epoch::advance`'s refusal while a chain is paused.
constexpr const char* EPOCH_PAUSED_ERROR = "epoch advancement is paused";

/// `sysio.epoch::advance`'s refusal, under its own authority, of an active outpost's undecided epoch.
constexpr const char* GOVERNANCE_ADVANCE_UNDECIDED_ERROR =
   "governance cannot advance an active outpost whose current epoch has not reached consensus";

/// sysio.msgch table identifiers used by the WNS-16 fixture.
namespace msgch_tables {
constexpr name ATTESTATIONS       = "attestations"_n;
constexpr name ENVELOPES          = "envelopes"_n;
constexpr name OUTBOUND_ENVELOPES = "outenvelopes"_n;
constexpr name QUEUE_REPORTS      = "qreports"_n;
} // namespace msgch_tables

/// sysio.msgch secondary-index identifiers used by the envelopes retention tests.
namespace msgch_indexes {
constexpr name BY_OUTPOST_EPOCH = "byoutepoch"_n;
} // namespace msgch_indexes

/// sysio.msgch ABI type identifiers used by the WNS-16 fixture.
namespace msgch_abi_types {
constexpr const char* ATTESTATION_ENTRY = "attestation_entry";
constexpr const char* ENVELOPE_ENTRY    = "envelope_entry";
constexpr const char* OUTBOUND_ENVELOPE = "outbound_envelope";
constexpr const char* QUEUE_REPORT_ENTRY = "queue_report_entry";
} // namespace msgch_abi_types

/// sysio.msgch ABI field identifiers used by the WNS-16 fixture.
namespace msgch_fields {
constexpr const char* ID            = "id";
constexpr const char* RAW_DATA      = "raw_data";
constexpr const char* CHAIN_CODE    = "chain_code";
constexpr const char* EPOCH_INDEX   = "epoch_index";
constexpr const char* BATCH_OP_NAME = "batch_op_name";
constexpr const char* WINNING_CHECKSUM = "winning_checksum";
constexpr const char* CHECKSUM         = "checksum";
constexpr const char* CONSENSUS_REACHED = "consensus_reached";
constexpr const char* STATUS            = "status";
constexpr const char* DATA              = "data";
constexpr const char* RECEIVED_AT       = "received_at";
constexpr const char* PREVIOUS_EPOCH_INDEX      = "previous_epoch_index";
constexpr const char* PREVIOUS_WINNING_CHECKSUM = "previous_winning_checksum";
constexpr const char* READY_TIMESTAMP           = "ready_timestamp";
constexpr const char* ATTEST_TYPE               = "attest_type";
constexpr const char* BATCH_OP                  = "batch_op";
constexpr const char* ACCOUNT                   = "account";
constexpr const char* PENDING_COUNT             = "pending_count";
constexpr const char* OLDEST_PENDING_MS         = "oldest_pending_ms";
constexpr const char* OLDEST_PENDING_AT         = "oldest_pending_at";
} // namespace msgch_fields

/// sysio.chalg table identifiers used by the split-consensus regressions.
namespace chalg_tables {
constexpr name DISPUTES = "disputes"_n;
} // namespace chalg_tables

/// sysio.chalg action identifiers used by the two-version split regressions.
namespace chalg_actions {
constexpr name CHECK_DISPUTE  = "chkdispute"_n;
constexpr name VOTE_DISPUTE   = "votedispute"_n;
constexpr name OPEN_DISPUTE   = "opendispute"_n;
constexpr name SLASH_OPERATOR = "slashop"_n;
} // namespace chalg_actions

/// sysio.chalg ABI type identifiers used by the split-consensus regressions.
namespace chalg_abi_types {
constexpr const char* DISPUTE_ENTRY  = "dispute_entry";
constexpr const char* SLASH_OPERATOR = "slashop";
} // namespace chalg_abi_types

/// sysio.chalg `slashop` ABI field identifiers.
namespace chalg_slash_fields {
constexpr const char* OPERATOR_ACCOUNT = "operator_acct";
constexpr const char* REASON           = "reason";
} // namespace chalg_slash_fields

/// sysio.chalg ABI field identifiers used by the split-consensus regressions.
namespace chalg_fields {
constexpr const char* CANDIDATES  = "candidates";
constexpr const char* CHAIN_CODE  = "chain_code";
constexpr const char* CHECKSUM    = "checksum";
constexpr const char* EPOCH_INDEX = "epoch_index";
constexpr const char* OPERATORS   = "operators";
constexpr const char* STATUS      = "status";
} // namespace chalg_fields

/// sysio.chalg vote-action ABI field identifiers used by the two-version split regressions.
namespace chalg_vote_fields {
constexpr const char* CHOSEN_CHECKSUM = "chosen_checksum";
constexpr const char* DISPUTE_ID       = "dispute_id";
constexpr const char* OWNER            = "owner";
} // namespace chalg_vote_fields

/// sysio.chains action identifiers used to activate a chain registered after genesis.
namespace chains_actions {
constexpr name ACTIVATE_CHAIN = "activchain"_n;
} // namespace chains_actions

/// sysio.chains table, ABI type and field identifiers of the chain row a test sets inactive.
namespace chains_tables {
constexpr name CHAINS = "chains"_n;
} // namespace chains_tables
namespace chains_abi_types {
constexpr const char* CHAIN_ROW = "chain_row";
} // namespace chains_abi_types
namespace chains_fields {
constexpr const char* ACTIVE = "active";
} // namespace chains_fields

/// sysio.epoch ABI field identifiers used by the WNS-16 fixture.
namespace epoch_fields {
constexpr const char* BATCH_OP_GROUPS        = "batch_op_groups";
constexpr const char* CURRENT_BATCH_OP_GROUP = "current_batch_op_group";
constexpr const char* IS_PAUSED              = "is_paused";
constexpr const char* CHAIN_CODE             = "chain_code";
constexpr const char* CURRENT_EPOCH_INDEX    = "current_epoch_index";
constexpr const char* CURRENT_EPOCH_START    = "current_epoch_start";
constexpr const char* TRAFFIC_SINCE          = "traffic_since";
constexpr const char* DUE_AT                 = "due_at";
constexpr const char* EPOCH_DURATION_SEC     = "epoch_duration_sec";
constexpr const char* TRAFFIC_WAIT_SEC       = "traffic_wait_sec";
constexpr const char* LATE_WINDOW_SEC        = "late_window_sec";
constexpr const char* EPOCH_INDEX            = "epoch_index";
constexpr const char* PREVIOUS_GROUP         = "previous_group";
} // namespace epoch_fields

/// sysio.epoch tables the per-chain epoch fixtures read.
namespace epoch_tables {
constexpr name EPOCH_STATE  = "epochstate"_n;
constexpr name EPOCH_CONFIG = "epochcfg"_n;
} // namespace epoch_tables

/// sysio.epoch ABI type names of those rows.
namespace epoch_abi_types {
constexpr const char* EPOCH_CONFIG = "epoch_config";
} // namespace epoch_abi_types

/// sysio.roa action identifiers used by the empty-electorate retry regression.
namespace roa_actions {
constexpr name ACTIVATE = "activateroa"_n;
constexpr name FORCE_REGISTER = "forcereg"_n;
} // namespace roa_actions

/// sysio.system identifiers used while activating the empty ROA fixture.
namespace system_actions {
constexpr name SET_PRIVILEGED = "setpriv"_n;
constexpr name RECORD_DUTY    = "rcrdduty"_n;
} // namespace system_actions

/// sysio.system tables the batch-duty assertions read.
namespace system_tables {
constexpr name BATCH_DUTY = "batchduty"_n;
constexpr name DUTY_STATE = "dutystate"_n;
constexpr name PAY_CLAIMS = "payclaims"_n;
constexpr name EPOCH_LOG  = "epochlog"_n;
} // namespace system_tables

/// sysio.system ABI type names used to decode those rows.
namespace system_abi_types {
constexpr const char* BATCH_DUTY = "batch_duty";
constexpr const char* DUTY_STATE = "duty_state";
constexpr const char* PAY_CLAIM  = "pay_claim";
constexpr const char* EPOCH_LOG  = "epoch_log";
} // namespace system_abi_types

namespace system_fields {
constexpr const char* ACCOUNT = "account";
constexpr const char* IS_PRIVILEGED = "is_priv";
constexpr const char* SEAT_SECONDS            = "seat_seconds";
constexpr const char* BALANCE                 = "balance";
constexpr const char* OPEN_SEAT_SECONDS       = "open_seat_seconds";
constexpr const char* SETTLING_PERIOD         = "settling_period";
constexpr const char* BATCH_EMISSION          = "batch_emission";
constexpr const char* BATCH_EMISSION_RETAINED = "batch_emission_retained";
constexpr const char* BATCH_SETTLED           = "batch_settled";
} // namespace system_fields

/// `tickeconomy`'s refusal of an economic epoch whose boundary is still ahead.
constexpr std::string_view ECONOMIC_BOUNDARY_NOT_PASSED_ERROR = "economic epoch boundary has not passed";

/// sysio.roa ABI field identifiers used by the empty-electorate retry regression.
namespace roa_fields {
constexpr const char* OWNER          = "owner";
constexpr const char* TIER           = "tier";
constexpr const char* TOTAL_SYSTEM   = "total_sys";
constexpr const char* BYTES_PER_UNIT = "bytes_per_unit";
} // namespace roa_fields

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Fixture: full epoch-advance capability (sysio.system + emissions config,
//  modeled on sysio_epoch_flushwtdw_tester) plus msgch deliver/queueout/
//  buildenv helpers and chain-tip table readers.
// ---------------------------------------------------------------------------
class sysio_msgch_chain_tester : public tester {
public:
   static constexpr auto SYSIO_ACCOUNT  = config::system_account_name;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto OPREG_ACCOUNT  = "sysio.opreg"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto ROA_ACCOUNT    = "sysio.roa"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto DCLAIM_ACCOUNT = "sysio.dclaim"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto BATCHOP_B      = "batchop.b"_n;
   static constexpr auto BATCHOP_C      = "batchop.c"_n;
   static constexpr auto BATCHOP_D      = "batchop.d"_n;
   static constexpr auto BATCHOP_E      = "batchop.e"_n;
   static constexpr auto BATCHOP_F      = "batchop.f"_n;
   static constexpr auto BATCHOP_G      = "batchop.g"_n;
   static constexpr auto BATCHOP_H      = "batchop.h"_n;
   static constexpr auto BATCHOP_I      = "batchop.i"_n;
   static constexpr auto BATCHOP_J      = "batchop.j"_n;
   static constexpr auto BATCHOP_K      = "batchop.k"_n;

   static constexpr uint32_t EPOCH_DURATION_SEC = 60;
   static constexpr uint32_t ONE_TO_ONE_TIE_GROUP_SIZE = 2;
   static constexpr uint32_t THREE_TO_THREE_TIE_GROUP_SIZE = 6;
   static constexpr uint32_t INCOMPLETE_TWO_WAY_SPLIT_GROUP_SIZE = 3;
   static constexpr uint32_t MAXIMUM_GROUP_SIZE = 7; ///< The outpost group-size ceiling: a strict majority is 4.
   static constexpr uint64_t FIRST_DISPUTE_ID = 1;
   static constexpr uint64_t SECOND_DISPUTE_ID = 2;
   static constexpr auto TIER_ONE_OWNER = "tierone"_n;
   static constexpr const char* EMPTY_ROA_TOTAL_SYSTEM = "75496.0000 SYS";
   static constexpr uint64_t EMPTY_ROA_BYTES_PER_UNIT = 104;
   static constexpr uint64_t MS_PER_SEC = 1000; ///< Milliseconds per second, for window spans.

   /// Rotation fixture schedule: single-operator groups, so a resident operator is on duty once per
   /// `ROTATION_GROUPS`-epoch rotation.
   static constexpr uint32_t ROTATION_GROUPS              = 3;
   static constexpr uint32_t ROTATION_OPERATORS_PER_GROUP = 1; ///< Operators seated in each rotation group.

   /// Consecutive-miss threshold the rotation fixture installs (the production default; see
   /// `CONSECUTIVE_MISS_EVICTION_REASON`).
   static constexpr uint32_t ROTATION_MAX_CONSECUTIVE_MISSES = 5;

   /// Percent-miss threshold the rotation fixture installs: the accepted ceiling, so an anchored run
   /// terminates on the consecutive rail.
   static constexpr uint32_t ROTATION_MAX_PERCENT_MISSES = 99;

   /// Percent-miss threshold of the default fixture: sysio.opreg's production default.
   static constexpr uint32_t DEFAULT_MAX_PERCENT_MISSES = 5;

   /// Duty epochs before the percent rail applies: sysio.opreg's production default.
   static constexpr uint32_t DEFAULT_MIN_PERCENT_SAMPLES = 20;

   /// Percent-rail sample that applies the rail from the first duty epoch.
   static constexpr uint32_t PERCENT_RAIL_FROM_FIRST_DUTY = 1;

   /// One duty rotation of the rotation schedule: a window this narrow keeps only a member's latest record.
   static constexpr uint64_t ROTATION_DUTY_MS = uint64_t{ROTATION_GROUPS} * EPOCH_DURATION_SEC * MS_PER_SEC;

   /// A window of (misses + 1) duty rotations: a run evicting on the rotation fixture's threshold has its
   /// delivered anchor on the window's edge at the evicting miss.
   static constexpr uint64_t ROTATION_WINDOW_MS = (uint64_t{ROTATION_MAX_CONSECUTIVE_MISSES} + 1) * ROTATION_DUTY_MS;

   /// Outposts the rotation fixture registers.
   enum class rotation_outposts { none, eth, eth_and_sol };

   /// Disjoint-group fixture schedule: the production shape of disjoint groups, three of three, so a member of
   /// the previous epoch's group sits in no current group.
   static constexpr uint32_t DISJOINT_GROUPS     = 3;
   static constexpr uint32_t DISJOINT_GROUP_SIZE = 3; ///< Operators seated in each disjoint group.

   /// Construct an OPP integration fixture, optionally with an activated ROA that has no
   /// registered node owners to exercise the msgch electorate preflight.
   explicit sysio_msgch_chain_tester(bool empty_roa = false)
      : tester(empty_roa ? setup_policy::full_except_do_not_set_finalizers : setup_policy::full) {
      if (empty_roa) activate_empty_roa();
      produce_blocks(2);

      // sysio.* accounts BEFORE sysio.system so they keep unlimited RAM; the payepoch
      // destinations (sysio.dclaim / sysio.gov / sysio.ops) must exist for the emissions
      // pay-epoch transfers. Same bootstrap rationale as sysio_epoch_flushwtdw_tester.
      create_accounts({
         TOKEN_ACCOUNT, EPOCH_ACCOUNT, OPREG_ACCOUNT, MSGCH_ACCOUNT,
         CHALG_ACCOUNT, CHAINS_ACCOUNT,
         BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E, BATCHOP_F, BATCHOP_G,
         DCLAIM_ACCOUNT, "sysio.gov"_n, "sysio.ops"_n
      }, false, true, !empty_roa);
      produce_blocks(2);

      deploy(CHALG_ACCOUNT,  contracts::chalg_wasm(),  contracts::chalg_abi(),  chalg_abi);
      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),  contracts::epoch_abi(),  epoch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),  contracts::opreg_abi(),  opreg_abi);
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi);
      deploy(TOKEN_ACCOUNT,  contracts::token_wasm(),  contracts::token_abi(),  token_abi);
      // A batch operator joins a chain only with its authex link for the chain's kind, and a
      // user-signed link needs a privileged sysio.dclaim.
      deploy(AUTHEX_ACCOUNT, contracts::authex_wasm(), contracts::authex_abi(), authex_abi);
      deploy(DCLAIM_ACCOUNT, contracts::dclaim_wasm(), contracts::dclaim_abi(), dclaim_abi);
      produce_blocks(1);

      load_abi(ROA_ACCOUNT, roa_abi);

      // sysio.system for the epoch::advance emissions readiness gate; without emitcfg/t5state
      // the gate returns CONFIG_MISSING and the epoch never advances.
      set_code(SYSIO_ACCOUNT, contracts::system_wasm());
      set_abi(SYSIO_ACCOUNT, contracts::system_abi().data());
      produce_blocks(1);
      load_abi(SYSIO_ACCOUNT, sysio_abi);

      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT,
         "init"_n, mvo()("version", 0)("core", "4,SYS")));

      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, TOKEN_ACCOUNT,
         "create"_n, mvo()
            ("issuer", SYSIO_ACCOUNT)
            ("maximum_supply", "1000000000.000000000 WIRE")));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, SYSIO_ACCOUNT,
         "issue"_n, mvo()
            ("to", SYSIO_ACCOUNT)
            ("quantity", "1000000000.000000000 WIRE")
            ("memo", "test bootstrap")));

      produce_blocks();
   }

   void load_abi(name account, abi_serializer& out_ser) {
      sysio_system::test_support::load_account_abi(*this, account, out_ser);
   }

   void deploy(name account, std::vector<uint8_t> wasm, std::vector<char> abi,
               abi_serializer& out_ser) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      set_privileged(account);
      load_abi(account, out_ser);
   }

   /// Activate the ROA registry without the normal genesis Tier-1 nodedaddy registration.
   void activate_empty_roa() {
      create_account(ROA_ACCOUNT, SYSIO_ACCOUNT, false, true, false, false);
      create_account("sysio.acct"_n, SYSIO_ACCOUNT, false, false, false, false);
      create_account("sysio.authex"_n, SYSIO_ACCOUNT, false, false, false, false);
      set_contract(ROA_ACCOUNT, contracts::roa_wasm(), contracts::roa_abi().data());
      base_tester::push_action(SYSIO_ACCOUNT, system_actions::SET_PRIVILEGED, SYSIO_ACCOUNT,
                               mvo()(system_fields::ACCOUNT, ROA_ACCOUNT.to_string())
                                    (system_fields::IS_PRIVILEGED, true));
      base_tester::push_action(ROA_ACCOUNT, roa_actions::ACTIVATE, ROA_ACCOUNT,
                               mvo()(roa_fields::TOTAL_SYSTEM, asset::from_string(EMPTY_ROA_TOTAL_SYSTEM))
                                    (roa_fields::BYTES_PER_UNIT, EMPTY_ROA_BYTES_PER_UNIT));
   }

   action_result push(name contract, abi_serializer& ser, name signer,
                      name action_name, const fc::variant_object& data) {
      return sysio_system::test_support::push_contract_action(
         *this, contract, ser, signer, action_name, data);
   }

   /// setemitcfg mirroring sysio_epoch_flushwtdw_tester / emissions_tests defaults; the epoch
   /// tests only need the readiness gate to pass, not any particular emission curve.
   action_result setemitcfg_defaults() {
      constexpr uint32_t SECONDS_PER_MONTH = 30u * 24u * 60u * 60u;
      return push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT, "setemitcfg"_n,
         mvo()("cfg", mvo()
            ("t1_allocation",          int64_t(7'500'000'000'000'000LL))
            ("t2_allocation",          int64_t(1'000'000'000'000'000LL))
            ("t3_allocation",          int64_t(  100'000'000'000'000LL))
            ("t1_duration",            12u * SECONDS_PER_MONTH)
            ("t2_duration",            24u * SECONDS_PER_MONTH)
            ("t3_duration",            36u * SECONDS_PER_MONTH)
            ("min_claimable",          int64_t(10'000'000'000LL))
            ("t5_distributable",       int64_t(375'000'000'000'000'000LL))
            ("t5_floor",               int64_t(125'000'000'000'000'000LL))
            ("target_annual_decay_bps", uint16_t(6940))
            ("annual_initial_emission", int64_t(563'150'000'000'000LL) * 365)
            ("annual_max_emission",     int64_t(3'000'000'000'000'000LL) * 365)
            ("annual_min_emission",     int64_t(100'000'000'000'000LL) * 365)
            ("compute_bps",            uint16_t(4000))
            ("capex_bps",              uint16_t(2000))
            ("governance_bps",         uint16_t(1000))
            ("producer_bps",           uint16_t(7000))
            ("batch_op_bps",           uint16_t(3000))
            ("standby_end_rank",       uint32_t(28))("standby_bps", uint16_t(800))
            ("epoch_log_retention_count", uint32_t(8640))
            ("pay_cadence_epochs",     uint16_t(1))));
   }

   /// Configure one test cohort of up to seven batch operators (the outpost group-size ceiling), the
   /// ETH/SOL outpost rows, each chain's one-group schedule, and each chain's genesis advance. The
   /// configurable cohort supports split regressions. SOL's epoch lasts `sol_duration_sec`.
   void bootstrap(uint32_t n_batch_ops = 1, bool batchop_is_bootstrapped = true,
                  uint32_t sol_duration_sec = EPOCH_DURATION_SEC) {
      // Both outposts run the same schedule, so their epochs advance in step unless a test drives
      // them apart.
      epoch_chains = {ETH_CHAIN_CODE, SOL_CHAIN_CODE};
      for (const std::string_view chain : epoch_chains) {
         // Sizes the schedule: a group seats up to this many members, and consensus needs a strict
         // majority of the seats the group on duty holds, counting only its eligible members'
         // deliveries (msgch's `duty_group_of`). The epoch contract enforces
         // minimum_active == operators_per_epoch * batch_op_groups.
         set_epoch_config(chain, chain == SOL_CHAIN_CODE ? sol_duration_sec : EPOCH_DURATION_SEC, n_batch_ops, 1);
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "seteconomy"_n, mvo()
         ("epoch_duration_sec", EPOCH_DURATION_SEC)("start_time", fc::time_point_sec(control->head().block_time()))));

      BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults());
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT,
         "initt5"_n, mvo()));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "setconfig"_n, mvo()
            ("terminate_prune_delay_ms",         600000)
            ("terminate_max_consecutive_misses", 5)
            ("terminate_max_pct_misses_24h",     5)
            ("terminate_min_pct_samples",        DEFAULT_MIN_PERCENT_SAMPLES)
            ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
            ("req_prod_collat",                  fc::variants{})
            // Empty collateral requirements intentionally keep non-bootstrapped
            // operators UNKNOWN. Give the WNS-16 fixture's non-bootstrapped
            // operator the one-unit native WIRE requirement it satisfies below.
            ("req_batchop_collat",               batchop_is_bootstrapped
                                                   ? fc::variants{}
                                                   : fc::variants{
                                                        make_chain_min_bond(WIRE_CHAIN_CODE, WIRE_CHAIN_CODE, BATCH_OPERATOR_MINIMUM_COLLATERAL) })
            ("req_uw_collat",                    fc::variants{})));

      const std::vector<name> available_batch_ops{
         BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E, BATCHOP_F, BATCHOP_G};
      BOOST_REQUIRE(n_batch_ops > 0 && n_batch_ops <= available_batch_ops.size());
      std::vector<name> batch_ops(available_batch_ops.begin(),
                                  available_batch_ops.begin() + n_batch_ops);
      for (const auto& op : batch_ops) {
         BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
            "regoperator"_n, mvo()
               ("account",          op.to_string())
               ("type",             opp::types::OperatorType::OPERATOR_TYPE_BATCH)
               ("is_bootstrapped",  op == BATCHOP ? batchop_is_bootstrapped : true)));
      }

      register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, ETH_CHAIN_CODE, ETH_EXTERNAL_CHAIN_ID);
      register_chain(opp::types::ChainKind::CHAIN_KIND_SVM, SOL_CHAIN_CODE, SOL_EXTERNAL_CHAIN_ID);

      // A non-bootstrapped batch operator starts UNKNOWN and becomes ACTIVE
      // only after a collateral update re-evaluates its role eligibility.
      if (!batchop_is_bootstrapped) {
         BOOST_REQUIRE_EQUAL(success(), fund_and_bond(BATCHOP, BATCH_OPERATOR_MINIMUM_COLLATERAL));
      }
      serve_chains(batch_ops, epoch_chains);

      for (const std::string_view chain : epoch_chains) {
         BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                             mvo()(epoch_fields::CHAIN_CODE, chain)));
         BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                                             mvo()(epoch_fields::CHAIN_CODE, chain)));
      }

      produce_blocks();
   }

   /// Install `chain`'s consensus epoch config: `groups` groups of `operators_per_group`.
   void set_epoch_config(std::string_view chain, uint32_t duration_sec, uint32_t operators_per_group,
                         uint32_t groups) {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SET_CONFIG, mvo()
         (epoch_fields::CHAIN_CODE,             chain)
         ("epoch_duration_sec",                 duration_sec)
         ("traffic_wait_sec",                   0)
         ("late_window_sec",                    0)
         ("operators_per_epoch",                operators_per_group)
         ("batch_operator_minimum_active",      operators_per_group * groups)
         ("batch_op_groups",                    groups)
         ("epoch_retention_envelope_log_count", 200)));
   }

   /// The outpost chains whose epochs the fixture configured, in the order it advances them.
   std::vector<std::string_view> epoch_chains;

   void register_chain(opp::types::ChainKind kind, std::string_view code, uint32_t chain_id) {
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              kind)
            ("code",              code)
            ("external_chain_id", chain_id)
            ("name",              std::string("outpost-test"))
            ("description",       std::string{})
            ("outpost", sysio_system::test_support::no_outpost_mvo())));
   }

   /// The chain family of fixture outpost `code`: SOL is SVM, every other outpost EVM.
   static opp::types::ChainKind outpost_kind(std::string_view code) {
      return code == SOL_CHAIN_CODE ? opp::types::ChainKind::CHAIN_KIND_SVM : opp::types::ChainKind::CHAIN_KIND_EVM;
   }

   /// Link each of `ops` to a fresh key of each chain's kind and join it to every chain of `chains`.
   void serve_chains(const std::vector<name>& ops, const std::vector<std::string_view>& chains) {
      std::vector<sysio_system::test_support::served_chains::outpost> outposts;
      for (const std::string_view code : chains) outposts.push_back({code, outpost_kind(code)});
      sysio_system::test_support::served_chains::serve(*this, ops, outposts);
   }

   /// Make every configured chain due (`elapse_epoch_boundary`) and advance its epoch, as sysio.msgch's chkcons would.
   uint32_t advance_one_epoch() {
      elapse_epoch_boundary();
      for (const std::string_view chain : epoch_chains) {
         BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT, epoch_actions::ADVANCE,
                                             mvo()(epoch_fields::CHAIN_CODE, chain)));
      }
      produce_blocks();
      return current_epoch();
   }

   /// `chain_code`'s current consensus epoch index (0 before its genesis advance).
   uint32_t current_epoch(uint64_t chain_code = ETH_OUTPOST_ID) {
      const auto state = read_epoch_state(chain_code);
      return state.is_null() ? 0 : state[epoch_fields::CURRENT_EPOCH_INDEX].as<uint32_t>();
   }

   // -- msgch actions --

   action_result deliver(uint64_t chain_code, const std::vector<char>& data) {
      return deliver_as(BATCHOP, chain_code, data);
   }

   action_result deliver_as(name op, uint64_t chain_code, const std::vector<char>& data) {
      return push(MSGCH_ACCOUNT, msgch_abi, op, "deliver"_n, mvo()
         ("batch_op_name", op.to_string())
         ("chain_code",    chain_code)
         ("data",          data));
   }

   /// Register `owner` as Tier-1 after the empty-ROA path has soft-declined a dispute.
   void register_tier_one_node_owner(name owner) {
      create_account(owner, SYSIO_ACCOUNT, false, true, false);
      const uint8_t tier_one = magic_enum::enum_integer(
         opp::types::NodeOwnerTier::NODE_OWNER_TIER_T1);
      BOOST_REQUIRE_EQUAL(success(), push(ROA_ACCOUNT, roa_abi, ROA_ACCOUNT, roa_actions::FORCE_REGISTER,
                                         mvo()(roa_fields::OWNER, owner.to_string())(roa_fields::TIER, tier_one)));
   }

   /// Record traffic on every configured chain since its epoch start (`mark_chain_traffic`), then let
   /// `EPOCH_DURATION_SEC` elapse WITHOUT advancing the epoch: a chain of that minimum is then due, so
   /// `chkcons` advances its decided epoch, while `deliver`'s epoch gate keeps accepting envelopes for the
   /// still-current epoch.
   void elapse_epoch_boundary() {
      mark_chain_traffic();
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
   }

   /// Record traffic on every configured chain since its current epoch started (`epoch_state::mark_traffic`), as an
   /// outbound attestation queued for it would: each chain is then due one minimum after its epoch start.
   void mark_chain_traffic() {
      for (const std::string_view chain : epoch_chains) {
         sysio_system::test_support::epoch_state::mark_traffic(*this, epoch_abi, chain);
      }
   }

   /// `chain_code`'s minimum duration, its `epochcfg` `epoch_duration_sec`.
   fc::microseconds minimum_duration(uint64_t chain_code = ETH_OUTPOST_ID) {
      return fc::seconds(epoch_config(chain_code)[epoch_fields::EPOCH_DURATION_SEC].as<uint32_t>());
   }

   /// `chain_code`'s traffic wait, its `epochcfg` `traffic_wait_sec`.
   fc::microseconds traffic_wait(uint64_t chain_code = ETH_OUTPOST_ID) {
      return fc::seconds(epoch_config(chain_code)[epoch_fields::TRAFFIC_WAIT_SEC].as<uint32_t>());
   }

   /// `chain_code`'s late-delivery window, its `epochcfg` `late_window_sec`.
   fc::microseconds late_window(uint64_t chain_code = ETH_OUTPOST_ID) {
      return fc::seconds(epoch_config(chain_code)[epoch_fields::LATE_WINDOW_SEC].as<uint32_t>());
   }

   /// When `chain_code` is due for traffic of its current epoch that began at `since`: the later of its epoch start
   /// plus its minimum and `since` plus its traffic wait.
   fc::time_point due_for(fc::time_point since, uint64_t chain_code = ETH_OUTPOST_ID) {
      const fc::time_point start = read_epoch_state(chain_code)[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>();
      return std::max(start + minimum_duration(chain_code), since + traffic_wait(chain_code));
   }

   /// `chain_code`'s `epochcfg` row.
   fc::variant epoch_config(uint64_t chain_code) {
      const auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, epoch_tables::EPOCH_CONFIG, chain_code);
      BOOST_REQUIRE(!data.empty());
      return epoch_abi.binary_to_variant(epoch_abi_types::EPOCH_CONFIG, data, fixture_abi_yield_function());
   }

   action_result queueout(uint64_t chain_code, uint32_t attest_type,
                          std::vector<char> data) {
      return push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("chain_code",   chain_code)
         ("attest_type",  attest_type)
         ("data",         std::move(data)));
   }

   /// `queueout` of a `type` row of `fill` bytes for `chain_code` on `contract` (sysio.msgch, or another account
   /// running its code), under that account's own authority, returning the trace.
   transaction_trace_ptr queueout_trace(uint64_t chain_code, opp::types::AttestationType type, char fill,
                                        name contract = MSGCH_ACCOUNT) {
      return sysio_system::test_support::push_contract_action_trace(
         *this, contract, msgch_abi, contract, msgch_actions::QUEUE_OUT,
         mvo()(msgch_fields::CHAIN_CODE, chain_code)(msgch_fields::ATTEST_TYPE, type)
              (msgch_fields::DATA, std::vector<char>(8, fill)));
   }

   /// How many times `trace` notified sysio.epoch of a `queueout` of `contract`.
   static uint32_t queueout_notifications(const transaction_trace_ptr& trace, name contract = MSGCH_ACCOUNT) {
      BOOST_REQUIRE(trace);
      uint32_t n = 0;
      for (const auto& at : trace->action_traces) {
         if (at.receiver == EPOCH_ACCOUNT && at.act.account == contract && at.act.name == msgch_actions::QUEUE_OUT) ++n;
      }
      return n;
   }

   /// The ready time of `chain_code`'s oldest READY general-lane attestation (`ready_timestamp`, in seconds).
   fc::time_point oldest_ready_general(uint64_t chain_code) {
      uint64_t oldest = std::numeric_limits<uint64_t>::max();
      for (const auto& row : table_rows(MSGCH_ACCOUNT, msgch_tables::ATTESTATIONS, msgch_abi,
                                        msgch_abi_types::ATTESTATION_ENTRY)) {
         if (row[msgch_fields::CHAIN_CODE].as_uint64() != chain_code ||
             row[msgch_fields::STATUS].as<opp::types::AttestationStatus>() !=
                opp::types::AttestationStatus::ATTESTATION_STATUS_READY) continue;
         oldest = std::min(oldest, row[msgch_fields::READY_TIMESTAMP].as<uint64_t>());
      }
      BOOST_REQUIRE_NE(oldest, std::numeric_limits<uint64_t>::max());
      return fc::time_point(fc::seconds(static_cast<int64_t>(oldest)));
   }

   action_result buildenv(uint64_t chain_code) {
      return push(MSGCH_ACCOUNT, msgch_abi, EPOCH_ACCOUNT, "buildenv"_n, mvo()
         ("chain_code", chain_code));
   }

   // -- Table readers --

   /// Every outbound envelope row for `chain_code`, in primary-key order.
   std::vector<fc::variant> outbound_envelope_rows(uint64_t chain_code) {
      std::vector<fc::variant> rows;
      for (auto& row : table_rows(MSGCH_ACCOUNT, msgch_tables::OUTBOUND_ENVELOPES, msgch_abi,
                                  msgch_abi_types::OUTBOUND_ENVELOPE)) {
         if (row[msgch_fields::CHAIN_CODE].as_uint64() == chain_code) rows.push_back(std::move(row));
      }
      return rows;
   }

   /// The newest outbound envelope row for `chain_code` (the chain tip); null variant when none.
   fc::variant find_outbound_envelope(uint64_t chain_code) {
      auto rows = outbound_envelope_rows(chain_code);
      return rows.empty() ? fc::variant{} : std::move(rows.back());
   }

   /// The outbound envelope row for `chain_code` at `epoch_index`; null variant when absent.
   fc::variant outbound_envelope_for_epoch(uint64_t chain_code, uint32_t epoch_index) {
      for (auto& row : outbound_envelope_rows(chain_code)) {
         if (row[msgch_fields::EPOCH_INDEX].as<uint32_t>() == epoch_index) return std::move(row);
      }
      return fc::variant{};
   }

   /// Live outbound envelope rows for `chain_code`.
   uint32_t outbound_envelope_count(uint64_t chain_code) {
      return static_cast<uint32_t>(outbound_envelope_rows(chain_code).size());
   }

   /// Per-outpost consensus row (`outpcons`, primary key = chain_code); null variant when absent.
   fc::variant get_outpcons(uint64_t chain_code) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outpcons"_n, chain_code);
      return data.empty() ? fc::variant() : msgch_abi.binary_to_variant(
         "outpost_consensus_entry", data,
         fixture_abi_yield_function());
   }

   /// `account`'s unpaid batch-duty seat-seconds on `chain_code` in sysio.system; 0 when it holds none there.
   /// The row key is (account, chain), the `[scope][id]` layout `get_row_by_id` reads.
   uint64_t batch_duty_seat_seconds(name account, uint64_t chain_code) {
      const auto data = get_row_by_id(SYSIO_ACCOUNT, account, system_tables::BATCH_DUTY, chain_code);
      if (data.empty()) return 0;
      return sysio_abi.binary_to_variant(system_abi_types::BATCH_DUTY, data, fixture_abi_yield_function())
         [system_fields::SEAT_SECONDS].as<uint64_t>();
   }

   /// sysio.system's duty accounting singleton (open period, settling period).
   fc::variant duty_state() {
      const auto data = get_row_by_id(SYSIO_ACCOUNT, SYSIO_ACCOUNT, system_tables::DUTY_STATE,
                                      system_tables::DUTY_STATE.to_uint64_t());
      BOOST_REQUIRE(!data.empty());
      return sysio_abi.binary_to_variant(system_abi_types::DUTY_STATE, data, fixture_abi_yield_function());
   }

   /// WIRE credited to `account` in sysio.system's `payclaims` and not yet claimed; 0 when none.
   int64_t pay_claimable(name account) {
      const auto data = get_row_by_id(SYSIO_ACCOUNT, SYSIO_ACCOUNT, system_tables::PAY_CLAIMS, account.to_uint64_t());
      if (data.empty()) return 0;
      return static_cast<int64_t>(sysio_abi.binary_to_variant(system_abi_types::PAY_CLAIM, data,
                                                              fixture_abi_yield_function())
                                     [system_fields::BALANCE].as_uint64());
   }

   /// Tick the economic clock to its first payout that spends. The ticks through every boundary that has passed
   /// catch the clock up to its grid and spend nothing, the one that reaches it included, so the duty stays open;
   /// the next boundary's tick pays the open pay period for all the duty counted so far, opening its batch-duty
   /// settlement. Returns the economic epoch that paid.
   uint32_t tick_first_spending_payout() {
      uint32_t       ticked = 0;
      action_result  result;
      while ((result = push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::TICK_ECONOMY,
                            mvo()(epoch_fields::EPOCH_INDEX, ticked + 1))) == success()) {
         ++ticked;
      }
      BOOST_REQUIRE_NE(std::string::npos, result.find(ECONOMIC_BOUNDARY_NOT_PASSED_ERROR));
      BOOST_REQUIRE_GT(ticked, 0u);
      // The next boundary lies less than one economic epoch ahead.
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::TICK_ECONOMY,
                                          mvo()(epoch_fields::EPOCH_INDEX, ticked + 1)));
      produce_blocks();
      return ticked + 1;
   }

   /// sysio.system's audit row of the economic epoch that paid a period.
   fc::variant epoch_log(uint32_t economic_epoch_index) {
      const auto data = get_row_by_id(SYSIO_ACCOUNT, SYSIO_ACCOUNT, system_tables::EPOCH_LOG, economic_epoch_index);
      BOOST_REQUIRE(!data.empty());
      return sysio_abi.binary_to_variant(system_abi_types::EPOCH_LOG, data, fixture_abi_yield_function());
   }

   /// Return whether the epoch contract currently holds `chain_code`'s advancement for an open dispute.
   bool epoch_is_paused(uint64_t chain_code = ETH_OUTPOST_ID) {
      auto state = read_epoch_state(chain_code);
      BOOST_REQUIRE(!state.is_null());
      return state[epoch_fields::IS_PAUSED].as<bool>();
   }

   /// Return the dispute row by id, or null when the consensus path did not open one.
   fc::variant get_dispute(uint64_t dispute_id) {
      auto data = get_row_by_id(CHALG_ACCOUNT, CHALG_ACCOUNT, chalg_tables::DISPUTES, dispute_id);
      return data.empty() ? fc::variant() : chalg_abi.binary_to_variant(
         chalg_abi_types::DISPUTE_ENTRY, data,
         fixture_abi_yield_function());
   }

   /// Assert that the `evalcons -> opendispute` route, reached through `deliver` or `chkcons`, recorded the
   /// exact split as dispute `dispute_id` on `chain_code`.
   void assert_open_tie_dispute(uint32_t expected_epoch,
                                const std::vector<fc::sha256>& expected_checksums,
                                const std::vector<uint32_t>& expected_operator_counts,
                                uint64_t chain_code = ETH_OUTPOST_ID,
                                uint64_t dispute_id = FIRST_DISPUTE_ID) {
      BOOST_REQUIRE_EQUAL(expected_checksums.size(), expected_operator_counts.size());
      const auto dispute = get_dispute(dispute_id);
      BOOST_REQUIRE(!dispute.is_null());
      BOOST_REQUIRE_EQUAL(
         dispute[chalg_fields::STATUS].as<opp::types::DisputeStatus>(),
         opp::types::DisputeStatus::DISPUTE_STATUS_OPEN);
      BOOST_REQUIRE_EQUAL(dispute[chalg_fields::CHAIN_CODE].as_uint64(), chain_code);
      BOOST_REQUIRE_EQUAL(dispute[chalg_fields::EPOCH_INDEX].as<uint32_t>(), expected_epoch);

      const auto candidates = dispute[chalg_fields::CANDIDATES].get_array();
      BOOST_REQUIRE_EQUAL(candidates.size(), expected_checksums.size());
      for (size_t index = 0; index < candidates.size(); ++index) {
         BOOST_REQUIRE_EQUAL(candidates[index][chalg_fields::CHECKSUM].as_string(),
                             expected_checksums[index].str());
         BOOST_REQUIRE_EQUAL(candidates[index][chalg_fields::OPERATORS].get_array().size(),
                             expected_operator_counts[index]);
      }
   }

   /// Cast the sole seeded Tier-1 vote for a two-version split's expected winning envelope.
   action_result vote_dispute(uint64_t dispute_id, const fc::sha256& chosen_checksum) {
      return push(CHALG_ACCOUNT, chalg_abi, NODE_DADDY, chalg_actions::VOTE_DISPUTE, mvo()
         (chalg_vote_fields::OWNER, NODE_DADDY.to_string())
         (chalg_vote_fields::DISPUTE_ID, dispute_id)
         (chalg_vote_fields::CHOSEN_CHECKSUM, chosen_checksum));
   }

   /// Resolve dispute `dispute_id` on `chain_code` and verify that its selected envelope is durably
   /// accepted by msgch.
   void resolve_tie_dispute(uint32_t expected_epoch, const fc::sha256& expected_winner,
                            uint64_t chain_code = ETH_OUTPOST_ID, uint64_t dispute_id = FIRST_DISPUTE_ID) {
      BOOST_REQUIRE_EQUAL(success(), vote_dispute(dispute_id, expected_winner));
      BOOST_REQUIRE_EQUAL(success(), push(CHALG_ACCOUNT, chalg_abi, BATCHOP,
                                         chalg_actions::CHECK_DISPUTE,
                                         mvo()(chalg_vote_fields::DISPUTE_ID, dispute_id)));

      const auto dispute = get_dispute(dispute_id);
      BOOST_REQUIRE_EQUAL(
         dispute[chalg_fields::STATUS].as<opp::types::DisputeStatus>(),
         opp::types::DisputeStatus::DISPUTE_STATUS_RESOLVED);
      const auto consensus = get_outpcons(chain_code);
      BOOST_REQUIRE(!consensus.is_null());
      BOOST_REQUIRE_EQUAL(consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>(), expected_epoch);
      BOOST_REQUIRE_EQUAL(consensus[msgch_fields::WINNING_CHECKSUM].as_string(), expected_winner.str());
   }

   /// Inbound delivery metadata for one (outpost, epoch, batch operator), or null when absent.
   /// Consensus deliberately clears only raw_data, leaving this row for advance() to classify.
   fc::variant find_inbound_delivery(uint64_t chain_code, uint32_t epoch_index, name batch_op,
                                     uint64_t scan_until = TABLE_SCAN_LIMIT) {
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, msgch_tables::ENVELOPES, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            msgch_abi_types::ENVELOPE_ENTRY, data,
            fixture_abi_yield_function());
         if (row[msgch_fields::CHAIN_CODE].as_uint64() == chain_code &&
             row[msgch_fields::EPOCH_INDEX].as<uint32_t>() == epoch_index &&
             row[msgch_fields::BATCH_OP_NAME].as_string() == batch_op.to_string()) return row;
      }
      return fc::variant{};
   }

   /// Every row of `code`'s `table` in primary-key order, decoded as `abi_type`. Read straight from
   /// the KV store so the walk does not depend on ids staying inside a fixed probe range.
   std::vector<fc::variant> table_rows(name code, name table, const abi_serializer& abi, const char* abi_type) {
      return sysio_system::test_support::table_rows(*this, code, table, abi, abi_type);
   }

   /// Every inbound `envelopes` row in primary-key order.
   std::vector<fc::variant> envelope_rows() {
      return table_rows(MSGCH_ACCOUNT, msgch_tables::ENVELOPES, msgch_abi, msgch_abi_types::ENVELOPE_ENTRY);
   }

   /// `envelopes` rows recorded for (`chain_code`, `epoch_index`), across operators.
   uint32_t envelope_rows_for(uint64_t chain_code, uint32_t epoch_index) {
      uint32_t n = 0;
      for (const auto& row : envelope_rows()) {
         if (row[msgch_fields::CHAIN_CODE].as_uint64() == chain_code &&
             row[msgch_fields::EPOCH_INDEX].as<uint32_t>() == epoch_index) ++n;
      }
      return n;
   }

   /// `envelopes` rows recorded for `epoch_index`, across outposts and operators.
   uint32_t envelope_rows_for_epoch(uint32_t epoch_index) {
      uint32_t n = 0;
      for (const auto& row : envelope_rows()) {
         if (row[msgch_fields::EPOCH_INDEX].as<uint32_t>() == epoch_index) ++n;
      }
      return n;
   }

   /// Stored `byoutepoch` secondary entries of `envelopes`; equals the row count unless an erase
   /// left an orphan behind.
   size_t envelope_index_entries() {
      const auto  table_id = compute_sec_table_id(msgch_tables::ENVELOPES.to_uint64_t(),
                                                  msgch_indexes::BY_OUTPOST_EPOCH.to_uint64_t());
      size_t n = 0;
      auto cursor = control->kv().open_index_cursor();
      chain::kv_index_entry entry;
      for (bool more = cursor->seek_lower({MSGCH_ACCOUNT, table_id}, entry);
           more && entry.code == MSGCH_ACCOUNT && entry.table_id == table_id; more = cursor->next(entry)) {
         ++n;
      }
      return n;
   }

   /// Chain tip of the inbound stream when a test feeds both outposts identical envelopes: the next
   /// delivery continues from the last accepted envelope's digest and message id (raw 32-byte
   /// strings, empty at stream genesis).
   struct inbound_stream_tip {
      std::string envelope_digest;
      std::string message_id;
   };

   /// Current-epoch envelope carrying `payload`, chained from `tip`.
   std::vector<char> encode_chained_delivery(std::string_view payload, const inbound_stream_tip& tip) {
      return encode_delivery(current_epoch(), std::string(payload), tip.envelope_digest,
                             tip.message_id);
   }

   /// `tip` after `envelope` is accepted.
   inbound_stream_tip next_stream_tip(const std::vector<char>& envelope) {
      return {oracle::digest_bytes(oracle::epoch_digest(decode_envelope(envelope))),
              delivery_message_id(envelope)};
   }

   /// Every operator in `ops` delivers the same chained envelope to both outposts, so each outpost
   /// reaches consensus for the current epoch; `tip` moves to the delivered envelope.
   void deliver_unanimous_epoch(const std::vector<name>& ops, std::string_view payload,
                                inbound_stream_tip& tip) {
      const auto envelope = encode_chained_delivery(payload, tip);
      for (const uint64_t outpost : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
         for (const auto& op : ops) {
            BOOST_REQUIRE_EQUAL(success(), deliver_as(op, outpost, envelope));
         }
      }
      produce_blocks();
      tip = next_stream_tip(envelope);
   }

   /// Every operator in `ops` delivers the same envelope to `chain_code` for that chain's current epoch,
   /// chained from `tips[chain_code]`, which moves to the delivered envelope.
   void deliver_chain_unanimous(const std::vector<name>& ops, uint64_t chain_code, std::string_view payload,
                                std::map<uint64_t, inbound_stream_tip>& tips) {
      inbound_stream_tip& tip      = tips[chain_code];
      const auto          envelope = encode_delivery(current_epoch(chain_code), std::string(payload),
                                                     tip.envelope_digest, tip.message_id);
      for (const auto& op : ops) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, chain_code, envelope));
      produce_blocks();
      tip = next_stream_tip(envelope);
   }

   /// Make the chain due and advance through the production `chkcons -> advance` route.
   void close_epoch() {
      const uint32_t epoch = current_epoch();
      elapse_epoch_boundary();
      advance_via_consensus();
      BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   }

   /// Count attestation rows recorded for (`chain_code`, `epoch_index`); the observable effect
   /// of an ACCEPTED inbound envelope (rows are emplaced before dispatch, even for types
   /// dispatch drops as out of scope).
   uint32_t attestation_count(uint64_t chain_code, uint32_t epoch_index,
                              uint64_t scan_until = TABLE_SCAN_LIMIT) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            "attestation_entry", data,
            fixture_abi_yield_function());
         if (row["chain_code"].as_uint64() == chain_code &&
             row["epoch_index"].as<uint32_t>() == epoch_index) ++n;
      }
      return n;
   }

   /// Attestation rows for `chain_code` with `status`, over the whole table.
   uint32_t attestation_status_count(uint64_t chain_code, opp::types::AttestationStatus status) {
      uint32_t n = 0;
      for (const auto& row : table_rows(MSGCH_ACCOUNT, msgch_tables::ATTESTATIONS, msgch_abi,
                                        msgch_abi_types::ATTESTATION_ENTRY)) {
         if (row[msgch_fields::CHAIN_CODE].as_uint64() == chain_code &&
             row[msgch_fields::STATUS].as<opp::types::AttestationStatus>() == status) ++n;
      }
      return n;
   }

   // -- Inbound envelope builder --

   /// One attestation of an inbound envelope.
   struct inbound_attestation {
      sysio::opp::types::AttestationType type;
      std::string                        data;
   };

   /// Encode a deliverable envelope carrying `attestations` in one message. The semantic header is
   /// derived per the spec — `apply_consensus` drops envelopes whose header fields do not recompute
   /// or whose message does not continue the per-outpost message chain. `prev`
   /// (previous_envelope_hash), `prev_message_id`, and `env_hash` are raw 32-byte strings (or empty
   /// for stream genesis).
   std::vector<char> encode_delivery(uint32_t epoch_index, const std::vector<inbound_attestation>& attestations,
                                     const std::string& prev = {},
                                     const std::string& prev_message_id = {},
                                     const std::string& env_hash = {}) {
      sysio::opp::Envelope env;
      env.set_epoch_index(epoch_index);
      env.set_epoch_envelope_index(1);
      env.set_epoch_timestamp(1'775'612'516'983ULL);
      if (!prev.empty()) env.set_previous_envelope_hash(prev);
      if (!env_hash.empty()) env.set_envelope_hash(env_hash);
      auto* payload = env.add_messages()->mutable_payload();
      payload->set_version(1); // The production emitters explicitly stamp V1.
      for (const auto& attestation : attestations) {
         auto* att = payload->add_attestations();
         att->set_type(attestation.type);
         att->set_data(attestation.data);
         att->set_data_size(static_cast<uint32_t>(attestation.data.size()));
      }
      oracle::finalize_header(*env.mutable_messages(0), prev_message_id, 1'775'612'516'983ULL);
      std::vector<char> out(env.ByteSizeLong());
      env.SerializeToArray(out.data(), static_cast<int>(out.size()));
      return out;
   }

   /// Encode a deliverable envelope carrying one OPERATORS roster echo (dispatch drops
   /// the attestation silently; acceptance is still fully observable via `outpcons` and the
   /// stored attestation row).
   std::vector<char> encode_delivery(uint32_t epoch_index, const std::string& att_data,
                                     const std::string& prev = {},
                                     const std::string& prev_message_id = {},
                                     const std::string& env_hash = {}) {
      const std::vector<inbound_attestation> attestations{
         {sysio::opp::types::ATTESTATION_TYPE_OPERATORS, att_data}};
      return encode_delivery(epoch_index, attestations, prev, prev_message_id, env_hash);
   }

   /// Decode the raw bytes back into the pb Envelope (asserts success).
   sysio::opp::Envelope decode_envelope(const std::vector<char>& raw) {
      sysio::opp::Envelope env;
      BOOST_REQUIRE(env.ParseFromArray(raw.data(), static_cast<int>(raw.size())));
      return env;
   }

   /// The raw 32-byte `message_id` of a delivery's single message — the value the NEXT delivery
   /// on the same outpost stream must carry in `previous_message_id`.
   std::string delivery_message_id(const std::vector<char>& raw) {
      return decode_envelope(raw).messages(0).header().message_id();
   }

   /// The decoded BATCH_OPERATOR_GROUPS attestation the most recent `advance` shipped to
   /// `chain_code`. Read from the surviving `outenvelopes` row, NOT the msgch `attestations`
   /// working table — `advance` queues its schedule attestations via `queueout` and then fans out
   /// `buildenv` inline in the SAME transaction, and `buildenv` packs the READY rows into the
   /// outbound envelope and ERASES them before the transaction ends. The
   /// newest `outenvelopes` row is therefore the only durable copy of what `advance`
   /// emitted; scanning `attestations` after an advance always finds zero rows.
   sysio::opp::attestations::BatchOperatorGroups shipped_batch_operator_groups(uint64_t chain_code) {
      auto row = find_outbound_envelope(chain_code);
      BOOST_REQUIRE(!row.is_null());
      auto env = decode_envelope(row["raw_envelope"].as<std::vector<char>>());
      BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
      // Scans to the LAST match, not the first. `buildenv`'s trim loop pops
      // overflowing entries off the end and leaves them READY to ride the next
      // envelope, so one envelope can legitimately carry a DEFERRED
      // BATCH_OPERATOR_GROUPS from epoch N -- lower attestation id, therefore
      // packed first -- ahead of epoch N+1's fresh one. Depot semantics are
      // last-write-wins (the outpost applies each in order and the final one
      // survives), so taking the first would silently assert against the stale
      // payload the moment a trim occurred.
      bool                                          found = false;
      sysio::opp::attestations::BatchOperatorGroups groups_attestation;
      for (const auto& att : env.messages(0).payload().attestations()) {
         if (att.type() != sysio::opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS) continue;
         BOOST_REQUIRE(groups_attestation.ParseFromArray(att.data().data(),
                                                         static_cast<int>(att.data().size())));
         found = true;
      }
      if (!found) {
         BOOST_FAIL("outbound envelope carries no BATCH_OPERATOR_GROUPS attestation");
      }
      return groups_attestation;
   }

   /// The OPERATORS roster `envelope_row` carries, if any: the last one, which the outpost applies last.
   /// When the envelope also carries BATCH_OPERATOR_GROUPS, the roster comes first so the groups resolve
   /// against it.
   std::optional<std::string> carried_roster(const fc::variant& envelope_row) {
      const auto env = decode_envelope(envelope_row["raw_envelope"].as<std::vector<char>>());
      BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
      std::optional<std::string> roster;
      int roster_at = -1;
      int groups_at = -1;
      int position  = 0;
      for (const auto& att : env.messages(0).payload().attestations()) {
         if (att.type() == opp::types::ATTESTATION_TYPE_OPERATORS) {
            roster    = att.data();
            roster_at = position;
         } else if (att.type() == opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS) {
            groups_at = position;
         }
         ++position;
      }
      if (roster && groups_at >= 0) BOOST_REQUIRE_LT(roster_at, groups_at);
      return roster;
   }

   /// Whether the newest outbound envelope for `chain_code` carries an OPERATORS roster.
   bool tip_carries_roster(uint64_t chain_code) {
      const auto row = find_outbound_envelope(chain_code);
      BOOST_REQUIRE(!row.is_null());
      return carried_roster(row).has_value();
   }

   /// The `rostersent` row for `chain_code` (primary key = chain_code); null variant when absent.
   fc::variant get_roster_sent(uint64_t chain_code) {
      auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "rostersent"_n, chain_code);
      return data.empty() ? fc::variant() : epoch_abi.binary_to_variant(
         "roster_sent", data, fixture_abi_yield_function());
   }

   /// The OPERATORS roster `chain_code` holds -- the one the newest envelope carrying a roster shipped --
   /// checked against the schedule after inline mutations: it lists exactly the window's members that
   /// have an opreg row, in account order, each with its current status, and `rostersent` holds its hash.
   /// Returns account name -> status.
   std::map<std::string, opp::types::OperatorStatus> require_roster_is_window(uint64_t chain_code) {
      std::optional<std::string> shipped;
      const auto rows = outbound_envelope_rows(chain_code);
      for (auto it = rows.rbegin(); it != rows.rend() && !shipped; ++it) shipped = carried_roster(*it);
      BOOST_REQUIRE_MESSAGE(shipped, "no outbound envelope carries an OPERATORS roster");

      const auto sent = get_roster_sent(chain_code);
      BOOST_REQUIRE(!sent.is_null());
      BOOST_REQUIRE_EQUAL(sent["roster_hash"].as_string(), fc::sha256::hash(shipped->data(), shipped->size()).str());

      opp::attestations::Operators operators;
      BOOST_REQUIRE(operators.ParseFromString(*shipped));
      std::map<std::string, opp::types::OperatorStatus> roster;
      std::vector<name> order;
      for (const auto& entry : operators.operators()) {
         roster[entry.account().name()] = entry.status();
         order.emplace_back(entry.account().name());
      }
      BOOST_REQUIRE(std::is_sorted(order.begin(), order.end()));

      // Held in a local: a range-for over a member of the temporary would read freed memory.
      const auto state = read_epoch_state(chain_code);
      std::set<std::string> window;
      for (const auto& group : state[epoch_fields::BATCH_OP_GROUPS].get_array()) {
         for (const auto& member : group.get_array()) {
            if (!get_operator(name{member.as_string()}).is_null()) window.insert(member.as_string());
         }
      }
      BOOST_REQUIRE_EQUAL(roster.size(), window.size());
      for (const auto& [account, status] : roster) {
         BOOST_REQUIRE_MESSAGE(window.count(account), account << " is in the roster but not seated");
         BOOST_REQUIRE_EQUAL(status, get_operator(name{account})["status"].as<opp::types::OperatorStatus>());
      }
      return roster;
   }

   /// How many BATCH_OPERATOR_GROUPS attestations the most recent `advance` shipped to
   /// `chain_code` -- 0 when the depot WITHHELD it. Distinct from
   /// `shipped_batch_operator_groups`, which fails the test on absence: the withhold path
   /// needs to assert absence while still proving the envelope itself was built (i.e. that
   /// `advance` skipped only this queueout and went on to emit the epoch's other
   /// attestations, rather than returning early).
   int shipped_batch_operator_groups_count(uint64_t chain_code) {
      auto row = find_outbound_envelope(chain_code);
      BOOST_REQUIRE(!row.is_null());
      auto env = decode_envelope(row["raw_envelope"].as<std::vector<char>>());
      BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
      int count = 0;
      for (const auto& att : env.messages(0).payload().attestations()) {
         if (att.type() == sysio::opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS) ++count;
      }
      return count;
   }

   /// Total attestations in the most recent outbound envelope for `chain_code`.
   int shipped_attestation_count(uint64_t chain_code) {
      auto row = find_outbound_envelope(chain_code);
      BOOST_REQUIRE(!row.is_null());
      auto env = decode_envelope(row["raw_envelope"].as<std::vector<char>>());
      BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
      return env.messages(0).payload().attestations_size();
   }

   // -- SEC-28 rotation-termination helpers (drive recorddel/termcheck through the REAL advance) --

   /// The opreg operator row for `account` (status + status_reason), or null when absent.
   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "operator_entry", data, fixture_abi_yield_function());
   }

   /// Count successful SLASH audit entries for `account` routed to `chain_code`.
   /// opreg emits and logs one action for each immediately slashable collateral balance.
   uint32_t slash_action_count(name account, uint64_t chain_code) {
      auto op = get_operator(account);
      if (op.is_null()) return 0;

      using operator_action_type = sysio::opp::attestations::OperatorAction_ActionType;
      uint32_t n = 0;
      for (const auto& log : op[opreg_fields::RECENT_ACTIONS].get_array()) {
         operator_action_type action_type =
            sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_UNKNOWN;
         const auto& action = log[opreg_fields::ACTION];
         const bool is_slash =
            sysio::opp::attestations::OperatorAction_ActionType_Parse(
               action[opreg_fields::ACTION_TYPE].as_string(), &action_type) &&
            action_type == sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_SLASH;
         if (!log[opreg_fields::SUCCESS].as_bool() || !is_slash ||
             action[opreg_fields::CHAIN_CODE].as_uint64() != chain_code) continue;
         ++n;
      }
      return n;
   }

   /// Every `dellog` row for `account`, oldest first.
   std::vector<fc::variant> dellog_rows(name account) {
      std::vector<fc::variant> rows;
      for (auto& row : table_rows(OPREG_ACCOUNT, opreg_tables::DELIVERY_LOG, opreg_abi,
                                  opreg_abi_types::DELIVERY_LOG_ENTRY)) {
         if (row[opreg_fields::ACCOUNT].as_string() == account.to_string()) rows.push_back(std::move(row));
      }
      return rows;
   }

   /// `account`'s `dellog` rows for `epoch_index`, of every chain.
   std::vector<fc::variant> dellog_rows(name account, uint32_t epoch_index) {
      std::vector<fc::variant> rows;
      for (auto& row : dellog_rows(account)) {
         if (row[opreg_fields::EPOCH].as<uint32_t>() == epoch_index) rows.push_back(std::move(row));
      }
      return rows;
   }

   /// `account`'s `dellog` rows for `chain_code`'s epoch `epoch_index`.
   std::vector<fc::variant> dellog_rows(name account, uint32_t epoch_index, uint64_t chain_code) {
      std::vector<fc::variant> rows;
      for (auto& row : dellog_rows(account, epoch_index)) {
         if (fc::slug_name{row[opreg_fields::CHAIN_CODE].as_string()}.value == chain_code) {
            rows.push_back(std::move(row));
         }
      }
      return rows;
   }

   /// `account`'s `servchains` row on `chain_code`; null when it has none.
   fc::variant membership(name account, uint64_t chain_code) {
      return sysio_system::test_support::served_chains::membership(*this, opreg_abi, account,
                                                                    fc::slug_name{chain_code}.to_string());
   }

   /// `account`'s membership status on `chain_code`, as the ABI spells it; empty when it has none.
   std::string membership_status(name account, uint64_t chain_code) {
      const auto row = membership(account, chain_code);
      return row.is_null() ? std::string{} : row[opreg_fields::STATUS].as_string();
   }

   /// Count DELIVERED dellog rows still present for `account`. recorddel PRUNES (erases) rows that
   /// have aged out of the rolling window, so a surviving delivered row proves that record is still
   /// inside the window -- the direct check that the edge anchor was not pruned.
   uint32_t delivered_dellog_count(name account) {
      uint32_t n = 0;
      for (const auto& row : dellog_rows(account)) {
         if (row[opreg_fields::DELIVERED].as<bool>()) ++n;
      }
      return n;
   }

   /// `chain_code`'s `epochstate` row; null variant before it has one.
   fc::variant read_epoch_state(uint64_t chain_code = ETH_OUTPOST_ID) {
      auto data = get_row_by_id(EPOCH_ACCOUNT, EPOCH_ACCOUNT, epoch_tables::EPOCH_STATE, chain_code);
      return data.empty() ? fc::variant() : epoch_abi.binary_to_variant(
         "epoch_state", data, fixture_abi_yield_function());
   }

   /// The single batch operator on duty for `chain_code`'s CURRENT epoch (rotation setup runs one op
   /// per group); name{} when the schedule is empty.
   name duty_member(uint64_t chain_code = ETH_OUTPOST_ID) {
      auto st = read_epoch_state(chain_code);
      if (st.is_null()) return name{};
      auto groups = st["batch_op_groups"].get_array();
      uint64_t cur = st["current_batch_op_group"].as_uint64();
      if (cur >= groups.size()) return name{};
      auto members = groups[cur].get_array();
      return members.empty() ? name{} : name(members[0].as_string());
   }

   /// True while `account` holds a seat in any group of `chain_code`'s schedule window.
   bool seated_on(name account, uint64_t chain_code) {
      // Held in a local: a range-for over a member of the temporary would read freed memory.
      const auto state = read_epoch_state(chain_code);
      if (state.is_null()) return false;
      for (const auto& group : state[epoch_fields::BATCH_OP_GROUPS].get_array()) {
         for (const auto& member : group.get_array()) {
            if (member.as_string() == account.to_string()) return true;
         }
      }
      return false;
   }

   /// The epochs whose groups in `chain_code`'s schedule window seat `account`, in window order: the window holds
   /// the current epoch's group, then the next epochs' in turn.
   std::vector<uint32_t> seated_epochs(name account, uint64_t chain_code) {
      std::vector<uint32_t> epochs;
      // Held in a local: a range-for over a member of the temporary would read freed memory.
      const auto state = read_epoch_state(chain_code);
      if (state.is_null()) return epochs;
      const uint32_t      current = state[epoch_fields::CURRENT_EPOCH_INDEX].as<uint32_t>();
      const fc::variants& groups  = state[epoch_fields::BATCH_OP_GROUPS].get_array();
      for (size_t g = 0; g < groups.size(); ++g) {
         for (const auto& member : groups[g].get_array()) {
            if (member.as_string() == account.to_string()) epochs.push_back(current + static_cast<uint32_t>(g));
         }
      }
      return epochs;
   }

   /// `chain_code`'s `previous_group`: the group that served the epoch before its current one.
   std::vector<name> previous_group(uint64_t chain_code = ETH_OUTPOST_ID) {
      const auto state = read_epoch_state(chain_code);
      BOOST_REQUIRE(!state.is_null());
      return state[epoch_fields::PREVIOUS_GROUP].as<std::vector<name>>();
   }

   /// The epoch whose books `chain_code`'s latest advance closed: the one before the epoch that advance closed, or 0
   /// before the chain's third epoch.
   uint32_t booked_epoch(uint64_t chain_code = ETH_OUTPOST_ID) {
      const uint32_t current = current_epoch(chain_code);
      return current > 2 ? current - 2 : 0;
   }

   /// True while `account` holds a seat in any configured chain's schedule, as `regoperator` reads it: in a group
   /// of its window, or in its `previous_group`, whose books the chain's next advance closes.
   bool holds_schedule_seat(name account) {
      for (const std::string_view chain : epoch_chains) {
         const uint64_t          chain_code = fc::slug_name{chain}.value;
         const std::vector<name> previous   = previous_group(chain_code);
         if (seated_on(account, chain_code) ||
             std::find(previous.begin(), previous.end(), account) != previous.end()) return true;
      }
      return false;
   }

   /// Advance exactly one epoch, landing the advance at the chain's `due_at`. Traffic recorded since the epoch
   /// start makes the chain due exactly one minimum after it, and the advance opens the next epoch at that
   /// instant, so every epoch's records are spaced exactly epoch_duration apart -- the exact-span timing the
   /// rotation tests pin the window's edge against, with no block-granularity drift.
   uint32_t advance_to_next_epoch() {
      advance_to_next_epoch_traces();
      return current_epoch();
   }

   /// `advance_to_next_epoch`, returning each configured chain's `advance` trace, in `epoch_chains`
   /// order, so a test can inspect its inline actions. The chains share one cadence.
   std::vector<transaction_trace_ptr> advance_to_next_epoch_traces() {
      mark_chain_traffic();
      const fc::variant st = read_epoch_state(fc::slug_name{epoch_chains.front()}.value);
      BOOST_REQUIRE(!st.is_null());
      const fc::time_point due = st[epoch_fields::DUE_AT].as<fc::time_point>();
      BOOST_REQUIRE(due != fc::time_point());
      if (due > control->pending_block_time()) open_block_at(due);
      std::vector<transaction_trace_ptr> traces;
      for (const std::string_view chain : epoch_chains) {
         traces.push_back(sysio_system::test_support::push_contract_action_trace(
            *this, EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT, epoch_actions::ADVANCE,
            mvo()(epoch_fields::CHAIN_CODE, chain)));
      }
      produce_blocks();
      return traces;
   }

   /// Push `advance` for `chain_code` under sysio.epoch's own authority, the governance path.
   action_result governance_advance(uint64_t chain_code = ETH_OUTPOST_ID) {
      return push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                  mvo()(epoch_fields::CHAIN_CODE, fc::slug_name{chain_code}.to_string()));
   }

   /// Make `chain_code` due and advance it under `authority`, returning the advance's trace. sysio.msgch's stands in
   /// for `chkcons`, with or without a decision on the chain's current epoch; sysio.epoch's own, the governance path,
   /// needs that epoch decided unless the chain is no longer an active outpost. An epoch's books close at the advance
   /// after it, so a test runs this to record the epoch the chain's previous advance closed.
   transaction_trace_ptr advance_trace(uint64_t chain_code, name authority) {
      const uint32_t epoch = current_epoch(chain_code);
      elapse_epoch_boundary();
      const transaction_trace_ptr trace = sysio_system::test_support::push_contract_action_trace(
         *this, EPOCH_ACCOUNT, epoch_abi, authority, epoch_actions::ADVANCE,
         mvo()(epoch_fields::CHAIN_CODE, fc::slug_name{chain_code}.to_string()));
      produce_blocks();
      BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch(chain_code));
      return trace;
   }

   /// `advance_trace` under sysio.epoch's own authority, the governance path.
   transaction_trace_ptr governance_advance_trace(uint64_t chain_code = ETH_OUTPOST_ID) {
      return advance_trace(chain_code, EPOCH_ACCOUNT);
   }

   /// One `sysio.chalg::slashop` a transaction ran: the operator it slashed, the reason it gave, and whether
   /// it carried exactly sysio.msgch's active authority.
   struct slashop_record {
      name        operator_account;
      std::string reason;
      bool        authorized_by_msgch = false;
   };

   /// Every `sysio.chalg::slashop` `trace` ran, inline actions included, in execution order.
   std::vector<slashop_record> slashops(const transaction_trace_ptr& trace) {
      BOOST_REQUIRE(trace);
      const std::vector<permission_level> msgch_active{{MSGCH_ACCOUNT, config::active_name}};
      std::vector<slashop_record> records;
      for (const auto& at : trace->action_traces) {
         if (at.receiver != CHALG_ACCOUNT || at.act.account != CHALG_ACCOUNT ||
             at.act.name != chalg_actions::SLASH_OPERATOR) continue;
         const fc::variant data = chalg_abi.binary_to_variant(chalg_abi_types::SLASH_OPERATOR, at.act.data,
                                                               fixture_abi_yield_function());
         records.push_back({.operator_account    = name(data[chalg_slash_fields::OPERATOR_ACCOUNT].as_string()),
                            .reason              = data[chalg_slash_fields::REASON].as_string(),
                            .authorized_by_msgch = at.act.authorization == msgch_active});
      }
      return records;
   }

   /// The reason sysio.msgch gives the slash of a delivery to `chain_code`'s epoch `epoch_index` that differs
   /// from the epoch's winner.
   static std::string non_canonical_delivery_reason(std::string_view chain_code, uint32_t epoch_index) {
      return std::string(NON_CANONICAL_DELIVERY_REASON_PREFIX) + std::string(chain_code) +
             std::string(NON_CANONICAL_DELIVERY_REASON_EPOCH) + std::to_string(epoch_index);
   }

   /// `account`'s sysio.opreg status.
   opp::types::OperatorStatus operator_status(name account) {
      return get_operator(account)[opreg_fields::STATUS].as<opp::types::OperatorStatus>();
   }

   /// How many times `account` executed its `action` in `trace`, inline actions included.
   static uint32_t action_count(const transaction_trace_ptr& trace, name account, name action) {
      BOOST_REQUIRE(trace);
      uint32_t n = 0;
      for (const auto& at : trace->action_traces) {
         if (at.receiver == account && at.act.account == account && at.act.name == action) ++n;
      }
      return n;
   }

   static fc::variant make_chain_min_bond(std::string_view chain_code, std::string_view token_code,
                                          uint64_t min_bond) {
      return fc::variant(mvo()
         ("chain_code",          chain_code)
         ("token_code",          token_code)
         ("min_bond",            min_bond)
         ("config_timestamp_ms", uint64_t{0}));
   }

   /// Reconfigure only the rolling-termination rails while preserving the WNS-16 fixture's
   /// collateral requirements. This lets the test build real history under a permissive policy,
   /// then demonstrate that deferred slashing wins when the same history becomes terminating.
   void set_termination_thresholds(uint32_t max_consecutive_misses, uint32_t max_percent_misses,
                                   uint32_t min_percent_samples = DEFAULT_MIN_PERCENT_SAMPLES) {
      constexpr uint64_t TERMINATE_PRUNE_DELAY_MS   = 600'000;
      constexpr uint64_t TERMINATE_WINDOW_MS        = 24ULL * 60 * 60 * 1000;

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         opreg_actions::SET_CONFIG, mvo()
            (opreg_config_fields::TERMINATE_PRUNE_DELAY_MS,         TERMINATE_PRUNE_DELAY_MS)
            (opreg_config_fields::TERMINATE_MAX_CONSECUTIVE_MISSES, max_consecutive_misses)
            (opreg_config_fields::TERMINATE_MAX_PERCENT_MISSES_24H, max_percent_misses)
            (opreg_config_fields::TERMINATE_MIN_PERCENT_SAMPLES,    min_percent_samples)
            (opreg_config_fields::TERMINATE_WINDOW_MS,              TERMINATE_WINDOW_MS)
            (opreg_config_fields::REQUIRED_PRODUCER_COLLATERAL,     fc::variants{})
            (opreg_config_fields::REQUIRED_BATCH_OPERATOR_COLLATERAL, fc::variants{
               make_chain_min_bond(WIRE_CHAIN_CODE, WIRE_CHAIN_CODE, BATCH_OPERATOR_MINIMUM_COLLATERAL) })
            (opreg_config_fields::REQUIRED_UNDERWRITER_COLLATERAL,  fc::variants{})));
   }

   /// Crank the production `chkcons -> advance` route for every configured chain: each chain with a
   /// durable consensus row for its current epoch advances, recording delivery history and evaluating
   /// termination.
   void advance_via_consensus() {
      for (const std::string_view chain : epoch_chains) {
         BOOST_REQUIRE_EQUAL(success(), check_consensus(fc::slug_name{chain}.value));
      }
      produce_blocks();
   }

   /// Push the permissionless `chkcons` crank for `chain_code`, as `signer`.
   action_result check_consensus(uint64_t chain_code, name signer = MSGCH_ACCOUNT) {
      return push(MSGCH_ACCOUNT, msgch_abi, signer, msgch_actions::CHECK_CONSENSUS,
                  mvo()(msgch_fields::CHAIN_CODE, chain_code));
   }

   /// `check_consensus`, returning the transaction's trace so a test can inspect what ran inline in it.
   transaction_trace_ptr check_consensus_trace(uint64_t chain_code, name signer) {
      return sysio_system::test_support::push_contract_action_trace(
         *this, MSGCH_ACCOUNT, msgch_abi, signer, msgch_actions::CHECK_CONSENSUS,
         mvo()(msgch_fields::CHAIN_CODE, chain_code));
   }

   /// `deliver_as`, returning the transaction's trace so a test can inspect what ran inline in it.
   transaction_trace_ptr deliver_trace(name op, uint64_t chain_code, const std::vector<char>& data) {
      return sysio_system::test_support::push_contract_action_trace(
         *this, MSGCH_ACCOUNT, msgch_abi, op, msgch_actions::DELIVER,
         mvo()(msgch_fields::BATCH_OP_NAME, op.to_string())(msgch_fields::CHAIN_CODE, chain_code)
              (msgch_fields::DATA, data));
   }

   /// `chain_code`'s `outpcons` row records `winner` as the accepted winner of `epoch_index`, its current epoch.
   void require_outpcons_winner(uint64_t chain_code, uint32_t epoch_index, const fc::sha256& winner) {
      const fc::variant consensus = get_outpcons(chain_code);
      BOOST_REQUIRE(!consensus.is_null());
      BOOST_REQUIRE_EQUAL(epoch_index, consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(winner.str(), consensus[msgch_fields::WINNING_CHECKSUM].as_string());
   }

   /// Whether `chain_code`'s `outpcons` row records an accepted winner for `epoch_index`.
   bool epoch_decided(uint64_t chain_code, uint32_t epoch_index) {
      const fc::variant consensus = get_outpcons(chain_code);
      return !consensus.is_null() && consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>() == epoch_index;
   }

   /// Whether the pending block still precedes `chain_code`'s minimum epoch end, its epoch start plus its minimum
   /// duration, before which the chain cannot be due.
   bool before_epoch_boundary(uint64_t chain_code = ETH_OUTPOST_ID) {
      return control->pending_block_time() <
             read_epoch_state(chain_code)[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>() +
                minimum_duration(chain_code);
   }

   /// Land the pending transactions, then produce blocks so the pending block opens at `target`, a later
   /// multiple of the block interval.
   void open_block_at(fc::time_point target) {
      produce_blocks();
      const fc::microseconds block_interval = fc::milliseconds(config::block_interval_ms);
      if (control->pending_block_time() != target) {
         const fc::microseconds skip = target - block_interval - control->head().block_time();
         BOOST_REQUIRE_GE(skip.count(), block_interval.count());
         produce_block(skip);
      }
      BOOST_REQUIRE(control->pending_block_time() == target);
   }

   /// Slash `account` through sysio.opreg, as sysio.chalg does: it leaves the eligible set mid-epoch.
   void slash_operator(name account) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, opreg_actions::SLASH,
         mvo()(opreg_fields::ACCOUNT, account.to_string())(opreg_fields::REASON, std::string(SLASH_REASON))));
      produce_blocks();
   }

   /// Fund the operator's native WIRE balance and deposit its bond through sysio.opreg.
   /// A non-bootstrapped operator becomes ACTIVE once it meets the configured minimum.
   action_result fund_and_bond(name account, uint64_t amount) {
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi, SYSIO_ACCOUNT, "transfer"_n,
         mvo()("from", SYSIO_ACCOUNT)("to", account)
         ("quantity", asset(static_cast<int64_t>(amount), symbol(9, "WIRE")))("memo", "bond funding")));
      return push(OPREG_ACCOUNT, opreg_abi, account, "deposit"_n,
         mvo()("account", account)("token_code", WIRE_CHAIN_CODE)("amount", amount));
   }

   /// bootstrap() variant for a real rotation: `ROTATION_GROUPS` single-operator groups (so a resident
   /// op is on duty once per rotation), the percent rail at its accepted ceiling (so an anchored run
   /// terminates on the CONSECUTIVE rail), and `terminate_window_ms` set by the caller (the exact span
   /// bound for this schedule). Registers `outposts`, runs each chain's genesis advance when it registered
   /// one (a chain starts only as an active outpost with a seated window), and returns the registered
   /// outposts' chain codes.
   std::vector<uint64_t> bootstrap_rotation(uint64_t terminate_window_ms, bool batchop_is_bootstrapped = false,
                                            rotation_outposts outposts = rotation_outposts::eth) {
      const bool with_eth = outposts != rotation_outposts::none;
      const bool with_sol = outposts == rotation_outposts::eth_and_sol;

      // With no outpost registered ETH is configured but cannot start.
      epoch_chains = {ETH_CHAIN_CODE};
      if (with_sol) epoch_chains.push_back(SOL_CHAIN_CODE);
      for (const std::string_view chain : epoch_chains) {
         set_epoch_config(chain, EPOCH_DURATION_SEC, ROTATION_OPERATORS_PER_GROUP, ROTATION_GROUPS);
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "seteconomy"_n, mvo()
         ("epoch_duration_sec", EPOCH_DURATION_SEC)("start_time", fc::time_point_sec(control->head().block_time()))));

      BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults());
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT, "initt5"_n, mvo()));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("terminate_prune_delay_ms",         600000)
         ("terminate_max_consecutive_misses", ROTATION_MAX_CONSECUTIVE_MISSES)
         ("terminate_max_pct_misses_24h",     ROTATION_MAX_PERCENT_MISSES)
         ("terminate_min_pct_samples",        DEFAULT_MIN_PERCENT_SAMPLES)
         ("terminate_window_ms",              terminate_window_ms)
         ("req_prod_collat",                  fc::variants{})
         ("req_batchop_collat",               fc::variants{
            make_chain_min_bond(WIRE_CHAIN_CODE, WIRE_CHAIN_CODE, BATCH_OPERATOR_MINIMUM_COLLATERAL) })
         ("req_uw_collat",                    fc::variants{})));

      if (with_eth) {
         register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, ETH_CHAIN_CODE, ETH_EXTERNAL_CHAIN_ID);
      }
      if (with_sol) {
         register_chain(opp::types::ChainKind::CHAIN_KIND_SVM, SOL_CHAIN_CODE, SOL_EXTERNAL_CHAIN_ID);
      }

      // BATCHOP is normally the non-bootstrapped termination target, bonded so it activates.
      // Healthy-rotation tests opt into bootstrap exemption and require no collateral deposit.
      // BATCHOP_B / BATCHOP_C are bootstrapped fillers for the other two groups. schbatchgps sorts
      // non-bootstrapped first, so BATCHOP lands in group 0 (on duty at epochs 1, 4, 7, ...).
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", BATCHOP.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
         ("is_bootstrapped", batchop_is_bootstrapped)));
      if (!batchop_is_bootstrapped) {
         BOOST_REQUIRE_EQUAL(success(), fund_and_bond(BATCHOP, BATCH_OPERATOR_MINIMUM_COLLATERAL));
      }
      for (const auto& op : {BATCHOP_B, BATCHOP_C}) {
         BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
            ("account", op.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped", true)));
      }
      BOOST_REQUIRE(!get_operator(BATCHOP).is_null());
      BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                    get_operator(BATCHOP)["status"].as<opp::types::OperatorStatus>());
      BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(batchop_is_bootstrapped),
                          get_operator(BATCHOP)["is_bootstrapped"].as_uint64());

      // Operators serve only an active outpost, so an unregistered chain gets no window and does not start.
      if (with_eth) {
         serve_chains({BATCHOP, BATCHOP_B, BATCHOP_C}, epoch_chains);
         for (const std::string_view chain : epoch_chains) {
            BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                                mvo()(epoch_fields::CHAIN_CODE, chain)));
            BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                                                mvo()(epoch_fields::CHAIN_CODE, chain)));
         }
      }
      produce_blocks();

      std::vector<uint64_t> registered;
      if (with_eth) registered.push_back(ETH_OUTPOST_ID);
      if (with_sol) registered.push_back(SOL_OUTPOST_ID);
      return registered;
   }

   /// Walk the rotation through advance() until BATCHOP is evicted from every outpost in `outposts`. On
   /// its first duty epoch BATCHOP delivers to each of them (the anchor that keeps the percent rail below
   /// its ceiling) and never again. Each duty epoch's books close at the advance after it. Each chain counts
   /// its own rows, so every chain evicts it at the same duty: the first whose misses exceed the threshold,
   /// with the anchor still inside the window when `anchor_in_window` and long erased otherwise. Its global
   /// status, and so its bond, is untouched.
   void walk_rotation_to_consecutive_eviction(const std::vector<uint64_t>& outposts, bool anchor_in_window = true) {
      // The anchor, then enough missed duties to exceed the threshold.
      const uint32_t evicting_duty = 1 + ROTATION_MAX_CONSECUTIVE_MISSES + 1;
      std::map<uint64_t, inbound_stream_tip> tips;
      // BATCHOP's duty epochs whose books are still open, each with whether it delivered.
      std::map<uint32_t, bool> open_duties;
      uint32_t batchop_duties = 0;
      bool     evicted        = false;
      for (uint32_t step = 0; step < ROTATION_GROUPS * (evicting_duty + 2) && !evicted; ++step) {
         const uint32_t duty_epoch = current_epoch();
         const bool     on_duty    = duty_member() == BATCHOP;
         const bool     anchor     = on_duty && batchop_duties == 0;
         if (on_duty) {
            ++batchop_duties;
            open_duties[duty_epoch] = anchor;
         }
         if (anchor) {
            for (const uint64_t outpost : outposts) {
               auto&      tip      = tips[outpost];
               const auto envelope = encode_chained_delivery(ROTATION_DELIVERY_PAYLOAD, tip);
               BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, outpost, envelope));
               tip = next_stream_tip(envelope);
            }
            // Finalize the deliveries before advance_to_next_epoch's epoch-length jump, or the pending
            // transactions expire across it.
            produce_blocks();
         }
         advance_to_next_epoch();

         // One record per chain for the duty epoch whose books this advance closed.
         if (const auto duty = open_duties.find(booked_epoch()); duty != open_duties.end()) {
            for (const uint64_t outpost : outposts) {
               const auto rows = dellog_rows(BATCHOP, duty->first, outpost);
               BOOST_REQUIRE_EQUAL(1u, rows.size());
               BOOST_REQUIRE_EQUAL(duty->second, rows.front()[opreg_fields::DELIVERED].as<bool>());
            }
            open_duties.erase(duty);
         }

         BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                       get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
         const std::string status = membership_status(BATCHOP, outposts.front());
         if (status == served_status::evicted) {
            evicted = true;
            BOOST_REQUIRE_EQUAL(evicting_duty, batchop_duties);
            for (const uint64_t outpost : outposts) {
               const auto row = membership(BATCHOP, outpost);
               BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), row[opreg_fields::STATUS].as_string());
               BOOST_REQUIRE_EQUAL(CONSECUTIVE_MISS_EVICTION_REASON, row[opreg_fields::STATUS_REASON].as_string());
            }
            // At ROTATION_WINDOW_MS the delivered anchor rows must survive the evicting advance's prune.
            // With one outpost the anchor sits exactly on the window edge at this miss, which pins the
            // exact boundary: an off-by-one in the prune bound would erase it. A narrower window erased
            // them long before, and the run still evicts.
            BOOST_REQUIRE_EQUAL(anchor_in_window ? outposts.size() : 0u, delivered_dellog_count(BATCHOP));
         } else {
            // Still serving: BATCHOP must not be evicted before the advance that closes its evicting duty's books.
            BOOST_REQUIRE_EQUAL(std::string(served_status::serving), status);
            BOOST_REQUIRE(batchop_duties < evicting_duty || !open_duties.empty());
         }
      }
      BOOST_REQUIRE(evicted);
      require_evicted_seat_slides_out(BATCHOP, outposts);
   }

   /// `account` was just evicted from every chain of `outposts` by the advance that closed its duty epoch's
   /// books. The slide out of that duty epoch already re-seated it, in the window's last group: it keeps that one
   /// seat, the epoch after the current one, until the seat slides out, and no group drawn after the eviction
   /// seats it. The roster then leaves it out.
   void require_evicted_seat_slides_out(name account, const std::vector<uint64_t>& outposts) {
      for (const uint64_t outpost : outposts) {
         BOOST_REQUIRE(seated_epochs(account, outpost) == std::vector<uint32_t>{current_epoch(outpost) + 1});
      }
      advance_to_next_epoch();
      for (const uint64_t outpost : outposts) {
         BOOST_REQUIRE(seated_epochs(account, outpost) == std::vector<uint32_t>{current_epoch(outpost)});
      }
      advance_to_next_epoch();
      for (const uint64_t outpost : outposts) {
         BOOST_REQUIRE(!seated_on(account, outpost));
         BOOST_REQUIRE(!require_roster_is_window(outpost).count(account.to_string()));
         BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), membership_status(account, outpost));
      }
   }

   /// bootstrap() variant with the production shape: ETH alone, `DISJOINT_GROUPS` disjoint groups of
   /// `DISJOINT_GROUP_SIZE`, every member bootstrapped except BATCHOP when `batchop_is_bootstrapped` is false,
   /// which bonds the one-unit WIRE minimum and, sorted first, sits in epoch 1's group. Runs ETH's genesis
   /// advance and returns its window, epoch 1's group first.
   std::vector<std::vector<name>> bootstrap_disjoint_groups(bool batchop_is_bootstrapped = true) {
      epoch_chains = {ETH_CHAIN_CODE};
      set_epoch_config(ETH_CHAIN_CODE, EPOCH_DURATION_SEC, DISJOINT_GROUP_SIZE, DISJOINT_GROUPS);
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "seteconomy"_n, mvo()
         ("epoch_duration_sec", EPOCH_DURATION_SEC)("start_time", fc::time_point_sec(control->head().block_time()))));
      BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults());
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT, "initt5"_n, mvo()));
      set_termination_thresholds(ROTATION_MAX_CONSECUTIVE_MISSES, DEFAULT_MAX_PERCENT_MISSES);

      create_accounts({BATCHOP_H, BATCHOP_I});
      const std::vector<name> batch_ops{BATCHOP,   BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E,
                                        BATCHOP_F, BATCHOP_G, BATCHOP_H, BATCHOP_I};
      for (const name op : batch_ops) {
         BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
            ("account", op.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped", op == BATCHOP ? batchop_is_bootstrapped : true)));
      }
      register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, ETH_CHAIN_CODE, ETH_EXTERNAL_CHAIN_ID);
      if (!batchop_is_bootstrapped) {
         BOOST_REQUIRE_EQUAL(success(), fund_and_bond(BATCHOP, BATCH_OPERATOR_MINIMUM_COLLATERAL));
      }
      serve_chains(batch_ops, epoch_chains);
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                          mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                                          mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
      produce_blocks();
      BOOST_REQUIRE_EQUAL(1u, current_epoch());
      const auto window = read_epoch_state()[epoch_fields::BATCH_OP_GROUPS].as<std::vector<std::vector<name>>>();
      BOOST_REQUIRE_EQUAL(DISJOINT_GROUPS, window.size());
      for (const auto& group : window) BOOST_REQUIRE_EQUAL(DISJOINT_GROUP_SIZE, group.size());
      return window;
   }

   /// `members`, sorted.
   static std::vector<name> sorted(std::vector<name> members) {
      std::sort(members.begin(), members.end());
      return members;
   }

   /// Each of `members` delivers `envelope` to ETH.
   void deliver_all(const std::vector<name>& members, const std::vector<char>& envelope) {
      for (const name member : members) BOOST_REQUIRE_EQUAL(success(), deliver_as(member, ETH_OUTPOST_ID, envelope));
      produce_blocks();
   }

   /// Make ETH due and push `chkcons`, which advances its decided epoch.
   void advance_decided_epoch() {
      const uint32_t epoch = current_epoch();
      elapse_epoch_boundary();
      BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
      produce_blocks();
      BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   }

   /// `account`'s one `dellog` row for ETH's epoch `epoch_index` records `delivered`.
   void require_booked(name account, uint32_t epoch_index, bool delivered) {
      const std::vector<fc::variant> rows = dellog_rows(account, epoch_index, ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(1u, rows.size());
      BOOST_REQUIRE_EQUAL(delivered, rows.front()[opreg_fields::DELIVERED].as<bool>());
   }

   /// Set `chain_code`'s `sysio.chains::chains` row active or not, as no action does once it is active, so a
   /// test can run a chain that stops being an active outpost.
   void set_chain_active(uint64_t chain_code, bool active) {
      const auto data = get_row_by_id(CHAINS_ACCOUNT, CHAINS_ACCOUNT, chains_tables::CHAINS, chain_code);
      BOOST_REQUIRE(!data.empty());
      fc::mutable_variant_object row(
         chains_abi.binary_to_variant(chains_abi_types::CHAIN_ROW, data, fixture_abi_yield_function()).get_object());
      row(chains_fields::ACTIVE, active);
      sysio_system::test_support::write_kv_row(
         *this, CHAINS_ACCOUNT, chains_tables::CHAINS, chain_code,
         chains_abi.variant_to_binary(chains_abi_types::CHAIN_ROW, row, fixture_abi_yield_function()));
   }

   // -- Outbound backlog reports --

   /// `t` in milliseconds since the Unix epoch, the unit of `reportqueue`'s `oldest_pending_ms`.
   static uint64_t to_ms(fc::time_point t) {
      return static_cast<uint64_t>(t.time_since_epoch().count() / fc::milliseconds(1).count());
   }

   /// `reportqueue`'s arguments: `op` reports `chain_code`'s backlog in `epoch_index`.
   static fc::variant_object report_queue_args(name op, uint64_t chain_code, uint32_t epoch_index,
                                               uint32_t pending_count, uint64_t oldest_pending_ms) {
      return mvo()(msgch_fields::BATCH_OP, op.to_string())(msgch_fields::CHAIN_CODE, chain_code)
                  (msgch_fields::EPOCH_INDEX, epoch_index)(msgch_fields::PENDING_COUNT, pending_count)
                  (msgch_fields::OLDEST_PENDING_MS, oldest_pending_ms);
   }

   /// Push `reportqueue` for `op`, signed by `signer` (`op` itself when empty).
   action_result report_queue(name op, uint64_t chain_code, uint32_t epoch_index, uint32_t pending_count,
                              uint64_t oldest_pending_ms, name signer = name{}) {
      return push(MSGCH_ACCOUNT, msgch_abi, signer == name{} ? op : signer, msgch_actions::REPORT_QUEUE,
                  report_queue_args(op, chain_code, epoch_index, pending_count, oldest_pending_ms));
   }

   /// `op` reports `REPORTED_PENDING_COUNT` messages pending on `chain_code`'s outpost since `oldest_pending_at`, in
   /// the chain's current epoch; returns the transaction's trace.
   transaction_trace_ptr report_backlog(name op, fc::time_point oldest_pending_at,
                                        uint64_t chain_code = ETH_OUTPOST_ID) {
      return sysio_system::test_support::push_contract_action_trace(
         *this, MSGCH_ACCOUNT, msgch_abi, op, msgch_actions::REPORT_QUEUE,
         report_queue_args(op, chain_code, current_epoch(chain_code), REPORTED_PENDING_COUNT,
                           to_ms(oldest_pending_at)));
   }

   /// How many `sysio.epoch::notetraffic` `trace` ran.
   static uint32_t note_traffic_count(const transaction_trace_ptr& trace) {
      return action_count(trace, EPOCH_ACCOUNT, epoch_actions::NOTE_TRAFFIC);
   }

   /// `account`'s `qreports` row for `chain_code`; null when it has none. The key is (chain, account), the
   /// `[scope][id]` layout `get_row_by_id` reads.
   fc::variant queue_report(name account, uint64_t chain_code = ETH_OUTPOST_ID) {
      const std::vector<char> data =
         get_row_by_id(MSGCH_ACCOUNT, name{chain_code}, msgch_tables::QUEUE_REPORTS, account.to_uint64_t());
      return data.empty() ? fc::variant()
                          : msgch_abi.binary_to_variant(msgch_abi_types::QUEUE_REPORT_ENTRY, data,
                                                        fixture_abi_yield_function());
   }

   /// The accounts of `chain_code`'s `qreports` rows, in key order.
   std::vector<name> queue_reporters(uint64_t chain_code = ETH_OUTPOST_ID) {
      std::vector<name> reporters;
      for (const auto& row : table_rows(MSGCH_ACCOUNT, msgch_tables::QUEUE_REPORTS, msgch_abi,
                                        msgch_abi_types::QUEUE_REPORT_ENTRY)) {
         if (row[msgch_fields::CHAIN_CODE].as_uint64() == chain_code) {
            reporters.emplace_back(row[msgch_fields::ACCOUNT].as_string());
         }
      }
      return reporters;
   }

   /// `account`'s `qreports` row for `chain_code` records `epoch_index`, `pending_count` and `oldest_pending_at`.
   void require_queue_report(name account, uint32_t epoch_index, uint32_t pending_count,
                             fc::time_point oldest_pending_at, uint64_t chain_code = ETH_OUTPOST_ID) {
      const fc::variant row = queue_report(account, chain_code);
      BOOST_REQUIRE(!row.is_null());
      BOOST_REQUIRE_EQUAL(chain_code, row[msgch_fields::CHAIN_CODE].as_uint64());
      BOOST_REQUIRE_EQUAL(account.to_string(), row[msgch_fields::ACCOUNT].as_string());
      BOOST_REQUIRE_EQUAL(epoch_index, row[msgch_fields::EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(pending_count, row[msgch_fields::PENDING_COUNT].as<uint32_t>());
      BOOST_REQUIRE(oldest_pending_at == row[msgch_fields::OLDEST_PENDING_AT].as<fc::time_point>());
   }

   /// `chain_code`'s `traffic_since`; zero while it has no traffic.
   fc::time_point traffic_since(uint64_t chain_code = ETH_OUTPOST_ID) {
      return read_epoch_state(chain_code)[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>();
   }

   /// `chain_code`'s `due_at`; zero while it is idle.
   fc::time_point due_at(uint64_t chain_code = ETH_OUTPOST_ID) {
      return read_epoch_state(chain_code)[epoch_fields::DUE_AT].as<fc::time_point>();
   }

   /// When `chain_code`'s current epoch started.
   fc::time_point epoch_start(uint64_t chain_code = ETH_OUTPOST_ID) {
      return read_epoch_state(chain_code)[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>();
   }

   /// `chain_code`'s group on duty.
   std::vector<name> current_group(uint64_t chain_code = ETH_OUTPOST_ID) {
      const fc::variant                    state  = read_epoch_state(chain_code);
      const std::vector<std::vector<name>> groups =
         state[epoch_fields::BATCH_OP_GROUPS].as<std::vector<std::vector<name>>>();
      const uint32_t                       cursor = state[epoch_fields::CURRENT_BATCH_OP_GROUP].as<uint32_t>();
      BOOST_REQUIRE_LT(cursor, groups.size());
      return groups[cursor];
   }

   abi_serializer sysio_abi, token_abi, epoch_abi, opreg_abi, msgch_abi, chalg_abi, chains_abi, roa_abi,
                  authex_abi, dclaim_abi;
};

// ---------------------------------------------------------------------------
//  Golden vectors: cross-language agreement with the Solidity codec.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(sysio_msgch_chain_tests)

/// WIRE-310: reject decoded versions 0 and 2 on both inbound routes. Unsupported messages never
/// enter the vote tally or dispatch, even after a valid message; a corrected V1 retry succeeds.
BOOST_FIXTURE_TEST_CASE(inbound_payload_versions_rejected_atomically, sysio_msgch_chain_tester) { try {
   bootstrap();
   const uint32_t epoch = current_epoch();
   constexpr uint32_t unsupported_versions[] = {0, 2};
   for (const auto chain_code : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      const auto valid = encode_delivery(epoch, "version-check");
      for (const uint32_t version : unsupported_versions) {
         for (const bool mixed : {false, true}) {
            auto env = decode_envelope(valid);
            if (mixed) *env.add_messages() = env.messages(0);
            auto* message = env.mutable_messages(mixed ? 1 : 0);
            message->mutable_payload()->set_version(version);
            // Keep every semantic header correct: only the version is unacceptable.
            oracle::finalize_header(*message, mixed ? env.messages(0).header().message_id() : "",
                                    1'775'612'516'983ULL);
            std::vector<char> raw(env.ByteSizeLong());
            env.SerializeToArray(raw.data(), static_cast<int>(raw.size()));
            BOOST_REQUIRE_EQUAL(error("assertion failure with message: unsupported OPP MessagePayload version"),
                                deliver(chain_code, raw));
            BOOST_REQUIRE(find_inbound_delivery(chain_code, epoch, BATCHOP).is_null());
            BOOST_REQUIRE(get_outpcons(chain_code).is_null());
            BOOST_REQUIRE_EQUAL(attestation_count(chain_code, epoch), 0u);
         }
      }

      BOOST_REQUIRE_EQUAL(success(), deliver(chain_code, valid));
      BOOST_REQUIRE(!find_inbound_delivery(chain_code, epoch, BATCHOP).is_null());
      BOOST_REQUIRE_EQUAL(get_outpcons(chain_code)[msgch_fields::EPOCH_INDEX].as<uint32_t>(), epoch);
      BOOST_REQUIRE_EQUAL(attestation_count(chain_code, epoch), 1u);
   }
} FC_LOG_AND_RETHROW() }

/// The encoding hex and keccak256 digests below pin the canonical encoding of these exact
/// logical envelopes for cross-language agreement: the identical values are pinned in the
/// wire-solana and wire-ethereum vector suites, where the Rust and Solidity implementations
/// must reproduce them independently (Solidity via `OPPCommon.epochHash` through the
/// `OPPEpochHashHelper` hardhat trampoline). Two consecutive depot-shape epochs — B chains from
/// A at BOTH levels (envelope digest and message id) — plus the wire-ethereum test-fixture
/// shape. The depot-shape headers are DERIVED per the spec (`oracle::finalize_header`), so the
/// pins cover the full semantic-header derivation, not just the byte layout. Depot-shape
/// envelopes carry populated route endpoints (`buildenv` stamps them from the destination's
/// `sysio.chains` row): WIRE(1) → EVM(31337), the local-devnet chain id the outpost fixtures
/// verify against.
BOOST_AUTO_TEST_CASE(canonical_oracle_matches_solidity_golden_vectors) { try {
   constexpr uint64_t GOLDEN_TS_MS = 1'775'612'516'983ULL;

   auto depot_shape = [&](uint32_t epoch_index, const std::string& prev,
                          const std::string& prev_message_id,
                          const std::vector<std::string>& att_datas) {
      sysio::opp::Envelope env;
      auto* eps = env.mutable_endpoints();
      eps->mutable_start()->set_kind(sysio::opp::types::CHAIN_KIND_WIRE);
      eps->mutable_start()->set_id(1);
      eps->mutable_end()->set_kind(sysio::opp::types::CHAIN_KIND_EVM);
      eps->mutable_end()->set_id(31337);
      env.set_epoch_timestamp(GOLDEN_TS_MS);
      env.set_epoch_index(epoch_index);
      if (!prev.empty()) env.set_previous_envelope_hash(prev);
      auto* msg = env.add_messages();
      auto* payload = msg->mutable_payload();
      payload->set_version(1);
      for (const auto& d : att_datas) {
         auto* att = payload->add_attestations();
         att->set_type(sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION);
         att->set_data(d);
         att->set_data_size(static_cast<uint32_t>(d.size()));
      }
      oracle::finalize_header(*msg, prev_message_id, GOLDEN_TS_MS);
      return env;
   };

   // Vector A: depot shape, epoch 7, stream genesis (empty envelope prev + empty message prev,
   // sequence number 1), one attestation 0xdeadbeef.
   const auto env_a = depot_shape(7, {}, {}, { std::string("\xde\xad\xbe\xef", 4) });
   const auto enc_a = oracle::encode(env_a);
   BOOST_REQUIRE_EQUAL(fc::to_hex(enc_a.data(), enc_a.size()),
      "0a00120e0a04080110011206080210e9f40128f7b483d6d63330073800a20100c20292010a7f0a0c0a04"
      "0800100012040800100012200000000000000001210103982d1ae1f083b047bde00e77e4a337f3b31c8d"
      "223c1a00280f32206429fe11b290953c3e28e6ed7887059307329591c6296d6e41d27e4e6ddcae9938f7"
      "b483d6d6334220fb2b80f90bf26934210103982d1ae1f083b047bde00e77e4a337f3b31c8d223c120f08"
      "01120b08d10f10041a04deadbeef");
   const auto digest_a = oracle::epoch_digest(env_a);
   BOOST_REQUIRE_EQUAL(digest_a.str(),
      "f2e3eaf3c62600a753b8207577f4b20554b2b4a9073cb732a3aeb63416bd90ac");

   // Vector B: depot shape, epoch 8, chained from A at both levels (envelope digest + message
   // id, so B's message carries sequence number 2), two attestations.
   const auto env_b = depot_shape(8, oracle::digest_bytes(digest_a),
      env_a.messages(0).header().message_id(),
      { std::string("\xde\xad\xbe\xef", 4), std::string("\xca\xfe\xba\xbe\x01", 5) });
   const auto enc_b = oracle::encode(env_b);
   BOOST_REQUIRE_EQUAL(fc::to_hex(enc_b.data(), enc_b.size()),
      "0a00120e0a04080110011206080210e9f40128f7b483d6d63330083800a20120f2e3eaf3c62600a753b8"
      "207577f4b20554b2b4a9073cb732a3aeb63416bd90acc202c1010a9f010a0c0a04080010001204080010"
      "00122000000000000000022437e72cf67a093c4c5753cbb3ce71b76c890da8f9965c351a200000000000"
      "000001210103982d1ae1f083b047bde00e77e4a337f3b31c8d223c281d3220fdbcffc45ad50a6a2d1376"
      "af8c498d86910751868ae7e14fe909477b319ec98d38f7b483d6d63342208d135355c556a6ed2437e72c"
      "f67a093c4c5753cbb3ce71b76c890da8f9965c35121d0801120b08d10f10041a04deadbeef120c08d10f"
      "10051a05cafebabe01");
   BOOST_REQUIRE_EQUAL(oracle::epoch_digest(env_b).str(),
      "11f1b9c451e62a63e0b903d49d4358ba65994670d33d3775408a89e9690434e3");

   // Vector C: wire-ethereum test-fixture shape: WIRE(1) -> EVM(31337) endpoints, message-free.
   sysio::opp::Envelope env_c;
   auto* eps = env_c.mutable_endpoints();
   eps->mutable_start()->set_kind(sysio::opp::types::CHAIN_KIND_WIRE);
   eps->mutable_start()->set_id(1);
   eps->mutable_end()->set_kind(sysio::opp::types::CHAIN_KIND_EVM);
   eps->mutable_end()->set_id(31337);
   env_c.set_epoch_timestamp(GOLDEN_TS_MS);
   env_c.set_epoch_index(1);
   const auto enc_c = oracle::encode(env_c);
   BOOST_REQUIRE_EQUAL(fc::to_hex(enc_c.data(), enc_c.size()),
      "0a00120e0a04080110011206080210e9f40128f7b483d6d63330013800a20100");
   BOOST_REQUIRE_EQUAL(oracle::epoch_digest(env_c).str(),
      "dc4f4d15bd8c5e9685e2c8e2bf9d52c736bd158dd3fc67f0afbe585e2e0a5fa6");

   // The digest must blank a populated envelope_hash: setting it changes the exact encoding but
   // not the epoch digest.
   sysio::opp::Envelope env_c_hashed = env_c;
   env_c_hashed.set_envelope_hash(std::string(32, '\x11'));
   BOOST_REQUIRE(oracle::encode(env_c_hashed) != enc_c);
   BOOST_REQUIRE_EQUAL(oracle::epoch_digest(env_c_hashed).str(), oracle::epoch_digest(env_c).str());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Outbound: buildenv chains consecutive emits per outpost.
// ---------------------------------------------------------------------------

/// Every buildenv emit must satisfy the canonical-form invariants and chain from whatever the
/// surviving `outenvelopes` row was when it ran. `sysio.epoch::advance` emits its own envelopes
/// (operator-schedule attestations) for registered outposts, so the predecessor is captured from
/// the table right before each emit rather than assumed to be this test's previous emit.
BOOST_FIXTURE_TEST_CASE(buildenv_chains_consecutive_envelopes, sysio_msgch_chain_tester) { try {
   bootstrap();

   for (int round = 0; round < 3; ++round) {
      if (round > 0) advance_one_epoch();

      auto predecessor = find_outbound_envelope(ETH_OUTPOST_ID);

      BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID,
         sysio::opp::types::ATTESTATION_TYPE_OPERATORS,
         std::vector<char>{0x01, 0x02, static_cast<char>(round)}));
      BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));

      auto row = find_outbound_envelope(ETH_OUTPOST_ID);
      BOOST_REQUIRE(!row.is_null());
      const auto raw      = row["raw_envelope"].as<std::vector<char>>();
      const auto hash_hex = row["envelope_hash"].as_string();
      auto env = decode_envelope(raw);

      // The stored envelope_hash is the canonical epoch digest == keccak256 of the emitted
      // bytes (they ARE the preimage: the in-envelope envelope_hash field is empty on the wire).
      BOOST_REQUIRE_EQUAL(env.envelope_hash().size(), 0u);
      BOOST_REQUIRE_EQUAL(hash_hex, oracle::keccak_of(raw).str());
      BOOST_REQUIRE_EQUAL(hash_hex, oracle::epoch_digest(env).str());

      // The emitted bytes are the canonical field-complete encoding: the independent host
      // oracle re-encodes the decoded envelope to the identical byte stream.
      BOOST_REQUIRE(oracle::encode(env) == raw);

      // Destination binding: the envelope names its route endpoints from the destination's
      // `sysio.chains` row (the fixture registers ETH as EVM/31337); the receiving outpost
      // verifies `end` against its own chain identity.
      BOOST_REQUIRE(env.endpoints().start().kind() == sysio::opp::types::CHAIN_KIND_WIRE);
      BOOST_REQUIRE_EQUAL(env.endpoints().start().id(), 1u);
      BOOST_REQUIRE(env.endpoints().end().kind() == sysio::opp::types::CHAIN_KIND_EVM);
      BOOST_REQUIRE_EQUAL(env.endpoints().end().id(), 31337u);

      // Chain link: previous_envelope_hash carries the digest of the envelope that was the
      // surviving row when buildenv ran.
      BOOST_REQUIRE(!predecessor.is_null());
      BOOST_REQUIRE_EQUAL(
         fc::to_hex(env.previous_envelope_hash().data(), env.previous_envelope_hash().size()),
         predecessor["envelope_hash"].as_string());

      // Semantic header (opp.proto MessageHeader): every field the contract derived on emit
      // recomputes identically through the independent oracle.
      BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
      const auto& header  = env.messages(0).header();
      const auto  payload_bytes = oracle::encode(env.messages(0).payload());
      BOOST_REQUIRE_EQUAL(header.payload_size(), static_cast<uint32_t>(payload_bytes.size()));
      BOOST_REQUIRE_EQUAL(
         fc::to_hex(header.payload_checksum().data(), header.payload_checksum().size()),
         oracle::keccak_of(payload_bytes).str());
      BOOST_REQUIRE_EQUAL(
         fc::to_hex(header.header_checksum().data(), header.header_checksum().size()),
         oracle::header_checksum(header).str());
      BOOST_REQUIRE_EQUAL(
         header.message_id(),
         oracle::derive_message_id(
            oracle::header_checksum(header),
            oracle::message_sequence(header.previous_message_id()).value() + 1));

      // Message chain link: previous_message_id carries the predecessor row's stream tip, and
      // the new row's `last_message_id` records this emit's id for the next link.
      BOOST_REQUIRE_EQUAL(
         fc::to_hex(header.previous_message_id().data(), header.previous_message_id().size()),
         predecessor["last_message_id"].as_string());
      BOOST_REQUIRE_EQUAL(
         row["last_message_id"].as_string(),
         fc::to_hex(header.message_id().data(), header.message_id().size()));
   }
} FC_LOG_AND_RETHROW() }

/// Genesis link: the FIRST envelope ever emitted for an outpost carries an empty
/// previous_envelope_hash. Uses a chain registered after bootstrap so no epoch-advance emit has
/// touched it yet.
BOOST_FIXTURE_TEST_CASE(buildenv_first_emit_chains_from_empty, sysio_msgch_chain_tester) { try {
   bootstrap();

   constexpr uint64_t BSC_OUTPOST_ID = "BSC"_s.value;
   register_chain(sysio::opp::types::CHAIN_KIND_EVM, "BSC", 56);
   produce_blocks();

   BOOST_REQUIRE(find_outbound_envelope(BSC_OUTPOST_ID).is_null());
   BOOST_REQUIRE_EQUAL(success(), queueout(BSC_OUTPOST_ID,
      sysio::opp::types::ATTESTATION_TYPE_OPERATORS, std::vector<char>{0x01}));
   BOOST_REQUIRE_EQUAL(success(), buildenv(BSC_OUTPOST_ID));

   auto row = find_outbound_envelope(BSC_OUTPOST_ID);
   BOOST_REQUIRE(!row.is_null());
   const auto raw = row["raw_envelope"].as<std::vector<char>>();
   auto env = decode_envelope(raw);
   BOOST_REQUIRE_EQUAL(env.previous_envelope_hash().size(), 0u);
   BOOST_REQUIRE_EQUAL(row["envelope_hash"].as_string(), oracle::keccak_of(raw).str());

   // Destination binding follows the freshly-registered row, not a fixture constant.
   BOOST_REQUIRE(env.endpoints().end().kind() == sysio::opp::types::CHAIN_KIND_EVM);
   BOOST_REQUIRE_EQUAL(env.endpoints().end().id(), 56u);

   // Message-stream genesis: empty previous_message_id, sequence number 1, tip recorded.
   BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
   const auto& header = env.messages(0).header();
   BOOST_REQUIRE_EQUAL(header.previous_message_id().size(), 0u);
   BOOST_REQUIRE_EQUAL(oracle::message_sequence(header.message_id()).value(), 1u);
   BOOST_REQUIRE_EQUAL(row["last_message_id"].as_string(),
                       fc::to_hex(header.message_id().data(), header.message_id().size()));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Outbound retention: an emit is erased only once the outpost acknowledges it.
// ---------------------------------------------------------------------------

/// WNS-18 / WIRE-348: unacknowledged emits accumulate; one acknowledgement drains every emit at or
/// below its epoch, leaving the table one-deep.
BOOST_FIXTURE_TEST_CASE(buildenv_erases_acknowledged_outenvelope, sysio_msgch_chain_tester) { try {
   bootstrap();

   // bootstrap()'s genesis `advance` fanned a buildenv to every registered outpost.
   const uint32_t epoch_a = current_epoch();
   BOOST_REQUIRE(!outbound_envelope_for_epoch(ETH_OUTPOST_ID, epoch_a).is_null());
   BOOST_REQUIRE(get_outpcons(ETH_OUTPOST_ID).is_null());   // nothing acknowledged yet

   // Unacknowledged: the next epoch's emit does NOT displace epoch A's.
   const uint32_t epoch_b = advance_one_epoch();
   BOOST_REQUIRE_NE(epoch_a, epoch_b);
   BOOST_REQUIRE(!outbound_envelope_for_epoch(ETH_OUTPOST_ID, epoch_a).is_null());
   BOOST_REQUIRE(!outbound_envelope_for_epoch(ETH_OUTPOST_ID, epoch_b).is_null());
   BOOST_REQUIRE_EQUAL(2u, outbound_envelope_count(ETH_OUTPOST_ID));

   // The tip is the NEWEST row, not the retained predecessor.
   BOOST_REQUIRE_EQUAL(find_outbound_envelope(ETH_OUTPOST_ID)["epoch_index"].as<uint32_t>(),
                       epoch_b);

   // An inbound envelope accepted for epoch B acknowledges the depot's epoch-B emit.
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, encode_delivery(epoch_b, "alpha")));
   produce_blocks();
   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch_b);

   // The next emit drains everything at or below the acknowledged epoch: A and B.
   const uint32_t epoch_c = advance_one_epoch();
   BOOST_REQUIRE(outbound_envelope_for_epoch(ETH_OUTPOST_ID, epoch_a).is_null());
   BOOST_REQUIRE(outbound_envelope_for_epoch(ETH_OUTPOST_ID, epoch_b).is_null());
   BOOST_REQUIRE(!outbound_envelope_for_epoch(ETH_OUTPOST_ID, epoch_c).is_null());
   BOOST_REQUIRE_EQUAL(1u, outbound_envelope_count(ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// WNS-18 / WIRE-348: with a long unacknowledged backlog of large envelopes, every emit still chains
/// from the newest retained row, and one acknowledgement prunes the whole prefix in the next emit.
BOOST_FIXTURE_TEST_CASE(buildenv_handles_unacknowledged_backlog, sysio_msgch_chain_tester) { try {
   constexpr uint32_t backlog       = 16;
   constexpr size_t   payload_bytes = 16 * 1024;
   bootstrap();

   const uint32_t first_epoch = current_epoch();
   uint32_t       epoch       = first_epoch;
   for (uint32_t i = 0; i < backlog; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, sysio::opp::types::ATTESTATION_TYPE_OPERATORS,
                                              std::vector<char>(payload_bytes, static_cast<char>(i))));
      epoch = advance_one_epoch();
   }
   BOOST_REQUIRE_EQUAL(backlog + 1, outbound_envelope_count(ETH_OUTPOST_ID));

   // Each retained emit chains from the one before it.
   for (uint32_t e = first_epoch + 1; e <= epoch; ++e) {
      const auto prev = outbound_envelope_for_epoch(ETH_OUTPOST_ID, e - 1);
      const auto cur  = outbound_envelope_for_epoch(ETH_OUTPOST_ID, e);
      BOOST_REQUIRE(!prev.is_null());
      BOOST_REQUIRE(!cur.is_null());
      const auto env = decode_envelope(cur["raw_envelope"].as<std::vector<char>>());
      BOOST_REQUIRE_EQUAL(
         fc::to_hex(env.previous_envelope_hash().data(), env.previous_envelope_hash().size()),
         prev["envelope_hash"].as_string());
   }

   // Acknowledging the newest epoch drains the whole prefix in the next emit.
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, encode_delivery(epoch, "alpha")));
   produce_blocks();
   const uint32_t next_epoch = advance_one_epoch();
   BOOST_REQUIRE_EQUAL(1u, outbound_envelope_count(ETH_OUTPOST_ID));
   BOOST_REQUIRE(!outbound_envelope_for_epoch(ETH_OUTPOST_ID, next_epoch).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Inbound cleanup: buildenv erases only its own outpost's dispatched rows.
// ---------------------------------------------------------------------------

/// WIRE-415: `evalcons` stores every inbound attestation PROCESSED, and `buildenv` erases the
/// destination outpost's PROCESSED rows through the `byqueue` range for that outpost. Each envelope
/// carries one attestation per lane, so a range that misses a lane or reads another status leaves a
/// row behind. Another outpost's rows, PROCESSED or READY, survive until its own `buildenv`.
BOOST_FIXTURE_TEST_CASE(buildenv_erases_only_its_outposts_processed_rows, sysio_msgch_chain_tester) { try {
   constexpr auto processed = opp::types::AttestationStatus::ATTESTATION_STATUS_PROCESSED;
   constexpr auto ready     = opp::types::AttestationStatus::ATTESTATION_STATUS_READY;
   bootstrap();

   // Each outpost's first envelope reaches consensus on the single bootstrapped operator's delivery.
   // OPERATORS is a schedule-lane type and OPERATOR_ACTION a general-lane one; dispatch drops both.
   const uint32_t epoch = current_epoch();
   const std::vector<inbound_attestation> attestations{
      {opp::types::ATTESTATION_TYPE_OPERATORS,       "schedule-lane"},
      {opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, "general-lane"}};
   for (const uint64_t outpost : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(success(), deliver(outpost, encode_delivery(epoch, attestations)));
   }
   produce_blocks();
   for (const uint64_t outpost : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(attestations.size(), attestation_status_count(outpost, processed));
      // A READY row on each outpost: `buildenv` builds nothing without one.
      BOOST_REQUIRE_EQUAL(success(), queueout(outpost, opp::types::ATTESTATION_TYPE_DESYNDICATE_LIQ,
                                              std::vector<char>(8, '\x01')));
   }
   produce_blocks();

   // ETH's build ships ETH's READY row and erases both of its inbound rows. SOL's are untouched.
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, attestation_status_count(ETH_OUTPOST_ID, processed));
   BOOST_REQUIRE_EQUAL(0u, attestation_status_count(ETH_OUTPOST_ID, ready));
   BOOST_REQUIRE_EQUAL(attestations.size(), attestation_status_count(SOL_OUTPOST_ID, processed));
   BOOST_REQUIRE_EQUAL(1u, attestation_status_count(SOL_OUTPOST_ID, ready));

   // The real advance path builds SOL's envelope, which erases SOL's inbound rows in turn.
   advance_one_epoch();
   for (const uint64_t outpost : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(0u, attestation_status_count(outpost, processed));
      BOOST_REQUIRE_EQUAL(0u, attestation_status_count(outpost, ready));
   }
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Inbound: apply_consensus records and verifies the per-outpost chain tip.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(inbound_chain_tip_recorded_and_verified, sysio_msgch_chain_tester) { try {
   bootstrap();

   // Epoch E: first envelope from ETH: no tip exists yet, accepted (bootstrap), tip recorded.
   uint32_t epoch = current_epoch();
   auto n1 = encode_delivery(epoch, "alpha");
   const auto n1_digest  = oracle::epoch_digest(decode_envelope(n1));
   const auto n1_msg_id  = delivery_message_id(n1);
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n1));
   produce_blocks();

   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n1_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);

   // Epoch E+1: correctly chained envelope (prev = tip at BOTH the envelope and message level) is
   // accepted and advances both tips.
   epoch = advance_one_epoch();
   auto n2 = encode_delivery(epoch, "bravo", oracle::digest_bytes(n1_digest), n1_msg_id);
   const auto n2_digest = oracle::epoch_digest(decode_envelope(n2));
   const auto n2_msg_id = delivery_message_id(n2);
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n2));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n2_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);

   // Epoch E+2: chain break: a non-empty prev that does not continue the tip is REJECTED at
   // ingress -- deliver reverts (see msgch::deliver's inbound_envelope_valid gate), so no row is
   // recorded, nothing is dispatched, and the tip does not move.
   const uint32_t break_epoch = advance_one_epoch();
   auto n3 = encode_delivery(break_epoch, "charlie", std::string(32, '\x11'));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
      deliver(ETH_OUTPOST_ID, n3));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);            // still E+1
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n2_digest.str()); // tip unchanged
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, break_epoch), 0u);  // nothing dispatched

   // Epoch E+3: enforcement (SEC-107 completion): an EMPTY prev on a non-genesis epoch is a chain
   // break. Both outposts self-chain per stream, so once a tip is recorded an empty prev-hash no
   // longer bootstraps — it is REJECTED at ingress (deliver reverts) and the tip does not move.
   const uint32_t empty_epoch = advance_one_epoch();
   auto n4 = encode_delivery(empty_epoch, "delta");   // no prev => empty field
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
      deliver(ETH_OUTPOST_ID, n4));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);            // still E+1
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n2_digest.str()); // tip unchanged
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, empty_epoch), 0u);  // nothing dispatched

   // Epoch E+4: a populated envelope_hash field is blanked out of the digest: the envelope is
   // byte-different but canonicalises to the same digest as its blank-hash twin, and a correctly
   // chained prev (from the still-current tip n2, since the empty E+3 delivery was dropped) keeps
   // the chain moving.
   const uint32_t blank_epoch = advance_one_epoch();
   auto n5 = encode_delivery(blank_epoch, "echo", oracle::digest_bytes(n2_digest), n2_msg_id,
                             /*env_hash=*/std::string(32, '\x22'));
   auto n5_env = decode_envelope(n5);
   BOOST_REQUIRE_EQUAL(n5_env.envelope_hash().size(), 32u);
   const auto n5_digest = oracle::epoch_digest(n5_env);
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n5));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), blank_epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n5_digest.str());
} FC_LOG_AND_RETHROW() }

/// Semantic-header enforcement: every header field must recompute per the spec derivation; a
/// forged field is REJECTED at ingress -- `deliver` reverts (the header check runs there), so no
/// envelope row is recorded, no attestation, no consensus record, no chain-tip movement. Each
/// forgery is exercised at its own epoch on a correctly CHAINED envelope, so the rejection is
/// attributable to the header check rather than the envelope-chain check; a final well-formed
/// delivery proves the stream resumes.
BOOST_FIXTURE_TEST_CASE(inbound_semantic_header_forgeries_dropped, sysio_msgch_chain_tester) { try {
   bootstrap();

   // Establish a tip so every subsequent forgery chains correctly at the envelope AND message
   // level -- so each drop is attributable to the mutated field, not the chain checks.
   uint32_t epoch = current_epoch();
   auto base = encode_delivery(epoch, "alpha");
   const auto tip        = oracle::epoch_digest(decode_envelope(base));
   const auto tip_msg_id = delivery_message_id(base);
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, base));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);
   const uint32_t accepted_epoch = epoch;

   // Deliver a correctly-chained envelope whose decoded form was altered by `mutate`, then
   // assert it was REJECTED at ingress: `deliver` reverts (the semantic-header check runs there,
   // see msgch::deliver's inbound_envelope_valid gate), so no envelope row is recorded, no
   // attestation lands for its epoch, the consensus record stays at the last accepted epoch, and
   // the tip does not move.
   auto forged_delivery_dropped = [&](const char* tag,
                                      const std::function<void(sysio::opp::Envelope&)>& mutate) {
      const uint32_t e = advance_one_epoch();
      auto env = decode_envelope(encode_delivery(e, tag, oracle::digest_bytes(tip), tip_msg_id));
      mutate(env);
      std::vector<char> out(env.ByteSizeLong());
      env.SerializeToArray(out.data(), static_cast<int>(out.size()));
      BOOST_REQUIRE_EQUAL(
         error("assertion failure with message: delivered envelope failed inbound-chain or "
               "semantic-header validation"),
         deliver(ETH_OUTPOST_ID, out));
      produce_blocks();
      BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, e), 0u);
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(!opc.is_null());
      BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), accepted_epoch);
      BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), tip.str());
   };

   forged_delivery_dropped("pc", [](sysio::opp::Envelope& env) {
      auto* h = env.mutable_messages(0)->mutable_header();
      std::string c = h->payload_checksum(); c[0] ^= 0x01; h->set_payload_checksum(c);
   });
   forged_delivery_dropped("hc", [](sysio::opp::Envelope& env) {
      auto* h = env.mutable_messages(0)->mutable_header();
      std::string c = h->header_checksum(); c[31] ^= 0x01; h->set_header_checksum(c);
   });
   forged_delivery_dropped("ps", [](sysio::opp::Envelope& env) {
      auto* h = env.mutable_messages(0)->mutable_header();
      h->set_payload_size(h->payload_size() + 1);
   });
   forged_delivery_dropped("seq", [](sysio::opp::Envelope& env) {
      // Correct checksum tail, wrong embedded sequence number: the derivation demands
      // previous_message_id's sequence + 1, so + 2 (skipping one) is always wrong regardless of
      // where the base sits in the stream.
      auto* h = env.mutable_messages(0)->mutable_header();
      const uint64_t wrong_seq =
         oracle::message_sequence(h->previous_message_id()).value() + 2;
      h->set_message_id(oracle::derive_message_id(oracle::header_checksum(*h), wrong_seq));
   });
   forged_delivery_dropped("ds", [](sysio::opp::Envelope& env) {
      auto* a = env.mutable_messages(0)->mutable_payload()->mutable_attestations(0);
      a->set_data_size(a->data_size() + 1);
   });
   forged_delivery_dropped("prevlen", [](sysio::opp::Envelope& env) {
      // Non-canonical previous_message_id length (neither empty nor 32 bytes), with the checksum
      // and id honestly re-derived over it the way a colluding emitter would -- the drop must
      // come from the length rule alone, not from a checksum mismatch.
      auto* h = env.mutable_messages(0)->mutable_header();
      h->set_previous_message_id(std::string(5, '\x07'));
      const auto checksum = oracle::header_checksum(*h);
      h->set_header_checksum(oracle::digest_bytes(checksum));
      h->set_message_id(oracle::derive_message_id(checksum, 1));
   });

   // The stream resumes: a well-formed envelope, chained from the still-current tip at both the
   // envelope and message level, is accepted and advances the tip past all the dropped epochs.
   const uint32_t resume_epoch = advance_one_epoch();
   auto resume = encode_delivery(resume_epoch, "omega", oracle::digest_bytes(tip), tip_msg_id);
   const auto resume_digest = oracle::epoch_digest(decode_envelope(resume));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, resume));
   produce_blocks();
   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), resume_epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), resume_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, resume_epoch), 1u);
} FC_LOG_AND_RETHROW() }

/// SEC-102 P1 (huang): the message chain stops replay of an earlier valid message inside a
/// correctly ENVELOPE-chained successor. The envelope chain orders envelopes but does not bind
/// the messages they carry, so without the per-outpost message tip a malicious batch-operator
/// quorum could re-emit an old `Message` verbatim -- self-consistent header and all -- in a fresh,
/// correctly-chained envelope and re-dispatch its attestations (e.g. crediting one escrow deposit
/// twice). Here the replay carries a value-bearing OPERATOR_ACTION so the double-dispatch would be
/// financially real; the message-tip check drops it before any dispatch.
BOOST_FIXTURE_TEST_CASE(inbound_message_replay_dropped, sysio_msgch_chain_tester) { try {
   bootstrap();

   // Epoch E: accept a genesis envelope carrying message M1. The message tip becomes M1's id.
   uint32_t epoch = current_epoch();
   auto e1 = encode_delivery(epoch, "m1");
   const auto e1_digest = oracle::epoch_digest(decode_envelope(e1));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, e1));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);

   // Epoch E+1: a fresh envelope, correctly chained at the ENVELOPE level (previous_envelope_hash
   // = E's digest), but carrying M1 replayed verbatim. M1's previous_message_id is empty (it was
   // the stream's genesis message), which no longer continues the recorded message tip, so the
   // envelope is REJECTED at ingress: deliver reverts, no new attestation row, the tip does not move.
   const uint32_t replay_epoch = advance_one_epoch();
   auto replay = [&]() {
      sysio::opp::Envelope env;
      env.set_epoch_index(replay_epoch);
      env.set_epoch_envelope_index(1);
      env.set_epoch_timestamp(1'775'612'516'983ULL);
      env.set_previous_envelope_hash(oracle::digest_bytes(e1_digest));
      *env.add_messages() = decode_envelope(e1).messages(0);   // M1 verbatim
      std::vector<char> out(env.ByteSizeLong());
      env.SerializeToArray(out.data(), static_cast<int>(out.size()));
      return out;
   }();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
      deliver(ETH_OUTPOST_ID, replay));
   produce_blocks();

   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);            // still E
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, replay_epoch), 0u); // replay not dispatched
} FC_LOG_AND_RETHROW() }

/// SEC-107 completion: the SVM cross-stream alternate is REMOVED. The Solana outpost now
/// self-chains per stream (wire-solana SEC-114 `previous_outbound_epoch_hash`), so an envelope
/// linking to the depot's own outbound emit digest instead of the outpost's inbound tip is a
/// chain break for SVM exactly as it is for EVM — the interim fallback no longer accepts it.
BOOST_FIXTURE_TEST_CASE(inbound_rejects_cross_stream_link_for_svm_outposts,
                        sysio_msgch_chain_tester) { try {
   bootstrap();

   // Establish an inbound tip for SOL (bootstrap accept), so the cross-stream link is what gets
   // exercised on the next delivery rather than the no-tip bootstrap rule.
   uint32_t epoch = current_epoch();
   auto n1 = encode_delivery(epoch, "xstream-a");
   const auto n1_digest = oracle::epoch_digest(decode_envelope(n1));
   BOOST_REQUIRE_EQUAL(success(), deliver(SOL_OUTPOST_ID, n1));
   produce_blocks();

   const uint32_t tip_epoch = epoch;

   // Next epoch: emit a depot outbound envelope, then deliver an inbound envelope whose prev is
   // that emit's digest (NOT the inbound tip). Building the outbound envelope in the delivery
   // epoch pins the outenvelopes tip so no epoch-advance emission supersedes it.
   epoch = advance_one_epoch();
   BOOST_REQUIRE_EQUAL(success(), queueout(SOL_OUTPOST_ID,
      sysio::opp::types::ATTESTATION_TYPE_OPERATORS, std::vector<char>{0x0a}));
   BOOST_REQUIRE_EQUAL(success(), buildenv(SOL_OUTPOST_ID));
   auto out_row = find_outbound_envelope(SOL_OUTPOST_ID);
   BOOST_REQUIRE(!out_row.is_null());
   std::string outbound_digest_bytes(32, '\0');
   BOOST_REQUIRE_EQUAL(32u, fc::from_hex(out_row["envelope_hash"].as_string(),
                                         outbound_digest_bytes.data(),
                                         outbound_digest_bytes.size()));

   // Cross-stream link (prev = depot's outbound emit digest) is a chain break: REJECTED at ingress
   // (deliver reverts), tip does not move, nothing recorded or dispatched.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
      deliver(SOL_OUTPOST_ID, encode_delivery(epoch, "xstream-b", outbound_digest_bytes)));
   produce_blocks();

   auto opc = get_outpcons(SOL_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), tip_epoch);          // not advanced
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n1_digest.str());   // tip unchanged
   BOOST_REQUIRE_EQUAL(attestation_count(SOL_OUTPOST_ID, epoch), 0u);          // nothing dispatched
} FC_LOG_AND_RETHROW() }

/// EVM outposts chain per-stream, so an envelope linking to the depot's outbound emit digest
/// instead of the inbound tip is a chain break and drops (same rule as SVM now).
BOOST_FIXTURE_TEST_CASE(inbound_rejects_cross_stream_link_for_evm_outposts,
                        sysio_msgch_chain_tester) { try {
   bootstrap();

   uint32_t epoch = current_epoch();
   auto n1 = encode_delivery(epoch, "evm-xstream-a");
   const auto n1_digest = oracle::epoch_digest(decode_envelope(n1));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n1));
   produce_blocks();

   const uint32_t tip_epoch = epoch;
   epoch = advance_one_epoch();
   BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID,
      sysio::opp::types::ATTESTATION_TYPE_OPERATORS, std::vector<char>{0x0b}));
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   auto out_row = find_outbound_envelope(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!out_row.is_null());
   std::string outbound_digest_bytes(32, '\0');
   BOOST_REQUIRE_EQUAL(32u, fc::from_hex(out_row["envelope_hash"].as_string(),
                                         outbound_digest_bytes.data(),
                                         outbound_digest_bytes.size()));

   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
      deliver(ETH_OUTPOST_ID, encode_delivery(epoch, "evm-xstream-b", outbound_digest_bytes)));
   produce_blocks();

   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), tip_epoch);          // not advanced
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n1_digest.str());   // tip unchanged
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 0u);          // nothing dispatched
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(inbound_bootstrap_accepts_unverifiable_prev, sysio_msgch_chain_tester) { try {
   bootstrap();

   // First envelope ever accepted from SOL carries a non-empty prev the depot cannot verify
   // (no tip recorded for the outpost): accepted, and the digest becomes the tip. Covers an
   // outpost registered after its chain has already been emitting chained envelopes.
   const uint32_t epoch = current_epoch();
   auto n1 = encode_delivery(epoch, "sol-alpha", std::string(32, '\x33'));
   const auto n1_digest = oracle::epoch_digest(decode_envelope(n1));
   BOOST_REQUIRE_EQUAL(success(), deliver(SOL_OUTPOST_ID, n1));
   produce_blocks();

   auto opc = get_outpcons(SOL_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n1_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(SOL_OUTPOST_ID, epoch), 1u);
} FC_LOG_AND_RETHROW() }

/// Late confirmation: once an epoch's winner is accepted, the per-stream tip advances to the
/// winner's own digest, so the winner no longer "continues" the chain it just extended. The
/// remaining operators of the group still deliver the exact accepted bytes (`sysio.epoch::advance`
/// records every member's delivery), and `deliver` must record -- not chain-break-revert -- them. A
/// three-operator group reaches majority consensus at the second delivery; the third operator's
/// byte-identical delivery is the late confirmation.
/// A DIVERGENT post-acceptance delivery reverts (fail closed): the acceptance already judged every
/// recorded delivery, so a decided epoch takes only the winner's bytes.
BOOST_FIXTURE_TEST_CASE(late_confirmation_after_consensus_recorded, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);

   const uint32_t epoch = current_epoch();
   auto winner = encode_delivery(epoch, "late-confirm");
   const auto winner_digest = oracle::epoch_digest(decode_envelope(winner));

   // First delivery: one of a group of three is no majority -- no consensus yet.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   produce_blocks();
   {
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(opc.is_null() || !opc["consensus_reached"].as<bool>());
      // Until a winner is accepted the delivered bytes are held for the consensus decode.
      auto row = find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP);
      BOOST_REQUIRE(!row.is_null());
      BOOST_REQUIRE(row[msgch_fields::RAW_DATA].as<std::vector<char>>() == winner);
   }

   // Second delivery: the majority (2 of 3) tips consensus and advances the tip to the winner's own
   // digest.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   {
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(!opc.is_null());
      BOOST_REQUIRE_EQUAL(opc["consensus_reached"].as<bool>(), true);
      BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
      BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), winner_digest.str());
   }

   // A post-acceptance DIVERGENT envelope is not a late confirmation: its bytes differ from the
   // recorded winner, so the decided epoch refuses it.
   auto divergent = encode_delivery(epoch, "late-diverge");
   BOOST_REQUIRE_EQUAL(error(DECIDED_EPOCH_DIFFERING_DELIVERY_ERROR),
                       deliver_as(BATCHOP_C, ETH_OUTPOST_ID, divergent));
   produce_blocks();

   // The third operator's byte-identical delivery IS a late confirmation: recorded, not reverted.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, winner));
   produce_blocks();

   // The duplicate guard now proves the confirmation row exists: a re-delivery from the same
   // operator reports "already delivered" (it would report a chain break if the row had not been
   // recorded).
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: operator already delivered for this outpost+epoch"),
      deliver_as(BATCHOP_C, ETH_OUTPOST_ID, winner));

   // The confirmation row keeps only metadata: its checksum matches the winner,
   // and the bytes, which duplicate the already-applied winner, are not stored.
   {
      auto row = find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_C);
      BOOST_REQUIRE(!row.is_null());
      BOOST_REQUIRE_EQUAL(row[msgch_fields::CHECKSUM].as_string(),
                          fc::sha256::hash(winner.data(), winner.size()).str());
      BOOST_REQUIRE(row[msgch_fields::RAW_DATA].as<std::vector<char>>().empty());
   }

   // Acceptance state is untouched by the late confirmation: same tip, same epoch, attestations
   // dispatched exactly once.
   {
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(opc["consensus_reached"].as<bool>(), true);
      BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
      BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), winner_digest.str());
      BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);
   }
} FC_LOG_AND_RETHROW() }

/// WNS-16: a non-canonical delivery must be slashed before its historical
/// miss window can evict it. The fixture builds its historical anchor and
/// misses through real `chkcons -> advance` transitions, then lowers the
/// consecutive-miss threshold before the divergent epoch. The acceptance of the
/// epoch's winner slashes BATCHOP, before the next advance's `termcheck` evaluates
/// the rails on the last missed epoch, and the advance after it, which closes the
/// divergent epoch's books, still records its delivery.
BOOST_FIXTURE_TEST_CASE(noncanonical_delivery_slashes_before_termination, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kBatchOperatorCount                  = 3;
   constexpr uint32_t kExpectedInitialEpoch                = 1;
   constexpr uint32_t kPermissiveMaxConsecutiveMisses      = 5;
   constexpr uint32_t kPermissiveMaxPercentMisses          = 99;
   constexpr uint32_t kTerminatingMaxConsecutiveMisses     = 1;
   // Each missed epoch records one miss, so the run needs one more epoch than the terminating
   // threshold allows.
   constexpr uint32_t kHistoricalMissEpochs                = kTerminatingMaxConsecutiveMisses + 1;
   constexpr const char* kHistoricalAnchorPayload          = "history-anchor";
   constexpr const char* kHistoricalMissPayload            = "history-miss";
   constexpr const char* kCanonicalPayload                 = "canonical";
   constexpr const char* kNonCanonicalPayload              = "non-canonical";
   constexpr const char* kFollowingPayload                 = "following";
   constexpr uint32_t kExpectedSlashActionsPerOutpost      = 0;
   constexpr uint32_t kEpochAdvanceCount                   = 1;
   constexpr uint32_t kOutpostCount                        = 2;
   // One delivered row per chain per duty epoch: the historical anchor and the non-canonical delivery.
   constexpr uint32_t kExpectedDeliveredLogCount           = 2 * kOutpostCount;

   bootstrap(/*n_batch_ops=*/kBatchOperatorCount, /*batchop_is_bootstrapped=*/false);
   BOOST_REQUIRE_EQUAL(kExpectedInitialEpoch, current_epoch());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE,
                       get_operator(BATCHOP)[opreg_fields::STATUS]
                          .as<opp::types::OperatorStatus>());
   {
      auto state = read_epoch_state();
      auto groups = state[epoch_fields::BATCH_OP_GROUPS].get_array();
      auto active_group =
         groups[state[epoch_fields::CURRENT_BATCH_OP_GROUP].as_uint64()].get_array();
      bool batchop_is_scheduled = false;
      for (const auto& member : active_group) {
         if (member.as_string() == BATCHOP.to_string()) batchop_is_scheduled = true;
      }
      BOOST_REQUIRE(batchop_is_scheduled);
   }

   // Create reachable history under permissive rails. The first epoch's canonical anchor prevents
   // the 99% percent rail from evicting BATCHOP while each following epoch records one real miss
   // through advance()'s inline recorddel/termcheck calls. Both outposts receive identical envelopes,
   // so their inbound streams share one tip.
   set_termination_thresholds(kPermissiveMaxConsecutiveMisses, kPermissiveMaxPercentMisses);
   inbound_stream_tip tip;
   const uint32_t anchor_epoch = current_epoch();
   const auto historical_anchor = encode_chained_delivery(kHistoricalAnchorPayload, tip);
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   ETH_OUTPOST_ID, historical_anchor));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   SOL_OUTPOST_ID, historical_anchor));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, historical_anchor));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, SOL_OUTPOST_ID, historical_anchor));
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, historical_anchor));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, SOL_OUTPOST_ID, historical_anchor));
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(anchor_epoch + kEpochAdvanceCount, current_epoch());
   tip = next_stream_tip(historical_anchor);

   for (uint32_t miss = 0; miss < kHistoricalMissEpochs; ++miss) {
      const uint32_t missed_epoch = current_epoch();
      const auto historical_miss = encode_chained_delivery(kHistoricalMissPayload + std::to_string(miss), tip);
      BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, historical_miss));
      BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, SOL_OUTPOST_ID, historical_miss));
      elapse_epoch_boundary();
      BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, historical_miss));
      BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, SOL_OUTPOST_ID, historical_miss));
      advance_via_consensus();
      BOOST_REQUIRE_EQUAL(missed_epoch + kEpochAdvanceCount, current_epoch());
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE,
                          get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
      tip = next_stream_tip(historical_miss);
   }

   // The accumulated real misses now breach a rail. A non-canonical delivery is still recorded
   // as delivered; the slash at acceptance precedes the close's termcheck.
   set_termination_thresholds(kTerminatingMaxConsecutiveMisses, kPermissiveMaxPercentMisses);

   const uint32_t epoch = current_epoch();
   const auto canonical = encode_chained_delivery(kCanonicalPayload, tip);
   const auto divergent = encode_chained_delivery(kNonCanonicalPayload, tip);
   const auto canonical_checksum = fc::sha256::hash(canonical.data(), canonical.size());
   const auto divergent_checksum = fc::sha256::hash(divergent.data(), divergent.size());
   BOOST_REQUIRE_NE(canonical_checksum, divergent_checksum);

   // Stage the split deliveries for both outposts. BATCHOP is non-canonical everywhere; BATCHOP_B/C
   // establish canonical consensus, and ETH's acceptance slashes BATCHOP. SOL's acceptance slashes it
   // again, without effect.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   ETH_OUTPOST_ID, divergent));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   SOL_OUTPOST_ID, divergent));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, canonical));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, SOL_OUTPOST_ID, canonical));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, canonical));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, SOL_OUTPOST_ID, canonical));
   elapse_epoch_boundary();
   {
      auto eth_consensus = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(!eth_consensus.is_null());
      BOOST_REQUIRE_EQUAL(epoch, eth_consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(canonical_checksum.str(),
                          eth_consensus[msgch_fields::WINNING_CHECKSUM].as_string());
      auto divergent_delivery = find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP);
      BOOST_REQUIRE(!divergent_delivery.is_null());
      BOOST_REQUIRE_EQUAL(divergent_checksum.str(),
                          divergent_delivery[msgch_fields::CHECKSUM].as_string());
   }
   {
      auto sol_consensus = get_outpcons(SOL_OUTPOST_ID);
      BOOST_REQUIRE(!sol_consensus.is_null());
      BOOST_REQUIRE_EQUAL(epoch, sol_consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(canonical_checksum.str(),
                          sol_consensus[msgch_fields::WINNING_CHECKSUM].as_string());
      auto divergent_delivery = find_inbound_delivery(SOL_OUTPOST_ID, epoch, BATCHOP);
      BOOST_REQUIRE(!divergent_delivery.is_null());
      BOOST_REQUIRE_EQUAL(divergent_checksum.str(),
                          divergent_delivery[msgch_fields::CHECKSUM].as_string());
   }
   advance_via_consensus();

   auto op = get_operator(BATCHOP);
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED,
                       op[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
   BOOST_REQUIRE_EQUAL(kExpectedSlashActionsPerOutpost,
                       slash_action_count(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(kExpectedSlashActionsPerOutpost,
                       slash_action_count(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(epoch + kEpochAdvanceCount, current_epoch());
   // That advance closed the last missed epoch's books: the slashed BATCHOP was not evaluated.
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID})
      BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, chain));
   // The slide reseated the group without BATCHOP, so the roster no longer lists it.
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID})
      BOOST_REQUIRE(!require_roster_is_window(chain).count(BATCHOP.to_string()));

   // The divergent epoch's books close at the next advance, which records its delivery.
   const uint32_t following_epoch = current_epoch();
   const auto     following       = encode_chained_delivery(kFollowingPayload, next_stream_tip(canonical));
   for (const name member : {BATCHOP_B, BATCHOP_C}) {
      for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID})
         BOOST_REQUIRE_EQUAL(success(), deliver_as(member, chain, following));
   }
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(following_epoch + kEpochAdvanceCount, current_epoch());
   BOOST_REQUIRE_EQUAL(kExpectedDeliveredLogCount, delivered_dellog_count(BATCHOP));
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      const auto rows = dellog_rows(BATCHOP, epoch, chain);
      BOOST_REQUIRE_EQUAL(1u, rows.size());
      BOOST_REQUIRE(rows.front()[opreg_fields::DELIVERED].as<bool>());
   }
} FC_LOG_AND_RETHROW() }

/// WNS-70: a member that delivered a non-canonical envelope and was terminated before the epoch's
/// winner was accepted leaves its delivery row behind. The acceptance still sends a slash for it,
/// which must not roll the accepting delivery back, and the close still advances.
BOOST_FIXTURE_TEST_CASE(noncanonical_delivery_then_termination_still_advances, sysio_msgch_chain_tester) { try {
   constexpr uint32_t    kBatchOperatorCount  = 3;
   constexpr uint32_t    kEpochAdvanceCount   = 1;
   constexpr const char* kCanonicalPayload    = "canonical";
   constexpr const char* kNonCanonicalPayload = "non-canonical";
   constexpr const char* kTerminationReason   = "terminated after a non-canonical delivery";

   bootstrap(/*n_batch_ops=*/kBatchOperatorCount, /*batchop_is_bootstrapped=*/false);
   const uint32_t epoch = current_epoch();
   inbound_stream_tip tip;
   const auto canonical = encode_chained_delivery(kCanonicalPayload, tip);
   const auto divergent = encode_chained_delivery(kNonCanonicalPayload, tip);
   const auto canonical_checksum = fc::sha256::hash(canonical.data(), canonical.size());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   ETH_OUTPOST_ID, divergent));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   SOL_OUTPOST_ID, divergent));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, canonical));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, SOL_OUTPOST_ID, canonical));
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "terminate"_n,
      mvo()("account", BATCHOP.to_string())("reason", kTerminationReason)));
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      const transaction_trace_ptr trace = deliver_trace(BATCHOP_C, chain, canonical);
      const std::vector<slashop_record> slashed = slashops(trace);
      BOOST_REQUIRE_EQUAL(1u, slashed.size());
      BOOST_REQUIRE_EQUAL(BATCHOP, slashed.front().operator_account);
   }
   produce_blocks();
   elapse_epoch_boundary();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      auto consensus = get_outpcons(chain);
      BOOST_REQUIRE(!consensus.is_null());
      BOOST_REQUIRE_EQUAL(epoch, consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(canonical_checksum.str(), consensus[msgch_fields::WINNING_CHECKSUM].as_string());
      BOOST_REQUIRE(!find_inbound_delivery(chain, epoch, BATCHOP).is_null());
   }

   advance_via_consensus();

   BOOST_REQUIRE_EQUAL(epoch + kEpochAdvanceCount, current_epoch());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_TERMINATED,
                       get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
} FC_LOG_AND_RETHROW() }

/// Each chain's advance() writes exactly one delivery-log row per member of the group that served the epoch
/// whose books it closes, the one before the epoch it closes, marked delivered when the member reached that chain.
BOOST_FIXTURE_TEST_CASE(advance_records_one_row_per_member_per_chain_epoch, sysio_msgch_chain_tester) { try {
   constexpr uint32_t    kBatchOperatorCount = 3;
   constexpr const char* kPayload            = "one-row-per-member";
   bootstrap(kBatchOperatorCount);

   const uint32_t epoch    = current_epoch();
   const auto     envelope = encode_delivery(epoch, kPayload);
   // BATCHOP reaches both outposts, BATCHOP_B only ETH, BATCHOP_C only SOL.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   ETH_OUTPOST_ID, envelope));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP,   SOL_OUTPOST_ID, envelope));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, SOL_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, advance_one_epoch());
   // The epoch's books are still open after its own close.
   for (const name member : {BATCHOP, BATCHOP_B, BATCHOP_C}) BOOST_REQUIRE(dellog_rows(member, epoch).empty());
   BOOST_REQUIRE_EQUAL(epoch + 2, advance_one_epoch());

   // Delivered on ETH, then on SOL.
   const std::vector<uint64_t> chains{ETH_OUTPOST_ID, SOL_OUTPOST_ID};
   const std::vector<std::pair<name, std::vector<bool>>> expected_delivered{
      {BATCHOP, {true, true}}, {BATCHOP_B, {true, false}}, {BATCHOP_C, {false, true}}};
   for (const auto& [member, delivered] : expected_delivered) {
      BOOST_REQUIRE_EQUAL(chains.size(), dellog_rows(member, epoch).size());
      for (size_t chain = 0; chain < chains.size(); ++chain) {
         const auto rows = dellog_rows(member, epoch, chains[chain]);
         BOOST_REQUIRE_EQUAL(1u, rows.size());
         BOOST_REQUIRE_EQUAL(delivered[chain], rows.front()[opreg_fields::DELIVERED].as<bool>());
      }
   }
} FC_LOG_AND_RETHROW() }

/// A chain starts only as an active outpost with a seated window: while it is not an active outpost its
/// operators cannot serve it and its genesis advance is refused, and once registered it still waits for
/// its schedule. Started, its genesis and its first advance record nothing, their `previous_group` empty;
/// the first advance keeps epoch 1's group as `previous_group`, and the advance after it records and checks
/// that member.
BOOST_FIXTURE_TEST_CASE(a_chain_starts_only_as_an_active_outpost_with_a_window, sysio_msgch_chain_tester) { try {
   BOOST_REQUIRE(bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/false,
                                    rotation_outposts::none).empty());
   BOOST_REQUIRE_EQUAL(0u, current_epoch());
   BOOST_REQUIRE_EQUAL(error(NOT_ACTIVE_OUTPOST_JOIN_ERROR),
                       sysio_system::test_support::served_chains::join(*this, BATCHOP, ETH_CHAIN_CODE));
   const auto genesis = [&] {
      return push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                  mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE));
   };
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(GENESIS_NOT_ACTIVE_OUTPOST_ERROR), genesis());

   // Registered inside the bootstrap window, the chain is active at once, but has no window yet.
   register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, ETH_CHAIN_CODE, ETH_EXTERNAL_CHAIN_ID);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(GENESIS_WITHOUT_SCHEDULE_ERROR), genesis());
   BOOST_REQUIRE_EQUAL(0u, current_epoch());

   serve_chains({BATCHOP, BATCHOP_B, BATCHOP_C}, {ETH_CHAIN_CODE});
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(success(), genesis());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, current_epoch());
   BOOST_REQUIRE(previous_group().empty());

   const name     expiring_member = duty_member();
   const uint32_t expiring_epoch  = current_epoch();
   const auto     first           = advance_to_next_epoch_traces().front();
   BOOST_REQUIRE_EQUAL(0u, action_count(first, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   BOOST_REQUIRE_EQUAL(0u, action_count(first, OPREG_ACCOUNT, opreg_actions::TERMINATION_CHECK));
   BOOST_REQUIRE(dellog_rows(expiring_member).empty());
   BOOST_REQUIRE(previous_group() == std::vector<name>{expiring_member});

   const auto trace = advance_to_next_epoch_traces().front();
   BOOST_REQUIRE_EQUAL(ROTATION_OPERATORS_PER_GROUP,
                       action_count(trace, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   BOOST_REQUIRE_EQUAL(ROTATION_OPERATORS_PER_GROUP,
                       action_count(trace, OPREG_ACCOUNT, opreg_actions::TERMINATION_CHECK));
   const auto rows = dellog_rows(expiring_member, expiring_epoch);
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   BOOST_REQUIRE(!rows.front()[opreg_fields::DELIVERED].as<bool>());
} FC_LOG_AND_RETHROW() }

// SEC-28 (huang review): evict on the CONSECUTIVE-miss rail through the REAL rotation -- a
// materialized three-group schedule driven by advance() with one outpost. A resident operator is on
// duty once per three-epoch rotation, and the window spans exactly (max_consecutive_misses + 1) rotations, so the delivered anchor that
// keeps the percent rail below its ceiling sits exactly on the window edge at the evicting miss.
// Eviction alone does NOT pin that edge: the six miss rows fire the consecutive rail whether or
// not the anchor is pruned, and termcheck prefers the consecutive reason over percent. So the test
// asserts the anchor's delivered dellog row SURVIVES the evicting advance -- an off-by-one in the
// span (or the prune/scan bound) ages it out, failing that assertion. Driven end to end by
// advance()'s inline recorddel/termcheck rather than fabricated timing.
BOOST_FIXTURE_TEST_CASE(evict_at_duty_rotation_via_advance, sysio_msgch_chain_tester) { try {
   const auto outposts = bootstrap_rotation(ROTATION_WINDOW_MS);

   // BATCHOP delivers on its first duty epoch (the anchor) and never again; every later duty epoch is
   // a miss recorded by advance()'s inline recorddel. The sixth consecutive miss is BATCHOP's 7th duty.
   walk_rotation_to_consecutive_eviction(outposts);
} FC_LOG_AND_RETHROW() }

/// The same rotation with two active outposts. Each chain counts only its own rows, so a full outage
/// evicts BATCHOP from both chains at the duty a single chain would, not sooner.
BOOST_FIXTURE_TEST_CASE(evict_at_duty_rotation_via_advance_with_two_outposts, sysio_msgch_chain_tester) { try {
   const auto outposts = bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/false,
                                            rotation_outposts::eth_and_sol);
   walk_rotation_to_consecutive_eviction(outposts);
} FC_LOG_AND_RETHROW() }

/// A window of one rotation keeps only a member's latest record, yet the consecutive rail counts every missed
/// duty epoch since the last delivery: the same rotation evicts BATCHOP at the same duty, its delivered anchor
/// long erased.
BOOST_FIXTURE_TEST_CASE(evict_a_run_whose_rows_aged_out_via_advance, sysio_msgch_chain_tester) { try {
   walk_rotation_to_consecutive_eviction(bootstrap_rotation(ROTATION_DUTY_MS), /*anchor_in_window=*/false);
} FC_LOG_AND_RETHROW() }

/// Delivering to one of two active outposts is a delivery on the chain reached and a miss on the other,
/// each recorded by that chain's own close in that chain's own log. After a first duty delivered to both
/// (the anchor that keeps each chain's percent rail below its ceiling), alternating the chain reached
/// never builds a consecutive run on either chain, so the member keeps serving both.
BOOST_FIXTURE_TEST_CASE(partial_delivery_is_a_miss_on_the_chain_left_out, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kDuties = ROTATION_MAX_CONSECUTIVE_MISSES + 2;
   const auto outposts = bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/false,
                                            rotation_outposts::eth_and_sol);
   std::map<uint64_t, inbound_stream_tip> tips;
   // BATCHOP's duty epochs whose books are still open, each with the chains it reached.
   std::map<uint32_t, std::vector<size_t>> open_duties;
   uint32_t duties = 0;
   while (duties < kDuties || !open_duties.empty()) {
      const uint32_t duty_epoch = current_epoch();
      if (duties < kDuties && duty_member() == BATCHOP) {
         std::vector<size_t> reached;
         if (duties == 0) {
            for (size_t chain = 0; chain < outposts.size(); ++chain) reached.push_back(chain);
         } else {
            reached.push_back(duties % outposts.size());
         }
         ++duties;
         for (const size_t chain : reached) {
            auto&      tip      = tips[outposts[chain]];
            const auto envelope = encode_chained_delivery(ROTATION_DELIVERY_PAYLOAD, tip);
            BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, outposts[chain], envelope));
            tip = next_stream_tip(envelope);
         }
         open_duties[duty_epoch] = reached;
         produce_blocks();
      }
      advance_to_next_epoch();
      // One row per chain for the duty epoch whose books this advance closed: delivered only on the chains reached.
      if (const auto duty = open_duties.find(booked_epoch()); duty != open_duties.end()) {
         for (size_t chain = 0; chain < outposts.size(); ++chain) {
            const auto rows = dellog_rows(BATCHOP, duty->first, outposts[chain]);
            BOOST_REQUIRE_EQUAL(1u, rows.size());
            const bool was_reached = std::find(duty->second.begin(), duty->second.end(), chain) != duty->second.end();
            BOOST_REQUIRE_EQUAL(was_reached, rows.front()[opreg_fields::DELIVERED].as<bool>());
         }
         open_duties.erase(duty);
      }
      for (const uint64_t outpost : outposts) {
         BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, outpost));
      }
   }
} FC_LOG_AND_RETHROW() }

// WNS-15(a) / WNS-08 (WIRE-346 / WIRE-322): consensus thresholds must derive from the group that
// can ACTUALLY deliver, not from the configured `operators_per_epoch`.
//
// The resident group is a snapshot; `deliver` additionally requires current opreg ACTIVE status, so
// slashing a member shrinks the deliverable set without shrinking the snapshot. With the configured
// size still 3, the pre-fix thresholds were both unreachable for the surviving two: unanimity
// required 3 deliveries, and the boundary-majority path required `count > 3/2`, i.e. 3 as well.
// Consensus was arithmetically impossible and the epoch stalled chain-wide with every remaining
// operator behaving correctly. CertiK shipped this as
// sysio_msgch_chain_tests/short_one_member_group_still_uses_configured_three_member_quorum.
BOOST_FIXTURE_TEST_CASE(short_group_after_slash_still_reaches_consensus, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);   // configured operators_per_epoch == 3, resident group == 3

   // Slash one member. It stays in batch_op_groups[current] until the next reschedule, but can no
   // longer deliver -- so the eligible set is 2 while the configured size is still 3.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n,
      mvo()("account", BATCHOP_C.to_string())("reason", "WNS-15 short-group regression")));
   produce_blocks();

   const uint32_t epoch = current_epoch();
   auto envelope = encode_delivery(epoch, "short-group");
   const auto digest = oracle::epoch_digest(decode_envelope(envelope));

   // The slashed member is refused outright -- it is not merely uncounted.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivering operator is not ACTIVE in sysio.opreg"),
      deliver_as(BATCHOP_C, ETH_OUTPOST_ID, envelope));
   produce_blocks();

   // Both surviving members deliver identical bytes: the strict majority of the ELIGIBLE group of two.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   produce_blocks();

   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc["consensus_reached"].as<bool>(), true);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), digest.str());
} FC_LOG_AND_RETHROW() }

/// The first strict majority decides the chain's current epoch in the delivering transaction, before the
/// chain's boundary and with no crank. Advancing the decided epoch still waits for the boundary.
BOOST_FIXTURE_TEST_CASE(majority_decides_before_the_boundary_in_the_delivering_transaction,
                        sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t          epoch    = current_epoch();
   const std::vector<char> envelope = encode_delivery(epoch, "majority-before-boundary");
   const fc::sha256        checksum = fc::sha256::hash(envelope.data(), envelope.size());

   // One of three is no majority.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));

   // Two of three is: the second delivery's own transaction evaluates and accepts the winner.
   const transaction_trace_ptr trace = deliver_trace(BATCHOP_B, ETH_OUTPOST_ID, envelope);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   BOOST_REQUIRE(before_epoch_boundary());
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(get_outpcons(ETH_OUTPOST_ID)[msgch_fields::WINNING_CHECKSUM].as_string(), checksum.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);
   produce_blocks();

   // The decided epoch advances only once the chain's boundary has passed.
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
} FC_LOG_AND_RETHROW() }

/// Once the epoch is decided, a delivery that differs from the accepted bytes is refused, a stale one and one
/// that continues the advanced inbound tip alike, before the chain's boundary as after it. The member that
/// sent it is not slashed: the advance that closes the epoch's books records a miss for it.
BOOST_FIXTURE_TEST_CASE(divergent_delivery_after_the_decision_is_refused_and_missed, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t          epoch     = current_epoch();
   const std::vector<char> winner    = encode_delivery(epoch, "decided-first");
   const std::vector<char> divergent = encode_delivery(epoch, "decided-divergent");

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));

   // An envelope chained from the accepted winner passes the inbound-chain validation; the decided epoch
   // refuses it all the same.
   const std::vector<char> continuing = encode_delivery(epoch, "decided-continuing",
                                                        next_stream_tip(winner).envelope_digest,
                                                        next_stream_tip(winner).message_id);

   BOOST_REQUIRE(before_epoch_boundary());
   for (const std::vector<char>* refused : {&divergent, &continuing}) {
      BOOST_REQUIRE_EQUAL(error(DECIDED_EPOCH_DIFFERING_DELIVERY_ERROR),
                          deliver_as(BATCHOP_C, ETH_OUTPOST_ID, *refused));
      produce_blocks();
   }
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_C).is_null());

   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(error(DECIDED_EPOCH_DIFFERING_DELIVERY_ERROR),
                       deliver_as(BATCHOP_C, ETH_OUTPOST_ID, continuing));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   advance_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);

   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE,
                       get_operator(BATCHOP_C)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
   const std::vector<fc::variant> missed = dellog_rows(BATCHOP_C, epoch, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, missed.size());
   BOOST_REQUIRE(!missed.front()[opreg_fields::DELIVERED].as<bool>());
   for (const name deliverer : {BATCHOP, BATCHOP_B}) {
      const std::vector<fc::variant> delivered = dellog_rows(deliverer, epoch, ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(1u, delivered.size());
      BOOST_REQUIRE(delivered.front()[opreg_fields::DELIVERED].as<bool>());
   }
} FC_LOG_AND_RETHROW() }

/// A late delivery, for the chain's previous epoch, of that epoch's accepted bytes from a member of the group that
/// served it is recorded during the next epoch: metadata only, with no `evalcons` (so no second dispatch), no slash
/// and no effect on the current epoch. The current epoch's acceptance shifts the previous winner into `outpcons`'s
/// previous fields, the previous epoch's rows survive every delivery of the current epoch, and the advance that
/// closes the previous epoch's books records the late member as delivered. A delivery for an epoch older than the
/// previous one is refused.
BOOST_FIXTURE_TEST_CASE(late_identical_delivery_is_recorded_at_the_following_advance, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);   // one group of three: epoch 1's group serves epoch 2 too

   const uint32_t          epoch           = current_epoch();
   const std::vector<char> winner          = encode_delivery(epoch, "late-winner");
   const fc::sha256        winner_checksum = fc::sha256::hash(winner.data(), winner.size());
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   {
      const fc::variant consensus = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(0u, consensus[msgch_fields::PREVIOUS_EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(fc::sha256().str(), consensus[msgch_fields::PREVIOUS_WINNING_CHECKSUM].as_string());
   }
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   {
      std::vector<name> previous = previous_group();
      std::sort(previous.begin(), previous.end());
      BOOST_REQUIRE(previous == (std::vector<name>{BATCHOP, BATCHOP_B, BATCHOP_C}));
   }

   // BATCHOP_C's late delivery of epoch 1's winner, while ETH sits in epoch 2.
   const transaction_trace_ptr late = deliver_trace(BATCHOP_C, ETH_OUTPOST_ID, winner);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, action_count(late, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   BOOST_REQUIRE(slashops(late).empty());
   {
      const fc::variant row = find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_C);
      BOOST_REQUIRE(!row.is_null());
      BOOST_REQUIRE_EQUAL(winner_checksum.str(), row[msgch_fields::CHECKSUM].as_string());
      BOOST_REQUIRE(row[msgch_fields::RAW_DATA].as<std::vector<char>>().empty());
   }
   require_outpcons_winner(ETH_OUTPOST_ID, epoch, winner_checksum);
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch + 1));
   BOOST_REQUIRE(dellog_rows(BATCHOP_C, epoch, ETH_OUTPOST_ID).empty());
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: operator already delivered for this outpost+epoch"),
                       deliver_as(BATCHOP_C, ETH_OUTPOST_ID, winner));

   // Epoch 2 decides. Its deliveries prune only older epochs, and its acceptance shifts epoch 1's winner.
   const std::vector<char> second          = encode_chained_delivery("late-second", next_stream_tip(winner));
   const fc::sha256        second_checksum = fc::sha256::hash(second.data(), second.size());
   for (const name member : {BATCHOP, BATCHOP_B, BATCHOP_C}) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(member, ETH_OUTPOST_ID, second));
   }
   produce_blocks();
   BOOST_REQUIRE_EQUAL(kGroupSize, envelope_rows_for(ETH_OUTPOST_ID, epoch));
   require_outpcons_winner(ETH_OUTPOST_ID, epoch + 1, second_checksum);
   {
      const fc::variant consensus = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(epoch, consensus[msgch_fields::PREVIOUS_EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(winner_checksum.str(), consensus[msgch_fields::PREVIOUS_WINNING_CHECKSUM].as_string());
   }

   // The advance from epoch 2 closes epoch 1's books: every member delivered, BATCHOP_C late.
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 2, current_epoch());
   for (const name member : {BATCHOP, BATCHOP_B, BATCHOP_C}) {
      const std::vector<fc::variant> rows = dellog_rows(member, epoch, ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(1u, rows.size());
      BOOST_REQUIRE(rows.front()[opreg_fields::DELIVERED].as<bool>());
      BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(member, ETH_OUTPOST_ID));
   }

   // Epoch 1 is now two epochs old: its winner is refused, and the first delivery of epoch 3 prunes its rows.
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: envelope epoch_index mismatch: envelope=" +
                             std::to_string(epoch) + " current=" + std::to_string(epoch + 2)),
                       deliver_as(BATCHOP_C, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID,
                                             encode_chained_delivery("late-third", next_stream_tip(second))));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, envelope_rows_for(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(kGroupSize, envelope_rows_for(ETH_OUTPOST_ID, epoch + 1));
} FC_LOG_AND_RETHROW() }

/// A late delivery that differs from the previous epoch's accepted envelope is refused, a stale one and one that
/// continues the advanced inbound tip alike, and the member is not slashed: the advance that closes that epoch's
/// books records its miss.
BOOST_FIXTURE_TEST_CASE(late_divergent_delivery_is_refused_and_missed, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t          epoch  = current_epoch();
   const std::vector<char> winner = encode_delivery(epoch, "late-refused-winner");
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());

   const std::vector<char> divergent  = encode_delivery(epoch, "late-refused-divergent");
   const std::vector<char> continuing = encode_delivery(epoch, "late-refused-continuing",
                                                        next_stream_tip(winner).envelope_digest,
                                                        next_stream_tip(winner).message_id);
   for (const std::vector<char>* refused : {&divergent, &continuing}) {
      BOOST_REQUIRE_EQUAL(error(LATE_DELIVERY_DIFFERING_ERROR), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, *refused));
      produce_blocks();
   }
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_C).is_null());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP_C));

   advance_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   const std::vector<fc::variant> missed = dellog_rows(BATCHOP_C, epoch, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, missed.size());
   BOOST_REQUIRE(!missed.front()[opreg_fields::DELIVERED].as<bool>());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP_C));
} FC_LOG_AND_RETHROW() }

/// A late delivery is admitted only from the group that served the previous epoch, and a current-epoch delivery
/// only from the group on duty: with single-member groups the three are disjoint, so the current member's late
/// delivery, the previous member's current-epoch delivery and the next member's delivery of either are refused. A
/// previous epoch with no accepted winner, which the fixture advances undecided, takes no late delivery, and its books
/// record the miss.
BOOST_FIXTURE_TEST_CASE(late_delivery_comes_only_from_the_previous_group, sysio_msgch_chain_tester) { try {
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/true);

   // Epoch 1: its one member decides it.
   const uint32_t          epoch  = current_epoch();
   const name              first  = duty_member();
   const std::vector<char> winner = encode_delivery(epoch, "previous-group-winner");
   BOOST_REQUIRE_EQUAL(success(), deliver_as(first, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   advance_to_next_epoch();
   BOOST_REQUIRE(previous_group() == std::vector<name>{first});

   const name second = duty_member();
   name       third;
   for (const name member : {BATCHOP, BATCHOP_B, BATCHOP_C}) {
      if (member != first && member != second) third = member;
   }
   BOOST_REQUIRE(seated_on(third, ETH_OUTPOST_ID));
   const std::vector<char> current = encode_chained_delivery("previous-group-current", next_stream_tip(winner));

   BOOST_REQUIRE_EQUAL(error(LATE_DELIVERER_NOT_IN_PREVIOUS_GROUP_ERROR), deliver_as(second, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(error(DELIVERER_NOT_IN_ACTIVE_GROUP_ERROR), deliver_as(first, ETH_OUTPOST_ID, current));
   for (const std::vector<char>* envelope : {&winner, &current}) {
      BOOST_REQUIRE_EQUAL(error(DELIVERER_NOT_IN_GROUP_ERROR), deliver_as(third, ETH_OUTPOST_ID, *envelope));
   }
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: operator already delivered for this outpost+epoch"),
                       deliver_as(first, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, second).is_null());
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, third).is_null());

   // Epoch 2 is left undecided and advanced all the same: its member's late delivery has no winner to repeat.
   const uint32_t undecided = current_epoch();
   advance_to_next_epoch();
   BOOST_REQUIRE(previous_group() == std::vector<name>{second});
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, undecided));
   BOOST_REQUIRE_EQUAL(error(LATE_DELIVERY_DIFFERING_ERROR), deliver_as(second, ETH_OUTPOST_ID, current));
   produce_blocks();

   // The advance closing epoch 2's books records its member's miss.
   advance_to_next_epoch();
   const std::vector<fc::variant> missed = dellog_rows(second, undecided, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, missed.size());
   BOOST_REQUIRE(!missed.front()[opreg_fields::DELIVERED].as<bool>());
} FC_LOG_AND_RETHROW() }

/// With the production layout of disjoint groups a member of the previous epoch's group sits in no current group:
/// it delivers only late, for the epoch it served, and the advance that closes that epoch's books records it as
/// delivered and credits its seat. Neither group delivers for the other's epoch.
BOOST_FIXTURE_TEST_CASE(disjoint_previous_group_member_delivers_only_late, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups();
   const std::vector<name>&             first  = window[0];
   const std::vector<name>&             second = window[1];
   const name                           late   = first[2];
   const uint32_t                       epoch  = current_epoch();
   const std::vector<char>              winner = encode_delivery(epoch, "disjoint-winner");
   deliver_all({first[0], first[1]}, winner);
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   advance_decided_epoch();
   BOOST_REQUIRE(sorted(previous_group()) == sorted(first));

   const std::vector<char> current = encode_chained_delivery("disjoint-current", next_stream_tip(winner));
   BOOST_REQUIRE_EQUAL(error(DELIVERER_NOT_IN_ACTIVE_GROUP_ERROR), deliver_as(late, ETH_OUTPOST_ID, current));
   BOOST_REQUIRE_EQUAL(error(LATE_DELIVERER_NOT_IN_PREVIOUS_GROUP_ERROR),
                       deliver_as(second[0], ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(late, ETH_OUTPOST_ID, winner));
   deliver_all({second[0], second[1]}, current);
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch + 1));
   advance_decided_epoch();

   for (const name member : first) {
      require_booked(member, epoch, true);
      BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(member, ETH_OUTPOST_ID));
   }
   for (const name member : second) BOOST_REQUIRE(dellog_rows(member, epoch + 1, ETH_OUTPOST_ID).empty());
} FC_LOG_AND_RETHROW() }

/// A late delivery counts only within the chain's late-delivery window, `late_window_sec` (by default half the
/// minimum) from the next epoch's start: one block before it closes a member's late delivery is taken, and from its
/// close another member's is refused, so the advance that closes the epoch's books records that member's miss.
BOOST_FIXTURE_TEST_CASE(a_late_delivery_counts_only_within_its_window, sysio_msgch_chain_tester) { try {
   constexpr uint32_t     kGroupSize     = 5;
   const fc::microseconds block_interval = fc::milliseconds(config::block_interval_ms);
   bootstrap(kGroupSize);
   BOOST_REQUIRE(late_window() == fc::seconds(EPOCH_DURATION_SEC / 2));
   const uint32_t          epoch  = current_epoch();
   const std::vector<char> winner = encode_delivery(epoch, "windowed-winner");
   deliver_all({BATCHOP, BATCHOP_B, BATCHOP_C}, winner);
   advance_decided_epoch();

   const fc::time_point closes = epoch_start() + late_window();
   open_block_at(closes - block_interval);
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_D, ETH_OUTPOST_ID, winner));
   open_block_at(closes);
   BOOST_REQUIRE_EQUAL(error(LATE_DELIVERY_AFTER_WINDOW_ERROR), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_E).is_null());

   const std::vector<char> current = encode_chained_delivery("windowed-next", next_stream_tip(winner));
   deliver_all({BATCHOP, BATCHOP_B, BATCHOP_C}, current);
   advance_decided_epoch();
   for (const name member : {BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D}) require_booked(member, epoch, true);
   require_booked(BATCHOP_E, epoch, false);
} FC_LOG_AND_RETHROW() }

/// An idle current epoch holds none of the previous epoch's duty back: once its late-delivery window has passed,
/// `chkcons` closes the previous epoch's books, recording each member's delivery or miss, running its `termcheck` and
/// crediting the group's duty, once. Until then they stay open. A second `chkcons` closes nothing, a late delivery
/// after the close is refused, and the advance from the idle epoch records those books nothing more.
BOOST_FIXTURE_TEST_CASE(chkcons_closes_the_previous_books_once_the_late_window_passes, sysio_msgch_chain_tester) { try {
   const fc::microseconds               block_interval = fc::milliseconds(config::block_interval_ms);
   const std::vector<std::vector<name>> window         = bootstrap_disjoint_groups();
   const std::vector<name>&             first          = window[0];
   const std::vector<name>&             second         = window[1];
   const name                           silent         = first[2];
   const uint32_t                       epoch          = current_epoch();
   const std::vector<char>              winner         = encode_delivery(epoch, "closed-books-winner");
   deliver_all({first[0], first[1]}, winner);
   advance_decided_epoch();
   deliver_all({second[0], second[1]}, encode_chained_delivery("closed-books-idle", next_stream_tip(winner)));
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch + 1));
   BOOST_REQUIRE(due_at() == fc::time_point());

   const fc::time_point closes = epoch_start() + late_window();
   open_block_at(closes - block_interval);
   const transaction_trace_ptr open = check_consensus_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, action_count(open, EPOCH_ACCOUNT, epoch_actions::CLOSE_BOOKS));
   BOOST_REQUIRE(sorted(previous_group()) == sorted(first));
   for (const name member : first) BOOST_REQUIRE(dellog_rows(member, epoch, ETH_OUTPOST_ID).empty());

   BOOST_REQUIRE(control->pending_block_time() == closes);
   const transaction_trace_ptr closed = check_consensus_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(1u, action_count(closed, EPOCH_ACCOUNT, epoch_actions::CLOSE_BOOKS));
   BOOST_REQUIRE_EQUAL(DISJOINT_GROUP_SIZE, action_count(closed, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   BOOST_REQUIRE_EQUAL(DISJOINT_GROUP_SIZE, action_count(closed, OPREG_ACCOUNT, opreg_actions::TERMINATION_CHECK));
   BOOST_REQUIRE_EQUAL(1u, action_count(closed, SYSIO_ACCOUNT, system_actions::RECORD_DUTY));
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(previous_group().empty());
   const auto require_closed_once = [&] {
      for (const name member : first) {
         require_booked(member, epoch, member != silent);
         BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(member, ETH_OUTPOST_ID));
      }
   };
   require_closed_once();

   const transaction_trace_ptr again = check_consensus_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, action_count(again, EPOCH_ACCOUNT, epoch_actions::CLOSE_BOOKS));
   BOOST_REQUIRE_EQUAL(0u, action_count(again, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   BOOST_REQUIRE_EQUAL(error(DELIVERER_NOT_IN_GROUP_ERROR), deliver_as(silent, ETH_OUTPOST_ID, winner));
   require_closed_once();

   elapse_epoch_boundary();
   const transaction_trace_ptr advanced = check_consensus_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 2, current_epoch());
   BOOST_REQUIRE_EQUAL(0u, action_count(advanced, EPOCH_ACCOUNT, epoch_actions::CLOSE_BOOKS));
   BOOST_REQUIRE_EQUAL(0u, action_count(advanced, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   BOOST_REQUIRE_EQUAL(0u, action_count(advanced, SYSIO_ACCOUNT, system_actions::RECORD_DUTY));
   require_closed_once();
   BOOST_REQUIRE(sorted(previous_group()) == sorted(second));
} FC_LOG_AND_RETHROW() }

/// `closebooks` is sysio.msgch's alone, and closes nothing while the late-delivery window is open.
BOOST_FIXTURE_TEST_CASE(closebooks_needs_msgch_and_a_passed_late_window, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups();
   const uint32_t                       epoch  = current_epoch();
   deliver_all({window[0][0], window[0][1]}, encode_delivery(epoch, "closebooks-auth"));
   advance_decided_epoch();
   const auto close_books = [&](name signer) {
      return push(EPOCH_ACCOUNT, epoch_abi, signer, epoch_actions::CLOSE_BOOKS,
                  mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE));
   };

   BOOST_REQUIRE_EQUAL(error("missing authority of " + MSGCH_ACCOUNT.to_string()), close_books(EPOCH_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), close_books(MSGCH_ACCOUNT));
   produce_blocks();
   BOOST_REQUIRE(sorted(previous_group()) == sorted(window[0]));
   for (const name member : window[0]) BOOST_REQUIRE(dellog_rows(member, epoch, ETH_OUTPOST_ID).empty());

   open_block_at(epoch_start() + late_window());
   BOOST_REQUIRE_EQUAL(success(), close_books(MSGCH_ACCOUNT));
   produce_blocks();
   BOOST_REQUIRE(previous_group().empty());
   for (const name member : window[0]) require_booked(member, epoch, member != window[0][2]);
} FC_LOG_AND_RETHROW() }

/// A previous epoch decided by dispute resolution takes a late delivery of the voted winner from the member that
/// stayed silent through the dispute, and refuses a losing version; the advance after the next records it as
/// delivered. Four versions of five leave the silent member unable to lift any to three, so the fourth delivery
/// opens the dispute.
BOOST_FIXTURE_TEST_CASE(late_delivery_repeats_a_winner_resolved_by_dispute, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 5;
   bootstrap(kGroupSize);
   const name              late   = BATCHOP_E;
   const std::vector<name> losers{BATCHOP_B, BATCHOP_C, BATCHOP_D};
   const uint32_t          epoch  = current_epoch();
   const std::vector<char> voted  = encode_delivery(epoch, "dispute-voted");
   const fc::sha256        voted_checksum = fc::sha256::hash(voted.data(), voted.size());
   std::vector<std::vector<char>> lost;
   for (const name loser : losers) lost.push_back(encode_delivery(epoch, "dispute-lost-" + loser.to_string()));

   deliver_all({BATCHOP}, voted);
   for (size_t i = 0; i < losers.size(); ++i) deliver_all({losers[i]}, lost[i]);
   BOOST_REQUIRE(epoch_is_paused());
   resolve_tie_dispute(epoch, voted_checksum);
   produce_blocks();
   for (const name loser : losers) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(loser));
   }
   advance_decided_epoch();

   BOOST_REQUIRE_EQUAL(error(LATE_DELIVERY_DIFFERING_ERROR), deliver_as(late, ETH_OUTPOST_ID, lost.front()));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(late, ETH_OUTPOST_ID, voted));
   // The next group is drawn from the members left ACTIVE.
   const std::vector<char> current = encode_chained_delivery("dispute-next", next_stream_tip(voted));
   deliver_all({BATCHOP, late}, current);
   advance_decided_epoch();
   for (const name member : {BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, late}) require_booked(member, epoch, true);
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(late));
} FC_LOG_AND_RETHROW() }

/// A late delivery lands while a dispute pauses the current epoch: it is recorded without any evaluation, the
/// dispute stays as it is, and once the resolution lets the chain advance the previous epoch's books record it.
BOOST_FIXTURE_TEST_CASE(late_delivery_lands_while_the_current_epoch_is_disputed, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups();
   const std::vector<name>&             first  = window[0];
   const std::vector<name>&             second = window[1];
   const name                           late   = first[2];
   const uint32_t                       epoch  = current_epoch();
   const std::vector<char>              winner = encode_delivery(epoch, "paused-winner");
   deliver_all({first[0], first[1]}, winner);
   advance_decided_epoch();

   // Three versions of three members: no version can win, and the third delivery opens the dispute.
   const inbound_stream_tip tip = next_stream_tip(winner);
   std::vector<std::vector<char>> versions;
   for (const std::string payload : {"paused-a", "paused-b", "paused-c"}) {
      versions.push_back(encode_chained_delivery(payload, tip));
   }
   for (size_t i = 0; i < versions.size(); ++i) deliver_all({second[i]}, versions[i]);
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE(!get_dispute(FIRST_DISPUTE_ID).is_null());

   const transaction_trace_ptr trace = deliver_trace(late, ETH_OUTPOST_ID, winner);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, action_count(trace, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   BOOST_REQUIRE_EQUAL(0u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   BOOST_REQUIRE(!find_inbound_delivery(ETH_OUTPOST_ID, epoch, late).is_null());
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE(get_dispute(SECOND_DISPUTE_ID).is_null());

   // The open dispute holds `chkcons`, closing the books included, past the late-delivery window.
   open_block_at(epoch_start() + late_window());
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE(sorted(previous_group()) == sorted(first));

   resolve_tie_dispute(epoch + 1, fc::sha256::hash(versions[0].data(), versions[0].size()));
   produce_blocks();
   advance_decided_epoch();
   for (const name member : first) require_booked(member, epoch, true);
} FC_LOG_AND_RETHROW() }

/// A member of the previous group that has stopped being ACTIVE since its seat, UNKNOWN after a withdrawal below the
/// minimum or SLASHED, is refused its late delivery, and the advance that closes its epoch's books records a miss.
BOOST_FIXTURE_TEST_CASE(previous_group_member_no_longer_active_is_refused_and_missed, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups(/*batchop_is_bootstrapped=*/false);
   const std::vector<name>&             first  = window[0];
   BOOST_REQUIRE(std::find(first.begin(), first.end(), BATCHOP) != first.end());
   std::vector<name> first_others;
   for (const name member : first) {
      if (member != BATCHOP) first_others.push_back(member);
   }

   // Epoch 1: BATCHOP is silent, then withdraws below the minimum and turns UNKNOWN.
   const uint32_t          epoch  = current_epoch();
   const std::vector<char> winner = encode_delivery(epoch, "inactive-winner");
   deliver_all(first_others, winner);
   advance_decided_epoch();
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, BATCHOP, "withdraw"_n, mvo()
      ("account", BATCHOP)("token_code", WIRE_CHAIN_CODE)("amount", BATCH_OPERATOR_MINIMUM_COLLATERAL)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_UNKNOWN, operator_status(BATCHOP));
   BOOST_REQUIRE_EQUAL(error(NOT_ACTIVE_DELIVERY_ERROR), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));

   // Epoch 2: its last member is silent, then slashed.
   const name              slashed = window[1][2];
   const std::vector<char> second  = encode_chained_delivery("inactive-second", next_stream_tip(winner));
   deliver_all({window[1][0], window[1][1]}, second);
   advance_decided_epoch();
   require_booked(BATCHOP, epoch, false);
   slash_operator(slashed);
   BOOST_REQUIRE_EQUAL(error(NOT_ACTIVE_DELIVERY_ERROR), deliver_as(slashed, ETH_OUTPOST_ID, second));

   const std::vector<char> third = encode_chained_delivery("inactive-third", next_stream_tip(second));
   deliver_all({window[2][0], window[2][1]}, third);
   advance_decided_epoch();
   require_booked(slashed, epoch + 1, false);
} FC_LOG_AND_RETHROW() }

/// A `schbatchgps` rebuild of the window keeps `previous_group`: the previous epoch's late delivery is still
/// admitted and its books are still closed for the group that served it.
BOOST_FIXTURE_TEST_CASE(previous_group_survives_a_schedule_rebuild, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups();
   const std::vector<name>&             first  = window[0];
   const name                           late   = first[2];
   const uint32_t                       epoch  = current_epoch();
   const std::vector<char>              winner = encode_delivery(epoch, "rebuild-winner");
   deliver_all({first[0], first[1]}, winner);
   advance_decided_epoch();
   const std::vector<name> previous = previous_group();

   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();
   BOOST_REQUIRE(previous_group() == previous);
   BOOST_REQUIRE_EQUAL(success(), deliver_as(late, ETH_OUTPOST_ID, winner));

   const auto rebuilt = read_epoch_state()[epoch_fields::BATCH_OP_GROUPS].as<std::vector<std::vector<name>>>();
   std::vector<name> deliverers;
   for (const name member : rebuilt.front()) {
      if (member != late && deliverers.size() < 2) deliverers.push_back(member);
   }
   deliver_all(deliverers, encode_chained_delivery("rebuild-next", next_stream_tip(winner)));
   advance_decided_epoch();
   for (const name member : first) require_booked(member, epoch, true);
} FC_LOG_AND_RETHROW() }

/// An epoch the chain closed as an active outpost keeps its books though the chain is no longer one at the
/// (governance) advance that closes them, and its seats are credited; an epoch the chain closed while not an active
/// outpost keeps none.
BOOST_FIXTURE_TEST_CASE(books_of_an_active_epoch_survive_the_chain_turning_inactive, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups();
   const std::vector<name>&             first  = window[0];
   const uint32_t                       epoch  = current_epoch();
   deliver_all(first, encode_delivery(epoch, "inactive-chain"));
   advance_decided_epoch();
   BOOST_REQUIRE(sorted(previous_group()) == sorted(first));

   set_chain_active(ETH_OUTPOST_ID, false);
   produce_blocks();
   const transaction_trace_ptr booked = governance_advance_trace(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(DISJOINT_GROUP_SIZE, action_count(booked, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   for (const name member : first) {
      require_booked(member, epoch, true);
      BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(member, ETH_OUTPOST_ID));
   }
   BOOST_REQUIRE(previous_group().empty());

   const transaction_trace_ptr unbooked = governance_advance_trace(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(0u, action_count(unbooked, OPREG_ACCOUNT, opreg_actions::RECORD_DELIVERY));
   for (const name member : window[1]) BOOST_REQUIRE(dellog_rows(member, epoch + 1, ETH_OUTPOST_ID).empty());
} FC_LOG_AND_RETHROW() }

/// The delivery that gives a version its strict majority slashes, in its own transaction and under sysio.msgch's
/// authority, every member whose delivery differs from it: two minority versions of a group of five here. The
/// members that delivered the winner are never slashed, neither the close nor the advance that closes the epoch's
/// books slashes anyone, and that advance records each slashed member as delivered.
BOOST_FIXTURE_TEST_CASE(divergent_minority_is_slashed_in_the_tipping_delivery, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 5;
   bootstrap(kGroupSize);

   const uint32_t          epoch    = current_epoch();
   const std::vector<char> winner   = encode_delivery(epoch, "tipping-winner");
   const std::vector<char> second   = encode_delivery(epoch, "tipping-second");
   const std::vector<char> third    = encode_delivery(epoch, "tipping-third");
   const std::vector<name> minority{BATCHOP_D, BATCHOP_E};
   const std::vector<name> majority{BATCHOP, BATCHOP_B, BATCHOP_C};

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_D, ETH_OUTPOST_ID, second));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, third));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   for (const name member : minority) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }

   const transaction_trace_ptr tipping = deliver_trace(BATCHOP_C, ETH_OUTPOST_ID, winner);
   const std::vector<slashop_record> slashed = slashops(tipping);
   BOOST_REQUIRE_EQUAL(minority.size(), slashed.size());
   for (size_t i = 0; i < slashed.size(); ++i) {
      BOOST_REQUIRE_EQUAL(minority[i], slashed[i].operator_account);
      BOOST_REQUIRE_EQUAL(non_canonical_delivery_reason(ETH_CHAIN_CODE, epoch), slashed[i].reason);
      BOOST_REQUIRE(slashed[i].authorized_by_msgch);
   }
   BOOST_REQUIRE_EQUAL(minority.size(), action_count(tipping, OPREG_ACCOUNT, opreg_actions::SLASH));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   for (const name member : minority) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(member));
   }
   for (const name member : majority) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }

   elapse_epoch_boundary();
   const transaction_trace_ptr close = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE_EQUAL(1u, action_count(close, EPOCH_ACCOUNT, epoch_actions::ADVANCE));
   BOOST_REQUIRE(slashops(close).empty());
   BOOST_REQUIRE_EQUAL(0u, action_count(close, OPREG_ACCOUNT, opreg_actions::SLASH));
   for (const name member : majority) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }
   const transaction_trace_ptr booked = advance_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   BOOST_REQUIRE(slashops(booked).empty());
   BOOST_REQUIRE_EQUAL(0u, action_count(booked, OPREG_ACCOUNT, opreg_actions::SLASH));
   for (const std::vector<name>* members : {&minority, &majority}) {
      for (const name member : *members) {
         const std::vector<fc::variant> rows = dellog_rows(member, epoch, ETH_OUTPOST_ID);
         BOOST_REQUIRE_EQUAL(1u, rows.size());
         BOOST_REQUIRE(rows.front()[opreg_fields::DELIVERED].as<bool>());
      }
   }
} FC_LOG_AND_RETHROW() }

/// A dispute's resolution slashes its losers in the `chkdispute` transaction, before the epoch advances: every
/// member whose delivery differs from the voted winner, including one that delivered after the dispute opened.
/// Four versions in a group of five leave no version able to win, so the fourth delivery opens the dispute; the
/// fifth member then delivers a losing version. The winner's deliverer is never slashed, and the close slashes
/// no one.
BOOST_FIXTURE_TEST_CASE(dispute_losers_are_slashed_at_resolution, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 5;
   bootstrap(kGroupSize);

   const uint32_t          epoch   = current_epoch();
   const std::vector<char> winner  = encode_delivery(epoch, "resolution-winner");
   const std::vector<char> second  = encode_delivery(epoch, "resolution-second");
   const std::vector<char> third   = encode_delivery(epoch, "resolution-third");
   const std::vector<char> fourth  = encode_delivery(epoch, "resolution-fourth");
   const fc::sha256        winner_checksum = fc::sha256::hash(winner.data(), winner.size());
   const std::vector<name> losers{BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E};

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, second));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, third));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   const transaction_trace_ptr opening = deliver_trace(BATCHOP_D, ETH_OUTPOST_ID, fourth);
   BOOST_REQUIRE_EQUAL(1u, action_count(opening, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   BOOST_REQUIRE(slashops(opening).empty());
   produce_blocks();
   BOOST_REQUIRE(epoch_is_paused());

   // E delivers a losing version while the dispute is open: recorded, and judged at the resolution.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE(!find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_E).is_null());
   for (const name member : losers) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }

   BOOST_REQUIRE_EQUAL(success(), vote_dispute(FIRST_DISPUTE_ID, winner_checksum));
   const transaction_trace_ptr resolution = sysio_system::test_support::push_contract_action_trace(
      *this, CHALG_ACCOUNT, chalg_abi, BATCHOP, chalg_actions::CHECK_DISPUTE,
      mvo()(chalg_vote_fields::DISPUTE_ID, FIRST_DISPUTE_ID));
   const std::vector<slashop_record> slashed = slashops(resolution);
   BOOST_REQUIRE_EQUAL(losers.size(), slashed.size());
   for (size_t i = 0; i < slashed.size(); ++i) {
      BOOST_REQUIRE_EQUAL(losers[i], slashed[i].operator_account);
      BOOST_REQUIRE_EQUAL(non_canonical_delivery_reason(ETH_CHAIN_CODE, epoch), slashed[i].reason);
      BOOST_REQUIRE(slashed[i].authorized_by_msgch);
   }
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE(!epoch_is_paused());
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   for (const name member : losers) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(member));
   }
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP));

   elapse_epoch_boundary();
   const transaction_trace_ptr close = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(slashops(close).empty());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP));
} FC_LOG_AND_RETHROW() }

/// Once a dispute's resolution has accepted the winner, the epoch takes only the winner's bytes: the member that
/// stayed silent through the dispute has a delivery of the winner recorded, and a differing one refused.
BOOST_FIXTURE_TEST_CASE(delivery_after_resolution_takes_only_the_winner, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 5;
   bootstrap(kGroupSize);

   const uint32_t          epoch           = current_epoch();
   const std::vector<char> winner          = encode_delivery(epoch, "resolved-winner");
   const std::vector<char> second          = encode_delivery(epoch, "resolved-second");
   const std::vector<char> third           = encode_delivery(epoch, "resolved-third");
   const std::vector<char> fourth          = encode_delivery(epoch, "resolved-fourth");
   const fc::sha256        winner_checksum = fc::sha256::hash(winner.data(), winner.size());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, second));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, third));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_D, ETH_OUTPOST_ID, fourth));
   produce_blocks();
   BOOST_REQUIRE(epoch_is_paused());
   resolve_tie_dispute(epoch, winner_checksum);
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));

   BOOST_REQUIRE_EQUAL(error(DECIDED_EPOCH_DIFFERING_DELIVERY_ERROR), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_E).is_null());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, winner));
   produce_blocks();
   const fc::variant row = find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_E);
   BOOST_REQUIRE(!row.is_null());
   BOOST_REQUIRE_EQUAL(row[msgch_fields::CHECKSUM].as_string(), winner_checksum.str());
   BOOST_REQUIRE(row[msgch_fields::RAW_DATA].as<std::vector<char>>().empty());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP_E));
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);
} FC_LOG_AND_RETHROW() }

/// A governance advance (sysio.epoch's own authority) of a decided epoch slashes no one: the acceptance already
/// slashed the member whose delivery differs from the winner. The next advance, which closes the epoch's books,
/// slashes no one either and records it as delivered.
BOOST_FIXTURE_TEST_CASE(governance_advance_of_a_decided_epoch_slashes_nothing, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t          epoch     = current_epoch();
   const std::vector<char> winner    = encode_delivery(epoch, "governance-winner");
   const std::vector<char> divergent = encode_delivery(epoch, "governance-divergent");

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, divergent));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_C));

   elapse_epoch_boundary();
   const transaction_trace_ptr close = sysio_system::test_support::push_contract_action_trace(
      *this, EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
      mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(slashops(close).empty());
   BOOST_REQUIRE_EQUAL(0u, action_count(close, OPREG_ACCOUNT, opreg_actions::SLASH));
   for (const name member : {BATCHOP, BATCHOP_B}) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }
   BOOST_REQUIRE(dellog_rows(BATCHOP_C, epoch, ETH_OUTPOST_ID).empty());

   const transaction_trace_ptr booked = advance_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   BOOST_REQUIRE(slashops(booked).empty());
   BOOST_REQUIRE_EQUAL(0u, action_count(booked, OPREG_ACCOUNT, opreg_actions::SLASH));
   const std::vector<fc::variant> rows = dellog_rows(BATCHOP_C, epoch, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   BOOST_REQUIRE(rows.front()[opreg_fields::DELIVERED].as<bool>());
} FC_LOG_AND_RETHROW() }

/// Governance cannot advance an active outpost whose current epoch has not reached consensus, even once the chain is
/// due: the outpost's next envelope chains to that epoch's winner. It advances the epoch once it is decided, and a
/// chain that is no longer an active outpost undecided.
BOOST_FIXTURE_TEST_CASE(governance_advance_refuses_an_active_outposts_undecided_epoch, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t epoch = current_epoch();
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(GOVERNANCE_ADVANCE_UNDECIDED_ERROR), governance_advance());
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());

   deliver_all({BATCHOP, BATCHOP_B}, encode_delivery(epoch, "governance-decided"));
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   governance_advance_trace(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch + 1));

   set_chain_active(ETH_OUTPOST_ID, false);
   produce_blocks();
   governance_advance_trace(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(epoch + 2, current_epoch());
} FC_LOG_AND_RETHROW() }

/// Recovery of a split held open by a silent member. Of three seats, two deliver different versions and the third is
/// silent, so either version can still win: the re-tally opens no dispute and governance cannot advance the epoch.
/// Once governance terminates the silent member, the chkcons re-tally finds that no version can win and opens the
/// dispute; its resolution decides the epoch, slashes the loser and records the chain's traffic, and the chain
/// advances to its next group.
BOOST_FIXTURE_TEST_CASE(split_held_open_by_a_silent_member_recovers_by_termination_and_retally,
                        sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t          epoch         = current_epoch();
   const std::vector<char> winner        = encode_delivery(epoch, "silent-member-winner");
   const std::vector<char> loser         = encode_delivery(epoch, "silent-member-loser");
   const fc::sha256        winner_digest = fc::sha256::hash(winner.data(), winner.size());
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, loser));
   elapse_epoch_boundary();
   const transaction_trace_ptr waiting = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   BOOST_REQUIRE_EQUAL(0u, action_count(waiting, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(GOVERNANCE_ADVANCE_UNDECIDED_ERROR), governance_advance());

   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, opreg_actions::TERMINATE,
      mvo()(opreg_fields::ACCOUNT, BATCHOP_C.to_string())(opreg_fields::REASON, std::string("silent member"))));
   produce_blocks();
   const transaction_trace_ptr retally = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   BOOST_REQUIRE_EQUAL(1u, action_count(retally, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   assert_open_tie_dispute(epoch, {winner_digest, fc::sha256::hash(loser.data(), loser.size())},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused());

   resolve_tie_dispute(epoch, winner_digest);
   BOOST_REQUIRE(!epoch_is_paused());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_B));
   advance_decided_epoch();
} FC_LOG_AND_RETHROW() }

/// The depot side of recovering a group that can no longer reach its seat majority; the outposts also need an upgrade
/// that installs the reseated window. Of three seats, one member delivers and the other two are slashed: one delivery
/// can never reach two of three, and one version leaves nothing to dispute, so the re-tally neither decides nor
/// disputes and governance cannot advance the epoch. Once two replacements serve the chain, a `schbatchgps` reseat
/// puts a group on duty that leaves out the slashed members; its deliveries of the outpost's envelope decide the
/// epoch, and the chain advances.
BOOST_FIXTURE_TEST_CASE(reseat_decides_an_epoch_below_the_seat_majority_on_the_depot, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window   = bootstrap_disjoint_groups();
   const std::vector<name>&             first    = window[0];
   const uint32_t                       epoch    = current_epoch();
   const std::vector<char>              envelope = encode_delivery(epoch, "reseat-envelope");
   BOOST_REQUIRE_EQUAL(success(), deliver_as(first[0], ETH_OUTPOST_ID, envelope));
   produce_blocks();
   slash_operator(first[1]);
   slash_operator(first[2]);

   const transaction_trace_ptr retally = check_consensus_trace(ETH_OUTPOST_ID, first[0]);
   BOOST_REQUIRE_EQUAL(1u, action_count(retally, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   BOOST_REQUIRE_EQUAL(0u, action_count(retally, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   elapse_epoch_boundary();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(GOVERNANCE_ADVANCE_UNDECIDED_ERROR), governance_advance());

   // The reseat seats `batch_operator_minimum_active` ACTIVE operators serving the chain: the replacements make up
   // for the slashed members.
   const std::vector<name> replacements{BATCHOP_J, BATCHOP_K};
   create_accounts(replacements);
   for (const name op : replacements) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", op.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   }
   serve_chains(replacements, epoch_chains);
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();

   const std::vector<name> on_duty = current_group();
   std::vector<name>       deliverers;
   for (const name member : on_duty) {
      BOOST_REQUIRE(member != first[1] && member != first[2]);
      if (member != first[0]) deliverers.push_back(member);
   }
   deliver_all(deliverers, envelope);
   require_outpcons_winner(ETH_OUTPOST_ID, epoch, fc::sha256::hash(envelope.data(), envelope.size()));
   advance_decided_epoch();
} FC_LOG_AND_RETHROW() }

/// A slashed member keeps its seat, so it never lowers the number of deliveries a majority needs. Two of four is no
/// majority, and stays none once a silent member is slashed: the chkcons re-tally leaves the epoch undecided, with no
/// dispute while the last eligible member can still give the version its third, and that member's delivery decides it.
BOOST_FIXTURE_TEST_CASE(slashed_seat_still_counts_toward_the_majority, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 4;
   bootstrap(kGroupSize);

   const uint32_t          epoch    = current_epoch();
   const std::vector<char> envelope = encode_delivery(epoch, "slashed-seat");
   const fc::sha256        checksum = fc::sha256::hash(envelope.data(), envelope.size());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));

   slash_operator(BATCHOP_D);
   const transaction_trace_ptr trace = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   BOOST_REQUIRE_EQUAL(0u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(get_outpcons(ETH_OUTPOST_ID)[msgch_fields::WINNING_CHECKSUM].as_string(), checksum.str());
} FC_LOG_AND_RETHROW() }

/// The eligible set shrinks with no new delivery. A 2-2 split of five can still be decided by the silent
/// member; once that member is slashed, no version can reach three, the majority the five seats still need. The
/// chkcons re-tally opens the dispute in its own transaction, before the chain's boundary.
BOOST_FIXTURE_TEST_CASE(eligibility_shrink_opens_a_dispute_on_the_chkcons_retally, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize             = 5;
   constexpr uint32_t kOperatorsPerVersion   = 2;
   bootstrap(kGroupSize);

   const uint32_t          epoch = current_epoch();
   const std::vector<char> left  = encode_delivery(epoch, "shrink-left");
   const std::vector<char> right = encode_delivery(epoch, "shrink-right");

   for (const name op : {BATCHOP, BATCHOP_B}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, left));
   for (const name op : {BATCHOP_C, BATCHOP_D}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, right));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused());

   slash_operator(BATCHOP_E);
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   const transaction_trace_ptr trace = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   BOOST_REQUIRE(before_epoch_boundary());
   produce_blocks();
   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(left.data(), left.size()), fc::sha256::hash(right.data(), right.size())},
                           {kOperatorsPerVersion, kOperatorsPerVersion});
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
} FC_LOG_AND_RETHROW() }

/// The chkcons re-tally also decides an epoch once a member is eligible again. Of five seats, X and Y deliver A and Z
/// delivers B. Y withdraws below its collateral minimum and turns UNKNOWN, so W's A gives A two of the three eligible
/// deliveries a majority of the seats needs, while the silent fifth member can still give it the third. Y cancels its
/// withdrawal and is ACTIVE again with no new delivery: the re-tally decides A in the chkcons transaction and slashes Z
/// there.
BOOST_FIXTURE_TEST_CASE(member_active_again_decides_the_epoch_on_the_chkcons_retally, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize        = 5;
   constexpr uint64_t kWithdrawRequest  = 1;
   bootstrap(kGroupSize, /*batchop_is_bootstrapped=*/false);
   // BATCHOP alone carries a collateral minimum, so it is the member that withdraws below it.
   const name x = BATCHOP_B;
   const name y = BATCHOP;
   const name z = BATCHOP_C;
   const name w = BATCHOP_D;

   const uint32_t          epoch    = current_epoch();
   const std::vector<char> accepted = encode_delivery(epoch, "regained-a");
   const std::vector<char> other    = encode_delivery(epoch, "regained-b");
   for (const name op : {x, y}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, accepted));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(z, ETH_OUTPOST_ID, other));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, y, "withdraw"_n, mvo()
      ("account", y)("token_code", WIRE_CHAIN_CODE)("amount", BATCH_OPERATOR_MINIMUM_COLLATERAL)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_UNKNOWN, operator_status(y));

   // Two eligible deliveries of A out of the three needed, and a silent member that can still give it the third.
   const transaction_trace_ptr second = deliver_trace(w, ETH_OUTPOST_ID, accepted);
   BOOST_REQUIRE_EQUAL(0u, action_count(second, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   BOOST_REQUIRE(slashops(second).empty());
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, y, "cancelwtdw"_n,
                                       mvo()("account", y)("request_id", kWithdrawRequest)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(y));
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));

   const transaction_trace_ptr retally = check_consensus_trace(ETH_OUTPOST_ID, x);
   BOOST_REQUIRE_EQUAL(1u, action_count(retally, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   const std::vector<slashop_record> slashed = slashops(retally);
   BOOST_REQUIRE_EQUAL(1u, slashed.size());
   BOOST_REQUIRE_EQUAL(z, slashed.front().operator_account);
   BOOST_REQUIRE_EQUAL(non_canonical_delivery_reason(ETH_CHAIN_CODE, epoch), slashed.front().reason);
   BOOST_REQUIRE(slashed.front().authorized_by_msgch);
   produce_blocks();
   require_outpcons_winner(ETH_OUTPOST_ID, epoch, fc::sha256::hash(accepted.data(), accepted.size()));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(z));
   for (const name member : {x, y, w}) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }
} FC_LOG_AND_RETHROW() }

/// A 1-1 split of a group of two leaves no version able to win. The second delivery's own transaction
/// opens the dispute, before the chain's boundary and with no crank.
BOOST_FIXTURE_TEST_CASE(one_to_one_tie_opens_a_dispute_on_its_terminal_delivery, sysio_msgch_chain_tester) { try {
   bootstrap(ONE_TO_ONE_TIE_GROUP_SIZE);

   const uint32_t epoch = current_epoch();
   const auto left = encode_delivery(epoch, std::string(ONE_TO_ONE_LEFT_PAYLOAD));
   const auto right = encode_delivery(epoch, std::string(ONE_TO_ONE_RIGHT_PAYLOAD));

   // The silent member can still make the delivered version a majority.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, left));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   const transaction_trace_ptr trace = deliver_trace(BATCHOP_B, ETH_OUTPOST_ID, right);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   BOOST_REQUIRE(before_epoch_boundary());
   produce_blocks();

   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(left.data(), left.size()),
                            fc::sha256::hash(right.data(), right.size())},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused());
   resolve_tie_dispute(epoch, fc::sha256::hash(left.data(), left.size()));
} FC_LOG_AND_RETHROW() }

/// The same rule for a larger even 3-3 split, with the full candidate tallies: the dispute opens on the
/// sixth delivery, the first after which neither version can reach four of six.
BOOST_FIXTURE_TEST_CASE(three_to_three_tie_opens_a_dispute_on_its_terminal_delivery, sysio_msgch_chain_tester) { try {
   bootstrap(THREE_TO_THREE_TIE_GROUP_SIZE);

   const uint32_t epoch = current_epoch();
   const auto left = encode_delivery(epoch, std::string(THREE_TO_THREE_LEFT_PAYLOAD));
   const auto right = encode_delivery(epoch, std::string(THREE_TO_THREE_RIGHT_PAYLOAD));
   const std::vector<name> left_operators{BATCHOP, BATCHOP_B, BATCHOP_C};
   const std::vector<name> waiting_right_operators{BATCHOP_D, BATCHOP_E};

   for (const auto& operator_name : left_operators) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(operator_name, ETH_OUTPOST_ID, left));
      produce_blocks();
   }
   // 3-1 and 3-2: a silent member can still give the left version its fourth.
   for (const auto& operator_name : waiting_right_operators) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(operator_name, ETH_OUTPOST_ID, right));
      produce_blocks();
      BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   }

   const transaction_trace_ptr trace = deliver_trace(BATCHOP_F, ETH_OUTPOST_ID, right);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();

   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(left.data(), left.size()),
                            fc::sha256::hash(right.data(), right.size())},
                           {THREE_OPERATORS_PER_TIED_VERSION, THREE_OPERATORS_PER_TIED_VERSION});
   resolve_tie_dispute(epoch, fc::sha256::hash(left.data(), left.size()));
} FC_LOG_AND_RETHROW() }

/// A group of seven needs four. 3-2 with two silent waits, and so does 3-3 with one silent (the last
/// member can still give either version its fourth), through the chkcons re-tally as well. The seventh
/// member's third version leaves 3-3-1 with none silent: its delivery opens the dispute.
BOOST_FIXTURE_TEST_CASE(split_of_seven_disputes_only_once_no_version_can_reach_four, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kLeadingVersionCount = 3;
   constexpr uint32_t kThirdVersionCount   = 1;
   bootstrap(MAXIMUM_GROUP_SIZE);

   const uint32_t          epoch  = current_epoch();
   const std::vector<char> first  = encode_delivery(epoch, "seven-first");
   const std::vector<char> second = encode_delivery(epoch, "seven-second");
   const std::vector<char> third  = encode_delivery(epoch, "seven-third");

   for (const name op : {BATCHOP, BATCHOP_B, BATCHOP_C}) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, first));
   }
   for (const name op : {BATCHOP_D, BATCHOP_E}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_F, ETH_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused());
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));

   const transaction_trace_ptr trace = deliver_trace(BATCHOP_G, ETH_OUTPOST_ID, third);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(first.data(), first.size()),
                            fc::sha256::hash(second.data(), second.size()),
                            fc::sha256::hash(third.data(), third.size())},
                           {kLeadingVersionCount, kLeadingVersionCount, kThirdVersionCount});
   BOOST_REQUIRE(epoch_is_paused());
} FC_LOG_AND_RETHROW() }

/// A 2-2-2 split of seven opens the dispute while a member is still silent: its one delivery would leave
/// every version at three, short of four. When that member then delivers, its row is recorded with its
/// bytes and the open dispute is left as it was.
BOOST_FIXTURE_TEST_CASE(three_way_split_of_seven_disputes_with_a_member_still_silent, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kOperatorsPerVersion = 2;
   bootstrap(MAXIMUM_GROUP_SIZE);

   const uint32_t          epoch  = current_epoch();
   const std::vector<char> first  = encode_delivery(epoch, "three-way-first");
   const std::vector<char> second = encode_delivery(epoch, "three-way-second");
   const std::vector<char> third  = encode_delivery(epoch, "three-way-third");

   for (const name op : {BATCHOP, BATCHOP_B}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, first));
   for (const name op : {BATCHOP_C, BATCHOP_D}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, second));
   // 2-2-1 with two silent: either silent pair can still give a version its fourth.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, third));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   const transaction_trace_ptr trace = deliver_trace(BATCHOP_F, ETH_OUTPOST_ID, third);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(first.data(), first.size()),
                            fc::sha256::hash(second.data(), second.size()),
                            fc::sha256::hash(third.data(), third.size())},
                           {kOperatorsPerVersion, kOperatorsPerVersion, kOperatorsPerVersion});
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_G).is_null());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_G, ETH_OUTPOST_ID, first));
   produce_blocks();
   const fc::variant late_row = find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_G);
   BOOST_REQUIRE(!late_row.is_null());
   BOOST_REQUIRE(late_row[msgch_fields::RAW_DATA].as<std::vector<char>>() == first);
   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(first.data(), first.size()),
                            fc::sha256::hash(second.data(), second.size()),
                            fc::sha256::hash(third.data(), third.size())},
                           {kOperatorsPerVersion, kOperatorsPerVersion, kOperatorsPerVersion});
   BOOST_REQUIRE(get_dispute(SECOND_DISPUTE_ID).is_null());
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
} FC_LOG_AND_RETHROW() }

/// A group of four needs three. On ETH 1-1-1 with one silent opens the dispute on the third delivery: the
/// last member can lift no version above two. On SOL 2-1 with one silent waits, and the last member's
/// delivery of the leading version decides it.
BOOST_FIXTURE_TEST_CASE(four_member_split_disputes_one_one_one_and_waits_on_two_one, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 4;
   bootstrap(kGroupSize);

   const uint32_t          epoch  = current_epoch();
   const std::vector<char> first  = encode_delivery(epoch, "four-first");
   const std::vector<char> second = encode_delivery(epoch, "four-second");
   const std::vector<char> third  = encode_delivery(epoch, "four-third");

   // ETH: 1-1 with two silent waits; 1-1-1 with one silent disputes.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, first));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   const transaction_trace_ptr trace = deliver_trace(BATCHOP_C, ETH_OUTPOST_ID, third);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(first.data(), first.size()),
                            fc::sha256::hash(second.data(), second.size()),
                            fc::sha256::hash(third.data(), third.size())},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION,
                            ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused(ETH_OUTPOST_ID));

   // SOL: 2-1 with one silent waits, through the chkcons re-tally as well.
   for (const name op : {BATCHOP, BATCHOP_B}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, SOL_OUTPOST_ID, first));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, SOL_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), check_consensus(SOL_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(SECOND_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused(SOL_OUTPOST_ID));
   BOOST_REQUIRE(!epoch_decided(SOL_OUTPOST_ID, epoch));

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_D, SOL_OUTPOST_ID, first));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(SOL_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(get_outpcons(SOL_OUTPOST_ID)[msgch_fields::WINNING_CHECKSUM].as_string(),
                       fc::sha256::hash(first.data(), first.size()).str());
   BOOST_REQUIRE(get_dispute(SECOND_DISPUTE_ID).is_null());
} FC_LOG_AND_RETHROW() }

/// Exactly half is no majority: three matching deliveries of a group of six do not decide the epoch, on
/// delivery or on the chkcons re-tally. The fourth does.
BOOST_FIXTURE_TEST_CASE(half_of_an_even_group_does_not_decide, sysio_msgch_chain_tester) { try {
   bootstrap(THREE_TO_THREE_TIE_GROUP_SIZE);

   const uint32_t          epoch    = current_epoch();
   const std::vector<char> envelope = encode_delivery(epoch, "half-of-six");

   for (const name op : {BATCHOP, BATCHOP_B, BATCHOP_C}) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, envelope));
   }
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_D, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
} FC_LOG_AND_RETHROW() }

/// A member that delivers and then leaves the eligible set is not a dispute candidate. In a group of
/// three, A delivers v1 and is slashed; B's v2 and C's v3 then split the two members left, and the dispute
/// lists v2 and v3 only.
BOOST_FIXTURE_TEST_CASE(ineligible_delivery_is_not_a_dispute_candidate, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 3;
   bootstrap(kGroupSize);

   const uint32_t          epoch  = current_epoch();
   const std::vector<char> first  = encode_delivery(epoch, "ineligible-first");
   const std::vector<char> second = encode_delivery(epoch, "ineligible-second");
   const std::vector<char> third  = encode_delivery(epoch, "ineligible-third");

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, first));
   produce_blocks();
   slash_operator(BATCHOP);

   // B's v2 against C still silent: C can still make it a majority of two.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, second));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());

   const transaction_trace_ptr trace = deliver_trace(BATCHOP_C, ETH_OUTPOST_ID, third);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(second.data(), second.size()),
                            fc::sha256::hash(third.data(), third.size())},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(!find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP).is_null());
} FC_LOG_AND_RETHROW() }

/// A split that a silent member can still decide waits for that member, however long it stays silent. 3-2-1 of
/// seven, with one member silent, can still reach four: the chkcons re-tally many minimums later opens no dispute
/// and leaves the epoch undecided and unpaused, and the silent member's delivery of the leading version decides it.
BOOST_FIXTURE_TEST_CASE(split_kept_open_by_a_silent_member_waits_for_its_delivery, sysio_msgch_chain_tester) { try {
   constexpr int64_t kMinimumsSilent = 10;
   bootstrap(MAXIMUM_GROUP_SIZE);

   const uint32_t          epoch  = current_epoch();
   const std::vector<char> first  = encode_delivery(epoch, "silent-first");
   const std::vector<char> second = encode_delivery(epoch, "silent-second");
   const std::vector<char> third  = encode_delivery(epoch, "silent-third");
   for (const name op : {BATCHOP, BATCHOP_B, BATCHOP_C}) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, first));
   }
   for (const name op : {BATCHOP_D, BATCHOP_E}) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, second));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_F, ETH_OUTPOST_ID, third));

   open_block_at(control->head().block_time() + fc::seconds(kMinimumsSilent * EPOCH_DURATION_SEC));
   const transaction_trace_ptr trace = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   BOOST_REQUIRE_EQUAL(1u, action_count(trace, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS));
   BOOST_REQUIRE_EQUAL(0u, action_count(trace, CHALG_ACCOUNT, chalg_actions::OPEN_DISPUTE));
   produce_blocks();
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused());
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_G, ETH_OUTPOST_ID, first));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
} FC_LOG_AND_RETHROW() }

/// A 1-1 split with one silent eligible operator is not terminal. The chkcons re-tally leaves it
/// undecided and unpaused, so the final delivery establishes a strict majority without a vote.
BOOST_FIXTURE_TEST_CASE(chkcons_waits_for_incomplete_two_way_split,
                        sysio_msgch_chain_tester) { try {
   bootstrap(INCOMPLETE_TWO_WAY_SPLIT_GROUP_SIZE);

   const uint32_t epoch = current_epoch();
   const auto left = encode_delivery(epoch, std::string(ONE_TO_ONE_LEFT_PAYLOAD));
   const auto right = encode_delivery(epoch, std::string(ONE_TO_ONE_RIGHT_PAYLOAD));
   const auto left_checksum = fc::sha256::hash(left.data(), left.size());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, left));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, right));
   produce_blocks();

   advance_via_consensus();

   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused());
   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(opc.is_null() || !opc[msgch_fields::CONSENSUS_REACHED].as<bool>());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, left));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc[msgch_fields::CONSENSUS_REACHED].as<bool>(), true);
   BOOST_REQUIRE_EQUAL(opc[msgch_fields::EPOCH_INDEX].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc[msgch_fields::WINNING_CHECKSUM].as_string(), left_checksum.str());
} FC_LOG_AND_RETHROW() }

/// `evalcons` evaluates only the chain's current epoch, whose group is the one on duty. Two of four deliver an
/// earlier epoch, then the two silent members are slashed, so the advance seats the two who delivered: their
/// deliveries would hold a majority of the current group, and are never accepted.
BOOST_FIXTURE_TEST_CASE(evalcons_ignores_an_epoch_that_is_not_current, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 4;
   bootstrap(kGroupSize);

   const uint32_t          earlier_epoch = current_epoch();
   const std::vector<char> envelope      = encode_delivery(earlier_epoch, "earlier-epoch");
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   slash_operator(BATCHOP_C);
   slash_operator(BATCHOP_D);
   BOOST_REQUIRE_EQUAL(earlier_epoch + 1, advance_one_epoch());
   const auto window = read_epoch_state()[epoch_fields::BATCH_OP_GROUPS].as<std::vector<std::vector<name>>>();
   BOOST_REQUIRE(sorted(window.front()) == sorted({BATCHOP, BATCHOP_B}));

   BOOST_REQUIRE_EQUAL(success(), push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, msgch_actions::EVALUATE_CONSENSUS,
                                       mvo()(msgch_fields::CHAIN_CODE, ETH_OUTPOST_ID)
                                            (msgch_fields::EPOCH_INDEX, earlier_epoch)));
   produce_blocks();
   BOOST_REQUIRE(get_outpcons(ETH_OUTPOST_ID).is_null());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, earlier_epoch), 0u);
} FC_LOG_AND_RETHROW() }

/// A terminal tie with no Tier-1 electorate must soft-decline, on its terminal delivery and on the
/// `chkcons` re-tally, leaving the epoch unpaused and retryable. Once a Tier-1 owner registers, the same
/// crank opens the dispute.
struct sysio_msgch_empty_roa_tester : sysio_msgch_chain_tester {
   sysio_msgch_empty_roa_tester() : sysio_msgch_chain_tester(/*empty_roa=*/true) {}
};

BOOST_FIXTURE_TEST_CASE(chkcons_retries_terminal_tie_after_tier_one_registration,
                        sysio_msgch_empty_roa_tester) { try {
   bootstrap(ONE_TO_ONE_TIE_GROUP_SIZE);

   const uint32_t epoch = current_epoch();
   const auto left = encode_delivery(epoch, std::string(ONE_TO_ONE_LEFT_PAYLOAD));
   const auto right = encode_delivery(epoch, std::string(ONE_TO_ONE_RIGHT_PAYLOAD));

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, left));
   produce_blocks();

   // The terminal conflicting delivery leaves no version able to win. The msgch preflight must
   // soft-decline instead of reverting this delivery or pausing the epoch.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, right));
   produce_blocks();

   advance_via_consensus();

   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused());

   register_tier_one_node_owner(TIER_ONE_OWNER);
   advance_via_consensus();

   assert_open_tie_dispute(epoch,
                           {fc::sha256::hash(left.data(), left.size()),
                            fc::sha256::hash(right.data(), right.size())},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
} FC_LOG_AND_RETHROW() }

/// An epoch that reached consensus is decided. Its acceptance slashes the member whose delivery differs from
/// the winner, in the delivery that tips it. When the chain's group then changes mid-epoch, the epoch opens no
/// dispute and keeps its recorded winner, and its close slashes no one.
BOOST_FIXTURE_TEST_CASE(decided_epoch_opens_no_dispute_after_its_group_changes, sysio_msgch_chain_tester) { try {
   constexpr uint32_t         kOriginalGroupSize = 3;
   constexpr uint32_t         kRebuiltGroupSize  = 1;
   constexpr uint32_t         kGroups            = 1;
   constexpr std::string_view kWinnerPayload     = "decided-winner";
   constexpr std::string_view kDivergentPayload  = "decided-divergent";
   namespace served = sysio_system::test_support::served_chains;

   bootstrap(kOriginalGroupSize);
   const uint32_t          epoch           = current_epoch();
   const std::vector<char> winner          = encode_delivery(epoch, std::string(kWinnerPayload));
   const std::vector<char> divergent       = encode_delivery(epoch, std::string(kDivergentPayload));
   const fc::sha256        winner_checksum = fc::sha256::hash(winner.data(), winner.size());

   // Group {A, B, C}: C delivers a divergent envelope, then A and B the winner. B's delivery records the
   // two-of-three majority and slashes C. C delivers first because a divergent envelope after the
   // acceptance is refused.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, divergent));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, winner));
   produce_blocks();
   {
      const fc::variant consensus = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(!consensus.is_null());
      BOOST_REQUIRE_EQUAL(consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>(), epoch);
      BOOST_REQUIRE_EQUAL(consensus[msgch_fields::WINNING_CHECKSUM].as_string(), winner_checksum.str());
   }
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_C));

   // The group is rebuilt mid-epoch: A and B leave the chain, D joins it, and the schedule seats {D}; the
   // slashed C is no longer schedulable.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, {ETH_CHAIN_CODE});
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, BATCHOP, ETH_CHAIN_CODE));
   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, BATCHOP_B, ETH_CHAIN_CODE));
   set_epoch_config(ETH_CHAIN_CODE, EPOCH_DURATION_SEC, kRebuiltGroupSize, kGroups);
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   BOOST_REQUIRE(seated_on(BATCHOP_D, ETH_OUTPOST_ID));
   for (const name unseated : {BATCHOP, BATCHOP_B, BATCHOP_C}) BOOST_REQUIRE(!seated_on(unseated, ETH_OUTPOST_ID));

   // D confirms the recorded winner. The decided epoch opens no dispute, stays unpaused, and keeps its
   // winner, dispatched once.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_D, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(!find_inbound_delivery(ETH_OUTPOST_ID, epoch, BATCHOP_D).is_null());
   BOOST_REQUIRE(get_dispute(FIRST_DISPUTE_ID).is_null());
   BOOST_REQUIRE(!epoch_is_paused());
   {
      const fc::variant consensus = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(consensus[msgch_fields::EPOCH_INDEX].as<uint32_t>(), epoch);
      BOOST_REQUIRE_EQUAL(consensus[msgch_fields::WINNING_CHECKSUM].as_string(), winner_checksum.str());
   }
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);

   // The close, past the chain's boundary, slashes no one; the members that delivered the winner stay ACTIVE.
   elapse_epoch_boundary();
   const transaction_trace_ptr close = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP_D);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(slashops(close).empty());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_C));
   for (const name canonical : {BATCHOP, BATCHOP_B, BATCHOP_D}) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(canonical));
   }
} FC_LOG_AND_RETHROW() }

/// A member that delivered a divergent envelope and is then seated out of the chain's group by a mid-epoch
/// rebuild, while staying ACTIVE, is still slashed when the new group decides the epoch: the acceptance judges
/// every recorded delivery, not only the eligible ones.
BOOST_FIXTURE_TEST_CASE(divergent_deliverer_seated_out_mid_epoch_is_slashed_at_acceptance,
                        sysio_msgch_chain_tester) { try {
   constexpr uint32_t kOriginalGroupSize = 3;
   constexpr uint32_t kRebuiltGroupSize  = 2;
   constexpr uint32_t kGroups            = 1;
   bootstrap(kOriginalGroupSize);

   const uint32_t          epoch     = current_epoch();
   const std::vector<char> winner    = encode_delivery(epoch, "rebuilt-winner");
   const std::vector<char> divergent = encode_delivery(epoch, "rebuilt-divergent");

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, divergent));
   produce_blocks();

   // The rebuild keeps the first two of the three ACTIVE serving operators: C leaves the group and stays ACTIVE.
   set_epoch_config(ETH_CHAIN_CODE, EPOCH_DURATION_SEC, kRebuiltGroupSize, kGroups);
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   BOOST_REQUIRE(!seated_on(BATCHOP_C, ETH_OUTPOST_ID));
   for (const name seated : {BATCHOP, BATCHOP_B}) BOOST_REQUIRE(seated_on(seated, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP_C));

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   produce_blocks();
   BOOST_REQUIRE(!epoch_decided(ETH_OUTPOST_ID, epoch));

   const transaction_trace_ptr tipping = deliver_trace(BATCHOP_B, ETH_OUTPOST_ID, winner);
   const std::vector<slashop_record> slashed = slashops(tipping);
   BOOST_REQUIRE_EQUAL(1u, slashed.size());
   BOOST_REQUIRE_EQUAL(BATCHOP_C, slashed.front().operator_account);
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_C));
   for (const name member : {BATCHOP, BATCHOP_B}) {
      BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(member));
   }
} FC_LOG_AND_RETHROW() }

// Review follow-up on WNS-15(a): the consensus tally and the threshold must be drawn from the SAME
// population. Sizing the group to the live set while still counting every delivery row lets a
// slashed operator's pre-slash vote carry a threshold it is no longer part of:
//
//   A delivers X -> A is slashed -> B delivers X
//   group_size == 2 (B, C), counts[X] == 2 (A's row included)  ->  a strict majority of two,
//
// finalizing on A+B even though C -- one of the two currently-ACTIVE members -- never delivered.
// Counted over the eligible set, X holds one of two, so it must not resolve until C speaks.
BOOST_FIXTURE_TEST_CASE(slash_after_delivery_does_not_count_toward_consensus, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);

   const uint32_t epoch = current_epoch();
   auto envelope = encode_delivery(epoch, "slash-after-delivery");
   const auto digest = oracle::epoch_digest(decode_envelope(envelope));

   // A delivers, then loses eligibility. Its row survives in the deliveries table.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n,
      mvo()("account", BATCHOP.to_string())("reason", "WNS-15 slash-after-delivery regression")));
   produce_blocks();

   // B delivers. Eligible set is {B, C}; only B has delivered among them, so one delivery counts toward the two
   // that three seats need, and A's stale row must not make up the difference.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   {
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(opc.is_null() || !opc["consensus_reached"].as<bool>());
   }

   // The chkcons re-tally must still say no: A is not part of the group being counted.
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP_B));
   produce_blocks();
   {
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(opc.is_null() || !opc["consensus_reached"].as<bool>());
   }

   // C delivers: two eligible members agree, a majority of the three seats, and consensus is legitimate.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_C, ETH_OUTPOST_ID, envelope));
   produce_blocks();

   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc["consensus_reached"].as<bool>(), true);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), digest.str());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  SOL-378: the shipped BATCH_OPERATOR_GROUPS active index is a one-epoch
//  lookahead into the shipped window.
//
//  Why lookahead: an outpost that scopes envelope admission to its seated
//  active group must hold epoch N's duty roster BEFORE epoch N's envelope
//  arrives — that envelope's own deliverer belongs to epoch N's group, so
//  shipping epoch N's roster inside epoch N's envelope is circular and stalls
//  the bridge at the first rotation. Envelope N-1 therefore seats epoch N's
//  group. Only the attestation looks ahead; the depot's `epoch_state`
//  (`current_batch_op_group`) still names the group on duty now.
// ---------------------------------------------------------------------------

/// Rotating schedule (three single-member groups): the member the shipped `active_group_index`
/// names must be the one ACTUALLY on duty one epoch later. Asserted against the OBSERVED
/// rotation — read the shipped index, advance one epoch, and require the promised member to be
/// the depot's own on-duty member — so an off-by-one in EITHER direction fails: index 0 promises
/// the group whose duty just started (stale after the advance), index 2 promises the group two
/// epochs out; only the true lookahead survives the comparison. Walked one epoch past a full
/// rotation so every group is promised (and verified) at least once.
BOOST_FIXTURE_TEST_CASE(advance_ships_lookahead_batch_operator_group, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroups = 3;
   // Use bootstrap-exempt operators so this test only exercises healthy rotation.
   constexpr uint64_t kRotationWindowMs = 12ULL * kGroups * EPOCH_DURATION_SEC * 1000ULL;
   bootstrap_rotation(kRotationWindowMs, /*batchop_is_bootstrapped=*/true);

   for (uint32_t round = 0; round < kGroups + 1; ++round) {
      const auto shipped = shipped_batch_operator_groups(ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(shipped.groups_size(), static_cast<int>(kGroups));
      const uint32_t shipped_index = shipped.active_group_index();
      BOOST_REQUIRE_LT(shipped_index, static_cast<uint32_t>(shipped.groups_size()));
      BOOST_REQUIRE_EQUAL(shipped.groups(shipped_index).operators_size(), 1);
      const std::string promised = shipped.groups(shipped_index).operators(0).address();

      // The depot's own schedule is NOT shifted: `current_batch_op_group` still names the group
      // on duty NOW, and with disjoint single-member groups the promised (next-epoch) member
      // must differ from it. A reverted lookahead fails here already — it promises the current
      // duty member.
      BOOST_REQUIRE(promised != duty_member().to_string());

      // One epoch later the promised member must be exactly the one the depot seats on duty.
      advance_to_next_epoch();
      BOOST_REQUIRE_EQUAL(promised, duty_member().to_string());
   }
} FC_LOG_AND_RETHROW() }

/// Single group: nothing rotates, so the shipped index must stay 0 on every advance — never a
/// wrapped/overrun 1 that runs off the one-group window — and must keep naming the one real
/// member, who is perpetually on duty.
BOOST_FIXTURE_TEST_CASE(advance_ships_group_index_zero_for_single_group, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kRounds = 3;
   bootstrap();

   for (uint32_t round = 0; round < kRounds; ++round) {
      const auto shipped = shipped_batch_operator_groups(ETH_OUTPOST_ID);
      BOOST_REQUIRE_EQUAL(shipped.groups_size(), 1);
      BOOST_REQUIRE_EQUAL(shipped.active_group_index(), 0u);
      BOOST_REQUIRE_EQUAL(shipped.groups(0).operators_size(), 1);
      BOOST_REQUIRE_EQUAL(shipped.groups(0).operators(0).address(), BATCHOP.to_string());

      // The lone group stays on duty across the advance — the shipped index keeps naming it.
      advance_to_next_epoch();
      BOOST_REQUIRE_EQUAL(duty_member().to_string(), BATCHOP.to_string());
   }
} FC_LOG_AND_RETHROW() }

/// The depot must NEVER publish an active index that names an EMPTY group: that index selects
/// the group an outpost admits against and sizes its quorum from, so an empty one admits nobody,
/// can never reach consensus, and wedges the outpost permanently — the handler that could replace
/// the window runs only PAST the gate the empty group breaks.
///
/// The state is reached by starving an EXISTING window, which is the only way it is reachable:
/// `schbatchgps` refuses to build a starved schedule up front ("not enough available batch
/// operators for group assignment"), so a pool smaller than the window can only arise AFTER the
/// schedule exists — operators leaving the ACTIVE set. Here two of the three are administratively
/// terminated. The slide then finds no ACTIVE operator outside the surviving groups (residency is
/// what keeps window groups DISJOINT, which Ethereum's `_resolveChunkPosition` depends on, so it
/// is not relaxed to fill the gap), pushes an empty tail, and the lookahead index — `cursor + 1`
/// — names it.
///
/// Asserted here: the BATCH_OPERATOR_GROUPS attestation is absent, AND the envelope still exists
/// carrying other attestations. The second half is the regression guard that matters — withholding
/// is implemented by skipping ONE queueout, and an early `return` from `finishadv` would also produce
/// a missing roster while silently dropping the rest of the epoch's emissions.
BOOST_FIXTURE_TEST_CASE(advance_withholds_batch_operator_groups_when_next_group_is_empty,
                        sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroups = 3;
   constexpr uint64_t kRotationWindowMs = 12ULL * kGroups * EPOCH_DURATION_SEC * 1000ULL;
   bootstrap_rotation(kRotationWindowMs);

   // A full window ships its roster every epoch — the baseline the withhold is measured against.
   BOOST_REQUIRE_EQUAL(1, shipped_batch_operator_groups_count(ETH_OUTPOST_ID));

   // Starve it: terminate two of the three, leaving one ACTIVE operator for a three-seat window.
   for (const auto& op : {BATCHOP_B, BATCHOP_C}) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "terminate"_n,
         mvo()("account", op.to_string())("reason", std::string("starve the schedule window"))));
   }
   produce_blocks();

   // The terminated pair still occupy their seats until they slide out, so the sole survivor is
   // resident and the residency-excluded pool is empty: every tail from here is empty. Walk the
   // window so an empty group reaches the lookahead seat, then hold there.
   bool observed_withhold = false;
   for (uint32_t round = 0; round < kGroups + 1; ++round) {
      advance_to_next_epoch();
      // `advance` skipped at most ONE queueout, never the rest of its work.
      BOOST_REQUIRE_GT(shipped_attestation_count(ETH_OUTPOST_ID), 0);
      if (shipped_batch_operator_groups_count(ETH_OUTPOST_ID) == 0) observed_withhold = true;
      if (observed_withhold) {
         // Once the lookahead seat is empty it stays empty — the roster is never republished
         // while starved, and an empty active index is never shipped.
         BOOST_REQUIRE_EQUAL(0, shipped_batch_operator_groups_count(ETH_OUTPOST_ID));
      }
   }
   BOOST_REQUIRE_MESSAGE(observed_withhold,
      "starved window never withheld BATCH_OPERATOR_GROUPS -- an empty active group was published");
} FC_LOG_AND_RETHROW() }

/// An operator each chain evicts at the same advance drops out of both outposts' rosters
/// and out of normal tail selection, while its global status stays ACTIVE. The epoch it missed is
/// judged at the advance after its own, which with one group also slides out the seat the first
/// advance re-drew for it.
BOOST_FIXTURE_TEST_CASE(advance_roster_excludes_same_epoch_eviction,
                        sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3, /*batchop_is_bootstrapped=*/false);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
      "regoperator"_n, mvo()("account", BATCHOP_D.to_string())
         ("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   // With the percent rail applying from the first duty, the one missed epoch evicts BATCHOP on each chain.
   set_termination_thresholds(ROTATION_MAX_CONSECUTIVE_MISSES, DEFAULT_MAX_PERCENT_MISSES,
                              PERCENT_RAIL_FROM_FIRST_DUTY);
   produce_blocks();
   advance_to_next_epoch();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, chain));
   }
   advance_to_next_epoch();
   BOOST_REQUIRE(get_operator(BATCHOP)["status"].as<opp::types::OperatorStatus>() ==
                 opp::types::OPERATOR_STATUS_ACTIVE);
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), membership_status(BATCHOP, chain));
      const auto roster = require_roster_is_window(chain);
      BOOST_REQUIRE(!roster.count(BATCHOP.to_string()));
      BOOST_REQUIRE_EQUAL(roster.at(BATCHOP_B.to_string()), opp::types::OPERATOR_STATUS_ACTIVE);
      const auto groups = shipped_batch_operator_groups(chain);
      BOOST_REQUIRE_EQUAL(groups.groups_size(), 1);
      BOOST_REQUIRE_EQUAL(groups.groups(0).operators_size(), 3);
      bool found_standby = false;
      for (const auto& member : groups.groups(0).operators()) {
         BOOST_REQUIRE_NE(member.address(), BATCHOP.to_string());
         BOOST_REQUIRE_EQUAL(get_operator(name{member.address()})["status"].as<opp::types::OperatorStatus>(),
                             opp::types::OPERATOR_STATUS_ACTIVE);
         if (member.address() == BATCHOP_D.to_string()) found_standby = true;
      }
      BOOST_REQUIRE(found_standby);
   }
} FC_LOG_AND_RETHROW() }

/// `terminate` leaves the operator's later seats in place. A registration made before they close would
/// take the miss the next one records, and with that one row in its sample the miss rate would
/// terminate it again, so `regoperator` refuses the account until its last seat has closed: until the
/// advance after that seat's slide out of the window, which closes its epoch's books.
BOOST_FIXTURE_TEST_CASE(regoperator_refuses_account_still_seated, sysio_msgch_chain_tester) { try {
   const auto outposts = bootstrap_rotation(ROTATION_WINDOW_MS);

   // BATCHOP delivers on its epoch-1 duty, so it stays ACTIVE and the slide seats it again in the tail.
   BOOST_REQUIRE_EQUAL(BATCHOP, duty_member());
   for (const uint64_t outpost : outposts) {
      BOOST_REQUIRE_EQUAL(success(),
         deliver_as(BATCHOP, outpost, encode_chained_delivery(ROTATION_DELIVERY_PAYLOAD, {})));
   }
   produce_blocks();
   advance_to_next_epoch();
   BOOST_REQUIRE(duty_member() != BATCHOP);
   BOOST_REQUIRE(holds_schedule_seat(BATCHOP));

   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "terminate"_n,
      mvo()("account", BATCHOP.to_string())("reason", std::string(SEATED_TERMINATION_REASON))));
   produce_blocks(); // land it before the advance's epoch-length jump expires it
   BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_TERMINATED ==
                 get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());

   const auto register_again = [&] {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", BATCHOP.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
         ("is_bootstrapped", false));
   };

   // Refused on every epoch the leftover seat is still in the window, its own duty epoch included, and on the
   // epoch after it, whose advance closes that seat's books.
   uint32_t leftover_duty_epoch = 0;
   for (uint32_t step = 0; step < ROTATION_GROUPS + 1 && holds_schedule_seat(BATCHOP); ++step) {
      BOOST_REQUIRE_EQUAL(error(STILL_SEATED_REGISTRATION_ERROR), register_again());
      if (duty_member() == BATCHOP) leftover_duty_epoch = current_epoch();
      advance_to_next_epoch();
   }
   BOOST_REQUIRE(!holds_schedule_seat(BATCHOP));
   BOOST_REQUIRE_GT(leftover_duty_epoch, 0u);
   BOOST_REQUIRE_EQUAL(leftover_duty_epoch, booked_epoch());

   // The closed seat recorded its miss against the terminated registration, which termcheck left alone.
   const auto leftover_rows = dellog_rows(BATCHOP, leftover_duty_epoch);
   BOOST_REQUIRE_EQUAL(1u, leftover_rows.size());
   BOOST_REQUIRE(!leftover_rows.front()[opreg_fields::DELIVERED].as<bool>());
   BOOST_REQUIRE_EQUAL(std::string(SEATED_TERMINATION_REASON),
                       get_operator(BATCHOP)[opreg_fields::STATUS_REASON].as_string());

   BOOST_REQUIRE_EQUAL(success(), register_again());
   BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_UNKNOWN ==
                 get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
} FC_LOG_AND_RETHROW() }

namespace served = sysio_system::test_support::served_chains;

/// Leaving a chain stops new seats on that chain only: the slide passes the operator over on the chain
/// it left and keeps seating it on the chain it still serves.
BOOST_FIXTURE_TEST_CASE(leaving_a_chain_stops_new_seats_there_only, sysio_msgch_chain_tester) { try {
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/true, rotation_outposts::eth_and_sol);
   // A spare serving both chains takes the seat the leaver gives up.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   BOOST_REQUIRE_EQUAL(BATCHOP, duty_member(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(BATCHOP, duty_member(SOL_OUTPOST_ID));

   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, BATCHOP, ETH_CHAIN_CODE));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(std::string(served::status::left),
                       served::membership(*this, opreg_abi, BATCHOP, ETH_CHAIN_CODE)["status"].as_string());
   BOOST_REQUIRE_EQUAL(std::string(served::status::serving),
                       served::membership(*this, opreg_abi, BATCHOP, SOL_CHAIN_CODE)["status"].as_string());
   advance_to_next_epoch();

   BOOST_REQUIRE(!seated_on(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE(seated_on(BATCHOP_D, ETH_OUTPOST_ID));
   BOOST_REQUIRE(seated_on(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE(!seated_on(BATCHOP_D, SOL_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// A member that left a chain still owes the seats it holds there: `deliver` accepts it, its delivery
/// counts toward consensus, and the advance that closes the seat's books records it as delivered.
BOOST_FIXTURE_TEST_CASE(a_member_that_left_delivers_its_remaining_seat, sysio_msgch_chain_tester) { try {
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/true);
   BOOST_REQUIRE_EQUAL(BATCHOP, duty_member());
   const uint32_t duty_epoch = current_epoch();

   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, BATCHOP, ETH_CHAIN_CODE));
   BOOST_REQUIRE_EQUAL(success(),
      deliver_as(BATCHOP, ETH_OUTPOST_ID, encode_chained_delivery(ROTATION_DELIVERY_PAYLOAD, {})));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(duty_epoch, get_outpcons(ETH_OUTPOST_ID)[msgch_fields::EPOCH_INDEX].as<uint32_t>());

   advance_to_next_epoch();
   advance_to_next_epoch();
   BOOST_REQUIRE_EQUAL(duty_epoch, booked_epoch());
   const auto rows = dellog_rows(BATCHOP, duty_epoch);
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   BOOST_REQUIRE(rows.front()[opreg_fields::DELIVERED].as<bool>());
} FC_LOG_AND_RETHROW() }

/// An operator that left a chain rejoins it only once its seats there have closed, the books of its last one
/// included; after the rejoin the slide seats it there again.
BOOST_FIXTURE_TEST_CASE(rejoining_a_chain_waits_for_its_seats_to_close, sysio_msgch_chain_tester) { try {
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/true);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   BOOST_REQUIRE_EQUAL(BATCHOP, duty_member());

   BOOST_REQUIRE_EQUAL(success(), served::leave(*this, BATCHOP, ETH_CHAIN_CODE));
   BOOST_REQUIRE_EQUAL(error(STILL_SEATED_JOIN_ERROR), served::join(*this, BATCHOP, ETH_CHAIN_CODE, BATCHOP));
   produce_blocks();

   // The leaver's only seat slides out of the window; the spare takes the new tail. The seat's books are
   // still open, so the rejoin waits for the advance that closes them.
   advance_to_next_epoch();
   BOOST_REQUIRE(!seated_on(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE(previous_group() == std::vector<name>{BATCHOP});
   BOOST_REQUIRE_EQUAL(error(STILL_SEATED_JOIN_ERROR), served::join(*this, BATCHOP, ETH_CHAIN_CODE, BATCHOP));
   produce_blocks();
   advance_to_next_epoch();
   BOOST_REQUIRE_EQUAL(1u, dellog_rows(BATCHOP).size());
   BOOST_REQUIRE_EQUAL(success(), served::join(*this, BATCHOP, ETH_CHAIN_CODE, BATCHOP));
   produce_blocks();

   advance_to_next_epoch();
   BOOST_REQUIRE(seated_on(BATCHOP, ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// A breach on one chain evicts the operator from that chain only: ETH, where it stops delivering,
/// evicts it and, once the seat drawn before the eviction slides out, seats a spare in its place, while
/// SOL, where it keeps delivering, keeps seating it. Its global status, and so its bond, is untouched.
BOOST_FIXTURE_TEST_CASE(eviction_on_one_chain_keeps_the_operator_on_the_other, sysio_msgch_chain_tester) { try {
   // The anchor on both chains, then misses on ETH alone until the consecutive rail fires.
   constexpr uint32_t kEvictingDuty = 1 + ROTATION_MAX_CONSECUTIVE_MISSES + 1;
   const auto outposts = bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/false,
                                            rotation_outposts::eth_and_sol);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   produce_blocks();

   std::map<uint64_t, inbound_stream_tip> tips;
   uint32_t duties = 0;
   for (uint32_t step = 0; step < ROTATION_GROUPS * (kEvictingDuty + 1) &&
                           membership_status(BATCHOP, ETH_OUTPOST_ID) != served_status::evicted; ++step) {
      const bool on_duty = duty_member(SOL_OUTPOST_ID) == BATCHOP;
      if (on_duty) {
         BOOST_REQUIRE_EQUAL(BATCHOP, duty_member(ETH_OUTPOST_ID));
         std::vector<uint64_t> reached{SOL_OUTPOST_ID};
         if (duties == 0) reached.push_back(ETH_OUTPOST_ID);
         ++duties;
         for (const uint64_t outpost : reached) {
            auto&      tip      = tips[outpost];
            const auto envelope = encode_chained_delivery(ROTATION_DELIVERY_PAYLOAD, tip);
            BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, outpost, envelope));
            tip = next_stream_tip(envelope);
         }
         produce_blocks();
      }
      advance_to_next_epoch();
   }
   BOOST_REQUIRE_EQUAL(kEvictingDuty, duties);

   const auto eth = membership(BATCHOP, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), eth[opreg_fields::STATUS].as_string());
   BOOST_REQUIRE_EQUAL(CONSECUTIVE_MISS_EVICTION_REASON, eth[opreg_fields::STATUS_REASON].as_string());
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());

   // ETH keeps the seat drawn for the member before its eviction until it slides out, then seats the spare in
   // its place; SOL seats the member again.
   require_evicted_seat_slides_out(BATCHOP, {ETH_OUTPOST_ID});
   BOOST_REQUIRE(seated_on(BATCHOP_D, ETH_OUTPOST_ID));
   BOOST_REQUIRE(seated_on(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE(!seated_on(BATCHOP_D, SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, SOL_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// An eviction forfeits the operator's unsettled duty on that chain alone. The advance that closes the books of
/// BATCHOP's breaching seat evicts it from ETH, erases its ETH duty and credits it nothing for that seat, while the
/// period keeps the forfeited seat-seconds and its SOL duty keeps accruing. The payout then pays BATCHOP its SOL
/// share alone, and the forfeited ETH share stays in the treasury.
BOOST_FIXTURE_TEST_CASE(eviction_forfeits_the_chain_duty_while_the_other_chain_pays, sysio_msgch_chain_tester) { try {
   // The anchor on both chains, then misses on ETH alone until the consecutive rail fires.
   constexpr uint32_t kEvictingDuty = 1 + ROTATION_MAX_CONSECUTIVE_MISSES + 1;
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/false, rotation_outposts::eth_and_sol);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   produce_blocks();

   // Walk to BATCHOP's evicting duty, delivering it to SOL every time and to ETH only the first time.
   std::map<uint64_t, inbound_stream_tip> tips;
   uint32_t duties = 0;
   for (uint32_t step = 0; step < ROTATION_GROUPS * kEvictingDuty && duties < kEvictingDuty; ++step) {
      if (duty_member(SOL_OUTPOST_ID) == BATCHOP) {
         BOOST_REQUIRE_EQUAL(BATCHOP, duty_member(ETH_OUTPOST_ID));
         std::vector<uint64_t> reached{SOL_OUTPOST_ID};
         if (duties == 0) reached.push_back(ETH_OUTPOST_ID);
         ++duties;
         for (const uint64_t outpost : reached) {
            auto&      tip      = tips[outpost];
            const auto envelope = encode_chained_delivery(ROTATION_DELIVERY_PAYLOAD, tip);
            BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, outpost, envelope));
            tip = next_stream_tip(envelope);
         }
         produce_blocks();
      }
      if (duties < kEvictingDuty) advance_to_next_epoch();
   }
   BOOST_REQUIRE_EQUAL(kEvictingDuty, duties);
   // The breaching seat's own close leaves its books open.
   advance_to_next_epoch();

   // Before the evicting advance BATCHOP holds one seat of duty per earlier duty on each chain.
   const uint64_t forfeited_seat_seconds = uint64_t{kEvictingDuty - 1} * EPOCH_DURATION_SEC;
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, ETH_OUTPOST_ID));
   for (const uint64_t chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(forfeited_seat_seconds, batch_duty_seat_seconds(BATCHOP, chain));
   }
   const uint64_t open_before = duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>();

   // The evicting advance forfeits the ETH duty and credits only the SOL seat; the period total keeps the forfeit.
   advance_to_next_epoch();
   BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), membership_status(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{kEvictingDuty} * EPOCH_DURATION_SEC, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));
   const uint64_t period_seat_seconds = duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>();
   BOOST_REQUIRE_EQUAL(open_before + EPOCH_DURATION_SEC, period_seat_seconds);

   // Each operator's rows settle at the period's rate: pool * seat-seconds / period seat-seconds, per row.
   const std::vector<name> operators{BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D};
   std::map<name, std::vector<uint64_t>> rows;
   for (const name op : operators) {
      BOOST_REQUIRE_EQUAL(0, pay_claimable(op));
      for (const uint64_t chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
         rows[op].push_back(batch_duty_seat_seconds(op, chain));
      }
   }

   const uint32_t paid_epoch = tick_first_spending_payout();
   const auto     log        = epoch_log(paid_epoch);
   BOOST_REQUIRE(log[system_fields::BATCH_SETTLED].as<bool>());
   BOOST_REQUIRE_EQUAL(0u, duty_state()[system_fields::SETTLING_PERIOD].as<uint32_t>());
   const int64_t pool = log[system_fields::BATCH_EMISSION].as<int64_t>();
   BOOST_REQUIRE_GT(pool, 0);
   const auto share = [&](uint64_t seat_seconds) {
      return static_cast<int64_t>(static_cast<__int128>(pool) * seat_seconds / period_seat_seconds);
   };
   int64_t paid = 0;
   for (const name op : operators) {
      int64_t expected = 0;
      for (const uint64_t seat_seconds : rows[op]) expected += share(seat_seconds);
      BOOST_REQUIRE_EQUAL(expected, pay_claimable(op));
      paid += expected;
      for (const uint64_t chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
         BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(op, chain));
      }
   }
   BOOST_REQUIRE_EQUAL(share(uint64_t{kEvictingDuty} * EPOCH_DURATION_SEC), pay_claimable(BATCHOP));
   const int64_t retained = log[system_fields::BATCH_EMISSION_RETAINED].as<int64_t>();
   BOOST_REQUIRE_EQUAL(pool - paid, retained);
   BOOST_REQUIRE_GE(retained, share(forfeited_seat_seconds));
} FC_LOG_AND_RETHROW() }

/// An eviction that lands while the member still holds its seat (here an out-of-band `termcheck` as
/// sysio.epoch, mid-epoch): `deliver` refuses it on that chain, the chain's consensus leaves it out, and
/// the advances that close its open epochs' books record their misses without evaluating it again. The
/// eviction forfeits its unsettled duty on that chain and those advances credit it none there. It rejoins
/// once its last seat's books have closed, and the rejoin restarts the rails, the misses that evicted it no
/// longer counting, and its duty on the chain, credited afresh from its first seat after the rejoin.
BOOST_FIXTURE_TEST_CASE(evicted_member_delivers_nothing_until_it_rejoins_with_fresh_rails,
                        sysio_msgch_chain_tester) { try {
   constexpr uint32_t kPermissiveMaxConsecutiveMisses = 5;
   constexpr uint32_t kPermissiveMaxPercentMisses     = 99;
   // One delivery and one miss is a 50% sample: above this ceiling.
   constexpr uint32_t kHalfSampleMaxPercentMisses     = 49;
   constexpr const char* kHalfSampleReason            = "rolling-window: >49% miss rate";
   bootstrap(/*n_batch_ops=*/3, /*batchop_is_bootstrapped=*/false);   // one group of three on each chain
   set_termination_thresholds(kPermissiveMaxConsecutiveMisses, kPermissiveMaxPercentMisses,
                              PERCENT_RAIL_FROM_FIRST_DUTY);

   inbound_stream_tip tip;
   // Every listed member delivers one chained envelope to its chain; the tip moves past it.
   const auto deliver_epoch = [&](const std::vector<name>& eth_ops, const std::vector<name>& sol_ops,
                                  std::string_view payload) {
      const auto envelope = encode_chained_delivery(payload, tip);
      for (const auto& op : eth_ops) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, ETH_OUTPOST_ID, envelope));
      for (const auto& op : sol_ops) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, SOL_OUTPOST_ID, envelope));
      produce_blocks();
      tip = next_stream_tip(envelope);
   };
   const std::vector<name> everyone{BATCHOP, BATCHOP_B, BATCHOP_C};
   const std::vector<name> others{BATCHOP_B, BATCHOP_C};

   // Epoch 1 delivered everywhere; epoch 2 missed everywhere; epoch 3 delivered everywhere but by BATCHOP on ETH.
   // By epoch 4 the advances have closed the books of epochs 1 and 2, crediting every seat of both on each chain.
   deliver_epoch(everyone, everyone, "anchor");
   advance_one_epoch();
   advance_one_epoch();
   deliver_epoch(others, everyone, "missed-on-eth");
   advance_one_epoch();
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, ETH_OUTPOST_ID));
   for (const uint64_t outpost : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(uint64_t{2 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, outpost));
   }
   const uint64_t open_before_eviction = duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>();

   // Epoch 4, BATCHOP seated: evict it from ETH alone on its sample of epochs 1 and 2.
   const uint32_t evicted_epoch = current_epoch();
   const uint32_t missed_epoch  = evicted_epoch - 1;
   set_termination_thresholds(kPermissiveMaxConsecutiveMisses, kHalfSampleMaxPercentMisses,
                              PERCENT_RAIL_FROM_FIRST_DUTY);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, EPOCH_ACCOUNT, opreg_actions::TERMINATION_CHECK,
      mvo()(opreg_fields::ACCOUNT, BATCHOP)(opreg_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();
   const auto evicted = membership(BATCHOP, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), evicted[opreg_fields::STATUS].as_string());
   BOOST_REQUIRE_EQUAL(kHalfSampleReason, evicted[opreg_fields::STATUS_REASON].as_string());
   BOOST_REQUIRE(seated_on(BATCHOP, ETH_OUTPOST_ID));
   // The eviction forfeited the ETH duty, leaving the period total as it was; the SOL duty stands.
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{2 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(open_before_eviction, duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>());

   const auto envelope = encode_chained_delivery("after-eviction", tip);
   BOOST_REQUIRE_EQUAL(error(EVICTED_DELIVERY_ERROR), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   BOOST_REQUIRE_EQUAL(error(STILL_SEATED_JOIN_ERROR),
                       sysio_system::test_support::served_chains::join(*this, BATCHOP, ETH_CHAIN_CODE, BATCHOP));
   produce_blocks();
   // The two members left are a strict majority of ETH's eligible set; SOL still counts BATCHOP.
   deliver_epoch(others, everyone, "after-eviction");
   for (const uint64_t outpost : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(evicted_epoch, get_outpcons(outpost)[msgch_fields::EPOCH_INDEX].as<uint32_t>());
   }

   // The advance that closes epoch 3's books records the miss and leaves the eviction as it was; its slide
   // drops BATCHOP from ETH's window.
   advance_one_epoch();
   const auto missed = dellog_rows(BATCHOP, missed_epoch, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, missed.size());
   BOOST_REQUIRE(!missed.front()[opreg_fields::DELIVERED].as<bool>());
   const auto still_evicted = membership(BATCHOP, ETH_OUTPOST_ID);
   for (const char* field : {opreg_fields::STATUS, opreg_fields::STATUS_REASON}) {
      BOOST_REQUIRE_EQUAL(evicted[field].as_string(), still_evicted[field].as_string());
   }
   BOOST_REQUIRE_EQUAL(evicted["updated_at_ms"].as_uint64(), still_evicted["updated_at_ms"].as_uint64());
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE(!seated_on(BATCHOP, ETH_OUTPOST_ID));
   // The seat it held earned nothing on ETH; SOL credited its seat as before. The two other members earned
   // their ETH seats and all three their SOL seats.
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{3 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(open_before_eviction + uint64_t{(2 + 3) * EPOCH_DURATION_SEC},
                       duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>());

   // The evicted epoch's books are still open, so the rejoin waits for the advance that closes them, which
   // records that miss too and credits it nothing on ETH.
   BOOST_REQUIRE_EQUAL(error(STILL_SEATED_JOIN_ERROR),
                       sysio_system::test_support::served_chains::join(*this, BATCHOP, ETH_CHAIN_CODE, BATCHOP));
   produce_blocks();
   advance_one_epoch();
   const auto missed_evicted = dellog_rows(BATCHOP, evicted_epoch, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, missed_evicted.size());
   BOOST_REQUIRE(!missed_evicted.front()[opreg_fields::DELIVERED].as<bool>());
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{4 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));

   // Rejoined, BATCHOP is seated again from the next slide.
   BOOST_REQUIRE_EQUAL(success(),
                       sysio_system::test_support::served_chains::join(*this, BATCHOP, ETH_CHAIN_CODE, BATCHOP));
   deliver_epoch(others, everyone, "rejoined");
   advance_one_epoch();
   BOOST_REQUIRE(seated_on(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));   // no epoch since seated it

   // Its first duty since the rejoin is delivered. Counted with the misses that evicted it the sample
   // would be 50% again; counted from the rejoin it is clean. Its books close at the advance after the next.
   const uint32_t rejoined_duty = current_epoch();
   deliver_epoch(everyone, everyone, "first-duty-after-rejoin");
   advance_one_epoch();
   advance_one_epoch();
   const auto delivered = dellog_rows(BATCHOP, rejoined_duty, ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, delivered.size());
   BOOST_REQUIRE(delivered.front()[opreg_fields::DELIVERED].as<bool>());
   BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(BATCHOP, ETH_OUTPOST_ID));
   // Its first seat since the rejoin is its whole ETH duty: nothing from before the eviction came back.
   BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// A terminated operator that registers again forfeits its earlier registration's duty on every chain. BATCHOP
/// serves ETH and SOL and is terminated while still seated; the advance that closes its last seat's books credits
/// that registration more duty. Once no schedule seats it, it registers again, bonds and is ACTIVE before the pay
/// period settles.
/// `regoperator` erases its duty on both chains while the period keeps the seat-seconds, so the payout pays it
/// only for the seats it serves after rejoining, and the earlier registration's share stays in the treasury.
BOOST_FIXTURE_TEST_CASE(registering_again_forfeits_the_earlier_registrations_duty, sysio_msgch_chain_tester) { try {
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/false, rotation_outposts::eth_and_sol);
   const std::vector<uint64_t> chains{ETH_OUTPOST_ID, SOL_OUTPOST_ID};

   // BATCHOP's epoch-1 seat closes on both chains and the slide seats it again in the tail.
   for (const uint64_t chain : chains) BOOST_REQUIRE_EQUAL(BATCHOP, duty_member(chain));
   advance_to_next_epoch();
   BOOST_REQUIRE(holds_schedule_seat(BATCHOP));
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "terminate"_n,
      mvo()("account", BATCHOP.to_string())("reason", std::string(SEATED_TERMINATION_REASON))));
   produce_blocks(); // land it before the advance's epoch-length jump expires it

   // Its remaining seats close, still crediting the terminated registration: the last one at the advance after
   // its slide out of the window.
   for (uint32_t step = 0; step < ROTATION_GROUPS + 1 && holds_schedule_seat(BATCHOP); ++step) {
      advance_to_next_epoch();
   }
   BOOST_REQUIRE(!holds_schedule_seat(BATCHOP));
   std::map<uint64_t, uint64_t> earlier_duty;
   for (const uint64_t chain : chains) {
      earlier_duty[chain] = batch_duty_seat_seconds(BATCHOP, chain);
      BOOST_REQUIRE_GT(earlier_duty[chain], uint64_t{EPOCH_DURATION_SEC});
   }
   const uint64_t open_before = duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>();

   // Registering again forfeits both chains' duty; the period keeps its seat-seconds.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
      ("account", BATCHOP.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
      ("is_bootstrapped", false)));
   for (const uint64_t chain : chains) BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, chain));
   BOOST_REQUIRE_EQUAL(open_before, duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>());

   // It bonds, ACTIVE again before the period settles, and rejoins both chains.
   BOOST_REQUIRE_EQUAL(success(), fund_and_bond(BATCHOP, BATCH_OPERATOR_MINIMUM_COLLATERAL));
   BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                 get_operator(BATCHOP)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
   for (const std::string_view chain : epoch_chains) {
      BOOST_REQUIRE_EQUAL(success(), served::join(*this, BATCHOP, chain));
   }
   produce_blocks(); // land them before the advance's epoch-length jump expires them

   // Walk until a seat of the new registration has closed on ETH.
   for (uint32_t step = 0; step < 2 * ROTATION_GROUPS && batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID) == 0;
        ++step) {
      advance_to_next_epoch();
   }
   // Its duty is the new registration's one seat on each chain, nothing of the earlier registration's.
   BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_LE(batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID), uint64_t{EPOCH_DURATION_SEC});
   const uint64_t period_seat_seconds = duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>();

   // Each operator's rows settle at the period's rate: pool * seat-seconds / period seat-seconds, per row.
   const std::vector<name> operators{BATCHOP, BATCHOP_B, BATCHOP_C};
   std::map<name, std::vector<uint64_t>> rows;
   for (const name op : operators) {
      BOOST_REQUIRE_EQUAL(0, pay_claimable(op));
      for (const uint64_t chain : chains) rows[op].push_back(batch_duty_seat_seconds(op, chain));
   }

   const uint32_t paid_epoch = tick_first_spending_payout();
   const auto     log        = epoch_log(paid_epoch);
   BOOST_REQUIRE(log[system_fields::BATCH_SETTLED].as<bool>());
   const int64_t pool = log[system_fields::BATCH_EMISSION].as<int64_t>();
   BOOST_REQUIRE_GT(pool, 0);
   const auto share = [&](uint64_t seat_seconds) {
      return static_cast<int64_t>(static_cast<__int128>(pool) * seat_seconds / period_seat_seconds);
   };
   int64_t paid = 0;
   for (const name op : operators) {
      int64_t expected = 0;
      for (const uint64_t seat_seconds : rows[op]) expected += share(seat_seconds);
      BOOST_REQUIRE_EQUAL(expected, pay_claimable(op));
      paid += expected;
   }
   const int64_t retained = log[system_fields::BATCH_EMISSION_RETAINED].as<int64_t>();
   BOOST_REQUIRE_EQUAL(pool - paid, retained);
   int64_t forfeited = 0;
   for (const uint64_t chain : chains) forfeited += share(earlier_duty[chain]);
   BOOST_REQUIRE_GE(retained, forfeited);
} FC_LOG_AND_RETHROW() }

/// WNS-56: the OPERATORS roster lists exactly the operators the window seats, so registrations cannot
/// grow it. Unbonded registrations, an ACTIVE batch operator the window does not seat, and an ACTIVE
/// challenger are all left out.
BOOST_FIXTURE_TEST_CASE(advance_roster_lists_only_the_window, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   // The one group of three seats batchop.a-c; batchop.d sorts after them, so it stays unseated.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_E.to_string())("type", opp::types::OPERATOR_TYPE_CHALLENGER)("is_bootstrapped", true)));
   std::vector<name> registrants;
   for (uint32_t i = 0; i < UNBONDED_REGISTRANTS; ++i) {
      registrants.emplace_back(std::string(UNBONDED_REGISTRANT_PREFIX) + static_cast<char>('a' + i));
   }
   create_accounts(registrants);
   for (const auto& registrant : registrants) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
         mvo()("account", registrant.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", false)));
   }
   produce_blocks();
   BOOST_REQUIRE(get_operator(BATCHOP_D)["status"].as<opp::types::OperatorStatus>() ==
                 opp::types::OPERATOR_STATUS_ACTIVE);
   BOOST_REQUIRE(get_operator(registrants.front())["status"].as<opp::types::OperatorStatus>() ==
                 opp::types::OPERATOR_STATUS_UNKNOWN);

   advance_to_next_epoch();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      const auto roster = require_roster_is_window(chain);
      BOOST_REQUIRE_EQUAL(roster.size(), 3u);
      BOOST_REQUIRE(!roster.count(BATCHOP_D.to_string()));
      BOOST_REQUIRE(!roster.count(BATCHOP_E.to_string()));
      for (const auto& registrant : registrants) BOOST_REQUIRE(!roster.count(registrant.to_string()));
   }
} FC_LOG_AND_RETHROW() }

/// The schedule draws only ACTIVE serving batch operators: members that joined a chain without bonding
/// collateral (UNKNOWN) are never seated, however many join. Non-bootstrapped operators sort first, so
/// the slide or a fresh `schbatchgps` would seat them if it drew them; the window stays as it was.
BOOST_FIXTURE_TEST_CASE(unbonded_members_never_reach_the_schedule, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/2);   // one group of two bootstrapped operators on each chain
   std::vector<name> registrants;
   for (uint32_t i = 0; i < UNBONDED_REGISTRANTS; ++i) {
      registrants.emplace_back(std::string(UNBONDED_REGISTRANT_PREFIX) + static_cast<char>('a' + i));
   }
   create_accounts(registrants);
   for (const auto& registrant : registrants) {
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
         mvo()("account", registrant.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", false)));
   }
   serve_chains(registrants, epoch_chains);
   produce_blocks();
   for (const auto& registrant : registrants) {
      BOOST_REQUIRE(get_operator(registrant)["status"].as<opp::types::OperatorStatus>() ==
                    opp::types::OPERATOR_STATUS_UNKNOWN);
      for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
         BOOST_REQUIRE_EQUAL(std::string(served_status::serving), membership_status(registrant, chain));
      }
   }

   const auto window_of = [&](uint64_t chain) {
      std::vector<std::vector<name>> window;
      // Held in a local: a range-for over a member of the temporary would read freed memory.
      const auto state = read_epoch_state(chain);
      for (const auto& group : state[epoch_fields::BATCH_OP_GROUPS].get_array()) {
         window.emplace_back();
         for (const auto& member : group.get_array()) window.back().emplace_back(member.as_string());
      }
      return window;
   };
   const std::vector<std::vector<name>> seated{{BATCHOP, BATCHOP_B}};
   advance_one_epoch();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) BOOST_REQUIRE(window_of(chain) == seated);
   for (const std::string_view chain : epoch_chains) {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                          mvo()(epoch_fields::CHAIN_CODE, chain)));
   }
   produce_blocks();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) BOOST_REQUIRE(window_of(chain) == seated);
} FC_LOG_AND_RETHROW() }

/// A slashed member keeps its roster entry, with SLASHED status, while the window still seats it, so
/// Solana keeps refusing that window instead of seating a smaller group. It drops out once its group
/// slides off.
BOOST_FIXTURE_TEST_CASE(advance_roster_keeps_a_seated_slashed_member_until_it_leaves,
                        sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroups = 3;
   constexpr uint64_t kRotationWindowMs = 12ULL * kGroups * EPOCH_DURATION_SEC * 1000ULL;
   bootstrap_rotation(kRotationWindowMs);
   // A spare so the slide can refill the slashed member's seat.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n,
      mvo()("account", BATCHOP_C.to_string())("reason", std::string("seated slash"))));
   produce_blocks();

   // batchop.c sits in the last group, so it stays seated for two more slides.
   bool listed_while_seated = false;
   for (uint32_t round = 0; round < kGroups; ++round) {
      advance_to_next_epoch();
      const auto roster = require_roster_is_window(ETH_OUTPOST_ID);
      const auto slashed = roster.find(BATCHOP_C.to_string());
      if (slashed != roster.end()) {
         BOOST_REQUIRE_EQUAL(slashed->second, opp::types::OPERATOR_STATUS_SLASHED);
         listed_while_seated = true;
      }
   }
   BOOST_REQUIRE(listed_while_seated);
   BOOST_REQUIRE(!require_roster_is_window(ETH_OUTPOST_ID).count(BATCHOP_C.to_string()));
} FC_LOG_AND_RETHROW() }

/// An empty window has no members to resolve, and Solana skips an empty roster as an anomaly, so once a
/// chain's window has emptied `advance` queues no OPERATORS attestation (and no BATCH_OPERATOR_GROUPS,
/// whose next group is empty): the outposts keep the roster they last received.
BOOST_FIXTURE_TEST_CASE(advance_queues_no_roster_for_an_empty_window, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/1);   // one one-member group on each chain
   std::map<uint64_t, std::string> genesis_hashes;
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      genesis_hashes[chain] = get_roster_sent(chain)["roster_hash"].as_string();
   }
   // The only member leaves both chains: the slide finds nobody serving to seat in the new tail.
   for (const std::string_view chain : epoch_chains) {
      BOOST_REQUIRE_EQUAL(success(), sysio_system::test_support::served_chains::leave(*this, BATCHOP, chain));
   }
   const uint32_t epoch = advance_one_epoch();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      const auto groups = read_epoch_state(chain)[epoch_fields::BATCH_OP_GROUPS].get_array();
      BOOST_REQUIRE_EQUAL(1u, groups.size());
      BOOST_REQUIRE(groups.front().get_array().empty());
      BOOST_REQUIRE_EQUAL(genesis_hashes[chain], get_roster_sent(chain)["roster_hash"].as_string());
      // Nothing else was queued either, so the outpost gets no envelope this epoch.
      BOOST_REQUIRE(outbound_envelope_for_epoch(chain, epoch).is_null());
   }
} FC_LOG_AND_RETHROW() }

/// An outpost keeps the last roster it received, so `advance` queues the roster to it only when the
/// roster differs from the one last queued: an advance that reseats the same members ships
/// BATCH_OPERATOR_GROUPS alone, and a slash that reseats the window ships the new roster.
BOOST_FIXTURE_TEST_CASE(advance_queues_the_roster_only_when_it_changes, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kUnchangedAdvances = 2;
   bootstrap(/*n_batch_ops=*/3);
   // A spare the slide seats once a member is slashed; it sorts after the seated three.
   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n,
      mvo()("account", BATCHOP_D.to_string())("type", opp::types::OPERATOR_TYPE_BATCH)("is_bootstrapped", true)));
   serve_chains({BATCHOP_D}, epoch_chains);
   produce_blocks();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) BOOST_REQUIRE(tip_carries_roster(chain));
   const std::string genesis_hash = get_roster_sent(ETH_OUTPOST_ID)["roster_hash"].as_string();

   for (uint32_t round = 0; round < kUnchangedAdvances; ++round) {
      advance_to_next_epoch();
      for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
         BOOST_REQUIRE(!tip_carries_roster(chain));
         BOOST_REQUIRE_EQUAL(shipped_batch_operator_groups(chain).groups_size(), 1);
         BOOST_REQUIRE(!require_roster_is_window(chain).count(BATCHOP_D.to_string()));
      }
   }
   BOOST_REQUIRE_EQUAL(get_roster_sent(ETH_OUTPOST_ID)["roster_hash"].as_string(), genesis_hash);

   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, CHALG_ACCOUNT, "slash"_n,
      mvo()("account", BATCHOP_C.to_string())("reason", std::string("seated slash"))));
   produce_blocks();
   advance_to_next_epoch();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE(tip_carries_roster(chain));
      const auto roster = require_roster_is_window(chain);
      BOOST_REQUIRE(!roster.count(BATCHOP_C.to_string()));
      BOOST_REQUIRE_EQUAL(roster.at(BATCHOP_D.to_string()), opp::types::OPERATOR_STATUS_ACTIVE);
   }
   BOOST_REQUIRE_NE(get_roster_sent(ETH_OUTPOST_ID)["roster_hash"].as_string(), genesis_hash);

   advance_to_next_epoch();
   for (const auto chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) BOOST_REQUIRE(!tip_carries_roster(chain));
} FC_LOG_AND_RETHROW() }

/// The roster lists the window's members by account, so a window that only rotates its groups encodes
/// the same roster and `advance` does not queue it again; BATCH_OPERATOR_GROUPS still ships every epoch.
BOOST_FIXTURE_TEST_CASE(advance_does_not_resend_the_roster_for_a_rotation, sysio_msgch_chain_tester) { try {
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/true);
   BOOST_REQUIRE(tip_carries_roster(ETH_OUTPOST_ID));
   for (uint32_t step = 0; step < ROTATION_GROUPS; ++step) {
      const name on_duty = duty_member();
      advance_to_next_epoch();
      BOOST_REQUIRE(duty_member() != on_duty);
      BOOST_REQUIRE(!tip_carries_roster(ETH_OUTPOST_ID));
      BOOST_REQUIRE_EQUAL(static_cast<uint32_t>(shipped_batch_operator_groups(ETH_OUTPOST_ID).groups_size()),
                          ROTATION_GROUPS);
      BOOST_REQUIRE_EQUAL(require_roster_is_window(ETH_OUTPOST_ID).size(), ROTATION_GROUPS);
   }
} FC_LOG_AND_RETHROW() }

/// An outpost activated after the network genesis starts its own epoch 1 through `bootchain`, while
/// the outpost already running keeps its own index. The new chain has no `rostersent` row, so its genesis
/// advance queues it the roster, and its first envelope is stamped with its own epoch 1 and duration.
/// Nothing reaches it before then: the running chain's advances queue only to their own outpost.
BOOST_FIXTURE_TEST_CASE(outpost_activated_later_starts_its_own_epoch_one, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kSolDurationSec = 2 * EPOCH_DURATION_SEC;
   const auto boot_chain = [&](name signer) {
      return push(MSGCH_ACCOUNT, msgch_abi, signer, msgch_actions::BOOT_CHAIN,
                  mvo()(msgch_fields::CHAIN_CODE, SOL_OUTPOST_ID));
   };
   // SOL's longer schedule is set later against the stored opreg window, which constrains no schedule.
   bootstrap_rotation(ROTATION_WINDOW_MS, /*batchop_is_bootstrapped=*/true);
   register_chain(opp::types::ChainKind::CHAIN_KIND_SVM, SOL_CHAIN_CODE, SOL_EXTERNAL_CHAIN_ID);
   produce_blocks();
   advance_to_next_epoch();
   advance_to_next_epoch();
   const uint32_t eth_epoch = current_epoch();
   BOOST_REQUIRE_GT(eth_epoch, 1u);
   BOOST_REQUIRE(find_outbound_envelope(SOL_OUTPOST_ID).is_null());
   BOOST_REQUIRE(get_roster_sent(SOL_OUTPOST_ID).is_null());

   // Registered after the genesis, SOL is inactive until activchain.
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: bootchain: chain is not an active outpost"),
                       boot_chain(MSGCH_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT, chains_actions::ACTIVATE_CHAIN,
                                       mvo()("code", SOL_CHAIN_CODE)));
   serve_chains({BATCHOP, BATCHOP_B, BATCHOP_C}, {SOL_CHAIN_CODE});
   set_epoch_config(SOL_CHAIN_CODE, kSolDurationSec, ROTATION_OPERATORS_PER_GROUP, ROTATION_GROUPS);
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, SOL_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.msgch"), boot_chain(BATCHOP));
   BOOST_REQUIRE_EQUAL(success(), boot_chain(MSGCH_ACCOUNT));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(1u, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(eth_epoch, current_epoch());
   BOOST_REQUIRE(tip_carries_roster(SOL_OUTPOST_ID));
   BOOST_REQUIRE(require_roster_is_window(SOL_OUTPOST_ID) == require_roster_is_window(ETH_OUTPOST_ID));
   const auto sol_tip = find_outbound_envelope(SOL_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, sol_tip[msgch_fields::EPOCH_INDEX].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(1u, decode_envelope(sol_tip["raw_envelope"].as<std::vector<char>>()).epoch_index());
   const auto sol_groups = shipped_batch_operator_groups(SOL_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(1u, sol_groups.epoch_index());
   BOOST_REQUIRE_EQUAL(kSolDurationSec, sol_groups.epoch_duration_sec());

   BOOST_REQUIRE_EQUAL(error("assertion failure with message: bootchain: the chain's epoch has already started"),
                       boot_chain(MSGCH_ACCOUNT));
} FC_LOG_AND_RETHROW() }

/// `resendroster` forgets the roster last queued to one outpost, so the next advance queues the roster to
/// it again while the outpost still holding it gets none. Only sysio.epoch may call it, and only for an
/// outpost a roster has been queued to.
BOOST_FIXTURE_TEST_CASE(resendroster_requeues_the_roster_to_one_outpost, sysio_msgch_chain_tester) { try {
   const auto resend = [&](name signer) {
      return push(EPOCH_ACCOUNT, epoch_abi, signer, "resendroster"_n, mvo()("code", SOL_CHAIN_CODE));
   };
   bootstrap(/*n_batch_ops=*/3);
   advance_to_next_epoch();
   BOOST_REQUIRE(!tip_carries_roster(SOL_OUTPOST_ID));

   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.epoch"), resend(BATCHOP));
   BOOST_REQUIRE_EQUAL(success(), resend(EPOCH_ACCOUNT));
   BOOST_REQUIRE(get_roster_sent(SOL_OUTPOST_ID).is_null());
   produce_blocks(); // the refused retry below is a new transaction, not a duplicate
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: no roster has been queued to this outpost"),
                       resend(EPOCH_ACCOUNT));

   advance_to_next_epoch();
   BOOST_REQUIRE(tip_carries_roster(SOL_OUTPOST_ID));
   BOOST_REQUIRE(!tip_carries_roster(ETH_OUTPOST_ID));
   BOOST_REQUIRE(require_roster_is_window(SOL_OUTPOST_ID) == require_roster_is_window(ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// Even sysio.epoch authority cannot invoke the continuation as a top-level action.
BOOST_FIXTURE_TEST_CASE(finishadv_rejects_direct_calls, sysio_msgch_chain_tester) { try {
   bootstrap();
   const auto args = mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)(epoch_fields::EPOCH_INDEX, current_epoch());
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio.epoch"),
      push(EPOCH_ACCOUNT, epoch_abi, BATCHOP, epoch_actions::FINISH_ADVANCE, args));
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: finishadv must be sent inline by sysio.epoch"),
      push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::FINISH_ADVANCE, args));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Inbound `envelopes` retention: `deliver` prunes rows older than the
//  previous epoch, a bounded number per call.
// ---------------------------------------------------------------------------

/// Real `deliver -> chkcons -> advance` cycles leave exactly the retained epochs' rows: nothing
/// older than the window, every row of the previous epoch (the window's lower edge), ids that keep
/// increasing across epochs, and no orphaned `byoutepoch` entries.
BOOST_FIXTURE_TEST_CASE(envelopes_pruned_beyond_retention_window, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kBatchOperatorCount = 3;
   constexpr uint32_t kOutpostCount       = 2;
   constexpr uint32_t kRowsPerEpoch       = kBatchOperatorCount * kOutpostCount;
   constexpr uint32_t kEpochsDriven       = INBOUND_ENVELOPE_RETENTION_EPOCHS + 2;
   const std::vector<name> ops{BATCHOP, BATCHOP_B, BATCHOP_C};

   bootstrap(kBatchOperatorCount);
   const uint32_t first_epoch = current_epoch();
   inbound_stream_tip tip;
   uint64_t newest_id = 0;
   for (uint32_t driven = 1; driven <= kEpochsDriven; ++driven) {
      const uint32_t epoch = current_epoch();
      deliver_unanimous_epoch(ops, "retention-" + std::to_string(epoch), tip);

      const auto rows = envelope_rows();
      const uint32_t retained_epochs = std::min(driven, INBOUND_ENVELOPE_RETENTION_EPOCHS);
      BOOST_REQUIRE_EQUAL(rows.size(), retained_epochs * kRowsPerEpoch);
      BOOST_REQUIRE_EQUAL(envelope_index_entries(), rows.size());
      for (const auto& row : rows) {
         BOOST_REQUIRE_GT(row[msgch_fields::EPOCH_INDEX].as<uint32_t>() +
                             INBOUND_ENVELOPE_RETENTION_EPOCHS, epoch);
      }
      if (epoch > first_epoch) BOOST_REQUIRE_EQUAL(envelope_rows_for_epoch(epoch - 1), kRowsPerEpoch);

      // Primary-key order is epoch order and ids never restart after a prune.
      uint64_t previous_id = 0;
      for (const auto& row : rows) {
         BOOST_REQUIRE_GT(row[msgch_fields::ID].as_uint64(), previous_id);
         previous_id = row[msgch_fields::ID].as_uint64();
      }
      BOOST_REQUIRE_GT(previous_id, newest_id);
      newest_id = previous_id;

      close_epoch();
   }
} FC_LOG_AND_RETHROW() }

/// One delivery erases at most `ENVELOPE_PRUNE_BUDGET` of its own outpost's expired rows, oldest first,
/// and never a retained one. With more expired rows per outpost than one budget, the first ETH delivery
/// of an epoch leaves ETH's newest expired rows for the next, which clears them; SOL's expired rows wait
/// for SOL's own deliveries.
BOOST_FIXTURE_TEST_CASE(envelope_prune_bounded_per_delivery, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kBatchOperatorCount = 6;
   constexpr uint32_t kRowsPerOutpost     = kBatchOperatorCount;
   static_assert(kRowsPerOutpost > ENVELOPE_PRUNE_BUDGET && kRowsPerOutpost <= 2 * ENVELOPE_PRUNE_BUDGET,
                 "an outpost's expired epoch must take exactly two of its deliveries to clear");
   const std::vector<name> ops{BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E, BATCHOP_F};

   bootstrap(kBatchOperatorCount);
   const uint32_t expiring_epoch = current_epoch();
   inbound_stream_tip tip;
   for (uint32_t i = 0; i < INBOUND_ENVELOPE_RETENTION_EPOCHS; ++i) {
      deliver_unanimous_epoch(ops, "budget-" + std::to_string(current_epoch()), tip);
      close_epoch();
   }
   const uint32_t epoch = current_epoch();
   BOOST_REQUIRE_EQUAL(expiring_epoch + INBOUND_ENVELOPE_RETENTION_EPOCHS, epoch);

   std::vector<uint64_t> expiring_eth_ids;
   for (const auto& row : envelope_rows()) {
      if (row[msgch_fields::EPOCH_INDEX].as<uint32_t>() == expiring_epoch &&
          row[msgch_fields::CHAIN_CODE].as_uint64() == ETH_OUTPOST_ID) {
         expiring_eth_ids.push_back(row[msgch_fields::ID].as_uint64());
      }
   }
   BOOST_REQUIRE_EQUAL(expiring_eth_ids.size(), kRowsPerOutpost);

   const auto envelope = encode_chained_delivery("budget-" + std::to_string(epoch), tip);
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(envelope_rows_for(ETH_OUTPOST_ID, expiring_epoch), kRowsPerOutpost - ENVELOPE_PRUNE_BUDGET);
   BOOST_REQUIRE_EQUAL(envelope_rows_for(SOL_OUTPOST_ID, expiring_epoch), kRowsPerOutpost);
   BOOST_REQUIRE(find_inbound_delivery(ETH_OUTPOST_ID, expiring_epoch, ops[ENVELOPE_PRUNE_BUDGET - 1]).is_null());
   BOOST_REQUIRE(!find_inbound_delivery(ETH_OUTPOST_ID, expiring_epoch, ops[ENVELOPE_PRUNE_BUDGET]).is_null());
   BOOST_REQUIRE_EQUAL(envelope_rows_for_epoch(expiring_epoch + 1), 2 * kRowsPerOutpost);

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(envelope_rows_for(ETH_OUTPOST_ID, expiring_epoch), 0u);
   BOOST_REQUIRE_EQUAL(envelope_rows_for(SOL_OUTPOST_ID, expiring_epoch), kRowsPerOutpost);

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, SOL_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(envelope_rows_for(SOL_OUTPOST_ID, expiring_epoch), kRowsPerOutpost - ENVELOPE_PRUNE_BUDGET);
   BOOST_REQUIRE_EQUAL(envelope_rows_for_epoch(expiring_epoch + 1), 2 * kRowsPerOutpost);
   BOOST_REQUIRE_EQUAL(envelope_rows_for_epoch(epoch), 3u);
   BOOST_REQUIRE_EQUAL(envelope_index_entries(), envelope_rows().size());
} FC_LOG_AND_RETHROW() }

/// The prune is per outpost. SOL's only deliveries are in the first epoch, so they hold the lowest
/// ids; ETH keeps delivering. ETH's deliveries past the window prune ETH's expired rows only, and
/// SOL's rows from the first epoch stay until SOL delivers again, which prunes them.
BOOST_FIXTURE_TEST_CASE(envelope_prune_is_per_outpost, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kBatchOperatorCount = 3;
   const std::vector<name> ops{BATCHOP, BATCHOP_B, BATCHOP_C};

   bootstrap(kBatchOperatorCount);
   const uint32_t first_epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   const auto deliver_all = [&](uint64_t outpost, std::string_view payload) {
      auto&      tip      = tips[outpost];
      const auto envelope = encode_chained_delivery(payload, tip);
      for (const auto& op : ops) BOOST_REQUIRE_EQUAL(success(), deliver_as(op, outpost, envelope));
      produce_blocks();
      tip = next_stream_tip(envelope);
   };

   deliver_all(SOL_OUTPOST_ID, "sol-first");
   for (uint32_t i = 0; i <= INBOUND_ENVELOPE_RETENTION_EPOCHS; ++i) {
      deliver_all(ETH_OUTPOST_ID, "eth-" + std::to_string(current_epoch()));
      advance_one_epoch();
   }
   const uint32_t epoch = current_epoch();
   BOOST_REQUIRE_EQUAL(first_epoch + INBOUND_ENVELOPE_RETENTION_EPOCHS + 1, epoch);

   // ETH's own expired epochs are gone; SOL's first-epoch rows survive ETH's prunes.
   BOOST_REQUIRE_EQUAL(envelope_rows_for(ETH_OUTPOST_ID, first_epoch), 0u);
   BOOST_REQUIRE_EQUAL(envelope_rows_for(SOL_OUTPOST_ID, first_epoch), kBatchOperatorCount);
   for (uint32_t retained = first_epoch + 1; retained < epoch; ++retained) {
      BOOST_REQUIRE_EQUAL(envelope_rows_for(ETH_OUTPOST_ID, retained), kBatchOperatorCount);
   }

   deliver_all(SOL_OUTPOST_ID, "sol-later");
   BOOST_REQUIRE_EQUAL(envelope_rows_for(SOL_OUTPOST_ID, first_epoch), 0u);
   BOOST_REQUIRE_EQUAL(envelope_rows_for(SOL_OUTPOST_ID, epoch), kBatchOperatorCount);
   BOOST_REQUIRE_EQUAL(envelope_index_entries(), envelope_rows().size());
} FC_LOG_AND_RETHROW() }

/// Dispute resolution still works once the prune is live. Two clean epochs run first, so the
/// disputed epoch's deliveries prune the oldest epoch; ETH then splits 1-1, a Tier-1 vote resolves
/// it from the retained rows (`opendispute` pauses the epoch, so they stay current), and the
/// resolution slashes the losing deliverer before the epoch advances.
BOOST_FIXTURE_TEST_CASE(dispute_resolves_while_envelopes_are_pruned, sysio_msgch_chain_tester) { try {
   const std::vector<name> ops{BATCHOP, BATCHOP_B};
   bootstrap(ONE_TO_ONE_TIE_GROUP_SIZE);
   const uint32_t first_epoch = current_epoch();
   inbound_stream_tip tip;
   for (uint32_t i = 0; i < INBOUND_ENVELOPE_RETENTION_EPOCHS; ++i) {
      deliver_unanimous_epoch(ops, "pre-dispute-" + std::to_string(current_epoch()), tip);
      close_epoch();
   }
   const uint32_t epoch = current_epoch();

   const auto left  = encode_chained_delivery(ONE_TO_ONE_LEFT_PAYLOAD, tip);
   const auto right = encode_chained_delivery(ONE_TO_ONE_RIGHT_PAYLOAD, tip);
   const auto left_checksum  = fc::sha256::hash(left.data(), left.size());
   const auto right_checksum = fc::sha256::hash(right.data(), right.size());
   for (const auto& op : ops) {
      BOOST_REQUIRE_EQUAL(success(), deliver_as(op, SOL_OUTPOST_ID, left));
   }
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, left));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, right));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(envelope_rows_for_epoch(first_epoch), 0u);

   elapse_epoch_boundary();
   advance_via_consensus();
   assert_open_tie_dispute(epoch, {left_checksum, right_checksum},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused());

   resolve_tie_dispute(epoch, left_checksum);
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_B));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP));
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Per-chain consensus epochs: each outpost chain runs its own counter, pacing and dispute pause.
// ---------------------------------------------------------------------------

/// Two chains with different minimum durations advance once each is due, and a chain that stops reaching
/// consensus holds only itself. Traffic since an epoch's start makes it due one chain minimum later. Each chain's
/// outbound envelope and BATCH_OPERATOR_GROUPS carry that chain's own epoch index and duration.
BOOST_FIXTURE_TEST_CASE(chains_advance_independently_at_their_own_pace, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kBatchOperatorCount = 3;
   constexpr uint32_t kSolDurationSec     = 2 * EPOCH_DURATION_SEC;
   constexpr uint32_t kStalledEthEpochs   = 3;
   const std::vector<name> ops{BATCHOP, BATCHOP_B, BATCHOP_C};
   // Seconds from `chain_code`'s epoch start to when traffic since that start makes it due.
   const auto epoch_length_sec = [&](uint64_t chain_code) {
      sysio_system::test_support::epoch_state::mark_traffic(*this, epoch_abi, fc::slug_name{chain_code}.to_string());
      const auto state = read_epoch_state(chain_code);
      return static_cast<uint32_t>((state[epoch_fields::DUE_AT].as<fc::time_point>() -
                                    state[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>()).to_seconds());
   };
   const auto require_stamped = [&](uint64_t chain_code, uint32_t duration_sec) {
      const uint32_t epoch = current_epoch(chain_code);
      const auto     tip   = find_outbound_envelope(chain_code);
      BOOST_REQUIRE_EQUAL(epoch, tip[msgch_fields::EPOCH_INDEX].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(epoch, decode_envelope(tip["raw_envelope"].as<std::vector<char>>()).epoch_index());
      const auto groups = shipped_batch_operator_groups(chain_code);
      BOOST_REQUIRE_EQUAL(epoch, groups.epoch_index());
      BOOST_REQUIRE_EQUAL(duration_sec, groups.epoch_duration_sec());
   };

   bootstrap(kBatchOperatorCount, /*batchop_is_bootstrapped=*/true, kSolDurationSec);
   BOOST_REQUIRE_EQUAL(1u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(1u, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(EPOCH_DURATION_SEC, epoch_length_sec(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(kSolDurationSec, epoch_length_sec(SOL_OUTPOST_ID));
   require_stamped(ETH_OUTPOST_ID, EPOCH_DURATION_SEC);
   require_stamped(SOL_OUTPOST_ID, kSolDurationSec);

   // Both reach consensus; only ETH is due, so only ETH advances.
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-1", tips);
   deliver_chain_unanimous(ops, SOL_OUTPOST_ID, "sol-1", tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(2u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(1u, current_epoch(SOL_OUTPOST_ID));
   require_stamped(ETH_OUTPOST_ID, EPOCH_DURATION_SEC);

   // SOL's longer epoch closes once SOL is due.
   deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-2", tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(3u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(2u, current_epoch(SOL_OUTPOST_ID));
   require_stamped(SOL_OUTPOST_ID, kSolDurationSec);

   // SOL stalls: nobody delivers to it. ETH keeps closing its epochs.
   for (uint32_t i = 0; i < kStalledEthEpochs; ++i) {
      deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-" + std::to_string(current_epoch()), tips);
      elapse_epoch_boundary();
      advance_via_consensus();
   }
   BOOST_REQUIRE_EQUAL(3u + kStalledEthEpochs, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(2u, current_epoch(SOL_OUTPOST_ID));
   require_stamped(ETH_OUTPOST_ID, EPOCH_DURATION_SEC);

   // A delivery stamped with the other chain's index is refused: each chain checks its own epoch.
   const auto& sol_tip = tips[SOL_OUTPOST_ID];
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: envelope epoch_index mismatch: envelope=" +
            std::to_string(current_epoch(ETH_OUTPOST_ID)) + " current=2"),
      deliver_as(BATCHOP, SOL_OUTPOST_ID,
                 encode_delivery(current_epoch(ETH_OUTPOST_ID), "wrong-epoch", sol_tip.envelope_digest,
                                 sol_tip.message_id)));
} FC_LOG_AND_RETHROW() }

/// Closing an epoch's books credits its group with that chain's minimum duration in seat-seconds, on each member's row
/// for that chain: a seat on the chain with the longer minimum counts for more. A chain's first advance, like its
/// genesis, credits nothing. The books close once the next epoch's late-delivery window has passed, at that chain's
/// next `chkcons`, so a chain that does not advance credits its previous epoch all the same.
BOOST_FIXTURE_TEST_CASE(epoch_close_credits_the_chain_minimum_as_seat_seconds, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kSolDurationSec = 2 * EPOCH_DURATION_SEC;
   const std::vector<name> ops{BATCHOP};
   bootstrap(1, /*batchop_is_bootstrapped=*/true, kSolDurationSec);
   for (const uint64_t chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, chain));   // a genesis advance records no duty
   }

   // Only ETH is due; its first advance credits nothing.
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-1", tips);
   deliver_chain_unanimous(ops, SOL_OUTPOST_ID, "sol-1", tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(2u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(1u, current_epoch(SOL_OUTPOST_ID));
   for (const uint64_t chain : {ETH_OUTPOST_ID, SOL_OUTPOST_ID}) {
      BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, chain));
   }

   // ETH's second advance credits its 60 s epoch 1; SOL's first advance credits nothing.
   deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-2", tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(3u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(2u, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));

   // ETH alone advances again. SOL, a 60 s late window into its epoch 2, closes its 120 s epoch 1's books.
   deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-3", tips);
   deliver_chain_unanimous(ops, SOL_OUTPOST_ID, "sol-2", tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(4u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(2u, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{2 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{kSolDurationSec}, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));

   // ETH's fourth advance closes the books of its epoch 3; SOL's second finds those of its epoch 1 closed.
   deliver_chain_unanimous(ops, ETH_OUTPOST_ID, "eth-4", tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(5u, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(3u, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{3 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{kSolDurationSec}, batch_duty_seat_seconds(BATCHOP, SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(uint64_t{3 * EPOCH_DURATION_SEC + kSolDurationSec},
                       duty_state()[system_fields::OPEN_SEAT_SECONDS].as<uint64_t>());
} FC_LOG_AND_RETHROW() }

/// A dispute pauses only its own chain: the disputed chain's chkcons holds while the other chain keeps
/// reaching consensus and advancing. A second chain's dispute opens at that chain's own epoch while the
/// first is still open, and each resolution releases only its own chain, in either order.
BOOST_FIXTURE_TEST_CASE(dispute_on_one_chain_does_not_hold_the_other, sysio_msgch_chain_tester) { try {
   const std::vector<name> ops{BATCHOP, BATCHOP_B};
   bootstrap(ONE_TO_ONE_TIE_GROUP_SIZE);
   const uint32_t epoch = current_epoch();

   // ETH splits 1-1; SOL is unanimous.
   const auto left  = encode_delivery(epoch, std::string(ONE_TO_ONE_LEFT_PAYLOAD));
   const auto right = encode_delivery(epoch, std::string(ONE_TO_ONE_RIGHT_PAYLOAD));
   const auto left_checksum  = fc::sha256::hash(left.data(), left.size());
   const auto right_checksum = fc::sha256::hash(right.data(), right.size());
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, left));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, right));
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(ops, SOL_OUTPOST_ID, "sol-" + std::to_string(epoch), tips);
   elapse_epoch_boundary();
   advance_via_consensus();

   assert_open_tie_dispute(epoch, {left_checksum, right_checksum},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused(ETH_OUTPOST_ID));
   BOOST_REQUIRE(!epoch_is_paused(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch(SOL_OUTPOST_ID));

   // SOL closes another epoch while ETH's chkcons stays held by its open dispute.
   deliver_chain_unanimous(ops, SOL_OUTPOST_ID, "sol-" + std::to_string(epoch + 1), tips);
   elapse_epoch_boundary();
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(epoch, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(epoch + 2, current_epoch(SOL_OUTPOST_ID));

   // SOL splits 1-1 at its own epoch while ETH's dispute is still open: a second dispute opens and
   // pauses SOL too.
   const uint32_t sol_epoch           = epoch + 2;
   const auto&    sol_tip             = tips[SOL_OUTPOST_ID];
   const auto     sol_left            = encode_delivery(sol_epoch, std::string(ONE_TO_ONE_LEFT_PAYLOAD),
                                                        sol_tip.envelope_digest, sol_tip.message_id);
   const auto     sol_right           = encode_delivery(sol_epoch, std::string(ONE_TO_ONE_RIGHT_PAYLOAD),
                                                        sol_tip.envelope_digest, sol_tip.message_id);
   const auto     sol_left_checksum   = fc::sha256::hash(sol_left.data(), sol_left.size());
   const auto     sol_right_checksum  = fc::sha256::hash(sol_right.data(), sol_right.size());
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, SOL_OUTPOST_ID, sol_left));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, SOL_OUTPOST_ID, sol_right));
   elapse_epoch_boundary();
   advance_via_consensus();

   assert_open_tie_dispute(sol_epoch, {sol_left_checksum, sol_right_checksum},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION}, SOL_OUTPOST_ID,
                           SECOND_DISPUTE_ID);
   assert_open_tie_dispute(epoch, {left_checksum, right_checksum},
                           {ONE_OPERATOR_PER_TIED_VERSION, ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused(ETH_OUTPOST_ID));
   BOOST_REQUIRE(epoch_is_paused(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(sol_epoch, current_epoch(SOL_OUTPOST_ID));

   // Resolve the later SOL dispute first: its resolution slashes SOL's losing deliverer, SOL resumes and
   // advances, ETH stays paused.
   resolve_tie_dispute(sol_epoch, sol_left_checksum, SOL_OUTPOST_ID, SECOND_DISPUTE_ID);
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_B));
   BOOST_REQUIRE(!epoch_is_paused(SOL_OUTPOST_ID));
   BOOST_REQUIRE(epoch_is_paused(ETH_OUTPOST_ID));
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(epoch, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(sol_epoch + 1, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(BATCHOP_B));

   // Resolve the ETH dispute: ETH resumes and advances; its resolution's slash of the already-slashed
   // deliverer is a no-op.
   resolve_tie_dispute(epoch, left_checksum);
   BOOST_REQUIRE(!epoch_is_paused(ETH_OUTPOST_ID));
   advance_via_consensus();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(sol_epoch + 1, current_epoch(SOL_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED,
                       get_operator(BATCHOP_B)[opreg_fields::STATUS].as<opp::types::OperatorStatus>());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Depot-paced advance: a decided chain advances only once its traffic makes it due.
// ---------------------------------------------------------------------------

/// A decided epoch on an idle chain stays current: neither `chkcons` nor governance's `advance` moves it, however
/// long it waits.
BOOST_FIXTURE_TEST_CASE(an_idle_decided_chain_never_advances, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kIdleMinimums = 5;
   bootstrap();
   const uint32_t epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "idle-decided", tips);
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));

   produce_blocks(kIdleMinimums * EPOCH_DURATION_SEC * 2);
   const transaction_trace_ptr crank = check_consensus_trace(ETH_OUTPOST_ID, BATCHOP);
   BOOST_REQUIRE_EQUAL(0u, action_count(crank, EPOCH_ACCOUNT, epoch_actions::ADVANCE));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// The first general-lane attestation queued to an empty outbound queue notifies sysio.epoch, which records the chain's
/// traffic at that moment, so the chain is due one minimum later; a further row and a schedule-lane row notify nothing.
/// `chkcons` advances the decided epoch only once it is due, and the advance that ships the rows leaves the chain idle.
BOOST_FIXTURE_TEST_CASE(queueout_records_traffic_when_the_general_queue_fills, sysio_msgch_chain_tester) { try {
   const fc::microseconds block_interval = fc::milliseconds(config::block_interval_ms);
   bootstrap();
   const uint32_t epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "queued-traffic", tips);
   produce_blocks(10);
   const auto queue_trace = [&](opp::types::AttestationType type, char fill) {
      return queueout_trace(ETH_OUTPOST_ID, type, fill);
   };

   const transaction_trace_ptr schedule = queue_trace(opp::types::ATTESTATION_TYPE_OPERATORS, 's');
   BOOST_REQUIRE_EQUAL(0u, queueout_notifications(schedule));
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());

   const fc::time_point        queued_at = control->pending_block_time();
   const transaction_trace_ptr first     = queue_trace(opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, 'a');
   BOOST_REQUIRE_EQUAL(1u, queueout_notifications(first));
   BOOST_REQUIRE_EQUAL(0u, action_count(first, EPOCH_ACCOUNT, epoch_actions::NOTE_TRAFFIC));
   const fc::time_point due = due_for(queued_at);
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == queued_at);
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == due);
   produce_blocks();
   const transaction_trace_ptr second = queue_trace(opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, 'b');
   BOOST_REQUIRE_EQUAL(0u, queueout_notifications(second));
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == queued_at);
   BOOST_REQUIRE(read_epoch_state(SOL_OUTPOST_ID)[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());

   open_block_at(due - block_interval);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   open_block_at(due);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>() == due);
   BOOST_REQUIRE_EQUAL(0u, attestation_status_count(ETH_OUTPOST_ID, opp::types::ATTESTATION_STATUS_READY));
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == fc::time_point());
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// General-lane rows an envelope leaves behind are traffic since the oldest of them became ready: the advance that
/// built it cleared the chain's traffic and its `buildenv` records it again, so the chain is due one minimum after its
/// new epoch starts. The advance that ships the rest leaves the chain idle.
BOOST_FIXTURE_TEST_CASE(a_backlog_left_by_buildenv_records_traffic, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kEarlyRows = MAX_ENVELOPE_ATTESTATIONS;
   constexpr uint32_t kLateRows  = 4;
   bootstrap();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "backlog-1", tips);
   const auto queue_general = [&](uint32_t first, uint32_t count) {
      for (uint32_t i = first; i < first + count; ++i) {
         std::vector<char> data(sizeof(i));
         std::memcpy(data.data(), &i, sizeof(i));
         BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, data));
      }
      produce_blocks();
   };
   queue_general(0, kEarlyRows);
   produce_blocks(4);
   queue_general(kEarlyRows, kLateRows);

   advance_decided_epoch();
   const fc::variant state = read_epoch_state();
   const fc::time_point start = state[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>();
   // The schedule lane rides first, so more than the late rows stay behind, the oldest from the early batch.
   BOOST_REQUIRE_GT(attestation_status_count(ETH_OUTPOST_ID, opp::types::ATTESTATION_STATUS_READY), kLateRows);
   const fc::time_point since = oldest_ready_general(ETH_OUTPOST_ID);
   BOOST_REQUIRE(since < start);
   BOOST_REQUIRE(state[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == since);
   BOOST_REQUIRE(state[epoch_fields::DUE_AT].as<fc::time_point>() == start + minimum_duration());

   // The next advance ships the rest and leaves the chain idle.
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "backlog-2", tips);
   open_block_at(start + minimum_duration());
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(0u, attestation_status_count(ETH_OUTPOST_ID, opp::types::ATTESTATION_STATUS_READY));
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == fc::time_point());
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// A governance advance of a chain that is not an active outpost builds no envelope, so the chain's queued general-lane
/// rows stay queued and record its traffic again, since the oldest became ready. Active again, the chain is due and
/// its next advance ships them.
BOOST_FIXTURE_TEST_CASE(an_inactive_chains_queued_rows_stay_its_traffic, sysio_msgch_chain_tester) { try {
   bootstrap();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "inactive-1", tips);
   BOOST_REQUIRE_EQUAL(1u, queueout_notifications(
                              queueout_trace(ETH_OUTPOST_ID, opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, 'q')));
   produce_blocks();
   const fc::time_point queued_at = oldest_ready_general(ETH_OUTPOST_ID);

   set_chain_active(ETH_OUTPOST_ID, false);
   governance_advance_trace(ETH_OUTPOST_ID);
   const fc::variant    state = read_epoch_state();
   const fc::time_point start = state[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>();
   BOOST_REQUIRE_EQUAL(1u, attestation_status_count(ETH_OUTPOST_ID, opp::types::ATTESTATION_STATUS_READY));
   BOOST_REQUIRE(state[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == queued_at);
   BOOST_REQUIRE(state[epoch_fields::DUE_AT].as<fc::time_point>() == start + minimum_duration());

   set_chain_active(ETH_OUTPOST_ID, true);
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "inactive-2", tips);
   open_block_at(start + minimum_duration());
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(3u, current_epoch());
   BOOST_REQUIRE_EQUAL(0u, attestation_status_count(ETH_OUTPOST_ID, opp::types::ATTESTATION_STATUS_READY));
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// sysio.epoch records traffic only from sysio.msgch's own `queueout`: the same code running on another account
/// notifies sysio.epoch of its `queueout`, which changes nothing.
BOOST_FIXTURE_TEST_CASE(a_foreign_queueout_notification_records_no_traffic, sysio_msgch_chain_tester) { try {
   constexpr name kForeignQueue = "fake.msgch"_n;
   bootstrap();
   create_accounts({kForeignQueue});
   abi_serializer foreign_abi;
   deploy(kForeignQueue, contracts::msgch_wasm(), contracts::msgch_abi(), foreign_abi);
   produce_blocks();

   const transaction_trace_ptr foreign =
      queueout_trace(ETH_OUTPOST_ID, opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, 'f', kForeignQueue);
   BOOST_REQUIRE_EQUAL(1u, queueout_notifications(foreign, kForeignQueue));
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == fc::time_point());
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());
   BOOST_REQUIRE_EQUAL(0u, attestation_status_count(ETH_OUTPOST_ID, opp::types::ATTESTATION_STATUS_READY));
} FC_LOG_AND_RETHROW() }

/// sysio.msgch's `queueout` notification records nothing before the chain's genesis advance.
BOOST_FIXTURE_TEST_CASE(a_queueout_before_genesis_records_no_traffic, sysio_msgch_chain_tester) { try {
   set_epoch_config(ETH_CHAIN_CODE, EPOCH_DURATION_SEC, 1, 1);
   register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, ETH_CHAIN_CODE, ETH_EXTERNAL_CHAIN_ID);
   sysio_system::test_support::epoch_state::seat_window(*this, epoch_abi, ETH_CHAIN_CODE, {{BATCHOP}});
   produce_blocks();

   const transaction_trace_ptr trace =
      queueout_trace(ETH_OUTPOST_ID, opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, 'g');
   BOOST_REQUIRE_EQUAL(1u, queueout_notifications(trace));
   const fc::variant state = read_epoch_state();
   BOOST_REQUIRE_EQUAL(0u, state[epoch_fields::CURRENT_EPOCH_INDEX].as<uint32_t>());
   BOOST_REQUIRE(state[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == fc::time_point());
   BOOST_REQUIRE(state[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// sysio.msgch's `queueout` notification records a paused chain's traffic, but the chain does not advance until it
/// resumes, though it is due.
BOOST_FIXTURE_TEST_CASE(a_queueout_on_a_paused_chain_records_traffic_that_waits, sysio_msgch_chain_tester) { try {
   bootstrap();
   const uint32_t epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "paused-traffic", tips);
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, CHALG_ACCOUNT, epoch_actions::PAUSE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();

   const fc::time_point queued_at = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(1u, queueout_notifications(
                              queueout_trace(ETH_OUTPOST_ID, opp::types::ATTESTATION_TYPE_OPERATOR_ACTION, 'p')));
   const fc::time_point due = due_for(queued_at);
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == queued_at);
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == due);

   open_block_at(due);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(EPOCH_PAUSED_ERROR),
                       push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                            mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == due);

   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, CHALG_ACCOUNT, epoch_actions::UNPAUSE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
} FC_LOG_AND_RETHROW() }

/// `resendroster` records traffic at now on the chain it resends to, so a decided chain idle past its minimum becomes
/// due its traffic wait later and its next envelope carries the roster; the other chain stays idle.
BOOST_FIXTURE_TEST_CASE(resendroster_makes_an_idle_chain_due, sysio_msgch_chain_tester) { try {
   constexpr int64_t kIdleSec = 10;
   bootstrap();
   const uint32_t epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "resend-roster", tips);
   open_block_at(epoch_start() + minimum_duration() + fc::seconds(kIdleSec));

   const fc::time_point resent_at = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::RESEND_ROSTER,
                                       mvo()("code", ETH_CHAIN_CODE)));
   const fc::time_point due = resent_at + traffic_wait();
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::TRAFFIC_SINCE].as<fc::time_point>() == resent_at);
   BOOST_REQUIRE(read_epoch_state()[epoch_fields::DUE_AT].as<fc::time_point>() == due);
   BOOST_REQUIRE(read_epoch_state(SOL_OUTPOST_ID)[epoch_fields::DUE_AT].as<fc::time_point>() == fc::time_point());

   open_block_at(due);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(tip_carries_roster(ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// The advance that closes an epoch's books credits each member one chain minimum of seat-seconds, however long the
/// epoch ran: an epoch held idle for several minimums before its traffic earns no more than one that ran the minimum.
BOOST_FIXTURE_TEST_CASE(duty_credits_the_chain_minimum_however_long_the_epoch_ran, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kIdleMinimums = 5;
   bootstrap();
   std::map<uint64_t, inbound_stream_tip> tips;

   // Epoch 1 runs idle for several minimums before traffic makes it due.
   const fc::time_point first_start = read_epoch_state()[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>();
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "long-1", tips);
   produce_blocks(kIdleMinimums * EPOCH_DURATION_SEC * 2);
   advance_decided_epoch();
   const fc::time_point second_start = read_epoch_state()[epoch_fields::CURRENT_EPOCH_START].as<fc::time_point>();
   BOOST_REQUIRE_GT((second_start - first_start).to_seconds(), int64_t{kIdleMinimums * EPOCH_DURATION_SEC});
   BOOST_REQUIRE_EQUAL(0u, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));

   // Epoch 2's advance closes epoch 1's books with one minimum, then epoch 3's closes epoch 2's with another.
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "long-2", tips);
   advance_decided_epoch();
   BOOST_REQUIRE_EQUAL(uint64_t{EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
   deliver_chain_unanimous({BATCHOP}, ETH_OUTPOST_ID, "long-3", tips);
   advance_decided_epoch();
   BOOST_REQUIRE_EQUAL(uint64_t{2 * EPOCH_DURATION_SEC}, batch_duty_seat_seconds(BATCHOP, ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// `chkcons` evaluates one chain and refuses a chain that is not an active outpost.
BOOST_FIXTURE_TEST_CASE(chkcons_refuses_a_chain_that_is_not_an_active_outpost, sysio_msgch_chain_tester) { try {
   bootstrap();
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: chkcons: chain is not an active outpost"),
                       check_consensus(fc::slug_name{"BASE"}.value));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
//  Outbound backlog reports: a strict majority of a chain's eligible group reporting a backlog on its outpost, in a
//  decided epoch, is traffic since the majority-th earliest reported time.
// ---------------------------------------------------------------------------

/// A report waits for its epoch's decision: refused while the epoch has no delivery and while it has only a minority,
/// accepted once the majority's envelope is accepted.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_waits_for_the_epochs_decision, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   const uint32_t          epoch    = current_epoch();
   const uint64_t          since_ms = to_ms(epoch_start());
   const std::vector<char> envelope = encode_delivery(epoch, std::string("decided-before-report"));
   const auto              report   = [&] {
      return report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, since_ms);
   };

   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORT_UNDECIDED_EPOCH), report());
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORT_UNDECIDED_EPOCH), report());
   BOOST_REQUIRE(queue_reporters().empty());

   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, envelope));
   produce_blocks();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(success(), report());
   require_queue_report(BATCHOP, epoch, REPORTED_PENDING_COUNT, epoch_start());
} FC_LOG_AND_RETHROW() }

/// A report from a minority of the eligible group records no traffic, and a member that reports again replaces its row
/// instead of counting twice.
BOOST_FIXTURE_TEST_CASE(a_minority_backlog_report_records_no_traffic, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kRepeatPendingCount = REPORTED_PENDING_COUNT + 4;
   bootstrap(/*n_batch_ops=*/3);
   const uint32_t       epoch = current_epoch();
   const fc::time_point first = epoch_start() + fc::seconds(1);
   const fc::time_point again = epoch_start() + fc::seconds(2);
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C}, ETH_OUTPOST_ID, "minority-report", tips);
   produce_blocks(10);

   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP, first)));
   require_queue_report(BATCHOP, epoch, REPORTED_PENDING_COUNT, first);
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE(due_at() == fc::time_point());

   BOOST_REQUIRE_EQUAL(success(), report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, kRepeatPendingCount, to_ms(again)));
   require_queue_report(BATCHOP, epoch, kRepeatPendingCount, again);
   BOOST_REQUIRE(queue_reporters() == std::vector<name>{BATCHOP});
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE(due_at() == fc::time_point());
   BOOST_REQUIRE(due_at(SOL_OUTPOST_ID) == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// A lying minority that reports an earlier backlog never moves the chain's traffic earlier than the other members
/// report: of five eligible members, two report early and the honest third makes the third-earliest time its own.
/// Each report runs the tally again: an honest member's later report leaves the traffic as it was, and so does a liar's
/// still earlier one.
BOOST_FIXTURE_TEST_CASE(a_lying_minority_never_moves_reported_traffic_earlier, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 5; // a strict majority is three
   bootstrap(kGroupSize);
   const fc::time_point early   = epoch_start() + fc::seconds(2);
   const fc::time_point earlier = epoch_start() + fc::seconds(1);
   const fc::time_point honest  = epoch_start() + fc::seconds(12);
   const fc::time_point later   = honest + fc::seconds(2);
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E}, ETH_OUTPOST_ID, "lying-minority",
                           tips);
   produce_blocks(40);

   for (const name liar : {BATCHOP, BATCHOP_B}) {
      BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(liar, early)));
   }
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   for (const name member : {BATCHOP_C, BATCHOP_D}) {
      BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(member, honest)));
      BOOST_REQUIRE(traffic_since() == honest);
      BOOST_REQUIRE(due_at() == due_for(honest));
   }

   // {early, early, honest, later}: the third earliest is still the honest time.
   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(BATCHOP_C, later)));
   require_queue_report(BATCHOP_C, current_epoch(), REPORTED_PENDING_COUNT, later);
   BOOST_REQUIRE(traffic_since() == honest);
   // {earlier, early, honest, later}: so is it after a liar's earlier report.
   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(BATCHOP, earlier)));
   require_queue_report(BATCHOP, current_epoch(), REPORTED_PENDING_COUNT, earlier);
   BOOST_REQUIRE(traffic_since() == honest);
   BOOST_REQUIRE(due_at() == due_for(honest));
} FC_LOG_AND_RETHROW() }

/// The chain's traffic is the majority-th earliest of its eligible group's reported times, moving earlier as more
/// members report: of five eligible members, the third earliest. `chkcons` then advances the decided epoch at the
/// `due_at` the reports set and not before.
BOOST_FIXTURE_TEST_CASE(the_majority_th_earliest_report_makes_the_chain_due, sysio_msgch_chain_tester) { try {
   constexpr uint32_t     kGroupSize     = 5; // a strict majority is three
   const fc::microseconds block_interval = fc::milliseconds(config::block_interval_ms);
   bootstrap(kGroupSize);
   const uint32_t       epoch = current_epoch();
   const fc::time_point start = epoch_start();
   const auto           at    = [&](int64_t sec) { return start + fc::seconds(sec); };
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E}, ETH_OUTPOST_ID, "reported-traffic",
                           tips);
   produce_blocks(30);

   // Two reports are a minority; a third makes a majority of {5, 7, 9}, whose third earliest is 9.
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP, at(9))));
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP_B, at(5))));
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(BATCHOP_C, at(7))));
   BOOST_REQUIRE(traffic_since() == at(9));
   // {3, 5, 7, 9}: 7. Then {3, 4, 5, 7, 9}: 5.
   report_backlog(BATCHOP_D, at(3));
   BOOST_REQUIRE(traffic_since() == at(7));
   report_backlog(BATCHOP_E, at(4));
   BOOST_REQUIRE(traffic_since() == at(5));
   const fc::time_point due = due_for(at(5));
   BOOST_REQUIRE(due_at() == due);

   open_block_at(due - block_interval);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   open_block_at(due);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(epoch_start() == due);
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE(due_at() == fc::time_point());
} FC_LOG_AND_RETHROW() }

/// A reported time later than now, the largest value included, counts as now, and so does a zero time.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_from_the_future_or_without_a_time_counts_as_now,
                        sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   const uint32_t epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C}, ETH_OUTPOST_ID, "report-at-now", tips);
   produce_blocks(4);

   const fc::time_point first_at = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT,
                                               std::numeric_limits<uint64_t>::max()));
   require_queue_report(BATCHOP, epoch, REPORTED_PENDING_COUNT, first_at);
   produce_blocks();

   const fc::time_point second_at = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), report_queue(BATCHOP_B, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, 0));
   require_queue_report(BATCHOP_B, epoch, REPORTED_PENDING_COUNT, second_at);
   BOOST_REQUIRE(traffic_since() == second_at);
   BOOST_REQUIRE(due_at() == due_for(second_at));
} FC_LOG_AND_RETHROW() }

/// A report of an earlier epoch never counts: after the advance, one member's report is a minority though another
/// member reported in the previous epoch, and the walk erases that earlier report.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_of_an_earlier_epoch_never_counts, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   const std::vector<name> group{BATCHOP, BATCHOP_B, BATCHOP_C};
   const uint32_t          epoch       = current_epoch();
   const fc::time_point    first_start = epoch_start();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(group, ETH_OUTPOST_ID, "earlier-report-1", tips);
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP, first_start)));
   advance_decided_epoch();
   require_queue_report(BATCHOP, epoch, REPORTED_PENDING_COUNT, first_start);
   BOOST_REQUIRE(traffic_since() == fc::time_point());

   deliver_chain_unanimous(group, ETH_OUTPOST_ID, "earlier-report-2", tips);
   const fc::time_point start = epoch_start();
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP_B, start)));
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE(queue_report(BATCHOP).is_null());
   BOOST_REQUIRE(queue_reporters() == std::vector<name>{BATCHOP_B});

   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(BATCHOP_C, start)));
   BOOST_REQUIRE(traffic_since() == start);
   BOOST_REQUIRE(due_at() == start + minimum_duration());
   BOOST_REQUIRE(queue_reporters() == (std::vector<name>{BATCHOP_B, BATCHOP_C}));
} FC_LOG_AND_RETHROW() }

/// A reporter that leaves the eligible set drops out of the tally: once the first reporter is slashed, the second's
/// report is one of the two members left, a minority until the third reports. The slashed member's row stays, ignored,
/// and it cannot report again.
BOOST_FIXTURE_TEST_CASE(a_slashed_reporter_drops_out_of_the_tally, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   const uint32_t       epoch      = current_epoch();
   const fc::time_point start      = epoch_start();
   const fc::time_point slashed_at = start + fc::seconds(1);
   const fc::time_point second     = start + fc::seconds(5);
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C}, ETH_OUTPOST_ID, "slashed-reporter", tips);
   produce_blocks(20);

   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP, slashed_at)));
   slash_operator(BATCHOP);
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP_B, second)));
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(BATCHOP_C, start + fc::seconds(4))));
   BOOST_REQUIRE(traffic_since() == second);
   BOOST_REQUIRE(due_at() == due_for(second));
   require_queue_report(BATCHOP, epoch, REPORTED_PENDING_COUNT, slashed_at);

   produce_blocks();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORTER_NOT_ELIGIBLE),
                       report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, to_ms(start)));
} FC_LOG_AND_RETHROW() }

/// A slashed member keeps its seat, so the reports need a strict majority of every seat: two reports of five are a
/// minority, and stay one, repeated or not, once two silent members are slashed. The last eligible member's report
/// makes three and records the traffic since the third earliest time.
BOOST_FIXTURE_TEST_CASE(slashed_seats_still_count_toward_the_reports_majority, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroupSize = 5; // a strict majority is three, whatever the members' status
   bootstrap(kGroupSize);
   const fc::time_point start = epoch_start();
   const fc::time_point third = start + fc::seconds(4);
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C, BATCHOP_D, BATCHOP_E}, ETH_OUTPOST_ID, "slashed-seats",
                           tips);
   produce_blocks(20);

   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP, start + fc::seconds(2))));
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP_B, start + fc::seconds(3))));
   for (const name silent : {BATCHOP_C, BATCHOP_D}) slash_operator(silent);
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(BATCHOP, start + fc::seconds(1))));
   BOOST_REQUIRE(traffic_since() == fc::time_point());

   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(BATCHOP_E, third)));
   BOOST_REQUIRE(traffic_since() == third);
   BOOST_REQUIRE(due_at() == due_for(third));
} FC_LOG_AND_RETHROW() }

/// A decided epoch takes reports, and a paused chain records their traffic but does not advance until it resumes.
BOOST_FIXTURE_TEST_CASE(a_paused_chain_records_reported_traffic_but_holds, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   const uint32_t epoch = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C}, ETH_OUTPOST_ID, "paused-report", tips);
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, CHALG_ACCOUNT, epoch_actions::PAUSE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   produce_blocks();

   const fc::time_point backlog_since = control->head().block_time();
   for (const name member : {BATCHOP, BATCHOP_B}) report_backlog(member, backlog_since);
   const fc::time_point due = due_for(backlog_since);
   BOOST_REQUIRE(traffic_since() == backlog_since);
   BOOST_REQUIRE(due_at() == due);

   open_block_at(due);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(EPOCH_PAUSED_ERROR),
                       push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::ADVANCE,
                            mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   BOOST_REQUIRE(due_at() == due);

   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, CHALG_ACCOUNT, epoch_actions::UNPAUSE,
                                       mvo()(epoch_fields::CHAIN_CODE, ETH_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
} FC_LOG_AND_RETHROW() }

/// A disputed epoch is undecided, so its reports are refused while the dispute is open. Once the vote resolves it, the
/// members left eligible report, but every seat still counts toward the majority: a dispute opens only when no
/// version can reach it, so the winners left cannot either, and their reports record no traffic. The resolution
/// records the chain's traffic itself, so the chain is due and `chkcons` advances it at that time.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_waits_out_a_dispute, sysio_msgch_chain_tester) { try {
   const fc::microseconds block_interval = fc::milliseconds(config::block_interval_ms);
   const std::vector<name> winners{BATCHOP, BATCHOP_B};
   const std::vector<name> losers{BATCHOP_C, BATCHOP_D};
   bootstrap(/*n_batch_ops=*/5);
   const uint32_t          epoch = current_epoch();
   const std::vector<char> left  = encode_delivery(epoch, std::string("disputed-left"));
   const std::vector<char> right = encode_delivery(epoch, std::string("disputed-right"));
   const std::vector<char> third = encode_delivery(epoch, std::string("disputed-third"));

   // Two, two and one of five: no version can reach three, so the fifth delivery opens the dispute.
   for (const name member : winners) BOOST_REQUIRE_EQUAL(success(), deliver_as(member, ETH_OUTPOST_ID, left));
   for (const name member : losers) BOOST_REQUIRE_EQUAL(success(), deliver_as(member, ETH_OUTPOST_ID, right));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_E, ETH_OUTPOST_ID, third));
   produce_blocks();
   const fc::sha256 left_checksum = fc::sha256::hash(left.data(), left.size());
   assert_open_tie_dispute(epoch,
                           {left_checksum, fc::sha256::hash(right.data(), right.size()),
                            fc::sha256::hash(third.data(), third.size())},
                           {static_cast<uint32_t>(winners.size()), static_cast<uint32_t>(losers.size()),
                            ONE_OPERATOR_PER_TIED_VERSION});
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORT_UNDECIDED_EPOCH),
                       report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, to_ms(epoch_start())));

   // The resolution slashes the three that delivered another version and records the chain's traffic. The slashed
   // members' seats still count, so the two winners' reports are taken but record no traffic.
   const fc::time_point resolved_at = control->pending_block_time();
   resolve_tie_dispute(epoch, left_checksum);
   BOOST_REQUIRE(!epoch_is_paused());
   BOOST_REQUIRE(traffic_since() == resolved_at);
   for (const name member : winners) {
      BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(member, resolved_at)));
      require_queue_report(member, epoch, REPORTED_PENDING_COUNT, resolved_at);
   }
   const fc::time_point due = due_for(resolved_at);
   BOOST_REQUIRE(due_at() == due);

   open_block_at(due - block_interval);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   open_block_at(due);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
} FC_LOG_AND_RETHROW() }

/// A dispute's resolution records the chain's traffic at its time, so with no other traffic the chain is due and
/// `chkcons` advances it to its next group. Here the vote resolves after the chain's minimum has passed.
BOOST_FIXTURE_TEST_CASE(a_dispute_resolution_makes_the_chain_due, sysio_msgch_chain_tester) { try {
   constexpr int64_t      kLateSec       = 10;
   const fc::microseconds block_interval = fc::milliseconds(config::block_interval_ms);
   bootstrap(ONE_TO_ONE_TIE_GROUP_SIZE);
   const uint32_t          epoch = current_epoch();
   const std::vector<char> left  = encode_delivery(epoch, std::string("resolved-left"));
   const std::vector<char> right = encode_delivery(epoch, std::string("resolved-right"));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, left));
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP_B, ETH_OUTPOST_ID, right));
   produce_blocks();
   BOOST_REQUIRE(epoch_is_paused());
   BOOST_REQUIRE(traffic_since() == fc::time_point());

   open_block_at(epoch_start() + minimum_duration() + fc::seconds(kLateSec));
   const fc::time_point resolved_at = control->pending_block_time();
   resolve_tie_dispute(epoch, fc::sha256::hash(left.data(), left.size()));
   BOOST_REQUIRE(!epoch_is_paused());
   BOOST_REQUIRE(traffic_since() == resolved_at);
   const fc::time_point due = due_for(resolved_at);
   BOOST_REQUIRE(due_at() == due);

   open_block_at(due - block_interval);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   open_block_at(due);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID, BATCHOP));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
} FC_LOG_AND_RETHROW() }

/// A report is refused for any epoch but the chain's current one, without a pending message, and without its
/// reporter's signature, and nothing is stored; a report of the current epoch is accepted.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_needs_the_current_epoch_and_a_backlog, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);
   const uint32_t epoch = advance_one_epoch();
   BOOST_REQUIRE_GT(epoch, 1u);
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous({BATCHOP, BATCHOP_B, BATCHOP_C}, ETH_OUTPOST_ID, "current-epoch-report", tips);
   const uint64_t since_ms = to_ms(epoch_start());
   const auto     mismatch = [&](uint32_t reported) {
      return wasm_assert_msg(std::string(REPORT_EPOCH_MISMATCH) + std::to_string(reported) +
                             " current=" + std::to_string(epoch));
   };

   for (const uint32_t other : {epoch - 1, epoch + 1}) {
      BOOST_REQUIRE_EQUAL(mismatch(other),
                          report_queue(BATCHOP, ETH_OUTPOST_ID, other, REPORTED_PENDING_COUNT, since_ms));
   }
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORT_WITHOUT_BACKLOG),
                       report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, 0, since_ms));
   BOOST_REQUIRE_EQUAL(error("missing authority of " + BATCHOP.to_string()),
                       report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, since_ms, BATCHOP_B));
   BOOST_REQUIRE(queue_reporters().empty());

   BOOST_REQUIRE_EQUAL(success(), report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, since_ms));
   require_queue_report(BATCHOP, epoch, REPORTED_PENDING_COUNT, epoch_start());
} FC_LOG_AND_RETHROW() }

/// A report is refused for a chain that is not an active outpost, and for an outpost before its genesis advance.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_needs_an_active_outpost_past_its_genesis, sysio_msgch_chain_tester) { try {
   set_epoch_config(ETH_CHAIN_CODE, EPOCH_DURATION_SEC, 1, 1);
   register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, ETH_CHAIN_CODE, ETH_EXTERNAL_CHAIN_ID);
   sysio_system::test_support::epoch_state::seat_window(*this, epoch_abi, ETH_CHAIN_CODE, {{BATCHOP}});
   produce_blocks();
   const uint64_t since_ms = to_ms(control->head().block_time());

   BOOST_REQUIRE_EQUAL(0u, current_epoch());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORT_BEFORE_GENESIS),
                       report_queue(BATCHOP, ETH_OUTPOST_ID, 0, REPORTED_PENDING_COUNT, since_ms));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORT_NOT_ACTIVE_OUTPOST),
                       report_queue(BATCHOP, fc::slug_name{"BASE"}.value, 1, REPORTED_PENDING_COUNT, since_ms));
   BOOST_REQUIRE(queue_reporters().empty());
} FC_LOG_AND_RETHROW() }

/// Only a member of the chain's group on duty or of its next group reports its backlog. A member of the window's
/// third group, which is another chain's group on duty, an account that is no batch operator and, after the advance,
/// a member of the group that served the previous epoch only are refused; the other chain's member reports there, and
/// after the advance the new group on duty and the new next group report here. One chain's report walks only its own
/// rows.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_comes_only_from_the_group_on_duty_or_the_next,
                        sysio_msgch_chain_tester) { try {
   constexpr name kStranger = "stranger"_n;
   // Registered inside the bootstrap window, SOL is active at once; it starts after ETH, with ETH's last group as its
   // only one.
   register_chain(opp::types::ChainKind::CHAIN_KIND_SVM, SOL_CHAIN_CODE, SOL_EXTERNAL_CHAIN_ID);
   const std::vector<std::vector<name>> window    = bootstrap_disjoint_groups();
   const std::vector<name>&             sol_group = window[2];
   serve_chains(sol_group, {SOL_CHAIN_CODE});
   set_epoch_config(SOL_CHAIN_CODE, EPOCH_DURATION_SEC, DISJOINT_GROUP_SIZE, 1);
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, epoch_actions::SCHEDULE,
                                       mvo()(epoch_fields::CHAIN_CODE, SOL_CHAIN_CODE)));
   BOOST_REQUIRE_EQUAL(success(), push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, msgch_actions::BOOT_CHAIN,
                                       mvo()(msgch_fields::CHAIN_CODE, SOL_OUTPOST_ID)));
   create_accounts({kStranger});
   produce_blocks();
   BOOST_REQUIRE(sorted(current_group(SOL_OUTPOST_ID)) == sorted(sol_group));
   BOOST_REQUIRE(current_group() == window[0]);
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(window[0], ETH_OUTPOST_ID, "on-duty-eth-1", tips);
   deliver_chain_unanimous(sol_group, SOL_OUTPOST_ID, "on-duty-sol-1", tips);

   const uint32_t epoch    = current_epoch();
   const uint64_t since_ms = to_ms(epoch_start());
   const auto     report   = [&](name member, uint32_t reported_epoch) {
      return report_queue(member, ETH_OUTPOST_ID, reported_epoch, REPORTED_PENDING_COUNT, since_ms);
   };
   for (const name outsider : {sol_group[0], kStranger}) {
      BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORTER_NOT_IN_GROUP), report(outsider, epoch));
   }
   BOOST_REQUIRE_EQUAL(success(), report(window[1][0], epoch));
   BOOST_REQUIRE_EQUAL(success(), report_queue(sol_group[0], SOL_OUTPOST_ID, current_epoch(SOL_OUTPOST_ID),
                                               REPORTED_PENDING_COUNT, to_ms(epoch_start(SOL_OUTPOST_ID))));
   BOOST_REQUIRE(queue_reporters() == std::vector<name>{window[1][0]});
   BOOST_REQUIRE(queue_reporters(SOL_OUTPOST_ID) == std::vector<name>{sol_group[0]});

   governance_advance_trace(ETH_OUTPOST_ID);
   BOOST_REQUIRE(previous_group() == window[0]);
   BOOST_REQUIRE(current_group() == window[1]);
   deliver_chain_unanimous(window[1], ETH_OUTPOST_ID, "on-duty-eth-2", tips);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORTER_NOT_IN_GROUP), report(window[0][0], epoch + 1));
   for (const name reporter : {window[1][0], sol_group[0]}) {
      BOOST_REQUIRE_EQUAL(success(), report(reporter, epoch + 1));
   }
   BOOST_REQUIRE(sorted(queue_reporters()) == sorted({window[1][0], sol_group[0]}));
   BOOST_REQUIRE(queue_reporters(SOL_OUTPOST_ID) == std::vector<name>{sol_group[0]});
} FC_LOG_AND_RETHROW() }

/// The next group of the chain's window reports for a group on duty left below a majority of eligible reporters, by
/// its own strict majority of its own seats: with two of the three members on duty slashed after the decision, the
/// third one's report is a minority it can never lift; one next-group report is a minority too, and with the on-duty
/// report beside it still makes no majority, since the two groups' reports never add up. The second next-group report
/// makes the next group's majority, which records the traffic since the second earliest next-group time, and `chkcons`
/// advances the chain at the `due_at` it sets.
BOOST_FIXTURE_TEST_CASE(the_next_group_reports_for_a_group_on_duty_below_its_majority,
                        sysio_msgch_chain_tester) { try {
   const fc::microseconds               block_interval = fc::milliseconds(config::block_interval_ms);
   const std::vector<std::vector<name>> window         = bootstrap_disjoint_groups();
   const std::vector<name>&             on_duty        = window[0];
   const std::vector<name>&             next           = window[1];
   const uint32_t                       epoch          = current_epoch();
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(on_duty, ETH_OUTPOST_ID, "next-group-reports", tips);
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   produce_blocks(20);
   for (const name member : {on_duty[1], on_duty[2]}) slash_operator(member);

   const fc::time_point start = epoch_start();
   const auto           at    = [&](int64_t sec) { return start + fc::seconds(sec); };
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(on_duty[0], at(1))));
   BOOST_REQUIRE_EQUAL(0u, note_traffic_count(report_backlog(next[0], at(3))));
   BOOST_REQUIRE(traffic_since() == fc::time_point());
   BOOST_REQUIRE(due_at() == fc::time_point());

   BOOST_REQUIRE_EQUAL(1u, note_traffic_count(report_backlog(next[1], at(2))));
   BOOST_REQUIRE(traffic_since() == at(3));
   const fc::time_point due = due_for(at(3));
   BOOST_REQUIRE(due_at() == due);
   BOOST_REQUIRE(sorted(queue_reporters()) == sorted({on_duty[0], next[0], next[1]}));

   open_block_at(due - block_interval);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(epoch, current_epoch());
   open_block_at(due);
   BOOST_REQUIRE_EQUAL(success(), check_consensus(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(epoch + 1, current_epoch());
   BOOST_REQUIRE(current_group() == next);
} FC_LOG_AND_RETHROW() }

/// A member of the group on duty reports only while it is ACTIVE: one UNKNOWN after a withdrawal below its minimum and
/// one SLASHED are refused, while an ACTIVE member reports.
BOOST_FIXTURE_TEST_CASE(a_backlog_report_needs_an_active_reporter, sysio_msgch_chain_tester) { try {
   const std::vector<std::vector<name>> window = bootstrap_disjoint_groups(/*batchop_is_bootstrapped=*/false);
   const std::vector<name>&             group  = window[0];
   BOOST_REQUIRE(current_group() == group);
   BOOST_REQUIRE(std::find(group.begin(), group.end(), BATCHOP) != group.end());
   std::vector<name> others;
   for (const name member : group) {
      if (member != BATCHOP) others.push_back(member);
   }
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(group, ETH_OUTPOST_ID, "active-reporter", tips);

   BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, BATCHOP, "withdraw"_n, mvo()
      ("account", BATCHOP)("token_code", WIRE_CHAIN_CODE)("amount", BATCH_OPERATOR_MINIMUM_COLLATERAL)));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_UNKNOWN, operator_status(BATCHOP));
   slash_operator(others[0]);
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED, operator_status(others[0]));

   const uint32_t epoch    = current_epoch();
   const uint64_t since_ms = to_ms(epoch_start());
   for (const name inactive : {BATCHOP, others[0]}) {
      BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORTER_NOT_ELIGIBLE),
                          report_queue(inactive, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, since_ms));
   }
   BOOST_REQUIRE_EQUAL(success(), report_queue(others[1], ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, since_ms));
   BOOST_REQUIRE(queue_reporters() == std::vector<name>{others[1]});
} FC_LOG_AND_RETHROW() }

/// A member evicted from the chain cannot report from the seat a group drawn before its eviction still gives it,
/// though it stays ACTIVE. The group's two other members are then its whole eligible set, so they decide the epoch and
/// their reports make the chain's traffic.
BOOST_FIXTURE_TEST_CASE(an_evicted_member_cannot_report_from_its_remaining_seat, sysio_msgch_chain_tester) { try {
   // Epoch 1's group, drawn again at the first slide, serves again at epoch 4.
   constexpr uint32_t kSeatEpoch = 4;
   bootstrap_disjoint_groups(/*batchop_is_bootstrapped=*/false);
   // With the percent rail applying from the first duty, BATCHOP's silent epoch 1 evicts it when the advance from
   // epoch 2 closes that epoch's books.
   set_termination_thresholds(ROTATION_MAX_CONSECUTIVE_MISSES, DEFAULT_MAX_PERCENT_MISSES,
                              PERCENT_RAIL_FROM_FIRST_DUTY);
   while (current_epoch() < kSeatEpoch) advance_trace(ETH_OUTPOST_ID, MSGCH_ACCOUNT);
   BOOST_REQUIRE_EQUAL(std::string(served_status::evicted), membership_status(BATCHOP, ETH_OUTPOST_ID));
   BOOST_REQUIRE_EQUAL(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, operator_status(BATCHOP));
   const std::vector<name> group = current_group();
   BOOST_REQUIRE(std::find(group.begin(), group.end(), BATCHOP) != group.end());
   std::vector<name> others;
   for (const name member : group) {
      if (member != BATCHOP) others.push_back(member);
   }
   std::map<uint64_t, inbound_stream_tip> tips;
   deliver_chain_unanimous(others, ETH_OUTPOST_ID, "evicted-seat", tips);

   const uint32_t       epoch = current_epoch();
   const fc::time_point start = epoch_start();
   BOOST_REQUIRE(epoch_decided(ETH_OUTPOST_ID, epoch));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(REPORTER_NOT_ELIGIBLE),
                       report_queue(BATCHOP, ETH_OUTPOST_ID, epoch, REPORTED_PENDING_COUNT, to_ms(start)));
   for (const name member : others) report_backlog(member, start);
   BOOST_REQUIRE(traffic_since() == start);
   BOOST_REQUIRE(due_at() == start + minimum_duration());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
