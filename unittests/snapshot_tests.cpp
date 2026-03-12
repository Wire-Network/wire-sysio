#include <sstream>

#include <sysio/chain/block_log.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/s_root_extension.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/testing/tester.hpp>
#include "snapshot_suites.hpp"
#include <snapshots.hpp>
#include <snapshot_tester.hpp>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>
#include "test_wasts.hpp"

using namespace sysio;
using namespace testing;
using namespace chain;


namespace {
   void variant_diff_helper(const fc::variant& lhs, const fc::variant& rhs, std::function<void(const std::string&, const fc::variant&, const fc::variant&)>&& out){
      if (lhs.get_type() != rhs.get_type()) {
         out("", lhs, rhs);
      } else if (lhs.is_object() ) {
         const auto& l_obj = lhs.get_object();
         const auto& r_obj = rhs.get_object();
         static const std::string sep = ".";

         // test keys from LHS
         std::set<std::string_view> keys;
         for (const auto& entry: l_obj) {
            const auto& l_val = entry.value();
            const auto& r_iter = r_obj.find(entry.key());
            if (r_iter == r_obj.end()) {
               out(sep + entry.key(), l_val, fc::variant());
            } else {
               const auto& r_val = r_iter->value();
               variant_diff_helper(l_val, r_val, [&out, &entry](const std::string& path, const fc::variant& lhs, const fc::variant& rhs){
                  out(sep + entry.key() + path, lhs, rhs);
               });
            }

            keys.insert(entry.key());
         }

         // print keys in RHS that were not tested
         for (const auto& entry: r_obj) {
            if (keys.find(entry.key()) != keys.end()) {
               continue;
            }
            const auto& r_val = entry.value();
            out(sep + entry.key(), fc::variant(), r_val);
         }
      } else if (lhs.is_array()) {
         const auto& l_arr = lhs.get_array();
         const auto& r_arr = rhs.get_array();

         // diff common
         auto common_count = std::min(l_arr.size(), r_arr.size());
         for (size_t idx = 0; idx < common_count; idx++) {
            const auto& l_val = l_arr.at(idx);
            const auto& r_val = r_arr.at(idx);
            variant_diff_helper(l_val, r_val, [&](const std::string& path, const fc::variant& lhs, const fc::variant& rhs){
               out( std::string("[") + std::to_string(idx) + std::string("]") + path, lhs, rhs);
            });
         }

         // print lhs additions
         for (size_t idx = common_count; idx < lhs.size(); idx++) {
            const auto& l_val = l_arr.at(idx);
            out( std::string("[") + std::to_string(idx) + std::string("]"), l_val, fc::variant());
         }

         // print rhs additions
         for (size_t idx = common_count; idx < rhs.size(); idx++) {
            const auto& r_val = r_arr.at(idx);
            out( std::string("[") + std::to_string(idx) + std::string("]"), fc::variant(), r_val);
         }

      } else if (!(lhs == rhs)) {
         out("", lhs, rhs);
      }
   }

   void print_variant_diff(const fc::variant& lhs, const fc::variant& rhs) {
      variant_diff_helper(lhs, rhs, [](const std::string& path, const fc::variant& lhs, const fc::variant& rhs){
         std::cout << path << std::endl;
         std::cout << "lhs:"  << std::endl;
         if (!lhs.is_null()) {
            std::cout << fc::json::to_pretty_string(lhs) << std::endl;
         }
         std::cout << std::endl;
         std::cout << "----------------------------------------------------------------" << std::endl;

         std::cout << "rhs:" << std::endl;
         if (!rhs.is_null()) {
            std::cout << fc::json::to_pretty_string(rhs);
         }
         std::cout << std::endl;
      });
   }

   template <typename SNAPSHOT_SUITE>
   void verify_integrity_hash(controller& lhs, controller& rhs) {
      const auto lhs_integrity_hash = lhs.calculate_integrity_hash();
      const auto rhs_integrity_hash = rhs.calculate_integrity_hash();
      if constexpr (std::is_same_v<SNAPSHOT_SUITE, variant_snapshot_suite>) {
         if(lhs_integrity_hash.str() != rhs_integrity_hash.str()) {
            auto lhs_latest_writer = SNAPSHOT_SUITE::get_writer();
            lhs.write_snapshot(lhs_latest_writer);
            auto lhs_latest = SNAPSHOT_SUITE::finalize(lhs_latest_writer);

            auto rhs_latest_writer = SNAPSHOT_SUITE::get_writer();
            rhs.write_snapshot(rhs_latest_writer);
            auto rhs_latest = SNAPSHOT_SUITE::finalize(rhs_latest_writer);

            print_variant_diff(lhs_latest, rhs_latest);
            // more than print the different, also save snapshots json gz files under path build/unittests/snapshots
            SNAPSHOT_SUITE::write_to_file("snapshot_debug_verify_integrity_hash_lhs", lhs_latest);
            SNAPSHOT_SUITE::write_to_file("snapshot_debug_verify_integrity_hash_rhs", rhs_latest);
         }
      }
      BOOST_REQUIRE_EQUAL(lhs_integrity_hash.str(), rhs_integrity_hash.str());
   }
}

// Split the tests into multiple parts which run approximately the same time
// so that they can finish within CICD time limits
BOOST_AUTO_TEST_SUITE(snapshot_part1_tests)

