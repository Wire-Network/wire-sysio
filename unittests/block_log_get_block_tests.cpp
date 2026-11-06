#include <sysio/chain/block_log.hpp>
#include <sysio/testing/tester.hpp> // for fc_exception_message_contains

#include <fc/io/cfile.hpp>
#include <fc/bitutil.hpp>

#include <boost/test/unit_test.hpp>

#include <numeric>

using namespace sysio::chain;
using namespace sysio::testing;

namespace {

/// Appends empty blocks numbered 2..last_block_num to a log reset at block 1, each linked to its predecessor.
void append_empty_blocks(block_log& blog, uint32_t last_block_num) {
   for (uint32_t i = 2; i < last_block_num + 1; ++i) {
      auto p = signed_block::create_mutable_block({});
      p->previous._hash[0] = fc::endian_reverse_u32(i - 1);
      auto sp = signed_block::create_signed_block(std::move(p));
      blog.append(sp, sp->calculate_id());
   }
}

/// Serialized sizes of blocks [first_block_num, first_block_num + count), read one block at a time.
std::vector<uint64_t> serialized_block_sizes(const block_log& blog, uint32_t first_block_num, uint32_t count) {
   std::vector<uint64_t> sizes;
   for (uint32_t i = 0; i < count; ++i)
      sizes.push_back(blog.read_serialized_block_by_num(first_block_num + i).size());
   return sizes;
}

/// Requires `batch` to be exactly blocks [first_block_num, first_block_num + expected_count) of `blog`.
void require_batch_matches(const block_log& blog, const std::vector<std::vector<char>>& batch,
                           uint32_t first_block_num, uint32_t expected_count) {
   BOOST_REQUIRE_EQUAL(batch.size(), expected_count);
   for (uint32_t i = 0; i < expected_count; ++i)
      BOOST_REQUIRE(batch[i] == blog.read_serialized_block_by_num(first_block_num + i));
}

/**
 * Checks every byte-budget boundary of a batch read of `count` blocks from `first_block_num`: a budget that exactly fits
 * the first k blocks returns k blocks, and one byte less returns k - 1 (never fewer than the first block).
 */
void check_batch_byte_budget(const block_log& blog, uint32_t first_block_num, uint32_t count) {
   const auto sizes = serialized_block_sizes(blog, first_block_num, count);
   for (uint32_t k = 1; k <= count; ++k) {
      const uint64_t exact = std::accumulate(sizes.begin(), sizes.begin() + k, uint64_t{0});
      require_batch_matches(blog, blog.read_serialized_blocks_by_num(first_block_num, count, exact), first_block_num, k);
      require_batch_matches(blog, blog.read_serialized_blocks_by_num(first_block_num, count, exact - 1), first_block_num,
                            std::max(k - 1, 1u));
   }
}

} // anonymous namespace

struct block_log_get_block_fixture {
   block_log_get_block_fixture() {
      block_dir = dir.path();

      log.emplace(block_dir);

      log->reset(genesis_state(), signed_block::create_signed_block(signed_block::create_mutable_block({})));
      BOOST_REQUIRE_EQUAL(log->first_block_num(), 1u);
      BOOST_REQUIRE_EQUAL(log->head()->block_num(), 1u);

      append_empty_blocks(*log, last_block_num);
      BOOST_REQUIRE_EQUAL(log->head()->block_num(), last_block_num);
   };

   void test_read_serialized_block(const block_log& blog, uint32_t block_num) { 
      // read the serialized block
      auto serialized_block = blog.read_serialized_block_by_num(block_num);

      // the serialized block can be deserialized
      BOOST_REQUIRE_NO_THROW(fc::raw::unpack<signed_block>(serialized_block));

      // read the signed block by regular read_block_by_num
      signed_block_ptr block = blog.read_block_by_num(block_num);
      BOOST_REQUIRE(block);

      // the serialized block should match the signed block's serialized form
      BOOST_REQUIRE(serialized_block == fc::raw::pack(*block));
   }

   fc::temp_directory        dir;
   std::filesystem::path     block_dir;
   std::optional<block_log>  log;
   static constexpr uint32_t last_block_num = 50;
};

