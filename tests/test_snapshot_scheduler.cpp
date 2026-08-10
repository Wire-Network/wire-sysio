#include <boost/test/unit_test.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/scoped_exit.hpp>

#include <regex>

using namespace sysio;
using namespace sysio::chain;

using snapshot_request_information = snapshot_scheduler::snapshot_request_information;
using snapshot_request_params = snapshot_scheduler::snapshot_request_params;
using snapshot_request_id_information = snapshot_scheduler::snapshot_request_id_information;

BOOST_AUTO_TEST_SUITE(producer_snapshot_scheduler_tests)

BOOST_AUTO_TEST_CASE(snapshot_scheduler_test) {
   fc::logger log;
   snapshot_scheduler scheduler;

   {
      // add/remove test; scheduling validation errors are thrown directly, the (empty) next
      // callback is only invoked for snapshots produced by the request
      snapshot_request_information sri1 = {.block_spacing = 100, .start_block_num = 5000, .end_block_num = 10000, .snapshot_description = "Example of recurring snapshot"};
      snapshot_request_information sri2 = {.block_spacing = 0, .start_block_num = 5200, .end_block_num = 5200, .snapshot_description = "Example of one-time snapshot"};

      scheduler.schedule_snapshot(sri1, {});
      scheduler.schedule_snapshot(sri2, {});

      BOOST_CHECK_EQUAL(2u, scheduler.get_snapshot_requests().snapshot_requests.size());

      BOOST_CHECK_EXCEPTION(scheduler.schedule_snapshot(sri1, {}), duplicate_snapshot_request, [](const fc::assert_exception& e) {
         return e.to_detail_string().find("Duplicate snapshot request") != std::string::npos;
      });

      scheduler.unschedule_snapshot(0);
      BOOST_CHECK_EQUAL(1u, scheduler.get_snapshot_requests().snapshot_requests.size());

      BOOST_CHECK_EXCEPTION(scheduler.unschedule_snapshot(0), snapshot_request_not_found, [](const fc::assert_exception& e) {
         return e.to_detail_string().find("Snapshot request not found") != std::string::npos;
      });

      scheduler.unschedule_snapshot(1);
      BOOST_CHECK_EQUAL(0u, scheduler.get_snapshot_requests().snapshot_requests.size());

      snapshot_request_information sri_large_spacing = {.block_spacing = 1000, .start_block_num = 5000, .end_block_num = 5010};
      BOOST_CHECK_EXCEPTION(scheduler.schedule_snapshot(sri_large_spacing, {}), invalid_snapshot_request, [](const fc::assert_exception& e) {
         return e.to_detail_string().find("Block spacing exceeds defined by start and end range") != std::string::npos;
      });

      snapshot_request_information sri_start_end = {.block_spacing = 1000, .start_block_num = 50000, .end_block_num = 5000};
      BOOST_CHECK_EXCEPTION(scheduler.schedule_snapshot(sri_start_end, {}), invalid_snapshot_request, [](const fc::assert_exception& e) {
         return e.to_detail_string().find("End block number should be greater or equal to start block number") != std::string::npos;
      });
   }
   {
      fc::temp_directory temp_dir;
      const auto& temp = temp_dir.path();
      appbase::scoped_app app;

      try {
         std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
         std::future<std::tuple<producer_plugin*, chain_plugin*>> plugin_fut = plugin_promise.get_future();

         std::promise<void> at_block_20_promise;
         std::future<void> at_block_20_fut = at_block_20_promise.get_future();

         std::thread app_thread([&]() {
            try {
               fc::logger::default_logger().set_log_level(fc::log_level::debug);
               std::vector<const char*> argv =
                     {"test", "--data-dir", temp.c_str(), "--config-dir", temp.c_str(),
                      "-p", "sysio", "-e"};
               app->initialize<chain_plugin, producer_plugin>(argv.size(), (char**) &argv[0]);
               app->startup();

               producer_plugin* prod_plug = app->find_plugin<producer_plugin>();
               chain_plugin* chain_plug = app->find_plugin<chain_plugin>();
               // app was constructed on the outer thread; capture this thread as main_thread_id_
               // before releasing the promise so producer_plugin's main-thread asserts see the loop thread.
               app->executor().set_main_thread_id();
               plugin_promise.set_value({prod_plug, chain_plug});

               auto bs = chain_plug->chain().block_start().connect([&prod_plug, &at_block_20_promise](uint32_t bn) {
                  if(bn == 20u)
                     at_block_20_promise.set_value();
                  // catching pending snapshot
                  if (!prod_plug->get_snapshot_requests().snapshot_requests.empty()) {
                     const auto& snapshot_requests = prod_plug->get_snapshot_requests().snapshot_requests;

                     auto validate_snapshot_request = [&](uint32_t sid, uint32_t block_num, uint32_t spacing = 0, bool fuzzy_start = false) {
                        auto it = find_if(snapshot_requests.begin(), snapshot_requests.end(), [sid](const snapshot_scheduler::snapshot_schedule_information& obj) {return obj.snapshot_request_id == sid;});
                        if (it != snapshot_requests.end()) {
                           auto& pending = it->pending_snapshots;
                           if (pending.size()==1u) {
                              // pending snapshot block number
                              auto pbn = pending.begin()->head_block_num;

                              // first pending snapshot
                              auto ps_start = (spacing != 0) ? (spacing + (pbn%spacing)) : pbn;

                              if (!fuzzy_start) {
                                 BOOST_CHECK_EQUAL(block_num, ps_start);
                              }
                              else {
                                 int diff = block_num - ps_start;
                                 BOOST_CHECK(std::abs(diff) <= 5); // accept +/- 5 blocks if start block not specified
                              }
                           }
                           return true;
                        }
                        return false;
                     };

                     BOOST_REQUIRE(validate_snapshot_request(0, 9,  8));         // snapshot #0 should have pending snapshot at block #9 (8 + 1) and it never expires
                     BOOST_REQUIRE(validate_snapshot_request(4, 12, 10, true));  // snapshot #4 should have pending snapshot at block # at the moment of scheduling (2) plus 10 = 12
                     BOOST_REQUIRE(validate_snapshot_request(5, 10, 10));        // snapshot #5 should have pending snapshot at block #10, #20 etc
                     BOOST_REQUIRE(validate_snapshot_request(6, 15, 15));        // snapshot #6 should have pending snapshot at block #15, #30 etc
                  }
               });

               app->exec();
               return;
            } FC_LOG_AND_DROP()
            BOOST_CHECK(!"app threw exception see logged error");
         });

         auto [prod_plug, chain_plug] = plugin_fut.get();

         snapshot_request_params sri1 = {.block_spacing = 8, .start_block_num = 1, .end_block_num = 300000, .snapshot_description = "Example of recurring snapshot 1"};
         snapshot_request_params sri2 = {.block_spacing = 5000, .start_block_num = 100000, .end_block_num = 300000, .snapshot_description = "Example of recurring snapshot 2 that wont happen in test"};
         snapshot_request_params sri3 = {.block_spacing = 2, .start_block_num = 0, .end_block_num = 3, .snapshot_description = "Example of recurring snapshot 3 that will expire"};
         snapshot_request_params sri4 = {.start_block_num = 1, .snapshot_description = "One time snapshot on first block"};
         snapshot_request_params sri5 = {.block_spacing = 10, .snapshot_description = "Recurring every 10 blocks snapshot starting now"};
         snapshot_request_params sri6 = {.block_spacing = 10, .start_block_num = 0, .snapshot_description = "Recurring every 10 blocks snapshot starting from 0"};
         snapshot_request_params sri7 = {.block_spacing = 15, .start_block_num = 0, .end_block_num = 0, .snapshot_description = "similar to above but with end_block_num=0 to be treated as max"};

         app->post(appbase::priority::medium_low, [&]() {
            prod_plug->schedule_snapshot(sri1);
            prod_plug->schedule_snapshot(sri2);
            prod_plug->schedule_snapshot(sri3);
            prod_plug->schedule_snapshot(sri4);
            prod_plug->schedule_snapshot(sri5);
            prod_plug->schedule_snapshot(sri6);
            prod_plug->schedule_snapshot(sri7);

            // all six snapshot requests should be present now
            BOOST_CHECK_EQUAL(7u, prod_plug->get_snapshot_requests().snapshot_requests.size());
         });

         at_block_20_fut.get();

         app->post(appbase::priority::medium_low, [&]() {
            // two of the snapshots are done here and requests, corresponding to them should be deleted
            BOOST_CHECK_EQUAL(5u, prod_plug->get_snapshot_requests().snapshot_requests.size());

            // check whether no pending snapshots present for a snapshot with id 0
            const auto& snapshot_requests = prod_plug->get_snapshot_requests().snapshot_requests;
            auto it = find_if(snapshot_requests.begin(), snapshot_requests.end(),[](const snapshot_scheduler::snapshot_schedule_information& obj) {return obj.snapshot_request_id == 0;});

            // snapshot request with id = 0 should be found and should not have any pending snapshots
            BOOST_REQUIRE(it != snapshot_requests.end());
            BOOST_CHECK(!it->pending_snapshots.size());

            // quit app
            app->quit();
         });
         app_thread.join();

         // lets check whether schedule can be read back after restart
         snapshot_scheduler::snapshot_db_json db;
         std::vector<snapshot_scheduler::snapshot_schedule_information> ssi;
         db.set_path(temp / "snapshots");
         db >> ssi;
         BOOST_CHECK_EQUAL(5u, ssi.size());
         BOOST_CHECK_EQUAL(ssi.begin()->block_spacing, *sri1.block_spacing);
      } catch(...) {
         throw;
      }
   }
}

