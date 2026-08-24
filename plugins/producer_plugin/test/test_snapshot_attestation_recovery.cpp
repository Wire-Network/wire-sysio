#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/producer_plugin/snapshot_attestation_recovery.hpp>

#include <sysio/protocol/snapshot_attestation.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/contract_root_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/testing/tester.hpp>

#include <sysio.system_tester.hpp>

#include "snapshot_attestation_test_utils.hpp"

#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>

#include <algorithm>

namespace sysio {
namespace {

/// Disable the operator-facing ABI decoder budget to exercise the internal attestation-read floor.
constexpr auto disabled_abi_serializer_timeout_ms = "0";

/** Minimal tester that loads an existing snapshot into a fresh controller configuration. */
class snapshot_loaded_tester : public testing::base_tester {
public:
   snapshot_loaded_tester(chain::controller::config config,
                          const chain::snapshot_reader_ptr& snapshot) {
      init(std::move(config), snapshot);
   }

   testing::produce_block_result_t produce_block_ex(
      fc::microseconds skip_time, bool no_throw) override {
      return _produce_block(skip_time, false, no_throw);
   }

   chain::signed_block_ptr produce_block(fc::microseconds skip_time, bool no_throw) override {
      return produce_block_ex(skip_time, no_throw).block;
   }

   chain::signed_block_ptr produce_empty_block(fc::microseconds skip_time) override {
      control->abort_block();
      return _produce_block(skip_time, true);
   }

