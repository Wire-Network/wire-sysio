#include <sstream>

#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/block_log.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/s_root_extension.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/testing/tester.hpp>
#include "snapshot_suites.hpp"
#include <snapshots.hpp>
#include <snapshot_tester.hpp>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>
#include "test_wasts.hpp"

#include <fc/filesystem.hpp>

// Performance benchmarks at the end of this file. Uncomment to enable.
//#define RUN_PERF_BENCHMARKS
#ifdef RUN_PERF_BENCHMARKS
#include <magic_enum/magic_enum.hpp>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace sysio;
using namespace testing;
using namespace chain;

// Minimal reflected row type for the binary-snapshot section-consumption regression test
// (threaded_snapshot_requires_full_section_consumption) in snapshot_part1_tests below.
namespace snapshot_consume_test { struct row { uint64_t v = 0; }; }
FC_REFLECT(snapshot_consume_test::row, (v))


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

// A binary (v1) snapshot section's row_count is stored in the section index, which is NOT covered
// by the integrity/root hash; only the row data (data_size) is. clear_section must therefore reject
// any selected section that is not consumed exactly. The case a per-read check in read_row cannot
// catch is the singleton-reader shape — one read_row with the returned "more" ignored — against a
// row_count tampered upward: the single read succeeds and the callback returns with cur_row <
// num_rows. Writing a genuine 2-row section and then reading only one row reproduces exactly that
// shape, so teardown must throw.
BOOST_AUTO_TEST_CASE(threaded_snapshot_requires_full_section_consumption)
{
   fc::temp_directory tmp_dir;
   const std::filesystem::path snap_path = tmp_dir.path() / "consume_test.bin";

   {
      threaded_snapshot_writer writer(snap_path);
      writer.write_section("test_section", [](auto& s) {
         s.add_row(snapshot_consume_test::row{1});
         s.add_row(snapshot_consume_test::row{2});
      });
      writer.finalize();
   }

   // Full consumption of the 2-row section loads cleanly.
   {
      threaded_snapshot_reader reader(snap_path);
      reader.validate();
      reader.read_section("test_section", [](auto& s) {
         snapshot_consume_test::row r;
         BOOST_CHECK(s.read_row(r));    // row 1 -> more rows remain
         BOOST_CHECK(!s.read_row(r));   // row 2 -> section exhausted
      });
   }

   // Under-consumption (read 1 of 2, ignoring the "more" signal, exactly as a singleton reader does
   // against an upward-tampered row_count) must be rejected at section teardown.
   {
      threaded_snapshot_reader reader(snap_path);
      reader.validate();
      BOOST_CHECK_THROW(
         reader.read_section("test_section", [](auto& s) {
            snapshot_consume_test::row r;
            s.read_row(r); // read only the first of two rows
         }),
         snapshot_exception);
   }
}

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

// Permission parent chains deeper than config::default_max_auth_depth (6) are legal: creation
// (updateauth) never caps chain depth -- only authority *satisfaction* recursion is bounded, by
// the separate max_authority_depth config. A snapshot of such a chain must therefore load. The
// load-time parent-chain walk previously reused default_max_auth_depth as its bound and threw
// snapshot_exception for any honest chain deeper than 6, so every node restarting or onboarding
// from such a snapshot failed.
template<typename TESTER, typename SNAPSHOT_SUITE>
void deep_permission_chain_in_snapshot_test()
{
   TESTER chain;
   const auto deep_account = "deepperms"_n;
   chain.create_account(deep_account);
   chain.produce_block();

   // Build a custom permission chain twice as deep as default_max_auth_depth:
   // active <- deepa <- deepb <- ... (12 links). Letter suffixes only: digits 6-9
   // are not valid name characters.
   constexpr uint32_t chain_depth = 2 * config::default_max_auth_depth;
   std::string parent = "active";
   for (uint32_t i = 0; i < chain_depth; ++i) {
      const std::string perm = std::string("deep") + static_cast<char>('a' + i);
      chain.set_authority(deep_account, name(perm),
                          authority(chain.get_public_key(deep_account, perm)),
                          name(parent));
      parent = perm;
   }
   chain.produce_block();
   chain.control->abort_block();

   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   // Loading must succeed; depth alone is not corruption.
   snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), 0);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);

   // The deepest permission survived the round trip with its parent chain intact.
   const auto& po = snap_chain.control->get_authorization_manager().get_permission(
      permission_level{deep_account, name(parent)});
   BOOST_TEST(po.name == name(parent));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_deep_permission_chain_in_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   deep_permission_chain_in_snapshot_test<savanna_tester, SNAPSHOT_SUITE>();
}

// The producer schedule is in production order, which sysio.system sets by producer location rather
// than by name. The sysio.prods authorities generated from it must still list their accounts in the
// ascending order validate() requires, or a snapshot taken under that schedule fails to load.
template<typename TESTER, typename SNAPSHOT_SUITE>
void unsorted_producer_schedule_in_snapshot_test()
{
   TESTER chain;
   const std::vector<account_name> producers = {"prodc"_n, "proda"_n, "prodb"_n};
   chain.create_accounts(producers);
   chain.set_producers(producers);

   const auto active_names = [&] {
      std::vector<account_name> names;
      for (const auto& p : chain.control->active_producers().producers) names.push_back(p.producer_name);
      return names;
   };
   constexpr uint32_t max_blocks = 200;
   for (uint32_t produced = 0; produced < max_blocks && active_names() != producers; ++produced) {
      chain.produce_block();
   }
   const auto schedule = active_names();
   BOOST_REQUIRE_EQUAL_COLLECTIONS(schedule.begin(), schedule.end(), producers.begin(), producers.end());

   for (const auto permission : {config::active_name, config::majority_producers_permission_name,
                                 config::minority_producers_permission_name}) {
      const auto auth = chain.control->get_authorization_manager()
                           .get_permission(permission_level{config::producers_account_name, permission})
                           .auth.to_authority();
      BOOST_TEST(auth.accounts.size() == producers.size());
      BOOST_REQUIRE_MESSAGE(validate(auth), "sysio.prods@" << permission << " is not a valid authority");
   }

   chain.control->abort_block();
   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), 0);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_kv_index_keys_in_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   savanna_tester chain;
   const account_name probe = "kvprobe"_n;
   chain.create_account(probe);
   chain.produce_block();
   chain.set_code(probe, test_contracts::kv_intrinsic_probe_wasm());
   chain.set_abi(probe, test_contracts::kv_intrinsic_probe_abi());
   chain.produce_block();

   // An entry with an empty key must never reach state: the snapshot writer refuses it.
   BOOST_CHECK_THROW(chain.push_action(probe, "zseckey"_n, probe, mutable_variant_object()),
                     kv_empty_key);
   BOOST_CHECK_THROW(chain.push_action(probe, "zprikey"_n, probe, mutable_variant_object()), kv_empty_key);

   chain.push_action(probe, "idxminkeep"_n, probe, mutable_variant_object());
   chain.produce_block();
   chain.control->abort_block();

   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   int ordinal = 1;
   snapshotted_tester snap_chain(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *snap_chain.control);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_unsorted_producer_schedule_in_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   unsorted_producer_schedule_in_snapshot_test<savanna_tester, SNAPSHOT_SUITE>();
}