//created via a schedule_snapshot with {"block_spacing":5,"snapshot_description":"banana"} on leap4
static const std::string old_schedule_json = R"===(
{
    "snapshot_requests": [
        {
            "snapshot_request_id": "0",
            "snapshot_description": "banana",
            "block_spacing": "5",
            "start_block_num": "0",
            "end_block_num": "0"
        }
    ]
}
)===";

BOOST_AUTO_TEST_CASE(snapshot_scheduler_old_json) {
   fc::temp_directory temp_dir;
   const std::filesystem::path& temp = temp_dir.path();
   appbase::scoped_app app;

   {
      std::filesystem::create_directory(temp / "snapshots");
      std::ofstream ofs(temp / "snapshots" / "snapshot-schedule.json");
      ofs << old_schedule_json;
   }

   std::promise<void> at_block_16;

   std::thread app_thread([&]() {
      try {
         fc::logger::default_logger().set_log_level(fc::log_level::debug);
         std::vector<const char*> argv =
               {"test", "--data-dir", temp.c_str(), "--config-dir", temp.c_str(),
                "-p", "sysio", "-e"};
         app->initialize<chain_plugin, producer_plugin>(argv.size(), (char**) &argv[0]);
         app->startup();

         app->get_plugin<chain_plugin>().chain().block_start().connect([&](uint32_t bn) {
            if(bn == 16u)
               at_block_16.set_value();
         });

         // app was constructed on the outer thread; capture this thread as main_thread_id_
         // so producer_plugin's main-thread asserts see the loop thread.
         app->executor().set_main_thread_id();
         app->exec();
         return;
      } FC_LOG_AND_DROP()
      BOOST_CHECK(!"app threw exception see logged error");
   });
   auto stopit = fc::make_scoped_exit([&](){app->quit(); app_thread.join();});

   at_block_16.get_future().get();

   const std::regex snapshotfile_regex(".bin$");

   int found = 0;
   for(const std::filesystem::directory_entry& dir_entry : std::filesystem::directory_iterator(temp / "snapshots"))
      found += std::regex_search(dir_entry.path().filename().string(), snapshotfile_regex);
   BOOST_REQUIRE_EQUAL(found, 3u);
}

