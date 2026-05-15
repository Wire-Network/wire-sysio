#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include <sysio/chain/action.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;

using mvo = fc::mutable_variant_object;

class sysio_msgch_tester : public tester {
public:
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

   sysio_msgch_tester() {
      produce_blocks(2);

      create_accounts({
         MSGCH_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT,
         "batchop1"_n, "batchop2"_n, "batchop3"_n,
         "batchop4"_n, "batchop5"_n, "batchop.a"_n,
         "batchop.b"_n
      });
      produce_blocks(2);

      set_code(MSGCH_ACCOUNT, contracts::msgch_wasm());
      set_abi(MSGCH_ACCOUNT, contracts::msgch_abi().data());
      set_privileged(MSGCH_ACCOUNT);

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

   action_result deliver(name op, uint64_t outpost_id,
                         std::vector<char> data = {}) {
      return push_msgch_action(op, "deliver"_n, mvo()
         ("batch_op_name", op)
         ("outpost_id", outpost_id)
         ("data", data)
      );
   }

   action_result evalcons(uint64_t req_id) {
      return push_msgch_action(MSGCH_ACCOUNT, "evalcons"_n, mvo()
         ("req_id", req_id)
      );
   }

   action_result queueout(uint64_t outpost_id, uint16_t attest_type, std::vector<char> data = {}) {
      return push_msgch_action(MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("outpost_id", outpost_id)
         ("attest_type", attest_type)
         ("data", data)
      );
   }

   action_result buildenv(uint64_t outpost_id) {
      return push_msgch_action(MSGCH_ACCOUNT, "buildenv"_n, {{
         EPOCH_ACCOUNT, config::active_name
      }}, mvo()
         ("outpost_id", outpost_id)
      );
   }

   fc::sha256 make_hash(const std::string& seed) {
      return fc::sha256::hash(seed);
   }

   // ── Table read helpers ──

   fc::variant get_message(uint64_t id) {
      auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "messages"_n, id);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "message_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

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
   // AttestationType: OPERATORS = 60947 — any in-range, non-removed enum value
   // suffices here; the test only verifies the queueout/table mechanics, not
   // any per-type dispatch logic.
   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60947));

   // Verify attestation written to table (first entry, id=0)
   auto attest = get_attestation(0);
   BOOST_REQUIRE(!attest.is_null());
   BOOST_REQUIRE_EQUAL(0, attest["outpost_id"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(buildenv_basic, sysio_msgch_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), buildenv(0));
   // buildenv with no queued messages produces an empty envelope — no row written
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
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

   sysio_msgch_envlog_tester() {
      produce_blocks(2);
      create_accounts({ MSGCH_ACCOUNT, EPOCH_ACCOUNT, CHALG_ACCOUNT });
      produce_blocks(2);

      set_code(MSGCH_ACCOUNT, contracts::msgch_wasm());
      set_abi (MSGCH_ACCOUNT, contracts::msgch_abi().data());
      set_privileged(MSGCH_ACCOUNT);

      set_code(EPOCH_ACCOUNT, contracts::epoch_wasm());
      set_abi (EPOCH_ACCOUNT, contracts::epoch_abi().data());
      set_privileged(EPOCH_ACCOUNT);

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

   void register_outpost(opp::types::ChainKind kind, uint32_t chain_id) {
      BOOST_REQUIRE_EQUAL(success(),
         push_action(EPOCH_ACCOUNT, EPOCH_ACCOUNT, "regoutpost"_n, mvo()
            ("chain_kind", static_cast<uint32_t>(kind))
            ("chain_id",   chain_id)
         ));
   }

   action_result queueout(uint64_t outpost_id, uint32_t attest_type) {
      return push_action(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("outpost_id",   outpost_id)
         ("attest_type",  attest_type)
         ("data",         std::vector<char>{0x01, 0x02, 0x03})
      );
   }

   /// Variable-payload `queueout` for envelope-cap tests. Pattern matches
   /// the production attestation flow but lets the test author dial in the
   /// exact per-attestation size needed to drive the packing loop across
   /// the `MAX_ENVELOPE_BYTES` boundary.
   action_result queueout_with_data(uint64_t outpost_id,
                                    uint32_t attest_type,
                                    std::vector<char> data) {
      return push_action(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "queueout"_n, mvo()
         ("outpost_id",   outpost_id)
         ("attest_type",  attest_type)
         ("data",         std::move(data))
      );
   }

   /// Count READY-status attestations for `outpost_id` by probing the
   /// table by-id. Avoids needing an ABI binding for the secondary index.
   uint32_t count_ready_attestations(uint64_t outpost_id, uint64_t scan_until) {
      uint32_t n = 0;
      for (uint64_t id = 0; id < scan_until; ++id) {
         auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id);
         if (data.empty()) continue;
         auto row = msgch_abi.binary_to_variant(
            "attestation_entry", data,
            abi_serializer::create_yield_function(abi_serializer_max_time));
         if (row["outpost_id"].as_uint64() != outpost_id) continue;
         // status == READY (matches AttestationStatus::ATTESTATION_STATUS_READY,
         // the value the contract emits for queued-but-not-yet-bundled rows).
         if (row["status"].as<sysio::opp::types::AttestationStatus>() ==
             sysio::opp::types::AttestationStatus::ATTESTATION_STATUS_READY) ++n;
      }
      return n;
   }

   action_result buildenv(uint64_t outpost_id) {
      return push_action(MSGCH_ACCOUNT, EPOCH_ACCOUNT, "buildenv"_n, mvo()
         ("outpost_id", outpost_id)
      );
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

/// Smoke: queueout + buildenv writes one row to `envlog` with the
/// expected `endpoints` (WIRE → outpost) and survives the post-buildenv
/// cleanup of consumed attestations.
BOOST_FIXTURE_TEST_CASE(buildenv_writes_envlog_row, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_ETHEREUM, 31337);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), queueout(/*outpost_id=*/0, /*type=*/60940));
   BOOST_REQUIRE_EQUAL(success(), buildenv(/*outpost_id=*/0));
   produce_blocks();

   auto data = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "envlog"_n, 0);
   BOOST_REQUIRE(!data.empty());
   auto row = msgch_abi.binary_to_variant(
      "envelope_log_entry", data,
      abi_serializer::create_yield_function(abi_serializer_max_time));
   BOOST_REQUIRE_EQUAL(0u, row["id"].as_uint64());
   // start = WIRE/1, end = ETH/31337. ABI serializer reflects the
   // ChainKind enum back as its symbolic name; the `chain_id` field is a
   // `vuint32_t` and surfaces as `{"value": N}`.
   BOOST_REQUIRE(opp::types::CHAIN_KIND_WIRE ==
                 row["endpoints"]["start"]["kind"].as<opp::types::ChainKind>());
   BOOST_REQUIRE_EQUAL(1u, row["endpoints"]["start"]["id"]["value"].as_uint64());
   BOOST_REQUIRE(opp::types::CHAIN_KIND_ETHEREUM ==
                 row["endpoints"]["end"]["kind"].as<opp::types::ChainKind>());
   BOOST_REQUIRE_EQUAL(31337u, row["endpoints"]["end"]["id"]["value"].as_uint64());
} FC_LOG_AND_RETHROW() }