// A permission row carrying a structurally invalid authority -- here a duplicated key entry,
// which both breaks the strict key ordering updateauth enforces at creation and double-counts
// that key's weight toward threshold -- must be rejected at snapshot load rather than silently
// reconstructed into chain state. Uses the variant snapshot format so the row can be mutated.
BOOST_AUTO_TEST_CASE(test_invalid_authority_in_snapshot_rejected)
{
   savanna_tester chain;
   const auto victim = "victim"_n;
   chain.create_account(victim);
   chain.produce_block();
   chain.control->abort_block();

   auto writer = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = variant_snapshot_suite::finalize(writer);

   // Rebuild the snapshot variant with victim@active's first key entry duplicated.
   const std::string perm_section = chain::detail::snapshot_section_traits<permission_object>::section_name();
   uint32_t mutated_rows = 0;
   fc::variants new_sections;
   for (const auto& section : snapshot["sections"].get_array()) {
      if (section["name"].as_string() != perm_section) {
         new_sections.push_back(section);
         continue;
      }
      fc::variants new_rows;
      for (const auto& row : section["rows"].get_array()) {
         const auto& ro = row.get_object();
         if (ro["owner"].as_string() == victim.to_string() && ro["name"].as_string() == "active") {
            fc::variants keys = ro["auth"]["keys"].get_array();
            BOOST_REQUIRE(!keys.empty());
            keys.push_back(keys.front());
            new_rows.emplace_back(
               fc::mutable_variant_object(ro)
                  ("auth", fc::mutable_variant_object(ro["auth"].get_object())("keys", std::move(keys))));
            ++mutated_rows;
         } else {
            new_rows.push_back(row);
         }
      }
      new_sections.emplace_back(fc::mutable_variant_object()("name", section["name"])("rows", std::move(new_rows)));
   }
   BOOST_REQUIRE_EQUAL(mutated_rows, 1u);
   fc::variant mutated = fc::mutable_variant_object()
      ("version", snapshot["version"])
      ("sections", std::move(new_sections));

   // The exception must point at the mutated row -- not at some other permission tripping the
   // check incidentally (e.g. sysio.null's deliberately empty authority, which is exempt).
   BOOST_REQUIRE_EXCEPTION(
      snapshotted_tester(chain.get_config(), variant_snapshot_suite::get_reader(mutated), 0),
      snapshot_exception,
      [](const snapshot_exception& e) {
         return e.to_detail_string().find("victim@active has invalid authority") != std::string::npos;
      });
}

// The empty-authority exemption is scoped to the three rows genesis deliberately creates
// unsatisfiable (sysio.null@owner, sysio.null@active, sysio.prods@owner). newaccount and
// updateauth reject empty authorities everywhere else via validate(), so a hand-crafted snapshot
// row carrying one on any other permission -- or carrying a non-genesis value on an exempt row --
// must be rejected rather than loaded past the structural checks.
BOOST_AUTO_TEST_CASE(test_unscoped_empty_authority_in_snapshot_rejected)
{
   savanna_tester chain;
   const auto victim = "victim"_n;
   chain.create_account(victim);
   chain.produce_block();
   chain.control->abort_block();

   auto writer = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = variant_snapshot_suite::finalize(writer);

   // Rebuild the snapshot variant with <owner>@<perm>'s auth object rewritten by `mutate`.
   const std::string perm_section = chain::detail::snapshot_section_traits<permission_object>::section_name();
   auto mutate_auth = [&](const std::string& owner, const std::string& perm, auto&& mutate) -> fc::variant {
      uint32_t mutated_rows = 0;
      fc::variants new_sections;
      for (const auto& section : snapshot["sections"].get_array()) {
         if (section["name"].as_string() != perm_section) {
            new_sections.push_back(section);
            continue;
         }
         fc::variants new_rows;
         for (const auto& row : section["rows"].get_array()) {
            const auto& ro = row.get_object();
            if (ro["owner"].as_string() == owner && ro["name"].as_string() == perm) {
               new_rows.emplace_back(fc::mutable_variant_object(ro)("auth", mutate(ro["auth"].get_object())));
               ++mutated_rows;
            } else {
               new_rows.push_back(row);
            }
         }
         new_sections.emplace_back(fc::mutable_variant_object()("name", section["name"])("rows", std::move(new_rows)));
      }
      BOOST_REQUIRE_EQUAL(mutated_rows, 1u);
      return fc::mutable_variant_object()
         ("version", snapshot["version"])
         ("sections", std::move(new_sections));
   };
   auto empty_auth = [](const fc::variant_object& auth) {
      return fc::mutable_variant_object(auth)("threshold", 1)("keys", fc::variants{})("accounts", fc::variants{});
   };
   auto expect_rejected = [&](const fc::variant& mutated, int ordinal, const std::string& msg) {
      BOOST_REQUIRE_EXCEPTION(
         snapshotted_tester(chain.get_config(), variant_snapshot_suite::get_reader(mutated), ordinal),
         snapshot_exception,
         [&](const snapshot_exception& e) {
            return e.to_detail_string().find(msg) != std::string::npos;
         });
   };

   // Exactly the empty shape genesis gives sysio.null -- but on a normal account: rejected.
   expect_rejected(mutate_auth(victim.to_string(), "active", empty_auth), 0,
                   "victim@active has invalid authority");

   // Empty authority on a non-exempt permission of an exempt-listed account: only
   // sysio.prods@owner is genesis-empty; @active must stay subject to the structural check.
   expect_rejected(mutate_auth("sysio.prods", "active", empty_auth), 1,
                   "sysio.prods@active has invalid authority");

   // An exempt row must carry exactly the genesis value: bumping the threshold of the
   // (legitimately empty) sysio.null@owner row is corruption, not a wider exemption.
   expect_rejected(mutate_auth("sysio.null", "owner",
                               [](const fc::variant_object& auth) {
                                  return fc::mutable_variant_object(auth)("threshold", 2);
                               }),
                   2, "sysio.null@owner has invalid authority");
}