// ---------------------------------------------------------------------------------------------------
// Snapshot-finalized callback contract: every registered callback fires exactly once per snapshot
// that reaches finality, regardless of how the snapshot was initiated (scheduled request or direct
// create_snapshot call), of the chain's read mode, and of whether a duplicate request for the same
// block is issued.
//
// Regression coverage for a bug where scheduled snapshots notified the callbacks twice: once from
// execute_snapshot()'s completion handler and again from on_irreversible_block() (or from
// create_snapshot() in irreversible read mode), which made provider mode submit duplicate
// votesnaphash transactions.
// ---------------------------------------------------------------------------------------------------

namespace {

/// Test fixture wiring a snapshot_scheduler to a temp directory with two counting callbacks.
struct scheduler_callback_fixture {
   fc::temp_directory temp_dir;
   snapshot_scheduler scheduler;
   uint32_t           cb1_count = 0;
   uint32_t           cb2_count = 0;

   scheduler_callback_fixture() {
      scheduler.set_db_path(temp_dir.path());
      scheduler.set_snapshots_path(temp_dir.path());
      scheduler.add_snapshot_finalized_callback(
         [this](const snapshot_scheduler::snapshot_information&) { ++cb1_count; });
      scheduler.add_snapshot_finalized_callback(
         [this](const snapshot_scheduler::snapshot_information&) { ++cb2_count; });
   }
};

/// Tally of how a stored request-completion callback was resolved. A callback that is never
/// resolved at all leaves both counters at zero -- that is the state that hangs a real HTTP caller.
struct next_outcome {
   uint32_t successes = 0;
   uint32_t errors    = 0;

