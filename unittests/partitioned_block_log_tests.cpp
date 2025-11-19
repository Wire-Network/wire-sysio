#include <sstream>

#include <sysio/chain/block_log.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/testing/tester.hpp>

#include <snapshot_suites.hpp>
#include <snapshot_tester.hpp>

#include <boost/test/unit_test.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <contracts.hpp>
#include <fc/io/cfile.hpp>


BOOST_AUTO_TEST_SUITE(partitioned_block_log_tests)

using signed_block_log = sysio::chain::block_log;

void remove_existing_states(sysio::chain::controller::config& config) {
   auto state_path = config.state_dir;
   remove_all(state_path);
   std::filesystem::create_directories(state_path);
}

std::filesystem::path get_retained_dir(const sysio::chain::controller::config& cfg) {
   std::filesystem::path retained_dir;
   auto     paritioned_config = std::get_if<sysio::chain::partitioned_blocklog_config>(&cfg.blog);
   if (paritioned_config) {
      retained_dir = paritioned_config->retained_dir;
      if (retained_dir.is_relative())
         retained_dir = cfg.blocks_dir / retained_dir;
   }
   return retained_dir;
}

struct restart_from_block_log_test_fixture {
   sysio::testing::tester chain;
   uint32_t               cutoff_block_num;

   restart_from_block_log_test_fixture() {
      using namespace sysio::chain;
      chain.create_account("replay1"_n, config::system_account_name, false, true, false);
      chain.produce_block();
      chain.create_account("replay2"_n, config::system_account_name, false, true, false);
      chain.produce_block();
      chain.create_account("replay3"_n, config::system_account_name, false, true, false);
      chain.produce_block();
      auto cutoff_block = chain.produce_block();
      cutoff_block_num  = cutoff_block->block_num();
      chain.produce_block();

      BOOST_REQUIRE_NO_THROW(chain.get_account("replay1"_n));
      BOOST_REQUIRE_NO_THROW(chain.get_account("replay2"_n));
      BOOST_REQUIRE_NO_THROW(chain.get_account("replay3"_n));

      chain.close();
   }

   void restart_chain() {
      sysio::chain::controller::config copied_config = chain.get_config();

      auto genesis = signed_block_log::extract_genesis_state(copied_config.blocks_dir, 
                                                                    get_retained_dir(copied_config));
      BOOST_REQUIRE(genesis);

      copied_config.blog =
            sysio::chain::basic_blocklog_config{};

      // remove the state files to make sure we are starting from block log
      remove_existing_states(copied_config);
      sysio::testing::tester from_block_log_chain(copied_config, *genesis);
      using namespace sysio::chain;
      BOOST_REQUIRE_NO_THROW(from_block_log_chain.get_account("replay1"_n));
      BOOST_REQUIRE_NO_THROW(from_block_log_chain.get_account("replay2"_n));
      BOOST_REQUIRE_NO_THROW(from_block_log_chain.get_account("replay3"_n));
   }
};

