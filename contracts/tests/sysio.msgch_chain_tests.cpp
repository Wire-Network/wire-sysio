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

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;

using mvo = fc::mutable_variant_object;

// Canonical-encoding + header-derivation oracle shared across the contract tests.
#include "opp_envelope_oracle.hpp"

namespace {

inline fc::mutable_variant_object codename_mvo(std::string_view s) {
   return mvo()("value", fc::slug_name{s}.value);
}

using fc::slug_name_literals::operator""_s;

constexpr uint64_t ETH_OUTPOST_ID = "ETH"_s.value;
constexpr uint64_t SOL_OUTPOST_ID = "SOL"_s.value;

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
   static constexpr auto UWRIT_ACCOUNT  = "sysio.uwrit"_n;
   static constexpr auto BATCHOP        = "batchop.a"_n;
   static constexpr auto BATCHOP_B      = "batchop.b"_n;
   static constexpr auto BATCHOP_C      = "batchop.c"_n;

   static constexpr uint32_t EPOCH_DURATION_SEC = 60;

   sysio_msgch_chain_tester() {
      produce_blocks(2);

      // sysio.* accounts BEFORE sysio.system so they keep unlimited RAM; the payepoch
      // destinations (sysio.dclaim / sysio.gov / sysio.ops) must exist for the emissions
      // pay-epoch transfers. Same bootstrap rationale as sysio_epoch_flushwtdw_tester.
      create_accounts({
         TOKEN_ACCOUNT, EPOCH_ACCOUNT, OPREG_ACCOUNT, MSGCH_ACCOUNT,
         CHALG_ACCOUNT, CHAINS_ACCOUNT, UWRIT_ACCOUNT, BATCHOP, BATCHOP_B, BATCHOP_C,
         "sysio.dclaim"_n, "sysio.gov"_n, "sysio.ops"_n
      });
      produce_blocks(2);

      deploy(EPOCH_ACCOUNT,  contracts::epoch_wasm(),  contracts::epoch_abi(),  epoch_abi);
      deploy(OPREG_ACCOUNT,  contracts::opreg_wasm(),  contracts::opreg_abi(),  opreg_abi);
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi);
      deploy(UWRIT_ACCOUNT,  contracts::uwrit_wasm(),  contracts::uwrit_abi(),  uwrit_abi);
      deploy(TOKEN_ACCOUNT,  contracts::token_wasm(),  contracts::token_abi(),  token_abi);
      produce_blocks(1);

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
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def parsed_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, parsed_abi), true);
      out_ser.set_abi(std::move(parsed_abi),
                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   void deploy(name account, std::vector<uint8_t> wasm, std::vector<char> abi,
               abi_serializer& out_ser) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      set_privileged(account);
      load_abi(account, out_ser);
   }

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
            ("standby_end_rank",       uint32_t(28))
            ("epoch_log_retention_count", uint32_t(8640))
            ("pay_cadence_epochs",     uint16_t(1))));
   }

   /// Epoch + opreg config, bootstrapped batch ops (`BATCHOP` always; `BATCHOP_B`/`BATCHOP_C` when
   /// `n_batch_ops` is 3 -- a single group of three, so consensus needs more than one delivery),
   /// ETH + SOL chain rows, group schedule, genesis advance.
   void bootstrap(uint32_t n_batch_ops = 1) {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "setconfig"_n, mvo()
            ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
            // Consensus group size: evalcons' unanimous/majority thresholds derive from
            // `epochcfg.operators_per_epoch` (see msgch's epoch_operators_per_group). The epoch
            // contract enforces minimum_active == operators_per_epoch * batch_op_groups.
            ("operators_per_epoch",                 n_batch_ops)
            ("batch_operator_minimum_active",       n_batch_ops)
            ("batch_op_groups",                     1)
            ("epoch_retention_envelope_log_count",  200)));

      BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults());
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT,
         "initt5"_n, mvo()
            ("start_time", fc::time_point_sec(control->head().block_time()))));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "setconfig"_n, mvo()
            ("max_available_producers",          21)
            ("max_available_batch_ops",          63)
            ("max_available_underwriters",       21)
            ("terminate_prune_delay_ms",         600000)
            ("terminate_max_consecutive_misses", 5)
            ("terminate_max_pct_misses_24h",     5)
            ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
            ("req_prod_collat",                  fc::variants{})
            ("req_batchop_collat",               fc::variants{})
            ("req_uw_collat",                    fc::variants{})));

      std::vector<name> batch_ops{BATCHOP};
      if (n_batch_ops == 3) {
         batch_ops.push_back(BATCHOP_B);
         batch_ops.push_back(BATCHOP_C);
      }
      for (const auto& op : batch_ops) {
         BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
            "regoperator"_n, mvo()
               ("account",          op.to_string())
               ("type",             opp::types::OperatorType::OPERATOR_TYPE_BATCH)
               ("is_bootstrapped",  true)));
      }

      register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, "ETH", 31337);
      register_chain(opp::types::ChainKind::CHAIN_KIND_SVM, "SOL", 1);

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "schbatchgps"_n, mvo()));

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "advance"_n, mvo()));

      produce_blocks();
   }

   void register_chain(opp::types::ChainKind kind, std::string_view code, uint32_t chain_id) {
      BOOST_REQUIRE_EQUAL(success(), push(CHAINS_ACCOUNT, chains_abi, CHAINS_ACCOUNT,
         "regchain"_n, mvo()
            ("kind",              kind)
            ("code",              codename_mvo(code))
            ("external_chain_id", chain_id)
            ("name",              std::string("outpost-test"))
            ("description",       std::string{})));
   }

   uint32_t advance_one_epoch() {
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT,
         "advance"_n, mvo()));
      produce_blocks();
      return current_epoch();
   }

   uint32_t current_epoch() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT,
                                     "epochstate"_n, "epochstate"_n);
      if (data.empty()) return 0;
      auto v = epoch_abi.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return v["current_epoch_index"].as<uint32_t>();
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

   /// Let the consensus boundary elapse WITHOUT advancing the epoch: evalcons' fallback
   /// (majority) path opens once `epoch_duration_sec` has passed since the epoch started,
   /// while `deliver`'s epoch gate keeps accepting envelopes for the still-current epoch.
   void elapse_epoch_boundary() {
      for (uint32_t i = 0; i < EPOCH_DURATION_SEC * 2 + 1; ++i) {
         produce_block();
      }
   }

   action_result queueout(uint64_t chain_code, uint32_t attest_type,
                          std::vector<char> data) {
      return push(MSGCH_ACCOUNT, msgch_abi, MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("chain_code",   chain_code)
         ("attest_type",  attest_type)
         ("data",         std::move(data)));
   }

   action_result buildenv(uint64_t chain_code) {
      return push(MSGCH_ACCOUNT, msgch_abi, EPOCH_ACCOUNT, "buildenv"_n, mvo()
         ("chain_code", chain_code));
   }

   // -- Table readers --

   /// The single surviving outbound envelope row for `chain_code` (the table is one-deep per
   /// outpost); null variant when none.
   fc::variant find_outbound_envelope(uint64_t chain_code, uint64_t scan_until = 32) {
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            "outbound_envelope", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["chain_code"].as_uint64() == chain_code) return row;
      }
      return fc::variant{};
   }

   /// Per-outpost consensus row (`outpcons`, primary key = chain_code); null variant when absent.
   fc::variant get_outpcons(uint64_t chain_code) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outpcons"_n, chain_code);
      return data.empty() ? fc::variant() : msgch_abi.binary_to_variant(
         "outpost_consensus_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Count attestation rows recorded for (`chain_code`, `epoch_index`); the observable effect
   /// of an ACCEPTED inbound envelope (rows are emplaced before dispatch, even for types
   /// dispatch drops as out of scope).
   uint32_t attestation_count(uint64_t chain_code, uint32_t epoch_index,
                              uint64_t scan_until = 64) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            "attestation_entry", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["chain_code"].as_uint64() == chain_code &&
             row["epoch_index"].as<uint32_t>() == epoch_index) ++n;
      }
      return n;
   }

   // -- Inbound envelope builder --

   /// Encode a deliverable envelope carrying one out-of-scope STAKE attestation (dispatch drops
   /// the attestation silently; acceptance is still fully observable via `outpcons` and the
   /// stored attestation row). The semantic header is derived per the spec — `apply_consensus`
   /// drops envelopes whose header fields do not recompute or whose message does not continue the
   /// per-outpost message chain. `prev` (previous_envelope_hash), `prev_message_id`, and
   /// `env_hash` are raw 32-byte strings (or empty for stream genesis).
   std::vector<char> encode_delivery(uint32_t epoch_index, const std::string& att_data,
                                     const std::string& prev = {},
                                     const std::string& prev_message_id = {},
                                     const std::string& env_hash = {}) {
      sysio::opp::Envelope env;
      env.set_epoch_index(epoch_index);
      env.set_epoch_envelope_index(1);
      env.set_epoch_timestamp(1'775'612'516'983ULL);
      if (!prev.empty()) env.set_previous_envelope_hash(prev);
      if (!env_hash.empty()) env.set_envelope_hash(env_hash);
      auto* att = env.add_messages()->mutable_payload()->add_attestations();
      att->set_type(sysio::opp::types::ATTESTATION_TYPE_STAKE);
      att->set_data(att_data);
      att->set_data_size(static_cast<uint32_t>(att_data.size()));
      oracle::finalize_header(*env.mutable_messages(0), prev_message_id, 1'775'612'516'983ULL);
      std::vector<char> out(env.ByteSizeLong());
      env.SerializeToArray(out.data(), static_cast<int>(out.size()));
      return out;
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

   // -- SEC-28 rotation-termination helpers (drive recorddel/termcheck through the REAL advance) --

   /// The opreg operator row for `account` (status + status_reason), or null when absent.
   fc::variant get_operator(name account) {
      auto data = get_row_by_account(OPREG_ACCOUNT, OPREG_ACCOUNT, "operators"_n, account);
      return data.empty() ? fc::variant() : opreg_abi.binary_to_variant(
         "operator_entry", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant read_epoch_state() {
      auto data = get_row_by_account(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "epochstate"_n, "epochstate"_n);
      return data.empty() ? fc::variant() : epoch_abi.binary_to_variant(
         "epoch_state", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// The single batch operator on duty for the CURRENT epoch (rotation setup runs one op per
   /// group); name{} when the schedule is empty.
   name duty_member() {
      auto st = read_epoch_state();
      if (st.is_null()) return name{};
      auto groups = st["batch_op_groups"].get_array();
      uint64_t cur = st["current_batch_op_group"].as_uint64();
      if (cur >= groups.size()) return name{};
      auto members = groups[cur].get_array();
      return members.empty() ? name{} : name(members[0].as_string());
   }

   /// Advance exactly one epoch, landing the advance at the fixed-cadence next_epoch_start. Because
   /// next_epoch_start is a fixed cadence (advance sets current_epoch_start = prior next_epoch_start,
   /// never `now`) and recorddel stamps at one-second granularity, running the advance in the second
   /// of next_epoch_start spaces every epoch's records exactly epoch_duration apart -- the exact-span
   /// timing the SEC-28 window bound is defined against, with no block-granularity drift.
   uint32_t advance_to_next_epoch() {
      auto st = read_epoch_state();
      BOOST_REQUIRE(!st.is_null());
      auto next  = st["next_epoch_start"].as<fc::time_point>();
      auto delta = next - control->head().block_time();
      if (delta.count() > 0) produce_block(delta);
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, MSGCH_ACCOUNT, "advance"_n, mvo()));
      produce_blocks();
      return current_epoch();
   }

   static fc::variant make_chain_min_bond(std::string_view chain_code, std::string_view token_code,
                                          uint64_t min_bond) {
      return fc::variant(mvo()
         ("chain_code",          codename_mvo(chain_code))
         ("token_code",          codename_mvo(token_code))
         ("min_bond",            min_bond)
         ("config_timestamp_ms", uint64_t{0}));
   }

   /// Inline collateral credit (the path sysio.msgch drives in production; pushed directly here) that
   /// lifts a non-bootstrapped operator to ACTIVE once its bond meets the configured minimum.
   action_result depositinle(name account, std::string_view chain_code, std::string_view token_code,
                             uint64_t amount) {
      return push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "depositinle"_n, mvo()
         ("account",             account.to_string())
         ("chain_code",          codename_mvo(chain_code))
         ("token_code",          codename_mvo(token_code))
         ("amount",              amount)
         ("actor_chain",         opp::types::ChainKind::CHAIN_KIND_EVM)
         ("actor_address",       std::vector<char>{})
         ("original_message_id", std::string(64, '0')));
   }

   /// bootstrap() variant for a real rotation: THREE single-operator groups (so a resident op is on
   /// duty once per 3-epoch rotation), the SEC-28 percent rail disabled up to its accepted ceiling
   /// (99, so an anchored run terminates on the CONSECUTIVE rail), and `terminate_window_ms` set by
   /// the caller (the exact span bound for this schedule). ETH outpost registered; genesis advance run.
   void bootstrap_rotation(uint64_t terminate_window_ms) {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "setconfig"_n, mvo()
         ("epoch_duration_sec",                 EPOCH_DURATION_SEC)
         ("operators_per_epoch",                1)
         ("batch_operator_minimum_active",      3)
         ("batch_op_groups",                    3)
         ("epoch_retention_envelope_log_count", 200)));

      BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults());
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, sysio_abi, SYSIO_ACCOUNT, "initt5"_n, mvo()
         ("start_time", fc::time_point_sec(control->head().block_time()))));

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "setconfig"_n, mvo()
         ("max_available_producers",          21)
         ("max_available_batch_ops",          63)
         ("max_available_underwriters",       21)
         ("terminate_prune_delay_ms",         600000)
         ("terminate_max_consecutive_misses", 5)
         ("terminate_max_pct_misses_24h",     99)
         ("terminate_window_ms",              terminate_window_ms)
         ("req_prod_collat",                  fc::variants{})
         ("req_batchop_collat",               fc::variants{ make_chain_min_bond("ETH", "ETH", 1) })
         ("req_uw_collat",                    fc::variants{})));

      register_chain(opp::types::ChainKind::CHAIN_KIND_EVM, "ETH", 31337);

      // BATCHOP is the termination target: NON-bootstrapped (bootstrapped operators are exempt from
      // rolling-window termination -- see opreg::termcheck) and collateralized so it activates.
      // BATCHOP_B / BATCHOP_C are bootstrapped fillers for the other two groups. schbatchgps sorts
      // non-bootstrapped first, so BATCHOP lands in group 0 (on duty at epochs 1, 4, 7, ...).
      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
         ("account", BATCHOP.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
         ("is_bootstrapped", false)));
      BOOST_REQUIRE_EQUAL(success(), depositinle(BATCHOP, "ETH", "ETH", 1));
      for (const auto& op : {BATCHOP_B, BATCHOP_C}) {
         BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT, "regoperator"_n, mvo()
            ("account", op.to_string())("type", opp::types::OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped", true)));
      }
      BOOST_REQUIRE(!get_operator(BATCHOP).is_null());
      BOOST_REQUIRE(opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE ==
                    get_operator(BATCHOP)["status"].as<opp::types::OperatorStatus>());
      BOOST_REQUIRE_EQUAL(0, get_operator(BATCHOP)["is_bootstrapped"].as_uint64());

      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "schbatchgps"_n, mvo()));
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT, "advance"_n, mvo()));
      produce_blocks();
   }

   abi_serializer sysio_abi, token_abi, epoch_abi, opreg_abi, msgch_abi, chains_abi, uwrit_abi;
};