   /// Callback to hand to schedule_snapshot(); records each resolution by kind.
   auto recorder() {
      return [this](const next_function_variant<snapshot_scheduler::snapshot_information>& res) {
         if (std::holds_alternative<fc::exception_ptr>(res))
            ++errors;
         else
            ++successes;
      };
   }
};

} // anonymous namespace

// Scheduled snapshot with the chain NOT in irreversible read mode: the snapshot is pending
// until its block becomes irreversible; each callback fires exactly once at promotion.
BOOST_AUTO_TEST_CASE(scheduled_snapshot_notifies_callbacks_once) {
   testing::tester            chain;
   scheduler_callback_fixture f;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block
   const uint32_t snapshot_height = chain.control->head().block_num();

   // one-time request that on_start_block() executes at height snapshot_height + 1
   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = snapshot_height;
   sri.end_block_num        = snapshot_height;
   sri.snapshot_description = "single-fire scheduled snapshot";
   f.scheduler.schedule_snapshot(sri, {});

   f.scheduler.on_start_block(snapshot_height + 1, *chain.control);

   // snapshot of head is pending; nothing finalized yet
   BOOST_TEST(f.cb1_count == 0u);
   BOOST_TEST(f.cb2_count == 0u);

   // a later irreversible block promotes the pending snapshot
   auto lib_block = chain.produce_block();
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.cb2_count == 1u);

   // further irreversible blocks must not re-notify
   auto next_block = chain.produce_block();
   f.scheduler.on_irreversible_block(next_block, next_block->calculate_id(), *chain.control);

   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.cb2_count == 1u);
}

// Scheduled snapshot with the chain in irreversible read mode: the snapshot finalizes
// immediately inside create_snapshot(); each callback fires exactly once.
BOOST_AUTO_TEST_CASE(irreversible_mode_snapshot_notifies_callbacks_once) {
   testing::tester            chain(testing::setup_policy::full, db_read_mode::IRREVERSIBLE);
   scheduler_callback_fixture f;

   chain.produce_blocks(3); // let finality advance so head (LIB) is past genesis
   chain.control->abort_block(); // snapshot creation requires no pending block
   const uint32_t snapshot_height = chain.control->head().block_num();

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = snapshot_height;
   sri.end_block_num        = snapshot_height;
   sri.snapshot_description = "single-fire irreversible-mode snapshot";
   f.scheduler.schedule_snapshot(sri, {});

   f.scheduler.on_start_block(snapshot_height + 1, *chain.control);

   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.cb2_count == 1u);
}

// Direct create_snapshot() calls for the same head block: the second request finds a snapshot
// already pending for that block and is ignored rather than chained onto the first. (In normal
// operation the producer_plugin guards against this earlier -- schedule_snapshot throws
// duplicate_snapshot_request -- so the dropped handler is not reachable through the HTTP API.)
// The single underlying snapshot still notifies each finalized callback exactly once.
BOOST_AUTO_TEST_CASE(api_snapshot_duplicate_block_request_ignored_notifies_callbacks_once) {
   testing::tester            chain;
   scheduler_callback_fixture f;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   uint32_t next1_success = 0, next2_success = 0;
   auto count_success = [](uint32_t& counter) {
      return [&counter](const next_function_variant<snapshot_scheduler::snapshot_information>& res) {
         if (!std::holds_alternative<fc::exception_ptr>(res))
            ++counter;
      };
   };

   // two create_snapshot calls for the same head block: the first writes the pending snapshot, the
   // second sees it already pending and is ignored (its handler is never stored or invoked)
   f.scheduler.create_snapshot(count_success(next1_success), *chain.control);
   f.scheduler.create_snapshot(count_success(next2_success), *chain.control);

   BOOST_TEST(f.cb1_count == 0u);
   BOOST_TEST(f.cb2_count == 0u);

   auto lib_block = chain.produce_block();
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_TEST(next1_success == 1u);
   BOOST_TEST(next2_success == 0u); // duplicate request was ignored, not chained
   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.cb2_count == 1u);
}