BOOST_AUTO_TEST_CASE(test_split_log) {
   fc::temp_directory temp_dir;

   sysio::testing::tester chain(
         temp_dir,
         [](sysio::chain::controller::config& config) {
            config.blog = sysio::chain::partitioned_blocklog_config{ .archive_dir        = "archive",
                                                                     .stride             = 20,
                                                                     .max_retained_files = 5 };
         },
         true);
   chain.produce_blocks(150);

   auto blocks_dir         = chain.get_config().blocks_dir;
   auto blocks_archive_dir = blocks_dir / "archive";

   BOOST_CHECK(std::filesystem::exists(blocks_archive_dir / "blocks-1-20.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_archive_dir / "blocks-1-20.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_archive_dir / "blocks-21-40.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_archive_dir / "blocks-21-40.index"));

   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-41-60.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-41-60.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-61-80.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-61-80.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-81-100.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-81-100.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-101-120.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-101-120.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-121-140.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-121-140.index"));

   BOOST_CHECK(!chain.fetch_block_by_number(40));

   BOOST_CHECK(chain.fetch_block_by_number(81)->block_num() == 81u);
   BOOST_CHECK(chain.fetch_block_by_number(90)->block_num() == 90u);
   BOOST_CHECK(chain.fetch_block_by_number(100)->block_num() == 100u);

   BOOST_CHECK(chain.fetch_block_by_number(41)->block_num() == 41u);
   BOOST_CHECK(chain.fetch_block_by_number(50)->block_num() == 50u);
   BOOST_CHECK(chain.fetch_block_by_number(60)->block_num() == 60u);

   BOOST_CHECK(chain.fetch_block_by_number(121)->block_num() == 121u);
   BOOST_CHECK(chain.fetch_block_by_number(130)->block_num() == 130u);
   BOOST_CHECK(chain.fetch_block_by_number(140)->block_num() == 140u);

   BOOST_CHECK(chain.fetch_block_by_number(145)->block_num() == 145u);

   BOOST_CHECK(!chain.fetch_block_by_number(160));
}

BOOST_AUTO_TEST_CASE(test_split_log_zero_retained_file) {
   fc::temp_directory temp_dir;
   sysio::testing::tester chain(
      temp_dir,
      [](sysio::chain::controller::config& config) {
         config.blog = sysio::chain::partitioned_blocklog_config{
            .retained_dir = "retained", .archive_dir = "archive", .stride = 50, .max_retained_files = 0
         };
      },
      true);
   chain.produce_blocks(150);
   auto blocks_dir   = chain.get_config().blocks_dir;
   auto retained_dir = blocks_dir / "retained";
   auto archive_dir  = blocks_dir / "archive";

   BOOST_CHECK(std::filesystem::is_empty(retained_dir));

   BOOST_CHECK(std::filesystem::exists(archive_dir / "blocks-1-50.log"));
   BOOST_CHECK(std::filesystem::exists(archive_dir / "blocks-1-50.index"));
   BOOST_CHECK(std::filesystem::exists(archive_dir / "blocks-51-100.log"));
   BOOST_CHECK(std::filesystem::exists(archive_dir / "blocks-51-100.index"));
   BOOST_CHECK(std::filesystem::exists(archive_dir / "blocks-101-150.log"));
   BOOST_CHECK(std::filesystem::exists(archive_dir / "blocks-101-150.index"));
}

BOOST_AUTO_TEST_CASE(test_split_log_all_in_retained_new_default) {
   fc::temp_directory temp_dir;
   sysio::testing::tester chain(
      temp_dir,
      [](sysio::chain::controller::config& config) {
         config.blog = sysio::chain::partitioned_blocklog_config{ .retained_dir = "retained",
                                                                  .archive_dir  = "archive",
                                                                  .stride       = 50 };
      },
      true);
   chain.produce_blocks(150);
   auto blocks_dir   = chain.get_config().blocks_dir;
   auto retained_dir = blocks_dir / "retained";
   auto archive_dir  = blocks_dir / "archive";

   BOOST_CHECK(std::filesystem::is_empty(archive_dir));

   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-1-50.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-1-50.index"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-51-100.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-51-100.index"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-101-150.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-101-150.index"));
}

BOOST_AUTO_TEST_CASE(test_split_log_util1) {
   sysio::testing::tester chain;
   chain.produce_blocks(160);

   uint32_t head_block_num = chain.head().block_num();
   uint32_t lib_block_num;
   lib_block_num = head_block_num - sysio::testing::num_chains_to_final; // two-chain

   sysio::chain::controller::config copied_config = chain.get_config();
   auto genesis = signed_block_log::extract_genesis_state(chain.get_config().blocks_dir,
                                                                 get_retained_dir(chain.get_config()));
   BOOST_REQUIRE(genesis);

   chain.close();

   fc::temp_directory temp_dir;
   auto blocks_dir   = chain.get_config().blocks_dir;
   auto retained_dir = temp_dir.path() / "retained";
   sysio::chain::block_log::split_blocklog(blocks_dir, retained_dir, 50);

   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-1-50.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-1-50.index"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-51-100.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-51-100.index"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-101-150.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-101-150.index"));
   char buf[64];
   snprintf(buf, 64, "blocks-151-%u.log", lib_block_num);
   std::filesystem::path last_block_file = retained_dir / buf;
   snprintf(buf, 64, "blocks-151-%u.index", lib_block_num);
   std::filesystem::path last_index_file = retained_dir / buf;
   BOOST_CHECK(std::filesystem::exists(last_block_file));
   BOOST_CHECK(std::filesystem::exists(last_index_file));

   std::filesystem::rename(last_block_file, blocks_dir / "blocks.log");
   std::filesystem::rename(last_index_file, blocks_dir / "blocks.index");

   // remove the state files to make sure we are starting from block log
   remove_existing_states(copied_config);
   // we need to remove the reversible blocks so that new blocks can be produced from the new chain
   std::filesystem::remove_all(copied_config.blocks_dir / "reversible");

   copied_config.blog = sysio::chain::partitioned_blocklog_config{ .retained_dir       = retained_dir,
                                                                   .stride             = 50,
                                                                   .max_retained_files = 5 };

   tester from_block_log_chain(copied_config, *genesis);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(1)->block_num() == 1u);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(75)->block_num() == 75u);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(100)->block_num() == 100u);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(150)->block_num() == 150u);

   from_block_log_chain.close();

   //
   // replay with no blocks.log, but blocks in retained_dir
   //

   // remove the state files to make sure we are starting from block log
   remove_existing_states(copied_config);
   // we need to remove all blocks but what is in retained
   std::filesystem::remove(blocks_dir / "blocks.log");
   std::filesystem::remove(blocks_dir / "blocks.index");

   // Create a replay chain without starting it
   sysio::testing::tester replay_chain(copied_config, *genesis, sysio::testing::call_startup_t::no);
   // no fork db head yet
   BOOST_REQUIRE(!replay_chain.fork_db_head().is_valid());
   // works because it pulls from retain dir
   BOOST_CHECK(replay_chain.fetch_block_by_number(42)->block_num() == 42u);

   // Simulate shutdown by CTRL-C
   bool is_quiting = false;
   auto check_shutdown = [&is_quiting](){ return is_quiting; };
   uint32_t stop_at = 25;
   // Set up shutdown at a particular block number
   replay_chain.control->irreversible_block().connect([&](const sysio::chain::block_signal_params& t) {
      const auto& [ block, id ] = t;
      // Stop replay at block `stop_at`
      if (block->block_num() == stop_at) {
         is_quiting = true;
      }
   });
   // Start replay and stop at block `stop_at`
   replay_chain.control->startup( [](){}, check_shutdown, *genesis );

   // create snapshot at stop_at block
   replay_chain.control->abort_block();
   auto writer = variant_snapshot_suite::get_writer();
   replay_chain.control->write_snapshot(writer);
   auto snapshot = variant_snapshot_suite::finalize(writer);

   BOOST_REQUIRE(replay_chain.head().is_valid());
   BOOST_CHECK(replay_chain.head().block_num() == stop_at);
   // no fork db head yet
   BOOST_CHECK(!replay_chain.fork_db_head().is_valid());

   replay_chain.close();

   // replay chain from stop_at with no blocks in block_log, pulls from retained dir
   sysio::testing::tester replay_chain_1(copied_config, *genesis, sysio::testing::call_startup_t::no);
   replay_chain_1.control->startup( [](){}, []()->bool{ return false; } );

   BOOST_REQUIRE(replay_chain_1.fork_db_head().is_valid());
   BOOST_CHECK(replay_chain_1.fork_db_head().block_num() == 150u);

   BOOST_CHECK(replay_chain_1.fetch_block_by_number(1)->block_num() == 1u);
   BOOST_CHECK(replay_chain_1.fetch_block_by_number(75)->block_num() == 75u);
   BOOST_CHECK(replay_chain_1.fetch_block_by_number(100)->block_num() == 100u);
   BOOST_CHECK(replay_chain_1.fetch_block_by_number(150)->block_num() == 150u);

   replay_chain_1.close();

   //
   // start chain from snapshot at stop_at with no blocks in block_log, pulls from retained dir
   //

   // remove the state files to make sure we are starting from block log
   remove_existing_states(copied_config);
   // we need to remove all blocks but what is in retained
   std::filesystem::remove(blocks_dir / "blocks.log");
   std::filesystem::remove(blocks_dir / "blocks.index");

   int ordinal = 0;
   snapshotted_tester replay_chain_2(copied_config, variant_snapshot_suite::get_reader(snapshot), ++ordinal);

   BOOST_REQUIRE(replay_chain_2.fork_db_head().is_valid());
   BOOST_CHECK(replay_chain_2.fork_db_head().block_num() == 150u);

   BOOST_CHECK(replay_chain_2.fetch_block_by_number(1)->block_num() == 1u);
   BOOST_CHECK(replay_chain_2.fetch_block_by_number(75)->block_num() == 75u);
   BOOST_CHECK(replay_chain_2.fetch_block_by_number(100)->block_num() == 100u);
   BOOST_CHECK(replay_chain_2.fetch_block_by_number(150)->block_num() == 150u);
}

