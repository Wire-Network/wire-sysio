#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <fc/variant_object.hpp>
#include <fc/slug_name.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"
#include <sysio/chain/action.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;

using mvo = fc::mutable_variant_object;

namespace {

} // anonymous namespace

class sysio_msgch_tester : public tester {
public:
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;

   sysio_msgch_tester() {
      produce_blocks(2);

      create_accounts({
         MSGCH_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT, CHAINS_ACCOUNT,
         "batchop1"_n, "batchop2"_n, "batchop3"_n,
         "batchop4"_n, "batchop5"_n, "batchop.a"_n,
         "batchop.b"_n
      });
      produce_blocks(2);

      set_code(MSGCH_ACCOUNT, contracts::msgch_wasm());
      set_abi(MSGCH_ACCOUNT, contracts::msgch_abi().data());
      set_privileged(MSGCH_ACCOUNT);

      set_code(CHAINS_ACCOUNT, contracts::chains_wasm());
      set_abi(CHAINS_ACCOUNT, contracts::chains_abi().data());
      set_privileged(CHAINS_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(MSGCH_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_msgch_action(name signer, name action_name, const variant_object& data) {
      return push_msgch_action(signer, action_name, vector<permission_level>{{signer, config::active_name}}, data);
   }
   action_result push_msgch_action(name signer, name action_name, std::vector<permission_level> auths, const variant_object& data) {
      base_tester::push_action(MSGCH_ACCOUNT, action_name, std::move(auths), data);
      return success();
   }

   action_result deliver(name op, uint64_t chain_code,
                         std::vector<char> data = {}) {
      return push_msgch_action(op, "deliver"_n, mvo()
         ("batch_op_name", op)
         ("chain_code", chain_code)
         ("data", data)
      );
   }

   action_result evalcons(uint64_t req_id) {
      return push_msgch_action(MSGCH_ACCOUNT, "evalcons"_n, mvo()
         ("req_id", req_id)
      );
   }

   action_result queueout(uint64_t chain_code, uint16_t attest_type, std::vector<char> data = {}) {
      return push_msgch_action(MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("chain_code", chain_code)
         ("attest_type", attest_type)
         ("data", data)
      );
   }

   action_result regchain(opp::types::ChainKind kind,
                          std::string_view code,
                          uint32_t chain_id) {
      base_tester::push_action(CHAINS_ACCOUNT, "regchain"_n, CHAINS_ACCOUNT, mvo()
         ("kind",              kind)
         ("code",              code)
         ("external_chain_id", chain_id)
         ("name",              std::string("outpost"))
         ("description",       std::string{})
         ("outpost", sysio_system::test_support::no_outpost_mvo())
      );
      return success();
   }

   action_result buildenv(uint64_t chain_code) {
      return push_msgch_action(MSGCH_ACCOUNT, "buildenv"_n, {{
         EPOCH_ACCOUNT, config::active_name
      }}, mvo()
         ("chain_code", chain_code)
      );
   }

   fc::sha256 make_hash(const std::string& seed) {
      return fc::sha256::hash(seed);
   }

   // ── Table read helpers ──

   fc::variant get_outbound_envelope(uint64_t id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, id);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "outbound_envelope", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_envelope(uint64_t id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envelopes"_n, id);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "envelope_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_attestation(uint64_t id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "attestation_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_msgch_tests)
BOOST_FIXTURE_TEST_CASE(deliver_invalid_request, sysio_msgch_tester) { try {
   opp::Envelope env;
   env.set_epoch_envelope_index(1);
   env.set_epoch_timestamp(1775612516983);

   std::vector<char> data(env.ByteSizeLong());
   env.SerializeToArray(data.data(), static_cast<int>(data.size()));
   BOOST_REQUIRE_EXCEPTION(
      deliver("batchop1"_n, 999, data),
      sysio_assert_message_exception,
      sysio_assert_message_is("epoch state not initialized")

   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(queueout_basic, sysio_msgch_tester) { try {
   // Chain_code is the chain's slug_name value. queueout requires a
   // registered chain row -- a pure registration/ops gate (the terminal
   // account estimator that once lived behind it is deleted): an unregistered
   // code would otherwise create a READY row no epoch fan-out ever drains.
   const uint64_t chain_code = fc::slug_name{"ETH"}.value;
   BOOST_REQUIRE_EQUAL(success(), regchain(opp::types::CHAIN_KIND_EVM, "ETH", 1));
   BOOST_REQUIRE_EQUAL(success(), queueout(chain_code, opp::types::ATTESTATION_TYPE_OPERATORS));

   // `mint_att_id`'s first call returns id=1 — the `attseq` singleton at
   // pk=0 is the sequence row itself, so attestation ids start at 1 to
   // avoid collision with it. See `sysio.msgch.cpp::mint_att_id` docstring.
   auto attest = get_attestation(1);
   BOOST_REQUIRE(!attest.is_null());
   BOOST_REQUIRE_EQUAL(chain_code, attest["chain_code"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(buildenv_basic, sysio_msgch_tester) { try {
   // buildenv looks up the chain row in sysio.chains before doing any
   // packing work; without a registered chain it would fail with "key not
   // found". The empty-queue early-return happens before that lookup, so
   // an unregistered chain_code still returns success when there are no
   // candidate attestations. This test pins THAT invariant.
   const uint64_t chain_code = fc::slug_name{"ETH"}.value;
   BOOST_REQUIRE_EQUAL(success(), buildenv(chain_code));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
//  envelope_log tests — exercises the audit-trail row + cap-and-evict
//  behaviour of `buildenv`. The fixture also deploys `sysio.epoch` so we
//  can register an outpost and the `write_envelope_log` helper can derive
//  `active_outposts × 2 × cfg.epoch_retention_envelope_log_count`.
// ---------------------------------------------------------------------------
class sysio_msgch_envlog_tester : public tester {
public:
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT  = "sysio.chalg"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;

   sysio_msgch_envlog_tester() {
      produce_blocks(2);
      create_accounts({ MSGCH_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT, CHAINS_ACCOUNT });
      produce_blocks(2);

      set_code(MSGCH_ACCOUNT, contracts::msgch_wasm());
      set_abi (MSGCH_ACCOUNT, contracts::msgch_abi().data());
      set_privileged(MSGCH_ACCOUNT);

      set_code(EPOCH_ACCOUNT, contracts::epoch_wasm());
      set_abi (EPOCH_ACCOUNT, contracts::epoch_abi().data());
      set_privileged(EPOCH_ACCOUNT);

      set_code(CHAINS_ACCOUNT, contracts::chains_wasm());
      set_abi (CHAINS_ACCOUNT, contracts::chains_abi().data());
      set_privileged(CHAINS_ACCOUNT);

      produce_blocks();

      const auto* msgch_accnt = control->find_account_metadata(MSGCH_ACCOUNT);
      BOOST_REQUIRE(msgch_accnt != nullptr);
      abi_def msgch_abi_;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(msgch_accnt->abi, msgch_abi_), true);
      msgch_abi.set_abi(std::move(msgch_abi_),
                        abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action(name account, name signer, name action_name, const variant_object& data) {
      try {
         base_tester::push_action(account, action_name, signer, data);
         return success();
      } catch (const fc::exception& e) {
         return error(e.top_message());
      }
   }

   /// Bring `epoch_config` to a known retention value and register `n`
   /// outposts so the `write_envelope_log` cap derivation has a stable
   /// `active_outposts` to read.
   void bootstrap_epoch_config(uint32_t retention_count) {
      // setconfig: allow any group/operator-count combination; we don't
      // exercise group rotation here, just the outpost roster.
      BOOST_REQUIRE_EQUAL(success(),
         push_action(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "setconfig"_n, mvo()
            ("epoch_duration_sec", 60)
            ("operators_per_epoch", 1)
            ("batch_operator_minimum_active", 3)
            ("batch_op_groups", 3)
            ("epoch_retention_envelope_log_count", retention_count)
         ));
   }

   /// Replacement for `regoutpost` — register a chain row in sysio.chains.
   /// The slug_name `code` carries the per-chain identity that used to come
   /// from `ChainKind`. Tests pass a deterministic spelling derived from the
   /// `kind` so successive callers don't collide.
   void register_outpost(opp::types::ChainKind kind, uint32_t chain_id) {
      const char* code = "ETH";
      if (kind == opp::types::CHAIN_KIND_SVM) code = "SOL";
      else if (kind == opp::types::CHAIN_KIND_EVM) code = "ETH";
      BOOST_REQUIRE_EQUAL(success(),
         push_action(CHAINS_ACCOUNT, CHAINS_ACCOUNT, "regchain"_n, mvo()
            ("kind",              kind)
            ("code",              code)
            ("external_chain_id", chain_id)
            ("name",              std::string("outpost"))
            ("description",       std::string{})
            ("outpost", sysio_system::test_support::no_outpost_mvo())
         ));
   }

   action_result queueout(uint64_t chain_code, uint32_t attest_type) {
      return push_action(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("chain_code",   chain_code)
         ("attest_type",  attest_type)
         ("data",         std::vector<char>{0x01, 0x02, 0x03})
      );
   }

   /// Variable-payload `queueout` for envelope-cap tests. Pattern matches
   /// the production attestation flow but lets the test author dial in the
   /// exact per-attestation size needed to drive the packing loop across
   /// the `MAX_ENVELOPE_BYTES` boundary.
   action_result queueout_with_data(uint64_t chain_code,
                                    uint32_t attest_type,
                                    std::vector<char> data) {
      return push_action(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("chain_code",   chain_code)
         ("attest_type",  attest_type)
         ("data",         std::move(data))
      );
   }

   /// Count READY-status attestations for `chain_code` by probing the
   /// table by-id. Avoids needing an ABI binding for the secondary index.
   uint32_t count_ready_attestations(uint64_t chain_code, uint64_t scan_until) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            "attestation_entry", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["chain_code"].as_uint64() != chain_code) continue;
         // status == READY (matches AttestationStatus::ATTESTATION_STATUS_READY,
         // the value the contract emits for queued-but-not-yet-bundled rows).
         if (row["status"].as<sysio::opp::types::AttestationStatus>() ==
             sysio::opp::types::AttestationStatus::ATTESTATION_STATUS_READY) ++n;
      }
      return n;
   }

   action_result buildenv(uint64_t chain_code) {
      return push_action(MSGCH_ACCOUNT, EPOCH_ACCOUNT, "buildenv"_n, mvo()
         ("chain_code", chain_code)
      );
   }

   /// Return the first outbound envelope row found in a small id scan.
   fc::variant find_outbound_envelope(uint64_t scan_until = 16) {
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, id);
         if (data.empty()) continue;
         return msgch_abi.binary_to_variant(
            "outbound_envelope", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      return fc::variant{};
   }

   /// Push `buildenv` for `chain_code` and return what it printed.
   std::string buildenv_console(uint64_t chain_code) {
      const auto trace = base_tester::push_action(MSGCH_ACCOUNT, "buildenv"_n, EPOCH_ACCOUNT,
                                                  mvo()("chain_code", chain_code));
      std::string console;
      for (const auto& action_trace : trace->action_traces) console += action_trace.console;
      return console;
   }

   /// Whether attestation `id` is still stored.
   bool attestation_stored(uint64_t id) {
      return !get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id).empty();
   }

   /// The stored `outenvelopes` row for `chain_code` (the table keeps one per outpost), or null.
   fc::variant outbound_envelope_row(uint64_t chain_code, uint64_t scan_until = 32) {
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

   /// The raw envelope stored for `chain_code`; fails the test when there is none.
   std::vector<char> outbound_envelope_bytes(uint64_t chain_code) {
      const auto row = outbound_envelope_row(chain_code);
      BOOST_REQUIRE(!row.is_null());
      return row["raw_envelope"].as<std::vector<char>>();
   }

   /// The envelope stored for `chain_code`, decoded; `buildenv` always wraps one message.
   opp::Envelope outbound_envelope_for(uint64_t chain_code) {
      const auto raw = outbound_envelope_bytes(chain_code);
      opp::Envelope env;
      BOOST_REQUIRE(env.ParseFromArray(raw.data(), static_cast<int>(raw.size())));
      BOOST_REQUIRE_EQUAL(env.messages_size(), 1);
      return env;
   }

   /// The first byte of every attestation the envelope stored for `chain_code` carries, in order.
   /// The packing tests fill each payload with its own tag byte.
   std::string shipped_tags(uint64_t chain_code) {
      const auto  env = outbound_envelope_for(chain_code);
      std::string tags;
      for (const auto& att : env.messages(0).payload().attestations()) {
         BOOST_REQUIRE(!att.data().empty());
         tags.push_back(att.data().front());
      }
      return tags;
   }

   /// Count populated `envlog` rows in the id range `[0, max_id_exclusive)`.
   /// Cheap enough for the test scales here (≤ a few thousand probes).
   uint32_t envlog_row_count_until(uint64_t max_id_exclusive) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < max_id_exclusive; ++id) {
         if (!get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envlog"_n, id).empty()) ++n;
      }
      return n;
   }

   abi_serializer msgch_abi;
};

BOOST_AUTO_TEST_SUITE(sysio_msgch_envlog_tests)

namespace {

using fc::slug_name_literals::operator""_s;

/// `chain_code` is the chain's slug_name value (uint64). All envlog
/// tests register one EVM-class chain via `register_outpost(...)` which uses
/// the spelling `"ETH"`. ETH_OUTPOST_ID is the slug_name's packed value.
constexpr uint64_t ETH_OUTPOST_ID = "ETH"_s.value;

/// Solana/SVM test outpost registered by `register_outpost(CHAIN_KIND_SVM, ...)`.
constexpr uint64_t SOL_OUTPOST_ID = "SOL"_s.value;

constexpr auto EVM_TEST_ATTESTATION_TYPE       = opp::types::ATTESTATION_TYPE_OPERATORS;
constexpr auto SWAP_REMIT_ATTESTATION_TYPE    = opp::types::ATTESTATION_TYPE_SWAP_REMIT;

/// Mirrors of the packing limits in `sysio.msgch.hpp` (contract headers are not host-compilable).
constexpr size_t MAX_ENVELOPE_BYTES         = 32'768;
constexpr size_t ENVELOPE_BASELINE_BYTES    = 512;
constexpr size_t ATTESTATION_OVERHEAD_BYTES = 24;
constexpr size_t SCHEDULE_LANE_BUDGET_BYTES = MAX_ENVELOPE_BYTES / 4 * 3;
/// The largest schedule-lane `data` that can ship; one byte more fits no envelope and is dropped.
constexpr size_t MAX_ATTESTATION_DATA_BYTES =
   MAX_ENVELOPE_BYTES - ENVELOPE_BASELINE_BYTES - ATTESTATION_OVERHEAD_BYTES;
/// The largest other-lane `data` that can ship; one byte more fits no envelope beside a full schedule lane.
constexpr size_t MAX_OTHER_ATTESTATION_DATA_BYTES =
   MAX_ENVELOPE_BYTES - ENVELOPE_BASELINE_BYTES - SCHEDULE_LANE_BUDGET_BYTES - ATTESTATION_OVERHEAD_BYTES;

} // anonymous namespace

/// Smoke: queueout + buildenv writes one row to `envlog` with the
/// expected `endpoints` (WIRE → outpost) and survives the post-buildenv
/// cleanup of consumed attestations.
BOOST_FIXTURE_TEST_CASE(buildenv_writes_envlog_row, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), queueout(/*chain_code=*/ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));
   BOOST_REQUIRE_EQUAL(success(), buildenv(/*chain_code=*/ETH_OUTPOST_ID));
   produce_blocks();

   // envlog ids start at 1 (write_envelope_log uses
   // `std::max<uint64_t>(1, tbl.available_primary_key())`).
   auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envlog"_n, 1);
   BOOST_REQUIRE(!data.empty());
   auto row = msgch_abi.binary_to_variant(
      "envelope_log_entry", data,
      abi_serializer::create_yield_function(abi_serializer_max_time));
   BOOST_REQUIRE_EQUAL(1u, row["id"].as_uint64());
   // start = WIRE/1, end = ETH/31337. ABI serializer reflects the
   // ChainKind enum back as its symbolic name; the `chain_id` field is a
   // `vuint32_t` and surfaces as `{"value": N}`.
   BOOST_REQUIRE(opp::types::CHAIN_KIND_WIRE ==
                 row["endpoints"]["start"]["kind"].as<opp::types::ChainKind>());
   BOOST_REQUIRE_EQUAL(1u, row["endpoints"]["start"]["id"]["value"].as_uint64());
   BOOST_REQUIRE(opp::types::CHAIN_KIND_EVM ==
                 row["endpoints"]["end"]["kind"].as<opp::types::ChainKind>());
   BOOST_REQUIRE_EQUAL(31337u, row["endpoints"]["end"]["id"]["value"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// Eviction at the boundary. Set `retention=2` and one outpost →
/// `cap = 1*2*2 = 4`. After 5 buildenv rounds (5 rows inserted), the
/// oldest full epoch (`per_epoch = 1*2 = 2` rows) gets evicted; final
/// row count is 4.
BOOST_FIXTURE_TEST_CASE(envlog_evicts_oldest_epoch_on_overflow, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/2);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   // Drive 5 queueout+buildenv rounds → 5 envlog rows inserted, last
   // overflow triggers a 2-row head drop. Final survivors: ids 2,3,4
   // (or higher set, depending on cap arithmetic). cap = 1*2*2 = 4 →
   // when live_count = 5 (after 5th insert) the helper drops 2 rows.
   for (uint32_t i = 0; i < 5; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));
      BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
      produce_blocks();
   }

   // Count surviving rows in [0..5].
   uint32_t alive = 0;
   uint64_t oldest_alive_id = std::numeric_limits<uint64_t>::max();
   for (uint64_t id = 0; id < 10; ++id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envlog"_n, id);
      if (data.empty()) continue;
      ++alive;
      if (id < oldest_alive_id) oldest_alive_id = id;
   }
   // After 5 inserts (ids 1..5) and a 2-row head eviction (on the 5th
   // insert when live_count crossed cap=4), 3 rows remain: ids 3, 4, 5.
   BOOST_REQUIRE_EQUAL(3u, alive);
   BOOST_REQUIRE_EQUAL(3u, oldest_alive_id);
} FC_LOG_AND_RETHROW() }

/// Roster change updates the cap. Start with 1 outpost (cap = 1*2*2 =
/// 4). Drive 4 rounds → 4 rows. Register a second outpost (cap now
/// 2*2*2 = 8). Drive 4 more rounds → 8 rows. No eviction yet because
/// each round only writes for outpost 0; the second outpost was added
/// but never received traffic. The cap math reads the current
/// outposts table size on every write.
BOOST_FIXTURE_TEST_CASE(envlog_cap_tracks_outpost_count, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/2);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   for (uint32_t i = 0; i < 4; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));
      BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
      produce_blocks();
   }
   // After 4 rounds with cap=4, no eviction yet.
   uint32_t alive = 0;
   for (uint64_t id = 0; id < 10; ++id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envlog"_n, id);
      if (!data.empty()) ++alive;
   }
   BOOST_REQUIRE_EQUAL(4u, alive);

   // Add a second outpost — cap doubles to 8.
   register_outpost(opp::types::CHAIN_KIND_SVM, 0);
   produce_blocks();

   // Three more rounds on outpost 0 → 7 rows total, still under cap=8.
   for (uint32_t i = 0; i < 3; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));
      BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
      produce_blocks();
   }
   alive = 0;
   for (uint64_t id = 0; id < 10; ++id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envlog"_n, id);
      if (!data.empty()) ++alive;
   }
   BOOST_REQUIRE_EQUAL(7u, alive);
} FC_LOG_AND_RETHROW() }

/// Existing `outenvelopes` row gets dropped on the next `buildenv` for
/// the same outpost — one-deep retention (the batch op only ever reads
/// the most-recent emit).
BOOST_FIXTURE_TEST_CASE(buildenv_drops_previous_outenvelopes, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   // outenvelopes ids start at 1 (same `std::max<uint64_t>(1, ...)` pattern
   // as envlog / attestations).
   BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   auto first = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, 1);
   BOOST_REQUIRE(!first.empty());

   BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();

   // First row is now gone (replaced by the second emit).
   first = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, 1);
   BOOST_REQUIRE(first.empty());
   auto second = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, 2);
   BOOST_REQUIRE(!second.empty());
} FC_LOG_AND_RETHROW() }