// ---------------------------------------------------------------------------
//  Golden vectors: cross-language agreement with the Solidity codec.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(sysio_msgch_chain_tests)

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
   // epoch pins the one-deep outenvelopes row so no epoch-advance emission replaces it.
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
/// classifies every operator's delivery as canonical or slashable), and `deliver` must record --
/// not chain-break-revert -- them. A three-operator group reaches majority consensus at the second
/// post-boundary delivery; the third operator's byte-identical delivery is the late confirmation.
/// A DIVERGENT post-acceptance delivery still reverts (fail closed): post-acceptance divergence
/// cannot open a dispute, so nothing legitimate is lost by rejecting it at ingress.
BOOST_FIXTURE_TEST_CASE(late_confirmation_after_consensus_recorded, sysio_msgch_chain_tester) { try {
   bootstrap(/*n_batch_ops=*/3);

   const uint32_t epoch = current_epoch();
   auto winner = encode_delivery(epoch, "late-confirm");
   const auto winner_digest = oracle::epoch_digest(decode_envelope(winner));

   // First delivery: group of three, boundary not elapsed -- no consensus yet.
   BOOST_REQUIRE_EQUAL(success(), deliver_as(BATCHOP, ETH_OUTPOST_ID, winner));
   produce_blocks();
   {
      auto opc = get_outpcons(ETH_OUTPOST_ID);
      BOOST_REQUIRE(opc.is_null() || !opc["consensus_reached"].as<bool>());
   }

   // Second delivery after the boundary: majority (2 of 3) tips consensus and advances the tip
   // to the winner's own digest.
   elapse_epoch_boundary();
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
   // recorded winner, so it validates against the advanced tip and reverts as a chain break.
   auto divergent = encode_delivery(epoch, "late-diverge");
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: delivered envelope failed inbound-chain or "
            "semantic-header validation"),
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

// SEC-28 (huang review): terminate on the CONSECUTIVE-miss rail through the REAL rotation -- a
// materialized three-group schedule driven by advance() with one outpost, at exactly the minimum
// window the span bound accepts. A resident operator is on duty once per three-epoch rotation, and
// the window spans exactly (max_consecutive_misses + 1) rotations, so the delivered anchor that
// keeps the percent rail below its ceiling sits exactly on the window edge at the terminating miss.
// If the bound (or the window scan) were off by one rotation the anchor would age out, the window
// would be all-miss (100% > 99), and the operator would terminate early on the PERCENT rail -- so
// requiring termination on the sixth consecutive miss with the consecutive reason pins the exact
// span, driven end to end by advance()'s inline recorddel/termcheck rather than fabricated timing.
BOOST_FIXTURE_TEST_CASE(terminate_at_duty_rotation_via_advance, sysio_msgch_chain_tester) { try {
   constexpr uint32_t kGroups          = 3;
   constexpr uint32_t kMaxConsecMisses = 5;
   // Exact minimum the SEC-28 bound accepts for this schedule: (misses + 1) duty rotations.
   const uint64_t window_ms =
      (uint64_t{kMaxConsecMisses} + 1) * kGroups * EPOCH_DURATION_SEC * 1000ULL;

   bootstrap_rotation(window_ms);

   // Walk the rotation via advance(). BATCHOP delivers on its first duty epoch (the anchor) and
   // never again; every later duty epoch is a miss recorded by advance()'s inline recorddel. The
   // sixth consecutive miss is BATCHOP's 7th duty (anchor + 6 misses).
   constexpr uint32_t kTerminatingDuty = kMaxConsecMisses + 2;
   uint32_t batchop_duties = 0;
   bool     terminated     = false;
   for (uint32_t epoch = 0; epoch < kGroups * (kMaxConsecMisses + 4) && !terminated; ++epoch) {
      if (duty_member() == BATCHOP) {
         ++batchop_duties;
         if (batchop_duties == 1) {
            // Anchor: one delivered inbound envelope (stream genesis) for the current epoch.
            BOOST_REQUIRE_EQUAL(success(),
               deliver_as(BATCHOP, ETH_OUTPOST_ID,
                          encode_delivery(current_epoch(), std::string("\x01", 1))));
            // Finalize the delivery before advance_to_next_epoch's epoch-length jump, or the pending
            // transaction expires across it.
            produce_blocks();
         }
         // otherwise: withhold delivery, so advance() records this duty epoch as a miss
      }
      advance_to_next_epoch();

      auto op = get_operator(BATCHOP);
      BOOST_REQUIRE(!op.is_null());
      const auto status = op["status"].as<opp::types::OperatorStatus>();
      if (status == opp::types::OperatorStatus::OPERATOR_STATUS_TERMINATED) {
         terminated = true;
         BOOST_REQUIRE_EQUAL(kTerminatingDuty, batchop_duties);
         BOOST_REQUIRE_EQUAL("rolling-window: >5 consecutive misses",
                             op["status_reason"].as_string());
      } else {
         // Still ACTIVE: BATCHOP must not terminate before its sixth miss (its 7th duty).
         BOOST_REQUIRE(status == opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE);
         BOOST_REQUIRE_LT(batchop_duties, kTerminatingDuty);
      }
   }
   BOOST_REQUIRE(terminated);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