BOOST_AUTO_TEST_CASE(test_split_log_no_archive) {

   fc::temp_directory temp_dir;

   sysio::testing::tester chain(
         temp_dir,
         [](sysio::chain::controller::config& config) {
            config.blog =
                  sysio::chain::partitioned_blocklog_config{ .archive_dir = "", .stride = 10, .max_retained_files = 5 };
         },
         true);
   chain.produce_blocks(75);

   auto blocks_dir         = chain.get_config().blocks_dir;
   auto blocks_archive_dir = blocks_dir;

   BOOST_CHECK(!std::filesystem::exists(blocks_archive_dir / "blocks-1-10.log"));
   BOOST_CHECK(!std::filesystem::exists(blocks_archive_dir / "blocks-1-10.index"));
   BOOST_CHECK(!std::filesystem::exists(blocks_archive_dir / "blocks-11-20.log"));
   BOOST_CHECK(!std::filesystem::exists(blocks_archive_dir / "blocks-11-20.index"));
   BOOST_CHECK(!std::filesystem::exists(blocks_dir / "blocks-1-10.log"));
   BOOST_CHECK(!std::filesystem::exists(blocks_dir / "blocks-1-10.index"));
   BOOST_CHECK(!std::filesystem::exists(blocks_dir / "blocks-11-20.log"));
   BOOST_CHECK(!std::filesystem::exists(blocks_dir / "blocks-11-20.index"));

   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-21-30.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-21-30.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-31-40.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-31-40.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-41-50.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-41-50.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-51-60.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-51-60.index"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-61-70.log"));
   BOOST_CHECK(std::filesystem::exists(blocks_dir / "blocks-61-70.index"));

   BOOST_CHECK(!chain.fetch_block_by_number(10));
   BOOST_CHECK(chain.fetch_block_by_number(70));
   BOOST_CHECK(!chain.fetch_block_by_number(80));
}