/// `attestations` PROCESSED rows for a given outpost are dropped at the
/// end of `buildenv`. The first round's row is gone after buildenv, and
/// the second round's queueout populates a fresh row that's still
/// present pre-buildenv.
BOOST_FIXTURE_TEST_CASE(buildenv_drops_processed_attestations, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));   // id 0, READY
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));          // → PROCESSED → erased
   produce_blocks();
   auto a0 = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, 0);
   BOOST_REQUIRE(a0.empty());

   BOOST_REQUIRE_EQUAL(success(), queueout(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE));   // id 1 (or next), READY
   produce_blocks();
   // Find the row at any id in [0..10) — `available_primary_key()` may
   // resume at 1 after a delete, but the precise value is an
   // implementation detail.
   bool found = false;
   for (uint64_t id = 0; id < 10; ++id) {
      if (!get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id).empty()) {
         found = true;
         break;
      }
   }
   BOOST_REQUIRE(found);
} FC_LOG_AND_RETHROW() }

/// Packing-loop coverage for `MAX_ENVELOPE_BYTES = 32 768`. Queue 6
/// attestations of 8 KiB each (cumulative ≈ 48 KiB, ~50 % over the cap),
/// call `buildenv`, and assert:
///   * the emitted `outenvelopes` row is ≤ 32 768 bytes (cross-chain cap),
///   * a non-trivial subset of READY attestations remained queued for the
///     next epoch (the un-included tail),
///   * a second `buildenv` drains the remainder under the same cap.
/// This is the §6c boundary test that turns "analytical max" into
/// "demonstrated max" on the WIRE side.
BOOST_FIXTURE_TEST_CASE(buildenv_packs_until_cap_then_leaves_remainder,
                        sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   // 8 KiB of payload per attestation. With ENVELOPE_BASELINE_BYTES = 512
   // and ATTESTATION_OVERHEAD_BYTES = 24 in the contract's estimator,
   // each entry costs 24 + 8192 = 8216 bytes; (32 768 − 512) / 8216 ≈ 3.9
   // → 3 entries fit, 3 stay queued. Exact fit count depends on the
   // estimator's conservative margin; the assertions below check the
   // invariants ("≤ cap", "some remainder", "drains on next emit") rather
   // than a hardcoded fit count, keeping the test robust to small
   // estimator tweaks. The queued total is sized so exactly two emits drain
   // it at the 32 KiB cap — invariant 3 requires an empty queue after emit#2.
   constexpr size_t  PER_ATTEST_BYTES   = 8 * 1024;
   constexpr uint32_t TOTAL_ATTESTATIONS = 6;
   constexpr size_t  MAX_ENV_BYTES      = 32'768;

   // Vary one byte per attestation so each tx has a distinct payload —
   // identical transactions in the same block collapse to a single
   // dedup'd tx, which would silently halve our queue depth.
   for (uint32_t i = 0; i < TOTAL_ATTESTATIONS; ++i) {
      std::vector<char> payload(PER_ATTEST_BYTES, 0x42);
      payload[0] = static_cast<char>(i);
      BOOST_REQUIRE_EQUAL(success(),
         queueout_with_data(/*chain_code=*/ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE, payload));
   }
   produce_blocks();

   // First emit: packs as many as fit, drops the rest in queue.
   BOOST_REQUIRE_EQUAL(success(), buildenv(/*chain_code=*/ETH_OUTPOST_ID));
   produce_blocks();

   // The most recent emit lives at one of the early ids; the one-deep
   // retention sweep means at most one row exists per outpost. Find it.
   fc::variant emitted_row;
   uint64_t    emitted_id = std::numeric_limits<uint64_t>::max();
   for (uint64_t id = 0; id < 16; ++id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, id);
      if (data.empty()) continue;
      emitted_row = msgch_abi.binary_to_variant(
         "outbound_envelope", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      emitted_id = id;
      break;
   }
   BOOST_REQUIRE(!emitted_row.is_null());

   // ── Invariant 1: emitted envelope is at or under the cross-chain cap.
   const auto& raw = emitted_row["raw_envelope"].as<std::vector<char>>();
   BOOST_TEST_MESSAGE("emit#1 envelope size = " << raw.size() << " bytes");
   BOOST_REQUIRE_LE(raw.size(), MAX_ENV_BYTES);

   // ── Invariant 2: NOT every attestation made it into this envelope —
   //    the packing loop genuinely dropped some onto the next epoch.
   //    `count_ready_attestations` counts un-emitted (still READY) rows.
   uint32_t still_ready = count_ready_attestations(/*chain_code=*/ETH_OUTPOST_ID,
                                                   /*scan_until=*/TOTAL_ATTESTATIONS + 4);
   BOOST_TEST_MESSAGE("emit#1 leftover READY = " << still_ready);
   BOOST_REQUIRE_GT(still_ready, 0u);
   BOOST_REQUIRE_LT(still_ready, TOTAL_ATTESTATIONS);

   // ── Invariant 3: a follow-up emit drains the remainder under the same
   //    cap. After this emit, no READY attestations should remain queued.
   BOOST_REQUIRE_EQUAL(success(), buildenv(/*chain_code=*/ETH_OUTPOST_ID));
   produce_blocks();

   // Find the new emitted row (one-deep retention dropped the prior one).
   fc::variant emitted_row_2;
   for (uint64_t id = 0; id < 16; ++id) {
      if (id == emitted_id) continue;        // prior row was evicted
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, id);
      if (data.empty()) continue;
      emitted_row_2 = msgch_abi.binary_to_variant(
         "outbound_envelope", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      break;
   }
   BOOST_REQUIRE(!emitted_row_2.is_null());
   const auto& raw2 = emitted_row_2["raw_envelope"].as<std::vector<char>>();
   BOOST_TEST_MESSAGE("emit#2 envelope size = " << raw2.size() << " bytes");
   BOOST_REQUIRE_LE(raw2.size(), MAX_ENV_BYTES);

   uint32_t still_ready_after_emit2 =
      count_ready_attestations(/*chain_code=*/ETH_OUTPOST_ID, /*scan_until=*/TOTAL_ATTESTATIONS + 4);
   BOOST_REQUIRE_EQUAL(still_ready_after_emit2, 0u);
} FC_LOG_AND_RETHROW() }