template<typename TESTER, typename SNAPSHOT_SUITE>
void exhaustive_snapshot_test()
{
   TESTER chain;

   // Create 2 accounts
   chain.create_accounts({"snapshot"_n, "snapshot1"_n});

   // Set code and increment the first account
   chain.produce_block();
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();
   chain.push_action("snapshot"_n, "increment"_n, "snapshot"_n, mutable_variant_object()
         ( "value", 1 )
   );

   // Set code and increment the second account
   chain.produce_block();
   chain.set_code("snapshot1"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot1"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();
   // increment the test contract
   chain.push_action("snapshot1"_n, "increment"_n, "snapshot1"_n, mutable_variant_object()
         ( "value", 1 )
   );

   chain.produce_block();

   chain.control->abort_block();

   static const int generation_count = 8;
   std::list<snapshotted_tester> sub_testers;

   for (int generation = 0; generation < generation_count; generation++) {
      // create a new snapshot child
      auto writer = SNAPSHOT_SUITE::get_writer();
      chain.control->write_snapshot(writer);
      auto snapshot = SNAPSHOT_SUITE::finalize(writer);

      // create a new child at this snapshot
      sub_testers.emplace_back(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), generation);

      // increment the test contract
      chain.push_action("snapshot"_n, "increment"_n, "snapshot"_n, mutable_variant_object()
         ( "value", 1 )
      );
      chain.push_action("snapshot1"_n, "increment"_n, "snapshot1"_n, mutable_variant_object()
         ( "value", 1 )
      );

      // produce block
      auto new_block = chain.produce_block();

      // undo the auto-pending from tester
      chain.control->abort_block();

      auto integrity_value = chain.control->calculate_integrity_hash();

      // push that block to all sub testers and validate the integrity of the database after it.
      for (auto& other: sub_testers) {
         other.push_block(new_block);
         BOOST_REQUIRE_EQUAL(integrity_value.str(), other.control->calculate_integrity_hash().str());
      }
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_exhaustive_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   exhaustive_snapshot_test<savanna_tester, SNAPSHOT_SUITE>();
}

template<typename TESTER, typename SNAPSHOT_SUITE>
void replay_over_snapshot_test()
{
   TESTER chain;
   const std::filesystem::path parent_path = chain.get_config().blocks_dir.parent_path();

   chain.create_account("snapshot"_n);
   chain.produce_block();
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();
   chain.control->abort_block();

   static const int pre_snapshot_block_count = 12;
   static const int post_snapshot_block_count = 12;

   for (int itr = 0; itr < pre_snapshot_block_count; itr++) {
      // increment the contract
      chain.push_action("snapshot"_n, "increment"_n, "snapshot"_n, mutable_variant_object()
         ( "value", 1 )
      );

      // produce block
      chain.produce_block();
   }

   chain.control->abort_block();

   // create a new snapshot child
   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   // create a new child at this snapshot
   int ordinal = 1;
   snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

   // push more blocks to build up a block log
   for (int itr = 0; itr < post_snapshot_block_count; itr++) {
      // increment the contract
      chain.push_action("snapshot"_n, "increment"_n, "snapshot"_n, mutable_variant_object()
         ( "value", 1 )
      );

      // produce & push block
      snap_chain.push_block(chain.produce_block());
   }

   // verify the hash at the end
   chain.control->abort_block();
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

   // replay the block log from the snapshot child, from the snapshot
   using config_file_handling = snapshotted_tester::config_file_handling;
   snapshotted_tester replay_chain(snap_chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++, config_file_handling::copy_config_files);
   const auto replay_head = replay_chain.head().block_num();
   auto snap_head = snap_chain.head().block_num();
   BOOST_REQUIRE_EQUAL(replay_head, snap_chain.last_irreversible_block_num());
   for (auto block_num = replay_head + 1; block_num <= snap_head; ++block_num) {
      auto block = snap_chain.fetch_block_by_number(block_num);
      replay_chain.push_block(block);
   }
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *replay_chain.control);

   auto block = chain.produce_block();
   chain.control->abort_block();
   snap_chain.push_block(block);
   replay_chain.push_block(block);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *replay_chain.control);

   snapshotted_tester replay2_chain(snap_chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++, config_file_handling::copy_config_files);
   const auto replay2_head = replay2_chain.head().block_num();
   snap_head = snap_chain.head().block_num();
   BOOST_REQUIRE_EQUAL(replay2_head, snap_chain.last_irreversible_block_num());
   for (auto block_num = replay2_head + 1; block_num <= snap_head; ++block_num) {
      auto block = snap_chain.fetch_block_by_number(block_num);
      replay2_chain.push_block(block);
   }
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *replay2_chain.control);

   // verifies that chain's block_log has a genesis_state (and blocks starting at 1)
   controller::config copied_config = copy_config_and_files(chain.get_config(), ordinal++);
   auto genesis = chain::block_log::extract_genesis_state(chain.get_config().blocks_dir);
   BOOST_REQUIRE(genesis);
   tester from_block_log_chain(copied_config, *genesis);
   const auto from_block_log_head = from_block_log_chain.head().block_num();
   BOOST_REQUIRE_EQUAL(from_block_log_head, snap_chain.last_irreversible_block_num());
   for (auto block_num = from_block_log_head + 1; block_num <= snap_head; ++block_num) {
      auto block = snap_chain.fetch_block_by_number(block_num);
      from_block_log_chain.push_block(block);
   }
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *from_block_log_chain.control);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_replay_over_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   replay_over_snapshot_test<savanna_tester, SNAPSHOT_SUITE>();
}