void split_log_replay(uint32_t replay_max_retained_block_files) {
   fc::temp_directory temp_dir;

   const uint32_t stride = 20;

   sysio::testing::tester chain(
         temp_dir,
         [](sysio::chain::controller::config& config) {
            config.blog = sysio::chain::partitioned_blocklog_config{ .stride = stride, .max_retained_files = 10 };
         },
         true);
   chain.produce_blocks(150);

   auto copied_config = chain.get_config();
   auto genesis =
       signed_block_log::extract_genesis_state(copied_config.blocks_dir, get_retained_dir(copied_config));
   BOOST_REQUIRE(genesis);

   chain.close();

   // remove the state files to make sure we are starting from block log
   remove_existing_states(copied_config);
   // we need to remove the reversible blocks so that new blocks can be produced from the new chain
   std::filesystem::remove_all(copied_config.blocks_dir / "reversible");
   copied_config.blog =
         sysio::chain::partitioned_blocklog_config{ .stride             = stride,
                                                    .max_retained_files = replay_max_retained_block_files };
   tester from_block_log_chain(copied_config, *genesis);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(1)->block_num() == 1);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(75)->block_num() == 75);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(100)->block_num() == 100);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(150)->block_num() == 150);

   // produce new blocks to cross the blocks_log_stride boundary
   from_block_log_chain.produce_blocks(stride);

   const auto previous_chunk_end_block_num = (from_block_log_chain.head().block_num() / stride) * stride;
   const auto num_removed_blocks = std::min(stride * replay_max_retained_block_files, previous_chunk_end_block_num);
   const auto min_retained_block_number = previous_chunk_end_block_num - num_removed_blocks + 1;

   if (min_retained_block_number > 1) {
      // old blocks beyond the max_retained_block_files will no longer be available
      BOOST_CHECK(!from_block_log_chain.fetch_block_by_number(min_retained_block_number - 1));
   }
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(min_retained_block_number)->block_num() ==
               min_retained_block_number);
}