/// `buildenv` walks only its own outpost's READY queue, oldest first, and stops at the first row that
/// does not fit. Rows behind that point are not reached, not even one its lane could never ship, and the
/// other outpost's rows, oversized or not, are neither packed nor dropped.
BOOST_FIXTURE_TEST_CASE(buildenv_packs_one_outpost_queue_in_order_up_to_the_budget,
                        sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   register_outpost(opp::types::CHAIN_KIND_SVM, 0);
   produce_blocks();

   // Four of these fit one envelope and a fifth does not; each is within the other lane's bound.
   constexpr size_t QUARTER_OF_AN_ENVELOPE = 7'000;
   const auto queue = [&](uint64_t chain_code, char tag, size_t size) {
      BOOST_REQUIRE_EQUAL(success(), queueout_with_data(chain_code, SWAP_REMIT_ATTESTATION_TYPE,
                                                        std::vector<char>(size, tag)));
   };
   // Attestation ids run 1..8 in queue order, the two outposts interleaved.
   queue(SOL_OUTPOST_ID, 'S', MAX_OTHER_ATTESTATION_DATA_BYTES + 1);   // 1
   queue(ETH_OUTPOST_ID, 'a', QUARTER_OF_AN_ENVELOPE);                 // 2
   queue(SOL_OUTPOST_ID, 's', 1);                                      // 3
   queue(ETH_OUTPOST_ID, 'b', QUARTER_OF_AN_ENVELOPE);                 // 4
   queue(ETH_OUTPOST_ID, 'c', QUARTER_OF_AN_ENVELOPE);                 // 5
   queue(ETH_OUTPOST_ID, 'd', QUARTER_OF_AN_ENVELOPE);                 // 6
   queue(ETH_OUTPOST_ID, 'e', QUARTER_OF_AN_ENVELOPE);                 // 7: does not fit behind a to d
   queue(ETH_OUTPOST_ID, 'X', MAX_OTHER_ATTESTATION_DATA_BYTES + 1);   // 8: behind the stop point
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL("abcd", shipped_tags(ETH_OUTPOST_ID));
   BOOST_REQUIRE(!attestation_stored(2) && !attestation_stored(4) && !attestation_stored(5) && !attestation_stored(6));
   BOOST_REQUIRE(attestation_stored(7));
   BOOST_REQUIRE_MESSAGE(attestation_stored(8), "the walk went past the first row that did not fit");
   BOOST_REQUIRE_MESSAGE(attestation_stored(1) && attestation_stored(3), "the walk touched another outpost's queue");
   BOOST_REQUIRE(outbound_envelope_row(SOL_OUTPOST_ID).is_null());

   // The next envelope ships e, and the walk now reaches X and drops it.
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL("e", shipped_tags(ETH_OUTPOST_ID));
   BOOST_REQUIRE(!attestation_stored(7) && !attestation_stored(8));

   // SOL's own walk drops its oversized head and ships the row behind it in the same envelope.
   BOOST_REQUIRE_EQUAL(success(), buildenv(SOL_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL("s", shipped_tags(SOL_OUTPOST_ID));
   BOOST_REQUIRE(!attestation_stored(1) && !attestation_stored(3));
} FC_LOG_AND_RETHROW() }

/// Each lane's drop bound is exact. A schedule row that just fits an otherwise empty envelope ships alone
/// within the cap. An other-lane row at its bound, the most that fits beside a full schedule lane, ships;
/// one byte over is dropped with a diagnostic, and the row queued behind it ships in the same envelope.
BOOST_FIXTURE_TEST_CASE(buildenv_drops_only_what_its_lane_can_never_ship, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   const auto queue = [&](opp::types::AttestationType type, char tag, size_t size) {
      BOOST_REQUIRE_EQUAL(success(), queueout_with_data(ETH_OUTPOST_ID, type, std::vector<char>(size, tag)));
   };
   queue(EVM_TEST_ATTESTATION_TYPE, 'a', MAX_ATTESTATION_DATA_BYTES);                // 1: schedule, fills it alone
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'b', MAX_OTHER_ATTESTATION_DATA_BYTES);        // 2: other, at its bound
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'x', MAX_OTHER_ATTESTATION_DATA_BYTES + 1);    // 3: other, one byte over
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'c', 1);                                       // 4
   produce_blocks();

   const auto first = buildenv_console(ETH_OUTPOST_ID);
   produce_blocks();
   BOOST_REQUIRE_MESSAGE(first.find("DROP") == std::string::npos, first);
   BOOST_REQUIRE_EQUAL("a", shipped_tags(ETH_OUTPOST_ID));
   BOOST_REQUIRE_LE(outbound_envelope_bytes(ETH_OUTPOST_ID).size(), MAX_ENVELOPE_BYTES);
   BOOST_REQUIRE(attestation_stored(2) && attestation_stored(3) && attestation_stored(4));

   const auto second = buildenv_console(ETH_OUTPOST_ID);
   produce_blocks();
   BOOST_REQUIRE_MESSAGE(second.find("DROP attestation 3 (") != std::string::npos, second);
   BOOST_REQUIRE_EQUAL("bc", shipped_tags(ETH_OUTPOST_ID));
   BOOST_REQUIRE(!attestation_stored(2) && !attestation_stored(3) && !attestation_stored(4));
} FC_LOG_AND_RETHROW() }