template<typename TESTER, typename SNAPSHOT_SUITE>
void chain_id_in_snapshot_test()
{
   TESTER chain;
   const std::filesystem::path parent_path = chain.get_config().blocks_dir.parent_path();

   chain.create_account("snapshot"_n);
   chain.produce_block();
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();
   chain.control->abort_block();

   // create a new snapshot child
   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), 0);
   BOOST_REQUIRE_EQUAL(chain.get_chain_id(), snap_chain.get_chain_id());
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_s_root_in_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   contract_action_matches matches;
   matches.push_back(contract_action_match("s"_n, config::system_account_name, contract_action_match::match_type::exact));
   matches[0].add_action("newaccount"_n, contract_action_match::match_type::exact);
   tester chain(matches);
   chain.produce_block();
   const std::filesystem::path parent_path = chain.get_config().blocks_dir.parent_path();
   auto find_s_root_ext = [](const auto& exts) {
      return std::find_if(exts.begin(), exts.end(),
         [](const auto& ext) { return ext.first == s_root_extension::extension_id(); });
   };

   const auto block = chain.control->fetch_block_by_number(chain.control->head().block_num());
   BOOST_CHECK_EQUAL(0u, block->header_extensions.size()); // finality fields are now direct header members

   flat_multimap<uint16_t, block_header_extension> header_exts = block->validate_and_extract_header_extensions();
   BOOST_CHECK_EQUAL(0u, header_exts.count(s_root_extension::extension_id()));

   auto crtd_it = find_s_root_ext(header_exts);
   BOOST_CHECK(crtd_it == header_exts.end());

   chain.create_account("snapshot"_n);
   auto block_with_snapshot = chain.produce_blocks(1);
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.create_account("brian"_n);
   auto block_with_multiple = chain.produce_blocks(1);
   chain.control->abort_block(); // aborts pending block

   const auto block_snapshot = chain.control->fetch_block_by_number(block_with_snapshot->block_num());

   BOOST_CHECK_EQUAL(1u, block_snapshot->header_extensions.size());
   header_exts = block_snapshot->validate_and_extract_header_extensions();
   BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

   crtd_it = find_s_root_ext(header_exts);

   BOOST_REQUIRE(crtd_it != header_exts.end());
   s_root_extension crtd_s_ext_snap = std::get<s_root_extension>(crtd_it->second);
   s_header crtd_s_header_snap = crtd_s_ext_snap.s_header_data;
   BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header_snap.contract_name);
   BOOST_CHECK_EQUAL(4u, crtd_s_header_snap.previous_block_num); // might change if tester setup is updated
   BOOST_CHECK(checksum256_type() != crtd_s_header_snap.current_s_id);
   BOOST_CHECK(checksum256_type() != crtd_s_header_snap.current_s_root);

   const auto block_multiple = chain.control->fetch_block_by_number(block_with_multiple->block_num());

   BOOST_CHECK_EQUAL(1u, block_multiple->header_extensions.size());
   header_exts = block_multiple->validate_and_extract_header_extensions();
   BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

   crtd_it = find_s_root_ext(header_exts);

   BOOST_CHECK(crtd_it != header_exts.end());
   s_root_extension crtd_s_ext_multiple = std::get<s_root_extension>(crtd_it->second);
   s_header crtd_s_header_multiple = crtd_s_ext_multiple.s_header_data;
   BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header_multiple.contract_name);
   BOOST_CHECK_EQUAL(crtd_s_header_snap.current_s_id, crtd_s_header_multiple.previous_s_id);
   BOOST_CHECK_EQUAL(block_with_snapshot->block_num(), crtd_s_header_multiple.previous_block_num);
   BOOST_CHECK(checksum256_type() != crtd_s_header_multiple.current_s_id);
   BOOST_CHECK(checksum256_type() != crtd_s_header_multiple.current_s_root);

   // create a new snapshot child
   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), 0, snapshotted_tester::dont_copy_config_files, matches);
   BOOST_REQUIRE_EQUAL(chain.control->get_chain_id(), snap_chain.control->get_chain_id());
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

   // verify that the snapshot_chain is producing the same s_header results
   chain.create_account("tom"_n);
   const auto block_tom = chain.produce_block();

   snap_chain.create_account("bob"_n);
   
   snap_chain.push_block(block_tom);

   BOOST_CHECK_EQUAL(1u, block_tom->header_extensions.size());
   header_exts = block_tom->validate_and_extract_header_extensions();
   BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

   crtd_it = find_s_root_ext(header_exts);

   BOOST_CHECK(crtd_it != header_exts.end());
   s_root_extension crtd_s_ext7 = std::get<s_root_extension>(crtd_it->second);
   s_header crtd_s_header7 = crtd_s_ext7.s_header_data;
   BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header7.contract_name);
   BOOST_CHECK_EQUAL(crtd_s_header_multiple.current_s_id, crtd_s_header7.previous_s_id);
   BOOST_CHECK_EQUAL(block_with_multiple->block_num(), crtd_s_header7.previous_block_num);
   BOOST_CHECK(checksum256_type() != crtd_s_header7.current_s_id);
   BOOST_CHECK(checksum256_type() != crtd_s_header7.current_s_root);
   
   const auto snap_block_tom = snap_chain.control->fetch_block_by_number(block_tom->block_num());
  
   BOOST_CHECK_EQUAL(1u, snap_block_tom->header_extensions.size());
   header_exts = snap_block_tom->validate_and_extract_header_extensions();
   BOOST_CHECK_EQUAL(1u, header_exts.count(s_root_extension::extension_id()));

   crtd_it = find_s_root_ext(header_exts);

   BOOST_CHECK(crtd_it != header_exts.end());
   s_root_extension crtd_s_ext_snap7 = std::get<s_root_extension>(crtd_it->second);
   s_header crtd_s_header_snap7 = crtd_s_ext_snap7.s_header_data;
   BOOST_CHECK_EQUAL(config::system_account_name, crtd_s_header_snap7.contract_name);
   BOOST_CHECK_EQUAL(crtd_s_header_multiple.current_s_id, crtd_s_header_snap7.previous_s_id);
   BOOST_CHECK_EQUAL(block_with_multiple->block_num(), crtd_s_header_snap7.previous_block_num);
   BOOST_CHECK_EQUAL(crtd_s_header7.current_s_id, crtd_s_header_snap7.current_s_id);
   BOOST_CHECK_EQUAL(crtd_s_header7.current_s_root, crtd_s_header_snap7.current_s_root);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_chain_id_in_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   chain_id_in_snapshot_test<savanna_tester, SNAPSHOT_SUITE>();
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE(snapshot_part2_tests)