/// Eviction at the boundary. Set `retention=2` and one outpost →
/// `cap = 1*2*2 = 4`. After 5 buildenv rounds (5 rows inserted), the
/// oldest full epoch (`per_epoch = 1*2 = 2` rows) gets evicted; final
/// row count is 4.
BOOST_FIXTURE_TEST_CASE(envlog_evicts_oldest_epoch_on_overflow, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/2);
   register_outpost(opp::types::CHAIN_KIND_ETHEREUM, 31337);
   produce_blocks();

   // Drive 5 queueout+buildenv rounds → 5 envlog rows inserted, last
   // overflow triggers a 2-row head drop. Final survivors: ids 2,3,4
   // (or higher set, depending on cap arithmetic). cap = 1*2*2 = 4 →
   // when live_count = 5 (after 5th insert) the helper drops 2 rows.
   for (uint32_t i = 0; i < 5; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
      BOOST_REQUIRE_EQUAL(success(), buildenv(0));
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
   // After 5 inserts and a 2-row head eviction (on the 5th insert when
   // live_count crossed cap=4), 3 rows remain.
   BOOST_REQUIRE_EQUAL(3u, alive);
   BOOST_REQUIRE_EQUAL(2u, oldest_alive_id);
} FC_LOG_AND_RETHROW() }

/// Roster change updates the cap. Start with 1 outpost (cap = 1*2*2 =
/// 4). Drive 4 rounds → 4 rows. Register a second outpost (cap now
/// 2*2*2 = 8). Drive 4 more rounds → 8 rows. No eviction yet because
/// each round only writes for outpost 0; the second outpost was added
/// but never received traffic. The cap math reads the current
/// outposts table size on every write.
BOOST_FIXTURE_TEST_CASE(envlog_cap_tracks_outpost_count, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/2);
   register_outpost(opp::types::CHAIN_KIND_ETHEREUM, 31337);
   produce_blocks();

   for (uint32_t i = 0; i < 4; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
      BOOST_REQUIRE_EQUAL(success(), buildenv(0));
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
   register_outpost(opp::types::CHAIN_KIND_SOLANA, 0);
   produce_blocks();

   // Three more rounds on outpost 0 → 7 rows total, still under cap=8.
   for (uint32_t i = 0; i < 3; ++i) {
      BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
      BOOST_REQUIRE_EQUAL(success(), buildenv(0));
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
   register_outpost(opp::types::CHAIN_KIND_ETHEREUM, 31337);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
   BOOST_REQUIRE_EQUAL(success(), buildenv(0));
   produce_blocks();
   auto first = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, 0);
   BOOST_REQUIRE(!first.empty());

   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));
   BOOST_REQUIRE_EQUAL(success(), buildenv(0));
   produce_blocks();

   // First row is now gone (replaced by the second emit).
   first = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, 0);
   BOOST_REQUIRE(first.empty());
   auto second = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "outenvelopes"_n, 1);
   BOOST_REQUIRE(!second.empty());
} FC_LOG_AND_RETHROW() }