/// The lane bounds leave no gap. A schedule whose estimate is its whole budget and an other-lane row at its
/// bound together estimate exactly `MAX_ENVELOPE_BYTES`, and both ship in one envelope within the cap: nothing
/// is dropped, and the trim loop does not pop the other row. A single schedule row leaves the estimate the
/// least slack over the real encoding, and an earlier emit gives the envelope both chain links.
BOOST_FIXTURE_TEST_CASE(buildenv_ships_the_largest_other_row_beside_a_full_schedule, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   const auto queue = [&](opp::types::AttestationType type, char tag, size_t size) {
      BOOST_REQUIRE_EQUAL(success(), queueout_with_data(ETH_OUTPOST_ID, type, std::vector<char>(size, tag)));
   };
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'p', 1);
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();

   queue(EVM_TEST_ATTESTATION_TYPE, 'a', SCHEDULE_LANE_BUDGET_BYTES - ATTESTATION_OVERHEAD_BYTES);
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'b', MAX_OTHER_ATTESTATION_DATA_BYTES);
   produce_blocks();

   const auto console = buildenv_console(ETH_OUTPOST_ID);
   produce_blocks();
   BOOST_REQUIRE_MESSAGE(console.find("DROP") == std::string::npos, console);
   BOOST_REQUIRE_EQUAL("ab", shipped_tags(ETH_OUTPOST_ID));
   const auto env = outbound_envelope_for(ETH_OUTPOST_ID);
   BOOST_REQUIRE(!env.previous_envelope_hash().empty() && !env.messages(0).header().previous_message_id().empty());
   const size_t envelope_bytes = outbound_envelope_bytes(ETH_OUTPOST_ID).size();
   BOOST_TEST_MESSAGE("a full schedule beside the largest other row encodes to " << envelope_bytes << " bytes");
   BOOST_REQUIRE_LE(envelope_bytes, MAX_ENVELOPE_BYTES);
} FC_LOG_AND_RETHROW() }