   chain::signed_block_ptr finish_block() override {
      return _finish_block();
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(snapshot_attestation_recovery_tests)

/** Persist, restore, and remove a populated recovery sidecar. */
BOOST_AUTO_TEST_CASE(round_trip_and_empty_state_cleanup) {
   fc::temp_directory temp;
   const auto state_path = temp.path() / "snapshot-provider-recovery.json";
   const auto expected = snapshot_attestation_test::make_recovery_state();

   save_snapshot_attestation_recovery_state(state_path, expected);
   const auto actual = load_snapshot_attestation_recovery_state(state_path);

   BOOST_REQUIRE(actual.pending_vote);
   BOOST_CHECK_EQUAL(expected.schema_version, actual.schema_version);
   BOOST_CHECK(expected.chain_id == actual.chain_id);
   BOOST_CHECK(expected.provider_account == actual.provider_account);
   BOOST_CHECK_EQUAL(expected.pending_vote_cursor, actual.pending_vote_cursor);
   BOOST_CHECK_EQUAL(expected.disagreement_detected, actual.disagreement_detected);
   BOOST_CHECK(expected.pending_vote->head_block_id == actual.pending_vote->head_block_id);
   BOOST_CHECK_EQUAL(expected.pending_vote->head_block_num, actual.pending_vote->head_block_num);
   BOOST_CHECK_EQUAL(expected.pending_vote->version, actual.pending_vote->version);
   BOOST_CHECK_EQUAL(expected.pending_vote->snapshot_name, actual.pending_vote->snapshot_name);
   BOOST_CHECK(expected.pending_vote->root_hash == actual.pending_vote->root_hash);

   save_snapshot_attestation_recovery_state(state_path, {});
   BOOST_CHECK(!std::filesystem::exists(state_path));
   BOOST_CHECK(load_snapshot_attestation_recovery_state(state_path).empty());
}

/** Reject a recovery sidecar written with an unknown schema version. */
BOOST_AUTO_TEST_CASE(rejects_unknown_schema_version) {
   fc::temp_directory temp;
   const auto state_path = temp.path() / "snapshot-provider-recovery.json";
   const auto incompatible = fc::mutable_variant_object()
      ("schema_version", 2)
      ("chain_id", snapshot_attestation_test::make_chain_id())
      ("provider_account", snapshot_attestation_test::provider_account_name)
      ("pending_vote", fc::variant())
      ("pending_vote_cursor", 0)
      ("disagreement_detected", false);
   BOOST_REQUIRE(fc::json::save_to_file(incompatible, state_path, true));

   BOOST_CHECK_THROW(load_snapshot_attestation_recovery_state(state_path), fc::exception);
}

/** Bind populated recovery state to the configured chain and provider identities. */
BOOST_AUTO_TEST_CASE(validates_chain_and_provider_identity) {
   const auto state = snapshot_attestation_test::make_recovery_state();

   BOOST_CHECK_NO_THROW(validate_snapshot_attestation_recovery_identity(
      state, snapshot_attestation_test::make_chain_id(),
      chain::account_name(snapshot_attestation_test::provider_account_name)));
   BOOST_CHECK_THROW(validate_snapshot_attestation_recovery_identity(
      state, snapshot_attestation_test::make_chain_id('b'),
      chain::account_name(snapshot_attestation_test::provider_account_name)), fc::exception);
   BOOST_CHECK_THROW(validate_snapshot_attestation_recovery_identity(
      state, snapshot_attestation_test::make_chain_id(), chain::account_name("otherprovidr")), fc::exception);
   BOOST_CHECK_NO_THROW(validate_snapshot_attestation_recovery_identity(
      {}, snapshot_attestation_test::make_chain_id('b'), chain::account_name("otherprovidr")));
}

/** Classify absent, reversible, matching, and conflicting final attestation records. */
BOOST_AUTO_TEST_CASE(classifies_complete_final_tuple) {
   constexpr uint32_t irreversible_block_num = 100;
   const auto pending = snapshot_attestation_test::make_snapshot_information();
   const snapshot_attestation_final_record matching{
      pending.head_block_id, chain::digest_type(pending.root_hash.str()), irreversible_block_num};
   const snapshot_attestation_final_record different_block{
      chain::block_id_type(std::string(64, '3')), matching.snapshot_hash, irreversible_block_num};
   const snapshot_attestation_final_record different_hash{
      matching.block_id, chain::digest_type(std::string(64, '4')), irreversible_block_num};
   const snapshot_attestation_final_record reversible{
      matching.block_id, matching.snapshot_hash, irreversible_block_num + 1};

   BOOST_CHECK(classify_snapshot_attestation_record(pending, std::nullopt, irreversible_block_num)
               == snapshot_attestation_record_status::retry);
   BOOST_CHECK(classify_snapshot_attestation_record(pending, reversible, irreversible_block_num)
               == snapshot_attestation_record_status::awaiting_irreversibility);
   BOOST_CHECK(classify_snapshot_attestation_record(pending, matching, irreversible_block_num)
               == snapshot_attestation_record_status::matching);
   BOOST_CHECK(classify_snapshot_attestation_record(pending, different_block, irreversible_block_num)
               == snapshot_attestation_record_status::conflicting);
   BOOST_CHECK(classify_snapshot_attestation_record(pending, different_hash, irreversible_block_num)
               == snapshot_attestation_record_status::conflicting);
}

/** Seed recovery finality from the root of a snapshot loaded in irreversible mode. */
BOOST_AUTO_TEST_CASE(snapshot_loaded_root_seeds_irreversible_recovery_height) {
   testing::tester source(testing::setup_policy::none);
   source.produce_blocks(3);
   source.control->abort_block();

   fc::temp_directory snapshot_dir;
   const auto snapshot_path = snapshot_dir.path() / "startup-snapshot.bin";
   auto writer = std::make_shared<chain::threaded_snapshot_writer>(snapshot_path);
   source.control->write_snapshot(writer);
   writer->finalize();

   const auto snapshot_block_num = source.control->head().block_num();
   const chain::snapshot_scheduler::snapshot_information pending{
      source.control->head().id(),
      snapshot_block_num,
      source.control->head().block_time(),
      chain::chain_snapshot_header::current_version,
      snapshot_path.generic_string(),
      writer->get_root_hash(),
   };

   fc::temp_directory loaded_dir;
   auto loaded_config = testing::base_tester::default_config(loaded_dir).first;
   loaded_config.read_mode = chain::db_read_mode::IRREVERSIBLE;
   auto reader = std::make_shared<chain::threaded_snapshot_reader>(snapshot_path);
   snapshot_loaded_tester loaded(std::move(loaded_config), reader);

   const auto fork_db_root = loaded.control->fork_db_root();
   BOOST_REQUIRE(fork_db_root.is_valid());
   BOOST_CHECK(!fork_db_root.block());
   const auto observed_irreversible_block_num =
      snapshot_attestation_startup_irreversible_block_num(fork_db_root);
   BOOST_CHECK_EQUAL(snapshot_block_num, observed_irreversible_block_num);

   const snapshot_attestation_final_record matching{
      pending.head_block_id,
      chain::digest_type(pending.root_hash.str()),
      snapshot_block_num,
   };
   BOOST_CHECK(classify_snapshot_attestation_record(pending, matching, 0)
               == snapshot_attestation_record_status::awaiting_irreversibility);
   BOOST_CHECK(classify_snapshot_attestation_record(
                  pending, matching, observed_irreversible_block_num)
               == snapshot_attestation_record_status::matching);

   const snapshot_attestation_final_record conflicting{
      pending.head_block_id,
      chain::digest_type(std::string(64, '4')),
      snapshot_block_num,
   };
   bool snapshot_execution_called = false;
   const bool executed = promote_snapshot_after_recovery(
      [&]() {
         return classify_snapshot_attestation_record(
                   pending, conflicting, observed_irreversible_block_num)
                != snapshot_attestation_record_status::conflicting;
      },
      [&]() { snapshot_execution_called = true; });
   BOOST_CHECK(!executed);
   BOOST_CHECK(!snapshot_execution_called);
}

/** Verify the production startup latch suppresses a due scheduler callback after a restored conflict. */
BOOST_AUTO_TEST_CASE(snapshot_loaded_conflict_blocks_scheduler_start) {
   sysio_system::sysio_system_tester source;
   source.setup_producer_accounts({"producer1"_n, "snapprov1"_n});
   source.produce_blocks();
   source.regproducer("producer1"_n);
   BOOST_REQUIRE_EQUAL("", source.push_action(
      chain::config::system_account_name,
      "setrank"_n,
      fc::mutable_variant_object()("producer", "producer1")("rank", 1)));
   BOOST_REQUIRE_EQUAL("", source.push_action(
      "producer1"_n,
      "regsnapprov"_n,
      fc::mutable_variant_object()("producer", "producer1")("snap_account", "snapprov1")));
   BOOST_REQUIRE_EQUAL("", source.push_action(
      chain::config::system_account_name,
      "setsnpcfg"_n,
      fc::mutable_variant_object()("min_providers", 1)("threshold_pct", 100)));
   source.produce_blocks();
   source.control->abort_block();

   fc::temp_directory local_snapshot_dir;
   const auto local_snapshot_path = local_snapshot_dir.path() / "local-snapshot.bin";
   auto local_writer = std::make_shared<chain::threaded_snapshot_writer>(local_snapshot_path);
   source.control->write_snapshot(local_writer);
   local_writer->finalize();
   const chain::snapshot_scheduler::snapshot_information pending{
      source.control->head().id(),
      source.control->head().block_num(),
      source.control->head().block_time(),
      chain::chain_snapshot_header::current_version,
      local_snapshot_path.generic_string(),
      local_writer->get_root_hash(),
   };

   const chain::digest_type conflicting_hash(std::string(64, '4'));
   BOOST_REQUIRE(conflicting_hash.str() != pending.root_hash.str());
   source.produce_block();
   BOOST_REQUIRE_EQUAL("", source.push_action(
      "snapprov1"_n,
      "votesnaphash"_n,
      fc::mutable_variant_object()
         ("snap_account", "snapprov1")
         ("block_id", pending.head_block_id)
         ("snapshot_hash", conflicting_hash)));
   source.produce_blocks(2);
   source.control->abort_block();

   fc::temp_directory startup_snapshot_dir;
   const auto startup_snapshot_path = startup_snapshot_dir.path() / "startup-snapshot.bin";
   auto startup_writer = std::make_shared<chain::threaded_snapshot_writer>(startup_snapshot_path);
   source.control->write_snapshot(startup_writer);
   // chain_plugin enables root-extension tracking while the system-contract fixture does not.
   // Supply the empty section that a snapshot from a production node always contains.
   startup_writer->write_section<chain::contract_root_object>([](auto&) {});
   startup_writer->finalize();

   fc::temp_directory node_dir;
   const auto data_dir = node_dir.path() / "data";
   const auto config_dir = node_dir.path() / "config";
   const auto snapshots_dir = node_dir.path() / "snapshots";
   std::filesystem::create_directories(data_dir);
   std::filesystem::create_directories(config_dir);
   std::filesystem::create_directories(snapshots_dir);
   const auto recovery_path = snapshots_dir / snapshot_attestation_recovery_filename;
   save_snapshot_attestation_recovery_state(
      recovery_path,
      snapshot_attestation_recovery_state{
         snapshot_attestation_recovery_schema_version,
         source.get_chain_id(),
         chain::account_name("snapprov1"),
         pending,
      });

   const auto data_dir_string = data_dir.string();
   const auto config_dir_string = config_dir.string();
   const auto snapshots_dir_string = snapshots_dir.string();
   const auto startup_snapshot_string = startup_snapshot_path.string();
   appbase::scoped_app app;
   std::vector<const char*> argv = {
      "test",
      "--data-dir", data_dir_string.c_str(),
      "--config-dir", config_dir_string.c_str(),
      "--snapshots-dir", snapshots_dir_string.c_str(),
      "--snapshot", startup_snapshot_string.c_str(),
      "--snapshot-provider-account", "snapprov1",
      "--abi-serializer-max-time-ms", disabled_abi_serializer_timeout_ms,
   };
   BOOST_REQUIRE((app->initialize<chain_plugin, producer_plugin>(argv.size(), (char**)&argv[0])));
   app->startup();

   const auto retained = load_snapshot_attestation_recovery_state(recovery_path);
   BOOST_REQUIRE(retained.disagreement_detected);
   auto* producer = app->find_plugin<producer_plugin>();
   auto* chain = app->find_plugin<chain_plugin>();
   BOOST_REQUIRE(producer != nullptr);
   BOOST_REQUIRE(chain != nullptr);

   const auto start_block_num = chain->chain().head().block_num();
   chain::snapshot_scheduler::snapshot_request_params request;
   request.block_spacing = 0;
   request.start_block_num = start_block_num;
   request.end_block_num = start_block_num;
   request.snapshot_description = "blocked startup probe";
   const auto scheduled = producer->schedule_snapshot(request);

   chain->chain().block_start()(start_block_num + 1);
   const auto requests = producer->get_snapshot_requests();
   const auto scheduled_request = std::find_if(
      requests.snapshot_requests.begin(), requests.snapshot_requests.end(),
      [&](const auto& candidate) {
         return candidate.snapshot_request_id == scheduled.snapshot_request_id;
      });
   BOOST_REQUIRE(scheduled_request != requests.snapshot_requests.end());
   BOOST_CHECK(scheduled_request->pending_snapshots.empty());
}

/** Restrict disagreement handling to the local provider's vote submission. */
BOOST_AUTO_TEST_CASE(disagreement_error_is_local_vote_specific) {
   constexpr uint64_t disagreement_error = protocol::snapshot_attestation::disagreement_error_code;

   BOOST_CHECK(is_snapshot_attestation_disagreement_error(
      snapshot_attestation_operation::votesnaphash, disagreement_error));
   BOOST_CHECK(!is_snapshot_attestation_disagreement_error(
      snapshot_attestation_operation::evalsnapvote, disagreement_error));
   BOOST_CHECK(!is_snapshot_attestation_disagreement_error(
      snapshot_attestation_operation::votesnaphash, std::nullopt));
   BOOST_CHECK(snapshot_attestation_operation_name(snapshot_attestation_operation::votesnaphash)
               == "votesnaphash");
   BOOST_CHECK(snapshot_attestation_operation_name(snapshot_attestation_operation::evalsnapvote)
               == "evalsnapvote");
}

/** Preserve a nonempty pending tuple whenever the durable quarantine latch is set. */
BOOST_AUTO_TEST_CASE(disagreement_quarantine_is_durable_and_nonempty) {
   fc::temp_directory temp;
   const auto state_path = temp.path() / snapshot_attestation_recovery_filename;
   const auto quarantined = snapshot_attestation_test::make_recovery_state(
      snapshot_attestation_test::make_chain_id(), true);

   BOOST_CHECK(!quarantined.empty());
   save_snapshot_attestation_recovery_state(state_path, quarantined);
   const auto restored = load_snapshot_attestation_recovery_state(state_path);
   BOOST_CHECK(restored.disagreement_detected);
   BOOST_REQUIRE(restored.pending_vote);
   BOOST_CHECK(restored.pending_vote->head_block_id == quarantined.pending_vote->head_block_id);
}

/** Prevent a pending snapshot from replacing recovery state after a detected conflict. */
BOOST_AUTO_TEST_CASE(recovery_blocks_snapshot_promotion_on_conflict) {
   bool recovery_called = false;
   bool promotion_called = false;
   const bool promoted = promote_snapshot_after_recovery(
      [&recovery_called]() {
         recovery_called = true;
         return false;
      },
      [&promotion_called]() { promotion_called = true; });

   BOOST_CHECK(recovery_called);
   BOOST_CHECK(!promotion_called);
   BOOST_CHECK(!promoted);
}

/** Recheck blocked recovery before promotion even when the periodic interval is not due. */
BOOST_AUTO_TEST_CASE(blocked_recovery_is_rechecked_between_periodic_intervals) {
   bool pending_safe = false;
   uint32_t resolution_count = 0;
   uint32_t promotion_count = 0;
   const auto attempt_promotion = [&](bool periodic_recovery_due) {
      return promote_snapshot_after_recovery(
         [&]() {
            const auto work = prepare_snapshot_attestation_recovery(
               periodic_recovery_due,
               [&]() {
                  ++resolution_count;
                  return pending_safe;
               });
            return work != snapshot_attestation_recovery_work::blocked;
         },
         [&]() { ++promotion_count; });
   };

   BOOST_CHECK(!attempt_promotion(true));
   BOOST_CHECK(!attempt_promotion(false));
   BOOST_CHECK_EQUAL(2u, resolution_count);
   BOOST_CHECK_EQUAL(0u, promotion_count);

   pending_safe = true;
   BOOST_CHECK(attempt_promotion(false));
   BOOST_CHECK_EQUAL(3u, resolution_count);
   BOOST_CHECK_EQUAL(1u, promotion_count);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace sysio