static auto get_extra_args() {
   bool save_snapshot = false;
   bool generate_log = false;

   auto argc = boost::unit_test::framework::master_test_suite().argc;
   auto argv = boost::unit_test::framework::master_test_suite().argv;
   std::for_each(argv, argv + argc, [&](const std::string &a){
      if (a == "--save-snapshot") {
         save_snapshot = true;
      } else if (a == "--generate-snapshot-log") {
         generate_log = true;
      }
   });

   return std::make_tuple(save_snapshot, generate_log);
}

template<typename TESTER, typename SNAPSHOT_SUITE>
void compatible_versions_test()
{
   const uint32_t legacy_default_max_inline_action_size = 4 * 1024;
   bool save_snapshot = false;
   bool generate_log = false;
   std::tie(save_snapshot, generate_log) = get_extra_args();
   const auto source_log_dir = std::filesystem::path(snapshot_file<snapshot::binary>::base_path);

   if (generate_log) {
      ///< Begin deterministic code to generate blockchain for comparison

      TESTER chain(setup_policy::full, db_read_mode::HEAD, {legacy_default_max_inline_action_size});
      chain.create_account("snapshot"_n);
      chain.produce_block();
      chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
      chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
      chain.produce_block();
      chain.control->abort_block();

      // continue until all the above blocks are in the blocks.log
      auto head_block_num = chain.head().block_num();
      while (chain.last_irreversible_block_num() < head_block_num) {
         chain.produce_block();
      }

      auto source = chain.get_config().blocks_dir / "blocks.log";
      std::filesystem::copy_file(source, source_log_dir / "blocks.log", std::filesystem::copy_options::overwrite_existing);
      auto source_i = chain.get_config().blocks_dir / "blocks.index";
      std::filesystem::copy_file(source_i, source_log_dir / "blocks.index", std::filesystem::copy_options::overwrite_existing);
      chain.close();
   }
   fc::temp_directory temp_dir;
   auto config = tester::default_config(temp_dir, legacy_default_max_inline_action_size).first;
   auto genesis = sysio::chain::block_log::extract_genesis_state(source_log_dir);
   std::filesystem::create_directories(config.blocks_dir);
   std::filesystem::copy(source_log_dir / "blocks.log", config.blocks_dir / "blocks.log");
   std::filesystem::copy(source_log_dir / "blocks.index", config.blocks_dir / "blocks.index");
   TESTER base_chain(config, *genesis);

   std::string current_version = "v1";

   int ordinal = 0;

   // Load the reference v1 snapshot from disk and verify it matches the base chain
   if(!save_snapshot) {
      auto old_snapshot = SNAPSHOT_SUITE::load_from_file("snap_" + current_version);
      BOOST_TEST_CHECKPOINT("loading reference snapshot: " << current_version);
      snapshotted_tester old_snapshot_tester(base_chain.get_config(), SNAPSHOT_SUITE::get_reader(old_snapshot), ordinal++);
      verify_integrity_hash<SNAPSHOT_SUITE>(*base_chain.control, *old_snapshot_tester.control);
   }

   // Create a snapshot from the current chain
   auto latest_writer = SNAPSHOT_SUITE::get_writer();
   base_chain.control->write_snapshot(latest_writer);
   auto latest = SNAPSHOT_SUITE::finalize(latest_writer);

   // Load snapshot and verify integrity
   snapshotted_tester latest_tester(base_chain.get_config(), SNAPSHOT_SUITE::get_reader(latest), ordinal++);
   verify_integrity_hash<SNAPSHOT_SUITE>(*base_chain.control, *latest_tester.control);

   // Round-trip: create another snapshot from the loaded one
   auto roundtrip_writer = SNAPSHOT_SUITE::get_writer();
   latest_tester.control->write_snapshot(roundtrip_writer);
   auto roundtrip = SNAPSHOT_SUITE::finalize(roundtrip_writer);

   // Load and verify the round-tripped snapshot
   snapshotted_tester roundtrip_tester(base_chain.get_config(), SNAPSHOT_SUITE::get_reader(roundtrip), ordinal++);
   verify_integrity_hash<SNAPSHOT_SUITE>(*base_chain.control, *roundtrip_tester.control);
   // This isn't quite fully automated.  The snapshots still need to be gzipped and moved to
   // the correct place in the source tree.
   // -------------------------------------------------------------------------------------------------------------
   // Process for supporting a new snapshot version in this test:
   // ----------------------------------------------------------
   // 1. update `current_version` and the list of versions in `for` loop
   // 2. run: `unittests/unit_test -t "snapshot_part2_tests/test_com*" -- --save-snapshot` to generate new snapshot files
   // 3. copy the newly generated files (see `ls -lrth ./unittests/snapshots/snap_*` to `spring/unittests/snapshots`
   //    for example `cp ./unittests/snapshots/snap_v8.* ../unittests/snapshots`
   // 4. edit `unittests/snapshots/CMakeLists.txt` and add the `configure_file` commands for the 3 new files.
   //    now the test should pass.
   // 5. add the 3 new snapshot files in git.
   // -------------------------------------------------------------------------------------------------------------
   // Only want to save one snapshot, use Savanna as that is the latest
   if constexpr (std::is_same_v<TESTER, savanna_tester>) {
      if (save_snapshot)
      {
         // create a latest snapshot
         auto latest_writer = SNAPSHOT_SUITE::get_writer();
         base_chain.control->write_snapshot(latest_writer);
         auto latest = SNAPSHOT_SUITE::finalize(latest_writer);

         SNAPSHOT_SUITE::write_to_file("snap_" + current_version, latest);
      }
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_compatible_versions, SNAPSHOT_SUITE, snapshot_suites)
{
   compatible_versions_test<savanna_tester, SNAPSHOT_SUITE>();
}

template<typename TESTER, typename SNAPSHOT_SUITE>
void restart_with_existing_state_and_truncated_block_log_test()
{
   TESTER chain;
   const std::filesystem::path parent_path = chain.get_config().blocks_dir.parent_path();

   chain.create_account("snapshot"_n);
   chain.produce_block();
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();
   chain.control->abort_block();

   static const int pre_snapshot_block_count = 12;

   for (int itr = 0; itr < pre_snapshot_block_count; itr++) {
      // increment the contract
      chain.push_action("snapshot"_n, "increment"_n, "snapshot"_n, mutable_variant_object()
                        ( "value", 1 )
                        );

      // produce block
      chain.produce_block();
   }

   chain.control->abort_block();
   {
      // create a new snapshot child
      auto writer = SNAPSHOT_SUITE::get_writer();
      chain.control->write_snapshot(writer);
      auto snapshot = SNAPSHOT_SUITE::finalize(writer);

      // create a new child at this snapshot
      int ordinal = 1;

      snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++);
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
      auto block = chain.produce_block();
      chain.control->abort_block();
      snap_chain.push_block(block);
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

      snap_chain.close();
      auto cfg = snap_chain.get_config();
      // restart chain with truncated block log and existing state, but no genesis state (chain_id)
      snap_chain.open();
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

      block = chain.produce_block();
      chain.control->abort_block();
      snap_chain.push_block(block);
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
   }
   // test with empty block log
   {
      // create a new snapshot child
      auto writer = SNAPSHOT_SUITE::get_writer();
      chain.control->write_snapshot(writer);
      auto snapshot = SNAPSHOT_SUITE::finalize(writer);

      // create a new child at this snapshot
      int ordinal = 2;
      auto chain_cfg = chain.get_config();
      chain_cfg.blog = sysio::chain::empty_blocklog_config{}; // use empty block log
      snapshotted_tester snap_chain(chain_cfg, SNAPSHOT_SUITE::get_reader(snapshot), ordinal++);
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
      auto block = chain.produce_block();
      chain.control->abort_block();
      snap_chain.push_block(block);
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

      snap_chain.close();
      auto cfg = snap_chain.get_config();
      // restart chain with truncated block log and existing state, but no genesis state (chain_id)
      snap_chain.open();
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

      block = chain.produce_block();
      chain.control->abort_block();
      snap_chain.push_block(block);
      verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
   }

}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_restart_with_existing_state_and_truncated_block_log, SNAPSHOT_SUITE, snapshot_suites)
{
   restart_with_existing_state_and_truncated_block_log_test<savanna_tester, SNAPSHOT_SUITE>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE( json_snapshot_validity_test, TESTER, testers )
{
   auto ordinal = 0;
   TESTER chain;

   // prep the chain
   chain.create_account("snapshot"_n);
   chain.produce_block();
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();
   chain.control->abort_block();

   // create binary snapshot
   auto writer_bin = threaded_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer_bin);
   auto snapshot_bin = threaded_snapshot_suite::finalize(writer_bin);

   // create json snapshot
   auto writer_json = json_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer_json);
   auto snapshot_json = json_snapshot_suite::finalize(writer_json);

   // load binary snapshot
   auto reader_bin = threaded_snapshot_suite::get_reader(snapshot_bin);
   snapshotted_tester tester_bin(chain.get_config(), reader_bin, ordinal++);

   // load json snapshot
   auto reader_json = json_snapshot_suite::get_reader(snapshot_json);
   snapshotted_tester tester_json(chain.get_config(), reader_json, ordinal++);

   // create binary snapshot from the JSON-loaded chain
   auto writer_bin_from_json = threaded_snapshot_suite::get_writer();
   tester_json.control->write_snapshot(writer_bin_from_json);
   auto snapshot_bin_from_json = threaded_snapshot_suite::finalize(writer_bin_from_json);

   // load the binary-from-json snapshot
   auto reader_bin_from_json = threaded_snapshot_suite::get_reader(snapshot_bin_from_json);
   snapshotted_tester tester_bin_from_json(chain.get_config(), reader_bin_from_json, ordinal++);

   // ensure all three chains have identical integrity hashes
   verify_integrity_hash<variant_snapshot_suite>(*tester_bin_from_json.control, *tester_bin.control);
   verify_integrity_hash<variant_snapshot_suite>(*tester_bin_from_json.control, *tester_json.control);
   verify_integrity_hash<variant_snapshot_suite>(*tester_json.control, *tester_bin.control);
}