// Permission parent integrity at load is guaranteed by name resolution against already-loaded
// rows (ids strictly decrease toward the root, so every ancestor precedes its children): a row
// whose parent cannot be resolved is corrupt -- a dangling reference, or a parent cycle, which
// necessarily places some parent after its child in row order. Both shapes must be rejected
// with an error naming the offending row. Uses the variant format so rows can be mutated.
BOOST_AUTO_TEST_CASE(test_corrupt_permission_parent_in_snapshot_rejected)
{
   savanna_tester chain;
   const auto victim = "victim"_n;
   chain.create_account(victim);
   // Two custom permissions: custa under active, custb under custa.
   chain.set_authority(victim, "custa"_n, authority(chain.get_public_key(victim, "custa")), "active"_n);
   chain.set_authority(victim, "custb"_n, authority(chain.get_public_key(victim, "custb")), "custa"_n);
   chain.produce_block();
   chain.control->abort_block();

   auto writer = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = variant_snapshot_suite::finalize(writer);

   // Rebuild the snapshot variant with victim@<perm>'s parent name rewritten.
   const std::string perm_section = chain::detail::snapshot_section_traits<permission_object>::section_name();
   auto mutate_parent = [&](const std::string& perm, const std::string& new_parent) -> fc::variant {
      uint32_t mutated_rows = 0;
      fc::variants new_sections;
      for (const auto& section : snapshot["sections"].get_array()) {
         if (section["name"].as_string() != perm_section) {
            new_sections.push_back(section);
            continue;
         }
         fc::variants new_rows;
         for (const auto& row : section["rows"].get_array()) {
            const auto& ro = row.get_object();
            if (ro["owner"].as_string() == victim.to_string() && ro["name"].as_string() == perm) {
               new_rows.emplace_back(fc::mutable_variant_object(ro)("parent", new_parent));
               ++mutated_rows;
            } else {
               new_rows.push_back(row);
            }
         }
         new_sections.emplace_back(fc::mutable_variant_object()("name", section["name"])("rows", std::move(new_rows)));
      }
      BOOST_REQUIRE_EQUAL(mutated_rows, 1u);
      return fc::mutable_variant_object()
         ("version", snapshot["version"])
         ("sections", std::move(new_sections));
   };

   /// Load `mutated`, require that it throws snapshot_exception, and return the exception's detail string. The caller
   /// matches it with BOOST_REQUIRE_MESSAGE so a mismatch prints what was thrown; BOOST_REQUIRE_EXCEPTION prints only
   /// its predicate's source.
   auto rejected_detail = [&](const fc::variant& mutated, uint32_t ordinal) {
      try {
         snapshotted_tester(chain.get_config(), variant_snapshot_suite::get_reader(mutated), ordinal);
      } catch (const snapshot_exception& e) {
         return e.to_detail_string();
      }
      BOOST_FAIL("corrupt snapshot was accepted; expected a snapshot_exception");
      return std::string{};
   };

   // Parent cycle: custa's parent rewritten to custb while custb's parent is custa. custa was created first and rows
   // load in id order, so the load must fail at custa; a rejection naming custb would mean rows left creation order.
   const std::string cyclic_detail = rejected_detail(mutate_parent("custa", "custb"), 0);
   BOOST_REQUIRE_MESSAGE(
      cyclic_detail.find("victim@custa references parent custb") != std::string::npos,
      "expected the cycle to be rejected at victim@custa, got: " << cyclic_detail);

   // Dangling parent: references a permission name that exists nowhere in the snapshot.
   const std::string dangling_detail = rejected_detail(mutate_parent("custb", "nosuchperm"), 1);
   BOOST_REQUIRE_MESSAGE(
      dangling_detail.find("victim@custb references parent nosuchperm") != std::string::npos,
      "expected a dangling-parent rejection naming victim@custb, got: " << dangling_detail);
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

/// Rows the jumborow contract stores, each just under the default value limit.
static constexpr int64_t jumbo_row_count = 132;

template<typename TESTER, typename SNAPSHOT_SUITE>
void jumbo_row_test()
{
   fc::temp_directory tempdir;
   auto config = tester::default_config(tempdir);
   config.first.state_size = 64*1024*1024;
   TESTER chain(config.first, config.second);
   chain.execute_setup_policy(setup_policy::full);

   // The default policy buys about 10 MiB of RAM, and these rows need about 34 MiB.
   chain.create_accounts({"jumbo"_n}, false, true, false);
   chain.add_roa_policy(chain.NODE_DADDY, "jumbo"_n, "0.0010 SYS", "0.0010 SYS", "40.0000 SYS", 0, 0);
   chain.set_code("jumbo"_n, test_contracts::jumborow_wasm());
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

   // Every row comes back, and the state hashes match.
   const auto row_count = [](const auto& c) {
      const auto code   = "jumbo"_n;
      const auto cursor = c.control->kv().open_row_cursor();
      int64_t    count  = 0;
      kv_row     row;
      for (bool more = cursor->seek_lower({code}, row); more && row.code == code; more = cursor->next(row)) ++count;
      return count;
   };
   BOOST_REQUIRE_EQUAL(row_count(chain), jumbo_row_count);
   BOOST_REQUIRE_EQUAL(row_count(sst), jumbo_row_count);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *sst.control);
}

