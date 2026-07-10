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
/// drops the envelope without dispatching and without throwing. An outpost's FIRST accepted
/// envelope (no tip yet) still bootstraps regardless of its prev-hash.
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

   static constexpr uint32_t EPOCH_DURATION_SEC = 60;

   sysio_msgch_chain_tester() {
      produce_blocks(2);

      // sysio.* accounts BEFORE sysio.system so they keep unlimited RAM; the payepoch
      // destinations (sysio.dclaim / sysio.gov / sysio.ops) must exist for the emissions
      // pay-epoch transfers. Same bootstrap rationale as sysio_epoch_flushwtdw_tester.
      create_accounts({
         TOKEN_ACCOUNT, EPOCH_ACCOUNT, OPREG_ACCOUNT, MSGCH_ACCOUNT,
         CHALG_ACCOUNT, CHAINS_ACCOUNT, UWRIT_ACCOUNT, BATCHOP,
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

   /// Epoch + opreg config, one bootstrapped batch op, ETH + SOL chain rows, group schedule,
   /// genesis advance.
   void bootstrap() {
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH_ACCOUNT, epoch_abi, EPOCH_ACCOUNT,
         "setconfig"_n, mvo()
            ("epoch_duration_sec",                  EPOCH_DURATION_SEC)
            ("operators_per_epoch",                 1)
            ("batch_operator_minimum_active",       1)
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

      BOOST_REQUIRE_EQUAL(success(), push(OPREG_ACCOUNT, opreg_abi, OPREG_ACCOUNT,
         "regoperator"_n, mvo()
            ("account",          BATCHOP.to_string())
            ("type",             opp::types::OperatorType::OPERATOR_TYPE_BATCH)
            ("is_bootstrapped",  true)));

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
      return push(MSGCH_ACCOUNT, msgch_abi, BATCHOP, "deliver"_n, mvo()
         ("batch_op_name", BATCHOP.to_string())
         ("chain_code",    chain_code)
         ("data",          data));
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
   /// drops envelopes whose header fields do not recompute. `prev` / `env_hash` are raw 32-byte
   /// strings (or empty).
   std::vector<char> encode_delivery(uint32_t epoch_index, const std::string& att_data,
                                     const std::string& prev = {},
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
      oracle::finalize_header(*env.mutable_messages(0), {}, 1'775'612'516'983ULL);
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
/// pins cover the full semantic-header derivation, not just the byte layout.
BOOST_AUTO_TEST_CASE(canonical_oracle_matches_solidity_golden_vectors) { try {
   constexpr uint64_t GOLDEN_TS_MS = 1'775'612'516'983ULL;

   auto depot_shape = [&](uint32_t epoch_index, const std::string& prev,
                          const std::string& prev_message_id,
                          const std::vector<std::string>& att_datas) {
      sysio::opp::Envelope env;
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
      "0a00120c0a040800100012040800100028f7b483d6d63330073800a20100c20292010a7f0a0c0a040800"
      "100012040800100012200000000000000001210103982d1ae1f083b047bde00e77e4a337f3b31c8d223c"
      "1a00280f32206429fe11b290953c3e28e6ed7887059307329591c6296d6e41d27e4e6ddcae9938f7b483"
      "d6d6334220fb2b80f90bf26934210103982d1ae1f083b047bde00e77e4a337f3b31c8d223c120f080112"
      "0b08d10f10041a04deadbeef");
   const auto digest_a = oracle::epoch_digest(env_a);
   BOOST_REQUIRE_EQUAL(digest_a.str(),
      "c7c6502a5b047c0742c887122350c0b6731c60f3f5f9d48cdde9d4f2b6b8880a");

   // Vector B: depot shape, epoch 8, chained from A at both levels (envelope digest + message
   // id, so B's message carries sequence number 2), two attestations.
   const auto env_b = depot_shape(8, oracle::digest_bytes(digest_a),
      env_a.messages(0).header().message_id(),
      { std::string("\xde\xad\xbe\xef", 4), std::string("\xca\xfe\xba\xbe\x01", 5) });
   const auto enc_b = oracle::encode(env_b);
   BOOST_REQUIRE_EQUAL(fc::to_hex(enc_b.data(), enc_b.size()),
      "0a00120c0a040800100012040800100028f7b483d6d63330083800a20120c7c6502a5b047c0742c88712"
      "2350c0b6731c60f3f5f9d48cdde9d4f2b6b8880ac202c1010a9f010a0c0a040800100012040800100012"
      "2000000000000000022437e72cf67a093c4c5753cbb3ce71b76c890da8f9965c351a2000000000000000"
      "01210103982d1ae1f083b047bde00e77e4a337f3b31c8d223c281d3220fdbcffc45ad50a6a2d1376af8c"
      "498d86910751868ae7e14fe909477b319ec98d38f7b483d6d63342208d135355c556a6ed2437e72cf67a"
      "093c4c5753cbb3ce71b76c890da8f9965c35121d0801120b08d10f10041a04deadbeef120c08d10f1005"
      "1a05cafebabe01");
   BOOST_REQUIRE_EQUAL(oracle::epoch_digest(env_b).str(),
      "c7e7d905b6c87a209fde8319701a5a7d95350c989e234eedb26bdeb14cd5bddd");

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
   const auto n1_digest = oracle::epoch_digest(decode_envelope(n1));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n1));
   produce_blocks();

   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!opc.is_null());
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n1_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);

   // Epoch E+1: correctly chained envelope (prev = tip) is accepted and advances the tip.
   epoch = advance_one_epoch();
   auto n2 = encode_delivery(epoch, "bravo", oracle::digest_bytes(n1_digest));
   const auto n2_digest = oracle::epoch_digest(decode_envelope(n2));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n2));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n2_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);

   // Epoch E+2: chain break: a non-empty prev that does not continue the tip is dropped
   // without throwing: deliver succeeds, nothing is dispatched, the tip does not move.
   const uint32_t break_epoch = advance_one_epoch();
   auto n3 = encode_delivery(break_epoch, "charlie", std::string(32, '\x11'));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n3));
   produce_blocks();

   opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), epoch);            // still E+1
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), n2_digest.str()); // tip unchanged
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, break_epoch), 0u);  // nothing dispatched

   // Epoch E+3: enforcement (SEC-107 completion): an EMPTY prev on a non-genesis epoch is now a
   // chain break. Both outposts self-chain per stream, so once a tip is recorded an empty
   // prev-hash no longer bootstraps — it is dropped without throwing and the tip does not move.
   const uint32_t empty_epoch = advance_one_epoch();
   auto n4 = encode_delivery(empty_epoch, "delta");   // no prev => empty field
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, n4));
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
   auto n5 = encode_delivery(blank_epoch, "echo", oracle::digest_bytes(n2_digest),
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
/// forged field drops the envelope before dispatch -- no attestation rows, no consensus record,
/// no chain-tip movement -- without throwing. Each forgery is exercised at its own epoch on a
/// correctly CHAINED envelope, so the drop is attributable to the header check rather than the
/// envelope-chain check; a final well-formed delivery proves the stream resumes.
BOOST_FIXTURE_TEST_CASE(inbound_semantic_header_forgeries_dropped, sysio_msgch_chain_tester) { try {
   bootstrap();

   // Establish a tip so every subsequent forgery chains correctly at the envelope level.
   uint32_t epoch = current_epoch();
   auto base = encode_delivery(epoch, "alpha");
   const auto tip = oracle::epoch_digest(decode_envelope(base));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, base));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, epoch), 1u);
   const uint32_t accepted_epoch = epoch;

   // Deliver a correctly-chained envelope whose decoded form was altered by `mutate`, then
   // assert it was dropped: deliver itself succeeds (never-throw), no attestation row for its
   // epoch, the consensus record stays at the last accepted epoch, the tip does not move.
   auto forged_delivery_dropped = [&](const char* tag,
                                      const std::function<void(sysio::opp::Envelope&)>& mutate) {
      const uint32_t e = advance_one_epoch();
      auto env = decode_envelope(encode_delivery(e, tag, oracle::digest_bytes(tip)));
      mutate(env);
      std::vector<char> out(env.ByteSizeLong());
      env.SerializeToArray(out.data(), static_cast<int>(out.size()));
      BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, out));
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
      // Correct checksum tail, wrong embedded sequence number (2 where the derivation demands
      // previous_message_id's sequence + 1 = 1).
      auto* h = env.mutable_messages(0)->mutable_header();
      h->set_message_id(oracle::derive_message_id(oracle::header_checksum(*h), 2));
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

   // The stream resumes: a well-formed, correctly chained envelope is accepted and advances the
   // tip past all the dropped epochs.
   const uint32_t resume_epoch = advance_one_epoch();
   auto resume = encode_delivery(resume_epoch, "omega", oracle::digest_bytes(tip));
   const auto resume_digest = oracle::epoch_digest(decode_envelope(resume));
   BOOST_REQUIRE_EQUAL(success(), deliver(ETH_OUTPOST_ID, resume));
   produce_blocks();
   auto opc = get_outpcons(ETH_OUTPOST_ID);
   BOOST_REQUIRE_EQUAL(opc["epoch_index"].as<uint32_t>(), resume_epoch);
   BOOST_REQUIRE_EQUAL(opc["envelope_digest"].as_string(), resume_digest.str());
   BOOST_REQUIRE_EQUAL(attestation_count(ETH_OUTPOST_ID, resume_epoch), 1u);
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

   // Cross-stream link (prev = depot's outbound emit digest) is now a chain break: dropped
   // without throwing, tip does not move, nothing dispatched.
   BOOST_REQUIRE_EQUAL(success(),
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

   BOOST_REQUIRE_EQUAL(success(),
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

BOOST_AUTO_TEST_SUITE_END()