/// `attestations` PROCESSED rows for a given outpost are dropped at the
/// end of `buildenv`. The first round's row is gone after buildenv, and
/// the second round's queueout populates a fresh row that's still
/// present pre-buildenv.
BOOST_FIXTURE_TEST_CASE(buildenv_drops_processed_attestations, sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_ETHEREUM, 31337);
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));   // id 0, READY
   BOOST_REQUIRE_EQUAL(success(), buildenv(0));          // → PROCESSED → erased
   produce_blocks();
   auto a0 = get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, 0);
   BOOST_REQUIRE(a0.empty());

   BOOST_REQUIRE_EQUAL(success(), queueout(0, 60940));   // id 1 (or next), READY
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

/// Packing-loop coverage for `MAX_ENVELOPE_BYTES = 65 536`. Queue 12
/// attestations of 8 KiB each (cumulative ≈ 96 KiB, ~50 % over the cap),
/// call `buildenv`, and assert:
///   * the emitted `outenvelopes` row is ≤ 65 536 bytes (cross-chain cap),
///   * a non-trivial subset of READY attestations remained queued for the
///     next epoch (the un-included tail),
///   * a second `buildenv` drains the remainder under the same cap.
/// This is the §6c boundary test that turns "analytical max" into
/// "demonstrated max" on the WIRE side.
BOOST_FIXTURE_TEST_CASE(buildenv_packs_until_cap_then_leaves_remainder,
                        sysio_msgch_envlog_tester) { try {
   bootstrap_epoch_config(/*retention=*/200);
   register_outpost(opp::types::CHAIN_KIND_ETHEREUM, 31337);
   produce_blocks();

   // 8 KiB of payload per attestation. With ENVELOPE_BASELINE_BYTES = 512
   // and ATTESTATION_OVERHEAD_BYTES = 24 in the contract's estimator,
   // each entry costs 24 + 8192 = 8216 bytes; (65 536 − 512) / 8216 ≈ 7.9
   // → 7 entries fit, 5 stay queued. Exact fit count depends on the
   // estimator's conservative margin; the assertions below check the
   // invariants ("≤ cap", "some remainder", "drains on next emit") rather
   // than a hardcoded fit count, keeping the test robust to small
   // estimator tweaks.
   constexpr size_t  PER_ATTEST_BYTES   = 8 * 1024;
   constexpr uint32_t TOTAL_ATTESTATIONS = 12;
   constexpr size_t  MAX_ENV_BYTES      = 65'536;

   // Vary one byte per attestation so each tx has a distinct payload —
   // identical transactions in the same block collapse to a single
   // dedup'd tx, which would silently halve our queue depth.
   for (uint32_t i = 0; i < TOTAL_ATTESTATIONS; ++i) {
      std::vector<char> payload(PER_ATTEST_BYTES, 0x42);
      payload[0] = static_cast<char>(i);
      BOOST_REQUIRE_EQUAL(success(),
         queueout_with_data(/*outpost_id=*/0, /*type=*/60940, payload));
   }
   produce_blocks();

   // First emit: packs as many as fit, drops the rest in queue.
   BOOST_REQUIRE_EQUAL(success(), buildenv(/*outpost_id=*/0));
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
   uint32_t still_ready = count_ready_attestations(/*outpost_id=*/0,
                                                   /*scan_until=*/TOTAL_ATTESTATIONS + 4);
   BOOST_TEST_MESSAGE("emit#1 leftover READY = " << still_ready);
   BOOST_REQUIRE_GT(still_ready, 0u);
   BOOST_REQUIRE_LT(still_ready, TOTAL_ATTESTATIONS);

   // ── Invariant 3: a follow-up emit drains the remainder under the same
   //    cap. After this emit, no READY attestations should remain queued.
   BOOST_REQUIRE_EQUAL(success(), buildenv(/*outpost_id=*/0));
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
      count_ready_attestations(/*outpost_id=*/0, /*scan_until=*/TOTAL_ATTESTATIONS + 4);
   BOOST_REQUIRE_EQUAL(still_ready_after_emit2, 0u);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