// Rows near the value limit, adding up to more than half of a 64 MiB state database.
BOOST_AUTO_TEST_CASE_TEMPLATE(test_jumbo_row, SNAPSHOT_SUITE, snapshot_suites)
{
   jumbo_row_test<savanna_tester, SNAPSHOT_SUITE>();
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

namespace {
   /// A KV row as these tests write it down. The fields the snapshot order depends on, nothing else.
   struct kv_row_spec {
      account_name code;
      uint16_t     table_id = 0;
      std::string  key;

      bool operator==(const kv_row_spec&) const = default;
   };

   /// A KV secondary index entry as these tests write it down.
   struct kv_entry_spec {
      account_name code;
      uint16_t     table_id = 0;
      std::string  sec_key;
      std::string  pri_key;

      bool operator==(const kv_entry_spec&) const = default;
   };

   /// Which way round insert_kv_rows inserts its rows.
   enum class kv_insertion_order { key_order, reverse_key_order };

   /// Rows in the order a snapshot must hold them, spelled out so a second storage engine has a literal to match.
   std::vector<kv_row_spec> kv_rows_in_key_order() {
      using namespace std::string_literals;
      return {
         {"alice"_n, 1, "\x01"s},                                 // a shorter key sorts before its extensions
         {"alice"_n, 1, "\x01\x00\x00\x00\x00\x00\x00\x00"s},     // 8-byte keys take the integer fast path
         {"alice"_n, 1, "\x01\x00\x00\x00\x00\x00\x00\x01"s},
         {"alice"_n, 1, "\x01\x00\x00\x00\x00\x00\x00\x01\x00"s}, // 9 bytes: past the fast path
         {"alice"_n, 1, "\x7f"s},
         {"alice"_n, 1, "\x7f\xff\xff\xff\xff\xff\xff\xff"s},     // the fast path compares unsigned as well
         {"alice"_n, 1, "\x80"s},                                 // key bytes compare unsigned
         {"alice"_n, 1, "\x80\x00\x00\x00\x00\x00\x00\x00"s},
         {"alice"_n, 1, "\xff"s},
         {"alice"_n, 1, "\xff\xff\xff\xff\xff\xff\xff\xff"s},
         {"alice"_n, 2, "\x00"s},                                 // table_id outranks key
         {"bob"_n,   1, "\x00"s},                                 // code outranks table_id
      };
   }

   /// Index entries in the order a snapshot must hold them.
   std::vector<kv_entry_spec> kv_entries_in_key_order() {
      using namespace std::string_literals;
      return {
         {"alice"_n, 1, "a"s,    "z"s}, // ("a", "z") sorts before ("ab", "a"): the pair compares, not the joined bytes
         {"alice"_n, 1, "ab"s,   "a"s},
         {"alice"_n, 1, "ab"s,   "b"s},
         {"alice"_n, 1, "\x7f"s, "a"s},
         {"alice"_n, 1, "\x7f\xff\xff\xff\xff\xff\xff\xff"s, "a"s}, // the fast path compares unsigned on sec_key
         {"alice"_n, 1, "\x80"s, "a"s}, // key bytes compare unsigned
         {"alice"_n, 1, "\x80\x00\x00\x00\x00\x00\x00\x00"s, "a"s},
         {"alice"_n, 1, "\xff"s, "\x7f\xff\xff\xff\xff\xff\xff\xff"s}, // and on pri_key
         {"alice"_n, 1, "\xff"s, "\x80\x00\x00\x00\x00\x00\x00\x00"s},
         {"alice"_n, 1, "\xff"s, "\xff\xff\xff\xff\xff\xff\xff\xff"s},
         {"alice"_n, 1, "\xff\xff\xff\xff\xff\xff\xff\xff"s, "a"s},
         {"alice"_n, 2, "a"s,    "a"s}, // table_id outranks sec_key
         {"bob"_n,   1, "a"s,    "a"s}, // code outranks table_id
      };
   }

   /**
    * Writes the fixture rows straight into chainbase, between blocks.
    *
    * Two chains can then differ in insertion order and nothing else. Transactions would change more: the integrity
    * hash also covers the head block and the block summaries, so chains built from different transactions never match.
    */
   void insert_kv_rows(controller& control, kv_insertion_order order) {
      auto& store = const_cast<kv_store&>(control.kv());
      auto rows = kv_rows_in_key_order();
      auto entries = kv_entries_in_key_order();
      if (order == kv_insertion_order::reverse_key_order) {
         std::ranges::reverse(rows);
         std::ranges::reverse(entries);
      }
      for (const auto& row : rows) {
         store.insert_row(row.code, row.table_id, row.code, row.key, {});
      }
      for (const auto& entry : entries) {
         store.insert_index_entry(entry.code, entry.table_id, entry.code, entry.sec_key, entry.pri_key);
      }
   }

   /// The rows of one section of a variant snapshot, in snapshot order.
   template<typename Object>
   fc::variants kv_section_rows(const fc::variant& snapshot) {
      const auto section_name = sysio::chain::detail::snapshot_section_traits<Object>::section_name();
      for (const auto& section : snapshot["sections"].get_array()) {
         if (section["name"].as_string() == section_name) {
            return section["rows"].get_array();
         }
      }
      BOOST_FAIL("snapshot has no section " + section_name);
      return {};
   }

   /// Builds a chain through real transactions, inserting its rows in the given order, and snapshots it.
   fc::variant snapshot_of_kv_transactions(kv_insertion_order order) {
      savanna_tester chain;
      std::vector<account_name> contracts = {"snapshot"_n, "snapshot1"_n};
      std::vector<uint64_t> ids = {1, 2, 3, 4, 5};

      chain.create_accounts(contracts);
      chain.produce_block();
      for (const auto& contract : contracts) {
         chain.set_code(contract, test_contracts::snapshot_test_wasm());
         chain.set_abi(contract, test_contracts::snapshot_test_abi());
      }
      chain.produce_block();

      if (order == kv_insertion_order::reverse_key_order) {
         std::ranges::reverse(contracts);
         std::ranges::reverse(ids);
      }
      for (const auto& contract : contracts) {
         // increment writes a row with 5 secondary index entries, so both KV sections take rows here.
         chain.push_action(contract, "increment"_n, contract, mutable_variant_object()("value", 1));
         for (const uint64_t id : ids) {
            chain.push_action(contract, "add"_n, contract, mutable_variant_object()
               ("scope", "scope"_n)
               ("id", id)
               ("payload", fc::sha256::hash(std::to_string(id))));
         }
      }
      chain.produce_block();
      chain.control->abort_block();

      auto writer = variant_snapshot_suite::get_writer();
      chain.control->write_snapshot(writer);
      return variant_snapshot_suite::finalize(writer);
   }
}

// Two chains holding the same KV rows must hash the same however the rows got there. Otherwise attested hashes would
// record insertion history, which a storage engine other than chainbase could not reproduce.
BOOST_AUTO_TEST_CASE_TEMPLATE(test_kv_insertion_order_does_not_change_integrity_hash, SNAPSHOT_SUITE, snapshot_suites)
{
   savanna_tester chain;
   chain.produce_block();
   chain.control->abort_block();

   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   // Two children of one snapshot differ only in what this test does to them next.
   int ordinal = 0;
   snapshotted_tester in_order(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++);
   snapshotted_tester reversed(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), ordinal++);
   verify_integrity_hash<SNAPSHOT_SUITE>(*in_order.control, *reversed.control);

   insert_kv_rows(*in_order.control, kv_insertion_order::key_order);
   insert_kv_rows(*reversed.control, kv_insertion_order::reverse_key_order);
   verify_integrity_hash<SNAPSHOT_SUITE>(*in_order.control, *reversed.control);

   // Loading renumbers the rows in snapshot order; the hash must not notice.
   auto reversed_writer = SNAPSHOT_SUITE::get_writer();
   reversed.control->write_snapshot(reversed_writer);
   auto reversed_snapshot = SNAPSHOT_SUITE::finalize(reversed_writer);
   snapshotted_tester loaded(chain.get_config(), SNAPSHOT_SUITE::get_reader(reversed_snapshot), ordinal++);
   verify_integrity_hash<SNAPSHOT_SUITE>(*in_order.control, *loaded.control);
}

// Pins the order itself, not just agreement between two chains: both chains could agree on a wrong order.
BOOST_AUTO_TEST_CASE( kv_snapshot_sections_are_in_key_order )
{
   savanna_tester chain;
   chain.produce_block();
   chain.control->abort_block();
   insert_kv_rows(*chain.control, kv_insertion_order::reverse_key_order);

   auto writer = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   const auto snapshot = variant_snapshot_suite::finalize(writer);

   // The chain's own rows sit in the same sections; only the fixture's accounts are of interest here.
   const auto from_fixture = [](account_name code) { return code == "alice"_n || code == "bob"_n; };

   std::vector<kv_row_spec> rows;
   for (const auto& v : kv_section_rows<kv_object>(snapshot)) {
      const auto row = v.as<snapshot_kv_object>();
      if (from_fixture(row.code)) {
         rows.push_back({row.code, row.table_id, std::string(row.key.begin(), row.key.end())});
      }
   }
   BOOST_TEST((rows == kv_rows_in_key_order()));

   std::vector<kv_entry_spec> entries;
   for (const auto& v : kv_section_rows<kv_index_object>(snapshot)) {
      const auto entry = v.as<snapshot_kv_index_object>();
      if (from_fixture(entry.code)) {
         entries.push_back({entry.code, entry.table_id, std::string(entry.sec_key.begin(), entry.sec_key.end()),
                            std::string(entry.pri_key.begin(), entry.pri_key.end())});
      }
   }
   BOOST_TEST((entries == kv_entries_in_key_order()));
}

// The same property for rows that come out of kv_set and kv_idx_store on a live chain. The transactions differ, so the
// blocks and the integrity hashes do too: compare the KV sections those hashes cover.
BOOST_AUTO_TEST_CASE( kv_sections_do_not_depend_on_transaction_order )
{
   const auto in_order = snapshot_of_kv_transactions(kv_insertion_order::key_order);
   const auto reversed = snapshot_of_kv_transactions(kv_insertion_order::reverse_key_order);

   const auto rows = kv_section_rows<kv_object>(in_order);
   const auto entries = kv_section_rows<kv_index_object>(in_order);
   BOOST_REQUIRE(!rows.empty());
   BOOST_REQUIRE(!entries.empty());

   const auto as_text = [](const fc::variants& section) {
      return fc::json::to_string(section, fc::time_point::maximum());
   };
   BOOST_TEST(as_text(rows) == as_text(kv_section_rows<kv_object>(reversed)));
   BOOST_TEST(as_text(entries) == as_text(kv_section_rows<kv_index_object>(reversed)));
}

namespace {
   /// `snapshot` with `row` appended to the section of `Object`.
   template<typename Object>
   fc::variant with_kv_row(const fc::variant& snapshot, const fc::variant& row) {
      const auto section_name = sysio::chain::detail::snapshot_section_traits<Object>::section_name();
      fc::variants sections = snapshot["sections"].get_array();
      bool found = false;
      for (auto& section : sections) {
         if (section["name"].as_string() != section_name) continue;
         fc::variants rows = section["rows"].get_array();
         rows.push_back(row);
         section = fc::mutable_variant_object(section)("rows", std::move(rows));
         found = true;
      }
      BOOST_REQUIRE(found);
      return fc::mutable_variant_object(snapshot)("sections", std::move(sections));
   }