/// A queue emptied by drops still emits the epoch's envelope, carrying no attestations, because each
/// outpost answers the depot's envelope with its own; an empty queue emits none.
BOOST_FIXTURE_TEST_CASE(buildenv_emits_an_empty_envelope_when_it_drops_the_whole_queue,
                        sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE(outbound_envelope_row(ETH_OUTPOST_ID).is_null());

   BOOST_REQUIRE_EQUAL(success(), queueout_with_data(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE,
                                                     std::vector<char>(MAX_ATTESTATION_DATA_BYTES + 1, 'x')));
   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE(!attestation_stored(1));
   BOOST_REQUIRE_EQUAL(0, outbound_envelope_for(ETH_OUTPOST_ID).messages(0).payload().attestations_size());
} FC_LOG_AND_RETHROW() }

/// The operator schedule packs ahead of other traffic whatever the queue order, and each lane keeps its own
/// queue order, so OPERATORS stays ahead of the BATCH_OPERATOR_GROUPS queued after it.
BOOST_FIXTURE_TEST_CASE(buildenv_packs_the_operator_schedule_first, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   const auto queue = [&](opp::types::AttestationType type, char tag) {
      BOOST_REQUIRE_EQUAL(success(), queueout_with_data(ETH_OUTPOST_ID, type, std::vector<char>{tag}));
   };
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'a');
   queue(opp::types::ATTESTATION_TYPE_OPERATORS, 'o');
   queue(SWAP_REMIT_ATTESTATION_TYPE, 'b');
   queue(opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS, 'g');
   queue(opp::types::ATTESTATION_TYPE_OPERATORS, 'p');
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), buildenv(ETH_OUTPOST_ID));
   produce_blocks();
   BOOST_REQUIRE_EQUAL("ogpab", shipped_tags(ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

/// A schedule over its budget still ships, since each row fits an envelope, but `buildenv` reports it: it can
/// hold other-lane rows back. A schedule exactly at the budget is not reported.
BOOST_FIXTURE_TEST_CASE(buildenv_reports_a_schedule_over_its_budget, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_EVM, 31337);
   produce_blocks();

   constexpr std::string_view over_budget = "over its 24576-byte budget";
   const auto queue = [&](char tag, size_t size) {
      BOOST_REQUIRE_EQUAL(success(), queueout_with_data(ETH_OUTPOST_ID, EVM_TEST_ATTESTATION_TYPE,
                                                        std::vector<char>(size, tag)));
   };

   queue('a', SCHEDULE_LANE_BUDGET_BYTES - ATTESTATION_OVERHEAD_BYTES);
   produce_blocks();
   const auto at_budget = buildenv_console(ETH_OUTPOST_ID);
   produce_blocks();
   BOOST_REQUIRE_MESSAGE(at_budget.find(over_budget) == std::string::npos, at_budget);
   BOOST_REQUIRE_EQUAL("a", shipped_tags(ETH_OUTPOST_ID));

   queue('b', SCHEDULE_LANE_BUDGET_BYTES - ATTESTATION_OVERHEAD_BYTES + 1);
   produce_blocks();
   const auto past_budget = buildenv_console(ETH_OUTPOST_ID);
   produce_blocks();
   BOOST_REQUIRE_MESSAGE(past_budget.find(over_budget) != std::string::npos, past_budget);
   BOOST_REQUIRE_EQUAL("b", shipped_tags(ETH_OUTPOST_ID));
} FC_LOG_AND_RETHROW() }

// queueout carries no ABI-level auth. Without the depot-contract gate, any account could call it
// directly and inject a forged READY attestation that buildenv() then packs into the depot's
// group-signed outbound envelope (a forged SWAP_REMIT / WITHDRAW_REMIT / SLASH the outpost executes).
// A direct call from a non-depot account must revert at the auth gate, which is the first statement
// in queueout (before any state read), so this holds regardless of epoch-state setup. The authorized
// path (msgch self / epoch / opreg / uwrit / reserv) is covered by the queueout()/buildenv() helpers
// used throughout this suite, which sign as the depot and succeed.
BOOST_FIXTURE_TEST_CASE(queueout_rejects_unauthorized_caller, sysio_msgch_tester) { try {
   std::vector<char> forged(8, 0x7f);
   BOOST_REQUIRE_EXCEPTION(
      push_msgch_action("batchop1"_n, "queueout"_n,
                        {{ "batchop1"_n, config::active_name }},
                        mvo()("chain_code", 999)("attest_type", (uint16_t)7)("data", forged)),
      sysio_assert_message_exception,
      sysio_assert_message_is("queueout: caller not authorized to queue outbound attestations"));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