BOOST_AUTO_TEST_CASE(test_split_log_replay_retained_block_files_10) { split_log_replay(10); }

BOOST_AUTO_TEST_CASE(test_split_log_replay_retained_block_files_5) { split_log_replay(5); }

BOOST_AUTO_TEST_CASE(test_split_log_replay_retained_block_files_1) { split_log_replay(1); }

BOOST_AUTO_TEST_CASE(test_split_log_replay_retained_block_files_0) { split_log_replay(0); }

BOOST_AUTO_TEST_CASE(test_restart_without_blocks_log_file) {

   fc::temp_directory temp_dir;

   const uint32_t stride = 20;

   sysio::testing::tester chain(
         temp_dir,
         [](sysio::chain::controller::config& config) {
            config.blog = sysio::chain::partitioned_blocklog_config{ .stride = stride, .max_retained_files = 10 };
         },
         true);
   chain.produce_blocks(160);

   sysio::chain::controller::config copied_config = chain.get_config();
   auto genesis = signed_block_log::extract_genesis_state(chain.get_config().blocks_dir, get_retained_dir(copied_config));
   BOOST_REQUIRE(genesis);

   chain.close();

   // remove the state files to make sure we are starting from block log
   remove_existing_states(copied_config);
   // we need to remove the reversible blocks so that new blocks can be produced from the new chain
   std::filesystem::remove_all(copied_config.blocks_dir / "reversible");
   std::filesystem::remove(copied_config.blocks_dir / "blocks.log");
   std::filesystem::remove(copied_config.blocks_dir / "blocks.index");
   copied_config.blog = sysio::chain::partitioned_blocklog_config{ .stride = stride, .max_retained_files = 10 };
   tester from_block_log_chain(copied_config, *genesis);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(1)->block_num() == 1u);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(75)->block_num() == 75u);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(100)->block_num() == 100u);
   BOOST_CHECK(from_block_log_chain.fetch_block_by_number(160)->block_num() == 160u);

   from_block_log_chain.produce_blocks(10);
}