   /// A hand-built KV row with keys and values of the given sizes, in the form a variant snapshot holds it.
   fc::variant kv_row_of_size(size_t key_size, size_t value_size) {
      fc::variant row;
      fc::to_variant(snapshot_kv_object{.code = "alice"_n, .payer = "alice"_n, .key = std::vector<char>(key_size, 'k'),
                                        .value = std::vector<char>(value_size, 'v'), .table_id = 1}, row);
      return row;
   }

   /// A hand-built KV index entry with keys of the given sizes, in the form a variant snapshot holds it.
   fc::variant kv_entry_of_size(size_t sec_key_size, size_t pri_key_size) {
      fc::variant entry;
      fc::to_variant(snapshot_kv_index_object{.code = "alice"_n, .payer = "alice"_n,
                                              .sec_key = std::vector<char>(sec_key_size, 's'),
                                              .pri_key = std::vector<char>(pri_key_size, 'p'), .table_id = 1}, entry);
      return entry;
   }
}

// A snapshot comes from outside the node, so the reader holds rows to the limits the intrinsics hold writes to. It uses
// the absolute limits: the governance limits may have been higher when a row was written.
BOOST_AUTO_TEST_CASE( kv_snapshot_reader_enforces_row_limits )
{
   savanna_tester chain;
   chain.produce_block();
   chain.control->abort_block();

   auto writer = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   const auto snapshot = variant_snapshot_suite::finalize(writer);

   constexpr size_t max_key   = config::max_kv_key_size_limit;
   constexpr size_t max_value = config::max_kv_value_size_limit;

   int ordinal = 0;
   const auto load = [&](const fc::variant& edited) {
      snapshotted_tester loaded(chain.get_config(), variant_snapshot_suite::get_reader(edited), ordinal++);
      const auto& kv = loaded.control->kv();
      return std::make_pair(kv.row_count(), kv.index_entry_count());
   };
   const auto check_refused = [&](const fc::variant& edited, const std::string& reason) {
      BOOST_CHECK_EXCEPTION(load(edited), snapshot_validation_exception, fc_exception_message_contains(reason));
   };

   // At the limits a hand-built row loads, so a refusal below comes from the row and not from the way it was built.
   // These rows go in last and their code sorts first, so this also shows the reader does not require key order.
   const auto rows_before    = kv_section_rows<kv_object>(snapshot).size();
   const auto entries_before = kv_section_rows<kv_index_object>(snapshot).size();
   BOOST_TEST(load(with_kv_row<kv_object>(snapshot, kv_row_of_size(max_key, max_value))).first == rows_before + 1);
   BOOST_TEST(load(with_kv_row<kv_index_object>(snapshot, kv_entry_of_size(max_key, max_key))).second ==
              entries_before + 1);

   check_refused(with_kv_row<kv_object>(snapshot, kv_row_of_size(0, 1)), "kv_object has empty key");
   check_refused(with_kv_row<kv_object>(snapshot, kv_row_of_size(max_key + 1, 1)), "kv_object key size");
   check_refused(with_kv_row<kv_object>(snapshot, kv_row_of_size(1, max_value + 1)), "kv_object value size");

   check_refused(with_kv_row<kv_index_object>(snapshot, kv_entry_of_size(0, 1)),
                 "kv_index_object has empty secondary key");
   check_refused(with_kv_row<kv_index_object>(snapshot, kv_entry_of_size(1, 0)),
                 "kv_index_object has empty primary key");
   check_refused(with_kv_row<kv_index_object>(snapshot, kv_entry_of_size(max_key + 1, 1)),
                 "kv_index_object secondary key size");
   check_refused(with_kv_row<kv_index_object>(snapshot, kv_entry_of_size(1, max_key + 1)),
                 "kv_index_object primary key size");

   // Two rows at one key cannot be held by any engine, so the reader refuses the second.
   const auto row = kv_row_of_size(1, 1);
   check_refused(with_kv_row<kv_object>(with_kv_row<kv_object>(snapshot, row), row), "kv_object key appears twice");
   const auto entry = kv_entry_of_size(1, 1);
   check_refused(with_kv_row<kv_index_object>(with_kv_row<kv_index_object>(snapshot, entry), entry),
                 "kv_index_object position appears twice");
}

// N4: a file's rows need not be in key order. Loading one gives the same state, and a chain writes key order back.
BOOST_AUTO_TEST_CASE( kv_snapshot_out_of_order_loads_to_the_same_state )
{
   savanna_tester chain;
   chain.produce_block();
   chain.control->abort_block();
   insert_kv_rows(*chain.control, kv_insertion_order::key_order);

   auto writer = variant_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   const auto in_order = variant_snapshot_suite::finalize(writer);

   /// `snapshot` with the rows of the section of `Object` reversed.
   const auto reversed_section = [](const fc::variant& snapshot, const std::string& section_name) {
      fc::variants sections = snapshot["sections"].get_array();
      for (auto& section : sections) {
         if (section["name"].as_string() != section_name) continue;
         fc::variants rows = section["rows"].get_array();
         std::ranges::reverse(rows);
         section = fc::mutable_variant_object(section)("rows", std::move(rows));
      }
      return fc::variant(fc::mutable_variant_object(snapshot)("sections", std::move(sections)));
   };
   using sysio::chain::detail::snapshot_section_traits;
   const auto out_of_order = reversed_section(reversed_section(in_order, snapshot_section_traits<kv_object>::section_name()),
                                              snapshot_section_traits<kv_index_object>::section_name());

   // The edit changed what the file holds, so the rest of this means something.
   const auto as_text = [](const fc::variants& section) { return fc::json::to_string(section, fc::time_point::maximum()); };
   BOOST_REQUIRE(as_text(kv_section_rows<kv_object>(out_of_order)) != as_text(kv_section_rows<kv_object>(in_order)));
   BOOST_REQUIRE(as_text(kv_section_rows<kv_index_object>(out_of_order)) !=
                 as_text(kv_section_rows<kv_index_object>(in_order)));

   int ordinal = 0;
   snapshotted_tester from_order(chain.get_config(), variant_snapshot_suite::get_reader(in_order), ordinal++);
   snapshotted_tester from_disorder(chain.get_config(), variant_snapshot_suite::get_reader(out_of_order), ordinal++);
   verify_integrity_hash<variant_snapshot_suite>(*from_order.control, *from_disorder.control);

   from_disorder.control->abort_block();
   auto rewriter = variant_snapshot_suite::get_writer();
   from_disorder.control->write_snapshot(rewriter);
   const auto written_back = variant_snapshot_suite::finalize(rewriter);
   BOOST_TEST(as_text(kv_section_rows<kv_object>(written_back)) == as_text(kv_section_rows<kv_object>(in_order)));
   BOOST_TEST(as_text(kv_section_rows<kv_index_object>(written_back)) ==
              as_text(kv_section_rows<kv_index_object>(in_order)));
}