// ---------------------------------------------------------------------------------------------------
// Request lifetime versus irreversibility.
//
// producer_plugin::create_snapshot() schedules a one-time request anchored one block ahead of the
// head the caller observed, and on_start_block() runs it at start_block_num + 1. A node that applies
// a run of blocks back to back -- catching up after a stall, syncing, or switching forks -- commits
// block start_block_num and lands LIB exactly on start_block_num before the firing height is ever
// applied. Collecting the request at that point drops it unexecuted, and with it the stored HTTP
// completion callback, closing the caller's connection with no response.
// ---------------------------------------------------------------------------------------------------

// Irreversibility reaching start_block_num must not collect the request: its firing height has not
// been applied yet. Regression coverage for a create_snapshot API call hanging on a catching-up node.
BOOST_AUTO_TEST_CASE(onetime_request_survives_lib_reaching_its_start_block) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   // exactly the request shape producer_plugin::create_snapshot() builds
   const uint32_t start_block_num = chain.control->head().block_num() + 1;

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "on-demand snapshot racing a catching-up node";
   f.scheduler.schedule_snapshot(sri, outcome.recorder());

   // Block start_block_num is applied and commits, carrying LIB onto start_block_num -- all before
   // block start_block_num + 1, whose block_start signal is what runs the request.
   auto lib_block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(lib_block->block_num(), start_block_num);
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_REQUIRE_EQUAL(f.scheduler.get_snapshot_requests().snapshot_requests.size(), 1u);
   BOOST_TEST(outcome.successes == 0u);
   BOOST_TEST(outcome.errors == 0u);

   // block start_block_num + 1 begins applying; head is still start_block_num
   chain.control->abort_block();
   f.scheduler.on_start_block(start_block_num + 1, *chain.control);

   // snapshot of head is pending until head becomes irreversible
   BOOST_TEST(outcome.successes == 0u);
   BOOST_TEST(f.cb1_count == 0u);

   auto next_lib = chain.produce_block();
   f.scheduler.on_irreversible_block(next_lib, next_lib->calculate_id(), *chain.control);

   BOOST_TEST(outcome.successes == 1u);
   BOOST_TEST(outcome.errors == 0u); // collecting the spent request must not re-resolve the callback
   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.cb2_count == 1u);

   // spent request is collected once irreversibility passes its start block
   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
}

// A one-time request whose firing height has gone by can never run. Collecting it is correct; doing
// so without resolving its stored callback is not -- the caller would wait forever on a request the
// scheduler has already given up on.
BOOST_AUTO_TEST_CASE(expired_onetime_request_reports_failure_to_caller) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   chain.produce_block();
   const uint32_t start_block_num = chain.control->head().block_num() + 1;

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "one-time request whose firing height is never applied";
   f.scheduler.schedule_snapshot(sri, outcome.recorder());

   // irreversibility moves past the firing height without on_start_block ever running the request
   chain.produce_blocks(2);
   auto lib_block = chain.produce_block();
   BOOST_REQUIRE_GT(lib_block->block_num(), start_block_num);
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
   BOOST_TEST(outcome.successes == 0u);
   BOOST_TEST(outcome.errors == 1u); // caller gets an error, not a dropped connection
   BOOST_TEST(f.cb1_count == 0u);    // nothing was ever snapshotted
}