BOOST_FIXTURE_TEST_CASE(start_with_incomplete_head, restart_from_block_log_test_fixture) {
   auto& config      = chain.get_config();
   auto  blocks_path = config.blocks_dir;
   // write a few random bytes to block log indicating the last block entry is incomplete
   fc::cfile logfile;
   logfile.set_file_path(config.blocks_dir / "blocks.log");
   logfile.open("ab");
   const char random_data[] = "12345678901231876983271649837";
   logfile.write(random_data, sizeof(random_data));
   logfile.close();
   BOOST_CHECK_THROW(restart_chain(), sysio::chain::block_log_exception);
}

BOOST_FIXTURE_TEST_CASE(start_with_corrupted_index, restart_from_block_log_test_fixture) {
   auto& config      = chain.get_config();
   auto  blocks_path = config.blocks_dir;
   // write a few random index to block log indicating the index is corrupted
   fc::cfile indexfile;
   indexfile.set_file_path(config.blocks_dir / "blocks.index");
   indexfile.open("ab");
   uint64_t data = UINT64_MAX;
   indexfile.write(reinterpret_cast<const char*>(&data), sizeof(data));
   indexfile.close();
   BOOST_CHECK_THROW(restart_chain(), sysio::chain::block_log_exception);
}

BOOST_FIXTURE_TEST_CASE(start_with_corrupted_log_and_index, restart_from_block_log_test_fixture) {
   auto& config      = chain.get_config();
   auto  blocks_path = config.blocks_dir;
   // write a few random bytes to block log and index
   fc::cfile indexfile;
   indexfile.set_file_path(config.blocks_dir / "blocks.index");
   indexfile.open("ab");
   const char random_index[] = "1234";
   indexfile.write(reinterpret_cast<const char*>(&random_index), sizeof(random_index));

   fc::cfile logfile;
   logfile.set_file_path(config.blocks_dir / "blocks.log");
   logfile.open("ab");
   const char random_data[] = "12345678901231876983271649837";
   logfile.write(random_data, sizeof(random_data));
   indexfile.close();
   BOOST_CHECK_THROW(restart_chain(), sysio::chain::block_log_exception);
}

struct blocklog_version_setter {
   blocklog_version_setter(uint32_t ver) { signed_block_log::set_initial_version(ver); };
   ~blocklog_version_setter() { signed_block_log::set_initial_version(signed_block_log::max_supported_version); };
};

BOOST_AUTO_TEST_CASE(test_split_from_v1_log) {
   fc::temp_directory      temp_dir;
   blocklog_version_setter set_version(1);
   sysio::testing::tester  chain(
          temp_dir,
          [](sysio::chain::controller::config& config) {
            config.blog = sysio::chain::partitioned_blocklog_config{ .stride = 20, .max_retained_files = 5 };
         },
          true);
   chain.produce_blocks(75);

   BOOST_CHECK(chain.fetch_block_by_number(1)->block_num() == 1u);
   BOOST_CHECK(chain.fetch_block_by_number(21)->block_num() == 21u);
   BOOST_CHECK(chain.fetch_block_by_number(41)->block_num() == 41u);
   BOOST_CHECK(chain.fetch_block_by_number(75)->block_num() == 75u);
}

void trim_blocklog_front(uint32_t version) {
   blocklog_version_setter set_version(version);
   sysio::testing::tester  chain;
   chain.produce_blocks(10);
   chain.produce_blocks(20);
   chain.close();


   auto blocks_dir     = chain.get_config().blocks_dir;
   auto old_index_size = std::filesystem::file_size(blocks_dir / "blocks.index");

   fc::temp_directory temp1, temp2;
   std::filesystem::copy(blocks_dir / "blocks.log", temp1.path() / "blocks.log");
   std::filesystem::copy(blocks_dir / "blocks.index", temp1.path() / "blocks.index");
   BOOST_REQUIRE_NO_THROW(signed_block_log::trim_blocklog_front(temp1.path(), temp2.path(), 10));
   BOOST_REQUIRE_NO_THROW(signed_block_log::smoke_test(temp1.path(), 1));

   signed_block_log old_log(blocks_dir, chain.get_config().blog);
   signed_block_log new_log(temp1.path());
   // double check if the version has been set to the desired version
   const auto old_log_version = old_log.version();
   BOOST_CHECK(old_log_version == version);
   BOOST_CHECK(new_log.first_block_num() == 10u);
   BOOST_CHECK(new_log.head()->block_num() == old_log.head()->block_num());

   int num_blocks_trimmed = 10 - 1;
   BOOST_CHECK(std::filesystem::file_size(temp1.path() / "blocks.index") == old_index_size - sizeof(uint64_t) * num_blocks_trimmed);
}

