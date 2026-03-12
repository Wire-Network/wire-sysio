#include <sysio/chain/global_property_object.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(database_tests)

   // Simple tests of undo infrastructure
   BOOST_AUTO_TEST_CASE_TEMPLATE( undo_test, T, validating_testers ) {
      try {
         T test;

         // Bypass read-only restriction on state DB access for this unit test which really needs to mutate the DB to properly conduct its test.
         sysio::chain::database& db = const_cast<sysio::chain::database&>( test.control->db() );

         auto ses = db.start_undo_session(true);

         // Create an account
         db.create<account_object>([](account_object &a) {
            a.name = name("billy");
         });

         // Make sure we can retrieve that account by name
         auto ptr = db.find<account_object, by_name>(name("billy"));
         BOOST_TEST(ptr != nullptr);

         // Undo creation of the account
         ses.undo();

         // Make sure we can no longer find the account
         ptr = db.find<account_object, by_name>(name("billy"));
         BOOST_TEST(ptr == nullptr);
      } FC_LOG_AND_RETHROW()
   }

   // Test the block fetching methods on database, fetch_bock_by_id, and fetch_block_by_number
   BOOST_AUTO_TEST_CASE( get_blocks ) {
      try {
         savanna_validating_tester test;
         vector<block_id_type> block_ids;

         const uint32_t num_of_blocks_to_prod = 20;
         // Produce 20 blocks and check their IDs should match the above
         test.produce_blocks(num_of_blocks_to_prod);
         for (uint32_t i = 0; i < num_of_blocks_to_prod; ++i) {
            block_ids.emplace_back(test.fetch_block_by_number(i + 1)->calculate_id());
            BOOST_TEST(block_header::num_from_id(block_ids.back()) == i + 1);
            BOOST_TEST(test.fetch_block_by_number(i + 1)->calculate_id() == block_ids.back());
         }

         // Check the last irreversible block number is set correctly.
         // In Savanna, after 2-chain finality is achieved.
         const auto expected_last_irreversible_block_number = test.head().block_num() - num_chains_to_final;
         BOOST_TEST(test.last_irreversible_block_num() == expected_last_irreversible_block_number);

         // Ensure that future block doesn't exist
         const auto nonexisting_future_block_num = test.head().block_num() + 1;
         BOOST_TEST(test.fetch_block_by_number(nonexisting_future_block_num) == nullptr);

         const uint32_t next_num_of_blocks_to_prod = 10;
         test.produce_blocks(next_num_of_blocks_to_prod);

         // Check the last irreversible block number is updated correctly
         const auto next_expected_last_irreversible_block_number = test.head().block_num() - num_chains_to_final;
         BOOST_TEST(test.last_irreversible_block_num() == next_expected_last_irreversible_block_number);
         // Previous nonexisting future block should exist by now
         BOOST_CHECK_NO_THROW(test.fetch_block_by_number(nonexisting_future_block_num));
         // Check the latest head block match
         BOOST_TEST(test.fetch_block_by_number(test.head().block_num())->calculate_id() ==
                    test.head().id());

         // Verify LIB can be found
         const auto lib_num = test.last_irreversible_block_num();
         auto lib           = test.fetch_block_by_number(lib_num);
         BOOST_REQUIRE(lib);
         auto lib_id = lib->calculate_id();
         BOOST_TEST(lib_num == lib->block_num());
         lib = test.fetch_block_by_id(lib_id);
         BOOST_REQUIRE(lib);
         BOOST_TEST(lib->calculate_id() == lib_id);

      } FC_LOG_AND_RETHROW()
   }

   // Test the block fetching methods on database, fetch_bock_by_id, and fetch_block_by_number
   BOOST_AUTO_TEST_CASE_TEMPLATE( get_blocks_no_block_log, T, validating_testers ) {
      try {
         fc::temp_directory tempdir;

         constexpr bool use_genesis = true;
         T test(
            tempdir,
            [&](controller::config& cfg) {
               cfg.blog = sysio::chain::empty_blocklog_config{};
            },
            use_genesis
         );

         // Ensure that future block doesn't exist
         const auto nonexisting_future_block_num = test.head().block_num() + 1;
         BOOST_TEST(test.fetch_block_by_number(nonexisting_future_block_num) == nullptr);
         BOOST_TEST(test.fetch_block_by_id(sha256::hash("xx")) == nullptr);

         test.produce_block();

         // Previous nonexisting future block should exist now
         BOOST_CHECK_NO_THROW(test.fetch_block_by_number(nonexisting_future_block_num));
         // Check the latest head block match
         BOOST_TEST(test.fetch_block_by_number(test.head().block_num())->calculate_id() == test.head().id());
         BOOST_TEST(test.fetch_block_by_id(test.head().id())->calculate_id() == test.head().id());

         // Verify LIB can be found
         const auto lib_num = test.last_irreversible_block_num();
         auto lib           = test.fetch_block_by_number(lib_num);
         BOOST_REQUIRE(lib);
         auto lib_id = lib->calculate_id();
         BOOST_TEST(lib_num == lib->block_num());
         lib = test.fetch_block_by_id(lib_id);
         BOOST_REQUIRE(lib);
         BOOST_TEST(lib->calculate_id() == lib_id);

      } FC_LOG_AND_RETHROW()
   }

   // Batch fetch spanning block_log and fork_db
   BOOST_AUTO_TEST_CASE( batch_fetch_spanning_blog_and_forkdb ) {
      try {
         savanna_tester chain;

         // Produce enough blocks so that some are irreversible (in block_log)
         // and some are reversible (in fork_db). Savanna finality: LIB = head - 2.
         chain.produce_blocks(30);

         uint32_t lib_num  = chain.last_irreversible_block_num();
         uint32_t head_num = chain.head().block_num();

         // Sanity: there should be blocks in fork_db (above LIB) and in block_log (at/below LIB)
         BOOST_REQUIRE_GT(lib_num, 5u);
         BOOST_REQUIRE_GT(head_num, lib_num);

         // Request a batch that spans the block_log/fork_db boundary:
         // start a few blocks before LIB and extend past head
         uint32_t start = lib_num - 3;
         uint32_t count = head_num - start + 5; // extends 5 past head
         auto batch = chain.control->fetch_serialized_blocks_by_number(start, count);

         // Should get exactly (head_num - start + 1) blocks — clamped at head
         uint32_t expected_count = head_num - start + 1;
         BOOST_REQUIRE_EQUAL(batch.size(), expected_count);

         // Verify each block matches the single-fetch API
         for (uint32_t i = 0; i < expected_count; ++i) {
            auto single = chain.control->fetch_serialized_block_by_number(start + i);
            BOOST_REQUIRE(!single.empty());
            BOOST_REQUIRE(batch[i] == single);
         }

         // Verify the batch includes blocks from both sources by checking block numbers
         for (uint32_t i = 0; i < expected_count; ++i) {
            auto blk = fc::raw::unpack<signed_block>(batch[i]);
            BOOST_REQUIRE_EQUAL(blk.block_num(), start + i);
         }
      } FC_LOG_AND_RETHROW()
   }

   // Batch fetch entirely from fork_db (all blocks above LIB)
   BOOST_AUTO_TEST_CASE( batch_fetch_entirely_in_forkdb ) {
      try {
         savanna_tester chain;
         chain.produce_blocks(10);

         uint32_t lib_num  = chain.last_irreversible_block_num();
         uint32_t head_num = chain.head().block_num();
         BOOST_REQUIRE_GT(head_num, lib_num);

         uint32_t start = lib_num + 1;
         uint32_t count = head_num - lib_num;
         auto batch = chain.control->fetch_serialized_blocks_by_number(start, count);
         BOOST_REQUIRE_EQUAL(batch.size(), count);

         for (uint32_t i = 0; i < count; ++i) {
            auto single = chain.control->fetch_serialized_block_by_number(start + i);
            BOOST_REQUIRE(batch[i] == single);
         }
      } FC_LOG_AND_RETHROW()
   }

   // Batch fetch entirely from block_log (all blocks below LIB)
   BOOST_AUTO_TEST_CASE( batch_fetch_entirely_in_blog ) {
      try {
         savanna_tester chain;
         chain.produce_blocks(30);

         uint32_t lib_num = chain.last_irreversible_block_num();
         BOOST_REQUIRE_GT(lib_num, 10u);

         uint32_t start = 2;
         uint32_t count = lib_num - 5;
         auto batch = chain.control->fetch_serialized_blocks_by_number(start, count);
         BOOST_REQUIRE_EQUAL(batch.size(), count);

         for (uint32_t i = 0; i < count; ++i) {
            auto single = chain.control->fetch_serialized_block_by_number(start + i);
            BOOST_REQUIRE(batch[i] == single);
         }
      } FC_LOG_AND_RETHROW()
   }

BOOST_AUTO_TEST_SUITE_END()