// A one-time request may pin itself to a single block: schedule_snapshot() accepts end_block_num
// equal to start_block_num, and that is the request /v1/producer/schedule_snapshot builds when a
// caller names one block. Expiring on end_block_num must not undercut the firing height, or the
// same catch-up ordering silently loses exactly the requests that are most specific about which
// block they want.
BOOST_AUTO_TEST_CASE(exact_range_onetime_request_survives_lib_reaching_its_start_block) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   const uint32_t start_block_num = chain.control->head().block_num() + 1;

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = start_block_num; // the one block this request is pinned to
   sri.snapshot_description = "one-time snapshot pinned to a single block";
   f.scheduler.schedule_snapshot(sri, outcome.recorder());

   // irreversibility lands on the pinned block, still one block short of the firing height
   auto lib_block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(lib_block->block_num(), start_block_num);
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_REQUIRE_EQUAL(f.scheduler.get_snapshot_requests().snapshot_requests.size(), 1u);
   BOOST_TEST(outcome.errors == 0u);

   // block start_block_num + 1 begins applying; head is still start_block_num
   chain.control->abort_block();
   f.scheduler.on_start_block(start_block_num + 1, *chain.control);

   auto next_lib = chain.produce_block();
   f.scheduler.on_irreversible_block(next_lib, next_lib->calculate_id(), *chain.control);

   BOOST_TEST(outcome.successes == 1u);
   BOOST_TEST(outcome.errors == 0u);
   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
}

// Executing a request is not delivering its snapshot. Outside irreversible read mode the snapshot
// is only pending, and it is discarded unfinalized if its block is forked out -- the caller has
// been told nothing at that point. The request is still scheduled, so it runs again when the
// adopted branch reapplies the firing height, and it is that regenerated snapshot that answers the
// caller. A request that surrendered its callback at execution has nothing left to answer with.
BOOST_AUTO_TEST_CASE(forked_out_snapshot_regenerates_and_still_answers_caller) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   auto snapshotted_block = chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   const uint32_t start_block_num = chain.control->head().block_num();
   BOOST_REQUIRE_EQUAL(snapshotted_block->block_num(), start_block_num);

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "on-demand snapshot whose block is forked out";
   f.scheduler.schedule_snapshot(sri, outcome.recorder());

   f.scheduler.on_start_block(start_block_num + 1, *chain.control); // snapshot of start_block_num pending

   // Irreversibility settles on a competing block at the snapshotted height, so the pending
   // snapshot is invalidated and discarded without its completion handler ever running.
   block_id_type competing_id = snapshotted_block->calculate_id();
   competing_id._hash[3] ^= 1; // a different id at the same height -- the block number is in _hash[0]
   BOOST_REQUIRE_EQUAL(block_header::num_from_id(competing_id), start_block_num);
   BOOST_REQUIRE(competing_id != snapshotted_block->calculate_id());
   f.scheduler.on_irreversible_block(snapshotted_block, competing_id, *chain.control);

   BOOST_TEST(outcome.successes == 0u);
   BOOST_TEST(outcome.errors == 0u);
   BOOST_TEST(f.cb1_count == 0u);
   // still scheduled, so the firing height can run it again on the adopted branch
   BOOST_REQUIRE_EQUAL(f.scheduler.get_snapshot_requests().snapshot_requests.size(), 1u);

   f.scheduler.on_start_block(start_block_num + 1, *chain.control);

   auto next_lib = chain.produce_block();
   f.scheduler.on_irreversible_block(next_lib, next_lib->calculate_id(), *chain.control);

   BOOST_TEST(outcome.successes == 1u); // the regenerated snapshot answers the original caller
   BOOST_TEST(outcome.errors == 0u);    // and collecting the spent request does not re-resolve it
   BOOST_TEST(f.cb1_count == 1u);
   BOOST_TEST(f.cb2_count == 1u);
   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
}

// A completion callback belongs to the API caller and can fail on its own account -- writing to a
// client that has gone away, say. It is still resolved: a next_function permits exactly one
// invocation across all of its copies, so a request left holding one for unschedule_snapshot_requests
// to resolve again would be undefined behavior. The snapshot itself is unaffected by the failure.
BOOST_AUTO_TEST_CASE(throwing_completion_callback_is_resolved_once) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   uint32_t                   invocations = 0;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   const uint32_t start_block_num = chain.control->head().block_num();

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "on-demand snapshot whose caller fails to take the answer";
   f.scheduler.schedule_snapshot(sri, [&invocations](const next_function_variant<snapshot_scheduler::snapshot_information>&) {
      ++invocations;
      throw std::runtime_error("completion callback failed");
   });

   f.scheduler.on_start_block(start_block_num + 1, *chain.control);

   // This one irreversible block both delivers the snapshot and, because it carries LIB past the
   // firing height, collects the request -- the ordering that would resolve the callback twice.
   auto lib_block = chain.produce_block();
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_TEST(invocations == 1u);
   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
   BOOST_TEST(f.cb1_count == 1u); // a failing caller does not cost the snapshot its finalized notification
   BOOST_TEST(f.cb2_count == 1u);
}