namespace {
   /// `size` bytes counting up from `seed`, wrapping at 256, as kv_intrinsic_probe's pattern() makes them.
   std::string size_class_bytes(uint32_t seed, uint32_t size) {
      std::string bytes(size, '\0');
      for (uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(seed + i);
      return bytes;
   }
}

// A round trip of every size a length prefix treats differently, up to the caps: the 1, 2 and 3 byte varint boundaries,
// the 64 KiB line, and rows at 1 MiB and keys at 512 bytes. 256 bytes or more hold every byte value.
BOOST_AUTO_TEST_CASE_TEMPLATE(test_kv_size_classes, SNAPSHOT_SUITE, snapshot_suites)
{
   static constexpr uint16_t rows_table = 598, entries_table = 599; // tid_sizes and tid_sizes_sec in the contract
   static constexpr std::array<uint32_t, 12> value_sizes = {0, 1, 127, 128, 255, 256, 16383, 16384, 65535, 65536, 262144,
                                                            1048576};
   static constexpr std::array<uint32_t, 5> key_sizes   = {1, 8, 9, 255, 512};
   static constexpr std::array<uint32_t, 3> entry_sizes = {1, 8, 512};
   static constexpr uint32_t key_seed = 100, entry_seed = 200; // keeps the keys of each group apart
   const account_name contract = "kvsizes"_n;

   fc::temp_directory tempdir;
   auto cfg = tester::default_config(tempdir);
   cfg.first.state_size = 64*1024*1024;
   savanna_tester chain(cfg.first, cfg.second);
   chain.execute_setup_policy(setup_policy::full);

   auto params = chain.control->get_global_properties().configuration;
   params.max_kv_key_size           = config::max_kv_key_size_limit;
   params.max_kv_secondary_key_size = config::max_kv_key_size_limit;
   params.max_kv_value_size         = config::max_kv_value_size_limit;
   chain.push_action(config::system_account_name, "setparams"_n, config::system_account_name,
                     mutable_variant_object()("params", params));
   chain.create_accounts({contract}, false, true, false);
   chain.add_roa_policy(chain.NODE_DADDY, contract, "0.0010 SYS", "0.0010 SYS", "10.0000 SYS", 0, 0);
   chain.set_code(contract, test_contracts::kv_intrinsic_probe_wasm());
   chain.set_abi(contract, test_contracts::kv_intrinsic_probe_abi().c_str());
   chain.produce_block();

   struct row   { std::string key, value; };
   struct entry { std::string sec_key, pri_key; };
   std::vector<row>   rows;
   std::vector<entry> entries;
   const auto write_row = [&](uint32_t seed, uint32_t key_size, uint32_t value_size) {
      chain.push_action(contract, "sizerow"_n, contract,
                        mutable_variant_object()("seed", seed)("key_size", key_size)("value_size", value_size));
      rows.push_back({size_class_bytes(seed, key_size), size_class_bytes(seed, value_size)});
   };
   for (uint32_t i = 0; i < value_sizes.size(); ++i) write_row(i, sizeof(uint64_t), value_sizes[i]);
   for (uint32_t i = 0; i < key_sizes.size(); ++i) write_row(key_seed + i, key_sizes[i], 1);
   uint32_t seed = entry_seed;
   for (uint32_t sec_size : entry_sizes) {
      for (uint32_t pri_size : entry_sizes) {
         chain.push_action(contract, "sizeentry"_n, contract,
                           mutable_variant_object()("seed", seed)("sec_size", sec_size)("pri_size", pri_size));
         entries.push_back({size_class_bytes(seed, sec_size), size_class_bytes(seed, pri_size)});
         ++seed;
      }
   }
   chain.produce_block();
   chain.control->abort_block();

   auto writer = SNAPSHOT_SUITE::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = SNAPSHOT_SUITE::finalize(writer);

   snapshotted_tester loaded(chain.get_config(), SNAPSHOT_SUITE::get_reader(snapshot), 0);
   verify_integrity_hash<SNAPSHOT_SUITE>(*chain.control, *loaded.control);

   // Both integrity hashes go through the same row writer, so a field it drops matches. Read the rows themselves.
   const auto& kv = loaded.control->kv();
   for (const auto& expected : rows) {
      const auto found = kv.find_row(contract, rows_table, expected.key);
      BOOST_REQUIRE(found.has_value());
      BOOST_TEST((found->value == expected.value), "value of " << expected.value.size() << " bytes");
      BOOST_TEST((found->payer == contract), "payer of a " << expected.key.size() << " byte key");
   }
   for (const auto& expected : entries) {
      const auto found = kv.find_index_entry(contract, entries_table, expected.sec_key, expected.pri_key);
      BOOST_REQUIRE_MESSAGE(found.has_value(),
                            "entry of " << expected.sec_key.size() << " and " << expected.pri_key.size() << " bytes");
      BOOST_TEST((found->payer == contract),
                 "payer of an entry of " << expected.sec_key.size() << " and " << expected.pri_key.size() << " bytes");
   }
}

//
// Performance Tests. Enable via RUN_PERF_BENCHMARKS. Take too long for normal test runs.
//

#ifdef RUN_PERF_BENCHMARKS
namespace {
   /// Deterministic pseudo-random numbers and bytes for benchmark rows.
   struct bench_rng {
      uint64_t seed = 0x12345678ABCDEF01ULL;

      uint64_t next() {
         seed ^= seed << 13;
         seed ^= seed >> 7;
         seed ^= seed << 17;
         return seed;
      }

      void fill(char* data, std::size_t size) {
         for (std::size_t i = 0; i < size; i += sizeof(uint64_t)) {
            const uint64_t v = next();
            std::memcpy(data + i, &v, std::min(sizeof(v), size - i));
         }
      }
   };

   /// How benchmark rows are laid out in memory relative to key order.
   enum class bench_insertion_order {
      sorted,      ///< keys ascend, so a walk by key matches allocation order
      interleaved, ///< tables take turns, each appending ascending keys, as contract tables do on a live chain
      random       ///< keys are unrelated to allocation order, the worst case for a walk by key
   };

   /// A numeric environment setting, or `fallback` when it is not set.
   uint64_t bench_setting(const char* name, uint64_t fallback) {
      const char* value = std::getenv(name);
      return value ? std::stoull(value) : fallback;
   }

   /**
    * Writes the state file back and drops it from the page cache, so the next walk reads every page from disk.
    *
    * This stands in for state bigger than RAM, and flatters it: a page read here stays cached for the rest of the
    * walk, which would not hold under real memory pressure. Needs mapped state on a real disk, and reports how much
    * of the state was actually evicted.
    */
   void bench_evict_state(chainbase::database& db) {
      static const auto page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
      const auto* segment = db.get_segment_manager();
      const auto begin = reinterpret_cast<uintptr_t>(segment) & ~(page_size - 1);
      const auto length = (reinterpret_cast<uintptr_t>(segment) + segment->get_size() - begin) & ~(page_size - 1);
      auto* start = reinterpret_cast<void*>(begin);

      BOOST_REQUIRE_EQUAL(msync(start, length, MS_SYNC), 0);
      BOOST_REQUIRE_EQUAL(madvise(start, length, MADV_PAGEOUT), 0);

      std::vector<unsigned char> resident(length / page_size);
      BOOST_REQUIRE_EQUAL(mincore(start, length, resident.data()), 0);
      const auto resident_pages = std::ranges::count_if(resident, [](unsigned char page) { return (page & 1) != 0; });
      BOOST_TEST_MESSAGE(fmt::format("  state evicted: {} of {} pages still resident", resident_pages,
                                     resident.size()));
   }
}

// Performance benchmark: threaded (parallel) vs variant (sequential) snapshot write/read
// Run with: ./unit_test --run_test=snapshot_part2_tests/snapshot_perf_benchmark -- --sys-vm
BOOST_AUTO_TEST_CASE( snapshot_perf_benchmark )
{
   // It stores rows straight in chainbase, so on another engine it would time a snapshot without them.
   if (default_kv_engine() != kv_engine::chainbase) return;

   // --- Configuration ---
   constexpr uint32_t num_tables  = 1000;   // number of tables, one per code
   constexpr uint32_t rows_per_table = 1000; // KV rows per table
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
      bench_rng rng;

      for (uint32_t tbl = 0; tbl < num_tables; tbl++) {
         // Create KV rows for this table
         for (uint32_t r = 0; r < rows_per_table; r++) {
            const auto key = make_kv_scoped_key("benchmark"_n, r);
            db.create<kv_object>([&](kv_object& obj) {
               obj.code = account_name(tbl + 100); // unique code per table
               obj.payer = config::system_account_name;
               obj.set_key(key.data, key.size);
               obj.value.resize_and_fill(value_size, [&](char* data, std::size_t sz) { rng.fill(data, sz); });
            });
         }
      }
   }