template<typename TESTER, typename SNAPSHOT_SUITE>
void jumbo_row_test()
{
   fc::temp_directory tempdir;
   auto config = tester::default_config(tempdir);
   config.first.state_size = 64*1024*1024;
   TESTER chain(config.first, config.second);
   chain.execute_setup_policy(setup_policy::full);

   chain.create_accounts({"jumbo"_n});
   chain.set_code("jumbo"_n, set_jumbo_row_wast);
   chain.produce_block();

   signed_transaction trx;
   action act;
   act.account = "jumbo"_n;
   act.name = "jumbo"_n;
   act.authorization = vector<permission_level>{{"jumbo"_n,config::active_name}};
   trx.actions.push_back(act);

   chain.set_transaction_headers(trx);
   trx.sign(tester::get_private_key("jumbo"_n, "active"), chain.get_chain_id());
   chain.push_transaction(trx);
   chain.produce_block();

   chain.control->abort_block();

   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   snapshotted_tester sst(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), 0);
}

// Verify that two independently generated snapshots at the same block produce
// identical root hashes (determinism test)
BOOST_AUTO_TEST_CASE( snapshot_determinism_test )
{
   savanna_tester chain;

   chain.create_account("snapshot"_n);
   chain.produce_block();
   chain.set_code("snapshot"_n, test_contracts::snapshot_test_wasm());
   chain.set_abi("snapshot"_n, test_contracts::snapshot_test_abi());
   chain.produce_block();

   // push some actions to create contract state
   for (int i = 0; i < 5; i++) {
      chain.push_action("snapshot"_n, "increment"_n, "snapshot"_n,
                        mutable_variant_object()("value", i + 1));
      chain.produce_block();
   }
   chain.control->abort_block();

   // Take two snapshots from the same chain state
   auto writer1 = threaded_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer1);
   auto snap1_path = threaded_snapshot_suite::finalize(writer1);

   auto writer2 = threaded_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer2);
   auto snap2_path = threaded_snapshot_suite::finalize(writer2);

   // Read back and compare root hashes
   auto reader1 = std::make_shared<threaded_snapshot_reader>(snap1_path);
   auto reader2 = std::make_shared<threaded_snapshot_reader>(snap2_path);

   BOOST_REQUIRE_EQUAL(reader1->get_root_hash().str(), reader2->get_root_hash().str());

   // Verify that the integrity hash matches the snapshot root hash
   auto integrity_hash = chain.control->calculate_integrity_hash();
   BOOST_REQUIRE_EQUAL(integrity_hash.str(), reader1->get_root_hash().str());

   // Also verify the snapshots produce working chains with matching integrity hashes
   int ordinal = 0;
   snapshotted_tester t1(chain.get_config(), reader1, ordinal++);
   reader2 = std::make_shared<threaded_snapshot_reader>(snap2_path);
   snapshotted_tester t2(chain.get_config(), reader2, ordinal++);

   verify_integrity_hash<threaded_snapshot_suite>(*t1.control, *t2.control);
   verify_integrity_hash<threaded_snapshot_suite>(*chain.control, *t1.control);
}

