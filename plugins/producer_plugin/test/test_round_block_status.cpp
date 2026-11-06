#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/round_block_status.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/bitutil.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

namespace {

/// Upper bound on blocks produced while waiting for LIB to reach a height; Savanna finalizes within a few blocks.
constexpr uint32_t max_blocks_to_finality = 50;

/// Produces blocks until `block_num` is at or below LIB, failing the test if it never gets there.
void produce_until_final(base_tester& t, block_num_type block_num) {
   for (uint32_t i = 0; i < max_blocks_to_finality && t.control->fork_db_root().block_num() < block_num; ++i)
      t.produce_block();
   BOOST_REQUIRE_GE(t.control->fork_db_root().block_num(), block_num);
}

/// Returns `id` with one hash bit flipped outside the block number: the id of a different block at the same height.
block_id_type other_block_at_same_height(block_id_type id) {
   id._hash[3] ^= 1;
   return id;
}

/// Returns `id` re-addressed to `block_num`. Mirrors block_header::calculate_id, which keeps the block number in the
/// low 32 bits of the first hash word.
block_id_type id_at_height(block_id_type id, block_num_type block_num) {
   constexpr uint64_t block_num_bits = 0xffffffff;
   id._hash[0] = (id._hash[0] & ~block_num_bits) | fc::endian_reverse_u32(block_num);
   return id;
}

/// Produces a full producing round of blocks and returns them as signed blocks, oldest first.
std::vector<round_signed_block> produce_round(base_tester& t) {
   std::vector<round_signed_block> round;
   for (uint32_t i = 0; i < config::producer_repetitions; ++i) {
      const signed_block_ptr b = t.produce_block();
      round.push_back({b->calculate_id(), b->previous});
   }
   return round;
}

/// Asserts that a round spans final and reversible heights, so it exercises both lookup paths.
void require_round_spans_lib(const base_tester& t, const std::vector<round_signed_block>& round) {
   const block_num_type lib = t.control->fork_db_root().block_num();
   BOOST_REQUIRE_LE(block_header::num_from_id(round.front().id), lib);
   BOOST_REQUIRE_GT(block_header::num_from_id(round.back().id), lib);
}

/// Produces one block, makes it final, then produces a reversible block on top. Returns {final id, reversible id}.
std::pair<block_id_type, block_id_type> produce_final_and_reversible(base_tester& t) {
   const block_id_type final_id = t.produce_block()->calculate_id();
   produce_until_final(t, block_header::num_from_id(final_id));
   t.produce_block();
   BOOST_REQUIRE_LT(t.control->fork_db_root().block_num(), t.head().block_num());
   return {final_id, t.head().id()};
}

} // namespace

BOOST_AUTO_TEST_SUITE(round_block_status_tests)

// A block that became final after it was signed is still on the chain. The round summary used to call it orphaned,
// because the finality core only answers extends() for blocks above LIB.
BOOST_AUTO_TEST_CASE(final_and_reversible_blocks_are_on_head) try {
   tester t;
   const auto [final_id, reversible_id] = produce_final_and_reversible(t);

   BOOST_TEST((classify_round_block(*t.control, final_id) == round_block_status::on_head));
   BOOST_TEST((classify_round_block(*t.control, reversible_id) == round_block_status::on_head));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(another_block_at_the_height_is_orphaned) try {
   tester t;
   const auto [final_id, reversible_id] = produce_final_and_reversible(t);

   // The chain holds a different block at a reversible height: this one was forked out.
   BOOST_TEST((classify_round_block(*t.control, other_block_at_same_height(reversible_id)) ==
               round_block_status::orphaned));
   // The chain holds a different block at a final height.
   BOOST_TEST((classify_round_block(*t.control, other_block_at_same_height(final_id)) ==
               round_block_status::orphaned));
   // The chain never reached this height: the producer's branch lost to a shorter one.
   const block_num_type above_head = t.head().block_num() + 2;
   BOOST_TEST((classify_round_block(*t.control, id_at_height(reversible_id, above_head)) ==
               round_block_status::orphaned));
} FC_LOG_AND_RETHROW()

// Without a block log a final block has nothing left to compare against, so it is reported as unverifiable rather
// than orphaned. Reversible blocks are still checked against the fork database.
BOOST_AUTO_TEST_CASE(final_block_without_block_log_is_unverifiable) try {
   fc::temp_directory tempdir;
   constexpr bool     use_genesis = true;
   tester t(tempdir, [](controller::config& cfg) { cfg.blog = empty_blocklog_config{}; }, use_genesis);
   t.execute_setup_policy(setup_policy::full); // sets the finalizers LIB needs to advance

   const block_id_type final_id = t.produce_block()->calculate_id();
   // strictly below LIB, so the block has also left the fork database
   produce_until_final(t, block_header::num_from_id(final_id) + 1);
   t.produce_block();
   BOOST_REQUIRE_LT(t.control->fork_db_root().block_num(), t.head().block_num());

   BOOST_TEST((classify_round_block(*t.control, final_id) == round_block_status::unverifiable));
   BOOST_TEST((classify_round_block(*t.control, t.head().id()) == round_block_status::on_head));
   BOOST_TEST((classify_round_block(*t.control, other_block_at_same_height(t.head().id())) ==
               round_block_status::orphaned));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(consecutive_round_is_all_on_head) try {
   tester t;
   const std::vector<round_signed_block> round = produce_round(t);
   require_round_spans_lib(t, round);

   const round_block_counts counts = classify_round_blocks(*t.control, round);
   BOOST_TEST(counts.on_head == round.size());
   BOOST_TEST(counts.orphaned == 0u);
   BOOST_TEST(counts.unverifiable == 0u);
} FC_LOG_AND_RETHROW()

// An orphaned block breaks the parent chain, so the block before it is looked up again rather than assumed.
BOOST_AUTO_TEST_CASE(orphaned_block_does_not_hide_older_blocks) try {
   tester t;
   std::vector<round_signed_block> round = produce_round(t);
   require_round_spans_lib(t, round);
   const size_t middle = round.size() / 2;
   round[middle].id    = other_block_at_same_height(round[middle].id);

   const round_block_counts counts = classify_round_blocks(*t.control, round);
   BOOST_TEST(counts.on_head == round.size() - 1);
   BOOST_TEST(counts.orphaned == 1u);
   BOOST_TEST(counts.unverifiable == 0u);
} FC_LOG_AND_RETHROW()

// Without a block log, a final block is still provably on head when its on-head child names it as `previous`.
// A round that is entirely final has no such child, so its blocks are unverifiable.
BOOST_AUTO_TEST_CASE(round_without_block_log_is_proven_through_parents) try {
   fc::temp_directory tempdir;
   constexpr bool     use_genesis = true;
   tester t(tempdir, [](controller::config& cfg) { cfg.blog = empty_blocklog_config{}; }, use_genesis);
   t.execute_setup_policy(setup_policy::full); // sets the finalizers LIB needs to advance

   const std::vector<round_signed_block> round = produce_round(t);
   require_round_spans_lib(t, round);
   const round_block_counts spanning = classify_round_blocks(*t.control, round);
   BOOST_TEST(spanning.on_head == round.size());
   BOOST_TEST(spanning.unverifiable == 0u);

   // strictly below LIB, so the newest block has also left the fork database
   produce_until_final(t, block_header::num_from_id(round.back().id) + 1);
   const round_block_counts all_final = classify_round_blocks(*t.control, round);
   BOOST_TEST(all_final.unverifiable == round.size());
   BOOST_TEST(all_final.on_head == 0u);
   BOOST_TEST(all_final.orphaned == 0u);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