   chain.produce_block();
   chain.control->abort_block();

   msg(fmt::format("Chain populated: {} KV rows in {} tables", total_rows, num_tables));

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

// Large-scale benchmark of the two KV sections, which hold nearly all contract state.
//
// Build with RUN_PERF_BENCHMARKS defined for this file, then run:
//   TMPDIR=<dir on a real disk> ./unit_test --run_test=snapshot_part2_tests/snapshot_large_benchmark --log_level=message -- --sys-vm
//
// TMPDIR holds the mapped state and the snapshot, so a tmpfs would hide the file I/O and rule out KV_SNAP_BENCH_COLD.
//
// Settings, from the environment:
//   KV_SNAP_BENCH_ROWS     KV rows, default 10 million
//   KV_SNAP_BENCH_ENTRIES  KV secondary index entries, default 14 million
//   KV_SNAP_BENCH_ORDER    sorted, interleaved or random (default): see bench_insertion_order
//   KV_SNAP_BENCH_COLD     1 evicts the state before each walk: see bench_evict_state
//
// The default counts keep the ratio of an EOS mainnet snapshot (snapshot-2026-03-10-19-eos-v8):
//   key_value_object:  262M rows, 20.6 GB (50.7%)  ~82 bytes/row
//   index256_object:   356M rows, 16.3 GB (40.0%)  ~48 bytes/row
//   index64_object:     13M rows,  0.3 GB
// Rows keep its sizes too: a 16-byte key and a 50-byte value, and a 32-byte secondary key with a 16-byte primary key.
BOOST_AUTO_TEST_CASE( snapshot_large_benchmark )
{
   constexpr uint64_t default_rows        = 10'000'000;
   constexpr uint64_t default_entries     = 14'000'000;
   constexpr uint32_t num_tables          = 20000;
   constexpr uint32_t kv_value_size       = 50;
   constexpr uint32_t sec_key_size        = 32;
   constexpr uint32_t sec_key_random_from = sizeof(uint64_t); // the leading bytes set the order, the rest are filler
   constexpr uint16_t table_id            = 1;
   constexpr uint64_t state_bytes_per_row = 320;              // generous: node, key allocation and value allocation
   constexpr uint64_t state_base_mib      = 1024;
   constexpr uint64_t mib                 = 1024 * 1024;      // chainbase takes a whole number of MiB
   constexpr uint64_t progress_interval   = 5'000'000;

   const uint64_t total_rows    = bench_setting("KV_SNAP_BENCH_ROWS", default_rows);
   const uint64_t total_entries = bench_setting("KV_SNAP_BENCH_ENTRIES", default_entries);
   const bool     cold          = bench_setting("KV_SNAP_BENCH_COLD", 0) != 0;
   const char*    order_setting = std::getenv("KV_SNAP_BENCH_ORDER");
   const auto     order = order_setting ? magic_enum::enum_cast<bench_insertion_order>(order_setting)
                                        : std::optional{bench_insertion_order::random};
   BOOST_REQUIRE(order.has_value());

   auto msg = [](const std::string& s) { BOOST_TEST_MESSAGE(s); };
   msg(fmt::format("Large benchmark: {} rows + {} index entries in {} tables, {} insertion, {} state",
       total_rows, total_entries, num_tables, magic_enum::enum_name(*order), cold ? "cold" : "hot"));

   fc::temp_directory tempdir;
   auto cfg = tester::default_config(tempdir);
   cfg.first.state_size = (state_base_mib + (total_rows + total_entries) * state_bytes_per_row / mib) * mib;
   savanna_tester chain(cfg.first, cfg.second);
   chain.produce_block();

   // Populate chainbase directly (bypasses WASM)
   auto pop_start = std::chrono::high_resolution_clock::now();
   {
      auto& db = const_cast<chainbase::database&>(chain.control->db());
      bench_rng rng;

      // The table and the ordering number of the i-th of `count` rows.
      struct placement { uint32_t table; uint64_t number; };
      const auto place = [&](uint64_t i, uint64_t count) -> placement {
         switch (*order) {
            case bench_insertion_order::sorted:      return {static_cast<uint32_t>(i * num_tables / count), i};
            case bench_insertion_order::interleaved: return {static_cast<uint32_t>(i % num_tables), i};
            case bench_insertion_order::random:
               return {static_cast<uint32_t>(rng.next() % num_tables), rng.next()};
         }
         BOOST_FAIL("unknown insertion order");
         return {};
      };

      for (uint64_t i = 0; i < total_rows; i++) {
         const auto [tbl, number] = place(i, total_rows);
         const auto key = make_kv_scoped_key("benchmark"_n, number);
         db.create<kv_object>([&](kv_object& obj) {
            obj.code     = account_name(tbl + 100);
            obj.payer    = config::system_account_name;
            obj.table_id = table_id;
            obj.set_key(key.data, key.size);
            obj.value.resize_and_fill(kv_value_size, [&](char* data, std::size_t sz) { rng.fill(data, sz); });
         });
         if ((i + 1) % progress_interval == 0)
            msg(fmt::format("  populated {}/{} rows...", i + 1, total_rows));
      }

      for (uint64_t i = 0; i < total_entries; i++) {
         const auto [tbl, number] = place(i, total_entries);
         const auto pri_key = make_kv_scoped_key("benchmark"_n, i);
         std::array<char, sec_key_size> sec_key;
         kv_encode_be64(sec_key.data(), number);
         rng.fill(sec_key.data() + sec_key_random_from, sec_key.size() - sec_key_random_from);
         db.create<kv_index_object>([&](kv_index_object& obj) {
            obj.code     = account_name(tbl + 100);
            obj.payer    = config::system_account_name;
            obj.table_id = table_id;
            obj.set_sec_key(sec_key.data(), sec_key.size());
            obj.set_pri_key(pri_key.data, pri_key.size);
         });
         if ((i + 1) % progress_interval == 0)
            msg(fmt::format("  populated {}/{} index entries...", i + 1, total_entries));
      }
   }

   chain.produce_block();
   chain.control->abort_block();

   auto pop_end = std::chrono::high_resolution_clock::now();
   auto pop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(pop_end - pop_start).count();
   msg(fmt::format("Chain populated: {} rows, {} index entries in {} tables ({} ms)",
       total_rows, total_entries, num_tables, pop_ms));

   auto& state_db = const_cast<chainbase::database&>(chain.control->db());

   // --- Integrity Hash (run first while state pages are hot) ---
   if (cold) bench_evict_state(state_db);
   auto tp_ih0 = std::chrono::high_resolution_clock::now();
   auto integrity_hash = chain.control->calculate_integrity_hash();
   auto tp_ih1 = std::chrono::high_resolution_clock::now();

   auto ihash_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp_ih1 - tp_ih0).count();
   msg(fmt::format("Integrity HASH: {} ms (no I/O)", ihash_ms));