//
// Performance Tests. Enable via RUN_PERF_BENCHMARKS. Take too long for normal test runs.
//

//#define RUN_PERF_BENCHMARKS
#ifdef RUN_PERF_BENCHMARKS
// Performance benchmark: threaded (parallel) vs variant (sequential) snapshot write/read
// Run with: ./unit_test --run_test=snapshot_part2_tests/snapshot_perf_benchmark -- --sys-vm
BOOST_AUTO_TEST_CASE( snapshot_perf_benchmark )
{
   // --- Configuration ---
   constexpr uint32_t num_tables  = 1000;   // number of table_id entries
   constexpr uint32_t rows_per_table = 1000; // key_value rows per table
   constexpr uint32_t value_size  = 512;    // bytes per row value

   const uint32_t total_rows = num_tables * rows_per_table;
   const uint64_t estimated_data = uint64_t(total_rows) * (value_size + 40);
   auto msg = [](const std::string& s) { BOOST_TEST_MESSAGE(s); };
   msg(fmt::format("Benchmark: {} tables x {} rows x {} bytes = ~{} MB estimated data",
       num_tables, rows_per_table, value_size, estimated_data / (1024*1024)));

   // --- Setup chain with data ---
   fc::temp_directory tempdir;
   auto cfg = tester::default_config(tempdir);
   cfg.first.state_size = 1024ull * 1024 * 1024; // 1GB state
   savanna_tester chain(cfg.first, cfg.second);
   chain.produce_block();

   // Directly insert rows into chainbase (bypasses WASM, much faster)
   {
      auto& db = const_cast<chainbase::database&>(chain.control->db());

      // Seed for deterministic pseudo-random data
      uint64_t seed = 0x12345678ABCDEF01ULL;
      auto next_seed = [&seed]() {
         seed ^= seed << 13;
         seed ^= seed >> 7;
         seed ^= seed << 17;
         return seed;
      };

      for (uint32_t tbl = 0; tbl < num_tables; tbl++) {
         // Create a table_id_object
         const auto& tid = db.create<table_id_object>([&](table_id_object& obj) {
            obj.code  = account_name(tbl + 100); // unique code per table
            obj.scope = name("benchmark");
            obj.table = name("data");
            obj.payer = config::system_account_name;
            obj.count = rows_per_table;
         });

         // Create key_value rows for this table
         for (uint32_t r = 0; r < rows_per_table; r++) {
            db.create<key_value_object>([&](key_value_object& obj) {
               obj.t_id = tid.id;
               obj.primary_key = r;
               obj.payer = config::system_account_name;
               obj.value.resize_and_fill(value_size, [&](char* data, std::size_t sz) {
                  for (std::size_t i = 0; i < sz; i += 8) {
                     uint64_t v = next_seed();
                     std::memcpy(data + i, &v, std::min<std::size_t>(8, sz - i));
                  }
               });
            });
         }
      }
   }

   chain.produce_block();
   chain.control->abort_block();

   msg(fmt::format("Chain populated: {} key_value rows in {} tables", total_rows, num_tables));

   // --- Benchmark: Threaded (parallel) snapshot write ---
   auto tp0 = std::chrono::high_resolution_clock::now();
   auto writer_thr = threaded_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer_thr);
   auto snap_thr = threaded_snapshot_suite::finalize(writer_thr);
   auto tp1 = std::chrono::high_resolution_clock::now();

   auto write_thr_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp1 - tp0).count();
   auto snap_size = std::filesystem::file_size(snap_thr);

   msg(fmt::format("Threaded WRITE: {} ms, size: {} bytes ({} MB)", write_thr_ms, snap_size, snap_size / (1024*1024)));

   // --- Benchmark: Threaded (parallel) snapshot read ---
   auto tp2 = std::chrono::high_resolution_clock::now();
   auto reader_thr = threaded_snapshot_suite::get_reader(snap_thr);
   snapshotted_tester snap_chain_thr(chain.get_config(), reader_thr, 0);
   auto tp3 = std::chrono::high_resolution_clock::now();

   auto read_thr_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp3 - tp2).count();
   msg(fmt::format("Threaded READ:  {} ms", read_thr_ms));

   // --- Benchmark: Variant (sequential) snapshot write ---
   auto tp4 = std::chrono::high_resolution_clock::now();
   auto writer_var = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer_var);
   auto snap_var = variant_snapshot_suite::finalize(writer_var);
   auto tp5 = std::chrono::high_resolution_clock::now();

   auto write_var_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp5 - tp4).count();
   msg(fmt::format("Variant WRITE:  {} ms (sequential, in-memory)", write_var_ms));

   // --- Benchmark: Variant (sequential) snapshot read ---
   auto tp6 = std::chrono::high_resolution_clock::now();
   auto reader_var = variant_snapshot_suite::get_reader(snap_var);
   snapshotted_tester snap_chain_var(chain.get_config(), reader_var, 1);
   auto tp7 = std::chrono::high_resolution_clock::now();

   auto read_var_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp7 - tp6).count();
   msg(fmt::format("Variant READ:   {} ms (sequential, in-memory)", read_var_ms));

   // --- Verify integrity ---
   verify_integrity_hash<variant_snapshot_suite>(*chain.control, *snap_chain_thr.control);
   verify_integrity_hash<variant_snapshot_suite>(*chain.control, *snap_chain_var.control);

   // --- Summary ---
   msg("=== BENCHMARK SUMMARY ===");
   msg(fmt::format("Data: {} rows, {} tables, ~{} MB payload", total_rows, num_tables, estimated_data / (1024*1024)));
   msg(fmt::format("Snapshot size: {} MB", snap_size / (1024*1024)));
   double write_speedup = write_thr_ms > 0 ? (double)write_var_ms / write_thr_ms : 0.0;
   double read_speedup  = read_thr_ms > 0 ? (double)read_var_ms / read_thr_ms : 0.0;
   msg(fmt::format("Threaded write: {} ms | Variant write: {} ms | Speedup: {:.2f}x",
       write_thr_ms, write_var_ms, write_speedup));
   msg(fmt::format("Threaded read:  {} ms | Variant read:  {} ms | Speedup: {:.2f}x",
       read_thr_ms, read_var_ms, read_speedup));
}