BOOST_AUTO_TEST_CASE(test_trim_blocklog_front) { trim_blocklog_front(signed_block_log::max_supported_version); }

BOOST_AUTO_TEST_CASE(test_trim_blocklog_front_v1) { trim_blocklog_front(1); }

BOOST_AUTO_TEST_CASE(test_trim_blocklog_front_v2) { trim_blocklog_front(2); }

BOOST_AUTO_TEST_CASE(test_blocklog_split_then_merge) {

   sysio::testing::tester chain;
   chain.produce_blocks(160);
   chain.close();

   auto               blocks_dir   = chain.get_config().blocks_dir;
   auto               retained_dir = blocks_dir / "retained";
   fc::temp_directory temp_dir;

   BOOST_REQUIRE_NO_THROW(signed_block_log::trim_blocklog_front(blocks_dir, temp_dir.path(), 50));
   BOOST_REQUIRE_NO_THROW(signed_block_log::trim_blocklog_end(blocks_dir, 150));

   BOOST_CHECK_NO_THROW(signed_block_log::split_blocklog(blocks_dir, retained_dir, 50));

   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-50-50.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-50-50.index"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-51-100.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-51-100.index"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-101-150.log"));
   BOOST_CHECK(std::filesystem::exists(retained_dir / "blocks-101-150.index"));

   std::filesystem::remove(blocks_dir / "blocks.log");
   std::filesystem::remove(blocks_dir / "blocks.index");

   signed_block_log blog(blocks_dir, sysio::chain::partitioned_blocklog_config{ .retained_dir = retained_dir });

   BOOST_CHECK(blog.version() != 0);
   BOOST_CHECK_EQUAL(blog.head()->block_num(), 150u);

   // test blocklog merge
   fc::temp_directory dest_dir;
   BOOST_CHECK_NO_THROW(signed_block_log::merge_blocklogs(retained_dir, dest_dir.path()));
   BOOST_CHECK(std::filesystem::exists(dest_dir.path() / "blocks-50-150.log"));

   if (std::filesystem::exists(dest_dir.path() / "blocks-50-150.log")) {
      std::filesystem::rename(dest_dir.path() / "blocks-50-150.log", dest_dir.path() / "blocks.log");
      std::filesystem::rename(dest_dir.path() / "blocks-50-150.index", dest_dir.path() / "blocks.index");
      BOOST_CHECK_NO_THROW(signed_block_log::smoke_test(dest_dir.path(), 1));
   }

   std::filesystem::remove(dest_dir.path() / "blocks.log");

   // test blocklog merge with gap
   std::filesystem::remove(retained_dir / "blocks-51-100.log");
   std::filesystem::remove(retained_dir / "blocks-51-100.index");

   BOOST_CHECK_NO_THROW(signed_block_log::merge_blocklogs(retained_dir, dest_dir.path()));
   BOOST_CHECK(std::filesystem::exists(dest_dir.path() / "blocks-50-50.log"));
   BOOST_CHECK(std::filesystem::exists(dest_dir.path() / "blocks-50-50.index"));

   BOOST_CHECK(std::filesystem::exists(dest_dir.path() / "blocks-101-150.log"));
   BOOST_CHECK(std::filesystem::exists(dest_dir.path() / "blocks-101-150.index"));
}

BOOST_AUTO_TEST_SUITE_END()