   // --- Threaded WRITE ---
   if (cold) bench_evict_state(state_db);
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

   // --- Verify, timing the hash again: a loaded chain allocates its rows in snapshot order ---
   auto tp_loaded0 = std::chrono::high_resolution_clock::now();
   auto snap_integrity_hash = snap_chain.control->calculate_integrity_hash();
   auto tp_loaded1 = std::chrono::high_resolution_clock::now();
   BOOST_REQUIRE_EQUAL(integrity_hash.str(), snap_integrity_hash.str());
   msg("Integrity hash verified");

   auto loaded_ihash_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp_loaded1 - tp_loaded0).count();

   // --- Summary ---
   msg("=== LARGE BENCHMARK SUMMARY ===");
   msg(fmt::format("Snapshot: {:.2f} GB, {} rows, {} index entries, {} tables",
       (double)snap_size / (1024*1024*1024), total_rows, total_entries, num_tables));
   msg(fmt::format("Populate: {:.1f}s", (double)pop_ms / 1000));
   msg(fmt::format("IHash:    {:.1f}s (no I/O, serialization + BLAKE3 only)", (double)ihash_ms / 1000));
   msg(fmt::format("Write:    {:.1f}s ({:.0f} MB/s)", (double)write_ms / 1000, write_throughput));
   msg(fmt::format("Read:     {:.1f}s ({:.0f} MB/s)", (double)read_ms / 1000, read_throughput));
   msg(fmt::format("IHash after load: {:.1f}s", (double)loaded_ihash_ms / 1000));
   msg(fmt::format("RESULT order={} cold={} rows={} entries={} snapshot_bytes={} populate_ms={} ihash_ms={} "
                   "write_ms={} read_ms={} loaded_ihash_ms={}", magic_enum::enum_name(*order), cold, total_rows,
                   total_entries, snap_size, pop_ms, ihash_ms, write_ms, read_ms, loaded_ihash_ms));
}

// BLAKE3 encoder buffer bypass threshold benchmark
// Tests whether `pos + len >= X * buf_size` threshold (flushing early when
// buffer-plus-incoming exceeds a fraction of buffer capacity) beats the
// current `len >= buf_size` strategy.
//
// Run with: ./unit_test --run_test=snapshot_part2_tests/blake3_buffer_threshold_benchmark -- --sys-vm
BOOST_AUTO_TEST_CASE( blake3_buffer_threshold_benchmark )
{
   auto msg = [](const std::string& s) { BOOST_TEST_MESSAGE(s); };

   static constexpr size_t buf_size = 64 * 1024; // matches blake3_encoder

   // Inline encoder that accepts a threshold multiplier
   struct bench_encoder {
      blake3_encoder enc;
      char           buf[buf_size];
      size_t         pos = 0;
      double         threshold; // 0 = current strategy

      explicit bench_encoder(double t) : threshold(t) {}

      void reset() { pos = 0; enc.reset(); }

      void flush() {
         if(pos > 0) {
            enc.write(buf, pos);
            pos = 0;
         }
      }

      void write(const char* d, size_t len) {
         bool bypass;
         if(threshold == 0)
            bypass = (len >= buf_size);
         else
            bypass = (len >= buf_size || pos + len >= (size_t)(threshold * buf_size));

         if(bypass) {
            flush();
            enc.write(d, len);
            return;
         }
         while(len > 0) {
            size_t space = buf_size - pos;
            size_t chunk = len < space ? len : space;
            memcpy(buf + pos, d, chunk);
            pos += chunk;
            d += chunk;
            len -= chunk;
            if(pos == buf_size) flush();
         }
      }

      fc::crypto::blake3 finalize() {
         flush();
         return enc.result();
      }
   };

   // Generate source data
   constexpr size_t src_size = 1 << 20; // 1 MB
   std::vector<char> src(src_size);
   uint64_t seed = 0x12345678ABCDEF01ULL;
   for(auto& c : src) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; c = seed & 0xFF; }

   // Write patterns
   struct pattern {
      std::vector<size_t> sizes;
      size_t total = 0;
      const char* name;
   };

   auto make_pattern = [&](const char* name, auto size_fn) {
      pattern p; p.name = name;
      constexpr size_t target = 100 * 1024 * 1024; // 100 MB
      std::mt19937 rng(42);
      while(p.total < target) { size_t s = size_fn(rng); p.sizes.push_back(s); p.total += s; }
      return p;
   };

   auto small    = make_pattern("small (1-8B)",         [](auto& r) -> size_t { return 1 << (r() % 4); });
   auto mixed    = make_pattern("mixed (1B-60KB)",      [](auto& r) -> size_t {
      int x = r() % 100;
      if(x < 60) return 1 + (r() % 8);
      if(x < 80) return 100 + (r() % 900);
      if(x < 95) return 1000 + (r() % 9000);
      return 30000 + (r() % 30000);
   });
   auto boundary = make_pattern("boundary (50KB+40-60KB)", [](auto& r) -> size_t {
      // Alternate: many small writes filling ~50KB, then a near-boundary write
      static int phase = 0;
      static size_t filled = 0;
      if(filled < 50000) { size_t s = 4 + (r() % 5); filled += s; return s; }
      filled = 0;
      return 40000 + (r() % 20000);
   });

   std::vector<pattern*> patterns = {&small, &mixed, &boundary};
   double thresholds[] = {0, 1.0, 1.5, 1.75};
   const char* labels[] = {"current (len>=buf)", "x=1.0", "x=1.5", "x=1.75"};

   msg("=== BLAKE3 BUFFER THRESHOLD BENCHMARK ===");
   msg(fmt::format("Buffer: {} KB, Data per run: 100 MB, Iterations: 5\n", buf_size / 1024));
   msg(fmt::format("{:<30s}  {:>14s}  {:>14s}  {:>14s}  {:>14s}", "Pattern", labels[0], labels[1], labels[2], labels[3]));
   msg(std::string(92, '-'));

   for(auto* pat : patterns) {
      std::string line = fmt::format("{:<30s}", pat->name);
      for(int t = 0; t < 4; t++) {
         constexpr int iterations = 5;
         bench_encoder enc(thresholds[t]);
         auto start = std::chrono::high_resolution_clock::now();
         for(int iter = 0; iter < iterations; iter++) {
            enc.reset();
            size_t offset = 0;
            for(size_t sz : pat->sizes) {
               enc.write(src.data() + (offset % (src_size - 65536)), sz);
               offset += sz;
            }
            enc.finalize();
         }
         auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - start).count() / iterations;
         line += fmt::format("  {:>11.2f} ms", elapsed);
      }
      msg(line);
   }
   msg("\nLower is better.");
}

#endif // RUN_PERF_BENCHMARKS

// --- ^^^^
// --- End Performance Tests

BOOST_AUTO_TEST_SUITE_END()