// An outstanding /v1/producer/create_snapshot is a scheduled request like any other and is listed by
// get_snapshot_requests(), so its id can be handed straight to /v1/producer/unschedule_snapshot.
// Cancelling it must answer the caller rather than destroy the callback holding its HTTP session.
BOOST_AUTO_TEST_CASE(unscheduling_an_outstanding_request_reports_failure_to_caller) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   chain.produce_block();

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = chain.control->head().block_num() + 1;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "on-demand snapshot cancelled before it runs";
   const auto scheduled = f.scheduler.schedule_snapshot(sri, outcome.recorder());

   f.scheduler.unschedule_snapshot(scheduled.snapshot_request_id);

   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
   BOOST_TEST(outcome.successes == 0u);
   BOOST_TEST(outcome.errors == 1u); // caller gets an error, not a dropped connection
   BOOST_TEST(f.cb1_count == 0u);    // nothing was ever snapshotted
}

// Cancellation can also land while a snapshot from the request is in flight: the pending snapshot
// carries a handler that answers the caller when it finalizes, and the request is cancellable the
// whole time. Both can reach the caller, so exactly one of them must, whichever gets there first.
BOOST_AUTO_TEST_CASE(unscheduling_an_in_flight_request_answers_caller_once) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   const uint32_t start_block_num = chain.control->head().block_num();

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "on-demand snapshot cancelled while its snapshot is pending";
   const auto scheduled = f.scheduler.schedule_snapshot(sri, outcome.recorder());

   // the snapshot is taken and pending; it answers the caller only once its block is irreversible
   f.scheduler.on_start_block(start_block_num + 1, *chain.control);
   BOOST_REQUIRE(outcome.successes == 0u);

   f.scheduler.unschedule_snapshot(scheduled.snapshot_request_id);
   BOOST_REQUIRE(outcome.errors == 1u); // cancellation is what reaches the caller here

   // the pending snapshot still finalizes, and must not answer the same caller a second time
   auto lib_block = chain.produce_block();
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);

   BOOST_TEST(outcome.successes == 0u);
   BOOST_TEST(outcome.errors == 1u);
   BOOST_TEST(f.cb1_count == 1u); // the snapshot itself finalized and notified as usual
   BOOST_TEST(f.cb2_count == 1u);
}

// A request that has delivered its snapshot stays scheduled until irreversibility passes its firing
// height, and an operator can cancel it in that window. Its caller has already been answered, so
// cancellation must not reach the same callback a second time.
BOOST_AUTO_TEST_CASE(unscheduling_a_delivered_request_does_not_re_resolve_caller) {
   testing::tester            chain;
   scheduler_callback_fixture f;
   next_outcome               outcome;

   chain.produce_block();
   chain.control->abort_block(); // snapshot creation requires no pending block

   const uint32_t start_block_num = chain.control->head().block_num() + 1;

   snapshot_request_information sri;
   sri.block_spacing        = 0;
   sri.start_block_num      = start_block_num;
   sri.end_block_num        = std::numeric_limits<uint32_t>::max();
   sri.snapshot_description = "on-demand snapshot cancelled after it is answered";
   const auto scheduled = f.scheduler.schedule_snapshot(sri, outcome.recorder());

   auto lib_block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(lib_block->block_num(), start_block_num);
   chain.control->abort_block();
   f.scheduler.on_start_block(start_block_num + 1, *chain.control);

   // irreversibility reaching the snapshotted block answers the caller but leaves the request
   // scheduled -- collection waits until it passes the firing height
   f.scheduler.on_irreversible_block(lib_block, lib_block->calculate_id(), *chain.control);
   BOOST_REQUIRE(outcome.successes == 1u);
   BOOST_REQUIRE_EQUAL(f.scheduler.get_snapshot_requests().snapshot_requests.size(), 1u);

   f.scheduler.unschedule_snapshot(scheduled.snapshot_request_id);

   BOOST_TEST(outcome.successes == 1u);
   BOOST_TEST(outcome.errors == 0u);
   BOOST_TEST(f.scheduler.get_snapshot_requests().snapshot_requests.empty());
}

BOOST_AUTO_TEST_SUITE_END()