// Large-scale snapshot benchmark (~40GB, mimicking EOS mainnet section distribution)
// Run with: ./unit_test --run_test=snapshot_part2_tests/snapshot_large_benchmark -- --sys-vm
// Requires ~60GB RAM. Skip variant (would need another 40GB+ for in-memory JSON).
//
// EOS mainnet snapshot section distribution (from snapshot-2026-03-10-19-eos-v8):
//   key_value_object:  262M rows, 20.6 GB (50.7%)  ~82 bytes/row
//   index256_object:   356M rows, 16.3 GB (40.0%)  ~48 bytes/row
//   permission_object:  13M rows,  1.1 GB  (2.7%)
//   table_id_object:    20M rows,  0.7 GB  (1.7%)
//   account_metadata:    6M rows,  0.5 GB  (1.2%)
//   remaining 20 sections: < 1 GB combined
//
// This benchmark populates key_value + index256 + index64 to match the two
// dominant sections while keeping total data ~40GB.
BOOST_AUTO_TEST_CASE( snapshot_large_benchmark )
{
   // Target: ~20.5 GB key_value + ~16 GB index256 + ~0.3 GB index64 ≈ ~37 GB
   // key_value: 20000 tables × 13000 rows × 82 bytes/row ≈ 20 GB
   // index256:  20000 tables × 18000 rows × 48 bytes/row ≈ 16 GB
   // index64:   20000 tables × 650 rows  × 23 bytes/row  ≈ 0.3 GB
   constexpr uint32_t num_tables          = 20000;
   constexpr uint32_t kv_rows_per_table   = 13000;
   constexpr uint32_t kv_value_size       = 50;    // ~82 bytes packed with overhead
   constexpr uint32_t i256_rows_per_table = 18000;
   constexpr uint32_t i64_rows_per_table  = 650;

   const uint64_t total_kv_rows   = uint64_t(num_tables) * kv_rows_per_table;
   const uint64_t total_i256_rows = uint64_t(num_tables) * i256_rows_per_table;
   const uint64_t total_i64_rows  = uint64_t(num_tables) * i64_rows_per_table;
   const uint64_t total_rows      = total_kv_rows + total_i256_rows + total_i64_rows + num_tables;
   const uint64_t est_kv_data     = total_kv_rows * 82;
   const uint64_t est_i256_data   = total_i256_rows * 48;
   const uint64_t est_i64_data    = total_i64_rows * 23;
   const uint64_t estimated_data  = est_kv_data + est_i256_data + est_i64_data;

   auto msg = [](const std::string& s) { BOOST_TEST_MESSAGE(s); };
   msg(fmt::format("Large benchmark: {} tables, {} kv + {} i256 + {} i64 rows = ~{} GB estimated",
       num_tables, total_kv_rows, total_i256_rows, total_i64_rows,
       estimated_data / (1024*1024*1024)));

   fc::temp_directory tempdir;
   auto cfg = tester::default_config(tempdir);
   cfg.first.state_size = 96ull * 1024 * 1024 * 1024; // 96GB state
   savanna_tester chain(cfg.first, cfg.second);
   chain.produce_block();

   // Populate chainbase directly (bypasses WASM)
   auto pop_start = std::chrono::high_resolution_clock::now();
   {
      auto& db = const_cast<chainbase::database&>(chain.control->db());

      uint64_t seed = 0x12345678ABCDEF01ULL;
      auto next_seed = [&seed]() {
         seed ^= seed << 13;
         seed ^= seed >> 7;
         seed ^= seed << 17;
         return seed;
      };

      for (uint32_t tbl = 0; tbl < num_tables; tbl++) {
         const auto& tid = db.create<table_id_object>([&](table_id_object& obj) {
            obj.code  = account_name(tbl + 100);
            obj.scope = name("benchmark");
            obj.table = name("data");
            obj.payer = config::system_account_name;
            obj.count = kv_rows_per_table;
         });

         // key_value rows (~82 bytes each packed)
         for (uint32_t r = 0; r < kv_rows_per_table; r++) {
            db.create<key_value_object>([&](key_value_object& obj) {
               obj.t_id = tid.id;
               obj.primary_key = r;
               obj.payer = config::system_account_name;
               obj.value.resize_and_fill(kv_value_size, [&](char* data, std::size_t sz) {
                  for (std::size_t i = 0; i < sz; i += 8) {
                     uint64_t v = next_seed();
                     std::memcpy(data + i, &v, std::min<std::size_t>(8, sz - i));
                  }
               });
            });
         }

         // index256 rows (~48 bytes each packed)
         for (uint32_t r = 0; r < i256_rows_per_table; r++) {
            __uint128_t lo = next_seed(); lo |= __uint128_t(next_seed()) << 64;
            __uint128_t hi = next_seed(); hi |= __uint128_t(next_seed()) << 64;
            key256_t k256;
            k256[0] = lo;
            k256[1] = hi;
            db.create<index256_object>([&](index256_object& obj) {
               obj.t_id = tid.id;
               obj.primary_key = r;
               obj.payer = config::system_account_name;
               obj.secondary_key = k256;
            });
         }

         // index64 rows (~23 bytes each packed)
         for (uint32_t r = 0; r < i64_rows_per_table; r++) {
            db.create<index64_object>([&](index64_object& obj) {
               obj.t_id = tid.id;
               obj.primary_key = r;
               obj.payer = config::system_account_name;
               obj.secondary_key = next_seed();
            });
         }

         if (tbl % 2000 == 0 && tbl > 0)
            msg(fmt::format("  populated {}/{} tables...", tbl, num_tables));
      }
   }

   chain.produce_block();
   chain.control->abort_block();

   auto pop_end = std::chrono::high_resolution_clock::now();
   auto pop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(pop_end - pop_start).count();
   msg(fmt::format("Chain populated: {} total rows ({} kv, {} i256, {} i64) in {} tables ({} ms)",
       total_rows, total_kv_rows, total_i256_rows, total_i64_rows, num_tables, pop_ms));

   // --- Integrity Hash (run first while state pages are hot) ---
   auto tp_ih0 = std::chrono::high_resolution_clock::now();
   auto integrity_hash = chain.control->calculate_integrity_hash();
   auto tp_ih1 = std::chrono::high_resolution_clock::now();

   auto ihash_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp_ih1 - tp_ih0).count();
   msg(fmt::format("Integrity HASH: {} ms (no I/O)", ihash_ms));

   // --- Threaded WRITE ---
   auto tp0 = std::chrono::high_resolution_clock::now();
   auto writer_thr = threaded_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer_thr);
   auto snap_path = threaded_snapshot_suite::finalize(writer_thr);
   auto tp1 = std::chrono::high_resolution_clock::now();

   auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp1 - tp0).count();
   auto snap_size = std::filesystem::file_size(snap_path);
   double write_throughput = (double)snap_size / (1024*1024) / ((double)write_ms / 1000);

   msg(fmt::format("Threaded WRITE: {} ms, size: {:.2f} GB, throughput: {:.0f} MB/s",
       write_ms, (double)snap_size / (1024*1024*1024), write_throughput));

   // --- Threaded READ ---
   auto tp2 = std::chrono::high_resolution_clock::now();
   auto reader_thr = threaded_snapshot_suite::get_reader(snap_path);
   snapshotted_tester snap_chain(chain.get_config(), reader_thr, 0);
   auto tp3 = std::chrono::high_resolution_clock::now();

   auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp3 - tp2).count();
   double read_throughput = (double)snap_size / (1024*1024) / ((double)read_ms / 1000);

   msg(fmt::format("Threaded READ:  {} ms, throughput: {:.0f} MB/s", read_ms, read_throughput));

   // --- Verify ---
   auto snap_integrity_hash = snap_chain.control->calculate_integrity_hash();
   BOOST_REQUIRE_EQUAL(integrity_hash.str(), snap_integrity_hash.str());
   msg("Integrity hash verified");

   // --- Summary ---
   msg("=== LARGE BENCHMARK SUMMARY ===");
   msg(fmt::format("Snapshot: {:.2f} GB, {} total rows, {} tables",
       (double)snap_size / (1024*1024*1024), total_rows, num_tables));
   msg(fmt::format("Populate: {:.1f}s", (double)pop_ms / 1000));
   msg(fmt::format("IHash:    {:.1f}s (no I/O, serialization + BLAKE3 only)", (double)ihash_ms / 1000));
   msg(fmt::format("Write:    {:.1f}s ({:.0f} MB/s)", (double)write_ms / 1000, write_throughput));
   msg(fmt::format("Read:     {:.1f}s ({:.0f} MB/s)", (double)read_ms / 1000, read_throughput));
}

#endif // RUN_PERF_BENCHMARKS

// --- ^^^^
// --- End Performance Tests

BOOST_AUTO_TEST_SUITE_END()