BOOST_AUTO_TEST_SUITE(block_log_get_block_tests)

BOOST_FIXTURE_TEST_CASE(basic_block_log, block_log_get_block_fixture) try {
   // test reading a non-last-block
   test_read_serialized_block(*log, last_block_num - 2);

   // test reading last block
   test_read_serialized_block(*log, last_block_num);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(splitted_block_log, block_log_get_block_fixture) try {
   uint32_t stride = last_block_num / 2;
   auto retained_dir = block_dir / "retained";

   block_log::split_blocklog(block_dir, retained_dir, stride);

   std::filesystem::remove(block_dir / "blocks.log");
   std::filesystem::remove(block_dir / "blocks.index");

   block_log blog(block_dir, partitioned_blocklog_config{ .retained_dir = retained_dir });

   // test reading a block in the first partitioned log
   test_read_serialized_block(blog, stride - 1);

   // test reading a block in the second partitioned log
   test_read_serialized_block(blog, stride + 1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(nonexisting_block_num, block_log_get_block_fixture) try {
   // read a non-existing block
   auto serialized_block = log->read_serialized_block_by_num(last_block_num + 1);

   // should return an empty vector of char
   BOOST_REQUIRE(serialized_block.empty());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(corrupted_next_block_position, block_log_get_block_fixture) try {
   // intentionally modify block position for next block (which is the last block)
   uint64_t bad_pos = sizeof(uint64_t) * (last_block_num);
   fc::datastream<fc::cfile> index_file;
   index_file.set_file_path(block_dir / "blocks.index");
   index_file.open(fc::cfile::update_rw_mode);
   index_file.seek_end(-sizeof(uint64_t));
   index_file.write((char*)&bad_pos, sizeof(bad_pos));
   index_file.flush();
   index_file.close();

   BOOST_CHECK_EXCEPTION(log->read_serialized_block_by_num(last_block_num - 1),
                         block_log_exception,
                         fc_exception_message_contains("next block position"));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(corrupted_file_size, block_log_get_block_fixture) try {
   // corrupt file size by truncating it
   auto new_size = log->get_block_pos(last_block_num) + sizeof(uint64_t);
   std::filesystem::resize_file(block_dir / "blocks.log", new_size);

   BOOST_CHECK_EXCEPTION(log->read_serialized_block_by_num(last_block_num),
                         block_log_exception,
                         fc_exception_message_contains("block log file size"));
} FC_LOG_AND_RETHROW()

// ------- Batch read tests -------

BOOST_FIXTURE_TEST_CASE(batch_read_basic, block_log_get_block_fixture) try {
   // Batch read a range of blocks and verify each matches single-read API
   uint32_t start = 5;
   uint32_t count = 10;
   auto batch = log->read_serialized_blocks_by_num(start, count);
   BOOST_REQUIRE_EQUAL(batch.size(), count);

   for (uint32_t i = 0; i < count; ++i) {
      auto single = log->read_serialized_block_by_num(start + i);
      BOOST_REQUIRE(!single.empty());
      BOOST_REQUIRE(batch[i] == single);
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_spanning_head, block_log_get_block_fixture) try {
   // Batch read that includes the last block in the log (file-size edge case)
   uint32_t start = last_block_num - 4;
   uint32_t count = 5;
   auto batch = log->read_serialized_blocks_by_num(start, count);
   BOOST_REQUIRE_EQUAL(batch.size(), count);

   for (uint32_t i = 0; i < count; ++i) {
      auto single = log->read_serialized_block_by_num(start + i);
      BOOST_REQUIRE(batch[i] == single);
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_partial_beyond_head, block_log_get_block_fixture) try {
   // Request extends beyond head block; verify clamped result
   uint32_t start = last_block_num - 2;
   uint32_t count = 10; // asks for 10, only 3 available
   auto batch = log->read_serialized_blocks_by_num(start, count);
   BOOST_REQUIRE_EQUAL(batch.size(), 3u);

   for (uint32_t i = 0; i < batch.size(); ++i) {
      auto single = log->read_serialized_block_by_num(start + i);
      BOOST_REQUIRE(batch[i] == single);
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_count_zero, block_log_get_block_fixture) try {
   auto batch = log->read_serialized_blocks_by_num(1, 0);
   BOOST_REQUIRE(batch.empty());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_count_one, block_log_get_block_fixture) try {
   auto batch = log->read_serialized_blocks_by_num(1, 1);
   BOOST_REQUIRE_EQUAL(batch.size(), 1u);
   auto single = log->read_serialized_block_by_num(1);
   BOOST_REQUIRE(batch[0] == single);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_all_blocks, block_log_get_block_fixture) try {
   // Read all blocks in one batch
   auto batch = log->read_serialized_blocks_by_num(1, last_block_num);
   BOOST_REQUIRE_EQUAL(batch.size(), last_block_num);

   for (uint32_t i = 0; i < last_block_num; ++i) {
      auto single = log->read_serialized_block_by_num(i + 1);
      BOOST_REQUIRE(batch[i] == single);
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_nonexisting_start, block_log_get_block_fixture) try {
   // Start beyond the log's range
   auto batch = log->read_serialized_blocks_by_num(last_block_num + 1, 5);
   BOOST_REQUIRE(batch.empty());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_partitioned_log, block_log_get_block_fixture) try {
   uint32_t stride = last_block_num / 2;
   auto retained_dir = block_dir / "retained";

   block_log::split_blocklog(block_dir, retained_dir, stride);

   std::filesystem::remove(block_dir / "blocks.log");
   std::filesystem::remove(block_dir / "blocks.index");

   block_log blog(block_dir, partitioned_blocklog_config{ .retained_dir = retained_dir });

   // Batch read spanning the partition boundary
   uint32_t start = stride - 3;
   uint32_t count = 8; // crosses the boundary between two catalog files
   auto batch = blog.read_serialized_blocks_by_num(start, count);
   BOOST_REQUIRE_EQUAL(batch.size(), count);

   for (uint32_t i = 0; i < count; ++i) {
      auto single = blog.read_serialized_block_by_num(start + i);
      BOOST_REQUIRE(!single.empty());
      BOOST_REQUIRE(batch[i] == single);
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_multiple_catalog_files, block_log_get_block_fixture) try {
   // Split into small strides to create multiple catalog files (stride=10 on 50 blocks
   // yields 5 catalog files: 1-10, 11-20, 21-30, 31-40, 41-50).
   uint32_t stride = 10;
   auto retained_dir = block_dir / "retained";

   block_log::split_blocklog(block_dir, retained_dir, stride);

   std::filesystem::remove(block_dir / "blocks.log");
   std::filesystem::remove(block_dir / "blocks.index");

   block_log blog(block_dir, partitioned_blocklog_config{ .retained_dir = retained_dir });

   // Batch read spanning multiple catalog file boundaries (blocks 8..33 crosses 3 files)
   uint32_t start = 8;
   uint32_t count = 26;
   auto batch = blog.read_serialized_blocks_by_num(start, count);
   BOOST_REQUIRE_EQUAL(batch.size(), count);

   for (uint32_t i = 0; i < count; ++i) {
      auto single = blog.read_serialized_block_by_num(start + i);
      BOOST_REQUIRE(!single.empty());
      BOOST_REQUIRE(batch[i] == single);
   }

   // Batch read entirely within a single catalog file (blocks 12..18)
   start = 12;
   count = 7;
   batch = blog.read_serialized_blocks_by_num(start, count);
   BOOST_REQUIRE_EQUAL(batch.size(), count);

   for (uint32_t i = 0; i < count; ++i) {
      auto single = blog.read_serialized_block_by_num(start + i);
      BOOST_REQUIRE(batch[i] == single);
   }

   // Batch read all blocks from catalog
   batch = blog.read_serialized_blocks_by_num(1, last_block_num);
   BOOST_REQUIRE_EQUAL(batch.size(), last_block_num);

   for (uint32_t i = 0; i < last_block_num; ++i) {
      auto single = blog.read_serialized_block_by_num(i + 1);
      BOOST_REQUIRE(batch[i] == single);
   }
} FC_LOG_AND_RETHROW()

// ------- Batch read byte-budget tests -------

BOOST_AUTO_TEST_CASE(batch_budget_admission) try {
   // The first block is always admitted, even when it alone exceeds the budget; nothing after it is.
   serialized_block_batch_budget oversized_first(10);
   BOOST_REQUIRE(oversized_first.admit(25));
   BOOST_REQUIRE(!oversized_first.admit(1));
   BOOST_REQUIRE_EQUAL(oversized_first.used(), 25u);
   BOOST_REQUIRE_EQUAL(oversized_first.admitted(), 1u);

   // Blocks that exactly fill the budget are admitted; the next byte is not.
   serialized_block_batch_budget exact_fit(10);
   BOOST_REQUIRE(exact_fit.admit(4));
   BOOST_REQUIRE(exact_fit.admit(6));
   BOOST_REQUIRE(!exact_fit.admit(1));
   BOOST_REQUIRE_EQUAL(exact_fit.used(), 10u);
   BOOST_REQUIRE_EQUAL(exact_fit.admitted(), 2u);

   // A refused block is not recorded, so a smaller one may still fit.
   serialized_block_batch_budget refuse_then_fit(10);
   BOOST_REQUIRE(refuse_then_fit.admit(7));
   BOOST_REQUIRE(!refuse_then_fit.admit(4));
   BOOST_REQUIRE(refuse_then_fit.admit(3));
   BOOST_REQUIRE_EQUAL(refuse_then_fit.used(), 10u);

   // Unlimited admits up to the full range without overflowing the running total.
   serialized_block_batch_budget unlimited(serialized_block_batch_budget::unlimited);
   BOOST_REQUIRE(unlimited.admit(serialized_block_batch_budget::unlimited - 1));
   BOOST_REQUIRE(unlimited.admit(1));
   BOOST_REQUIRE(!unlimited.admit(1));
   BOOST_REQUIRE_EQUAL(unlimited.used(), serialized_block_batch_budget::unlimited);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_byte_budget, block_log_get_block_fixture) try {
   check_batch_byte_budget(*log, 5, 10);

   // A zero budget still returns the first block.
   require_batch_matches(*log, log->read_serialized_blocks_by_num(5, 10, 0), 5, 1);

   // Budget and count both bound the batch: an unlimited budget stops at the end of the log.
   require_batch_matches(*log, log->read_serialized_blocks_by_num(last_block_num - 2, 10), last_block_num - 2, 3);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(batch_read_byte_budget_catalog, block_log_get_block_fixture) try {
   // stride=10 on 50 blocks: catalog files 1-10, 11-20, 21-30, 31-40, 41-50, and no working file
   constexpr uint32_t stride = 10;
   auto retained_dir = block_dir / "retained";

   block_log::split_blocklog(block_dir, retained_dir, stride);

   std::filesystem::remove(block_dir / "blocks.log");
   std::filesystem::remove(block_dir / "blocks.index");

   block_log blog(block_dir, partitioned_blocklog_config{ .retained_dir = retained_dir });

   // blocks 7..14 cross the 10/11 catalog file boundary
   check_batch_byte_budget(blog, 7, 8);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(batch_read_byte_budget_catalog_and_working_file) try {
   // stride=10 on 25 blocks: catalog files 1-10 and 11-20, working file 21-25
   constexpr uint32_t stride         = 10;
   constexpr uint32_t last_block_num = 25;
   fc::temp_directory dir;
   const auto         block_dir    = dir.path();
   const auto         retained_dir = block_dir / "retained";

   block_log blog(block_dir, partitioned_blocklog_config{ .retained_dir = retained_dir, .stride = stride });
   blog.reset(genesis_state(), signed_block::create_signed_block(signed_block::create_mutable_block({})));
   append_empty_blocks(blog, last_block_num);
   BOOST_REQUIRE_EQUAL(blog.head()->block_num(), last_block_num);

   // blocks 17..24 run from the second catalog file into the working file
   check_batch_byte_budget(blog, 17, 8);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
