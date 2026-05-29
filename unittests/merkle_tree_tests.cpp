#include <sysio/chain/incremental_merkle.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/crypto/sha256.hpp>

using namespace sysio::chain;

std::vector<digest_type> create_test_digests(size_t n) {
   std::vector<digest_type> v;
   v.reserve(n);
   for (size_t i=0; i<n; ++i)
      v.push_back(fc::sha256::hash(std::string{"Node"} + std::to_string(i)));
   return v;
}

constexpr auto hash = sysio::chain::detail::hash_combine;

BOOST_AUTO_TEST_SUITE(merkle_tree_tests)

BOOST_AUTO_TEST_CASE(basic_append_and_root_check) {
   incremental_merkle_tree tree;
   BOOST_CHECK_EQUAL(tree.get_root(), fc::sha256());

   auto node1 = fc::sha256::hash("Node1");
   tree.append(node1);
   BOOST_CHECK_EQUAL(tree.get_root(), node1);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(&node1, 1)), node1);
}

BOOST_AUTO_TEST_CASE(multiple_appends) {
   incremental_merkle_tree tree;
   auto node1 = fc::sha256::hash("Node1");
   auto node2 = fc::sha256::hash("Node2");
   auto node3 = fc::sha256::hash("Node3");
   auto node4 = fc::sha256::hash("Node4");
   auto node5 = fc::sha256::hash("Node5");
   auto node6 = fc::sha256::hash("Node6");
   auto node7 = fc::sha256::hash("Node7");
   auto node8 = fc::sha256::hash("Node8");
   auto node9 = fc::sha256::hash("Node9");

   std::vector<digest_type> digests { node1, node2, node3, node4, node5, node6, node7, node8, node9 };
   auto first = digests.cbegin();

   tree.append(node1);
   BOOST_CHECK_EQUAL(tree.get_root(), node1);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 1)), node1);

   tree.append(node2);
   BOOST_CHECK_EQUAL(tree.get_root(), hash(node1, node2));
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 2)), hash(node1, node2));

   tree.append(node3);
   auto calculated_root = hash(hash(node1, node2), node3);
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 3)), calculated_root);

   tree.append(node4);
   auto first_four_tree = hash(hash(node1, node2), hash(node3, node4));
   calculated_root = first_four_tree;
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 4)), calculated_root);

   tree.append(node5);
   calculated_root = hash(first_four_tree, node5);
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 5)), calculated_root);

   tree.append(node6);
   calculated_root = hash(first_four_tree, hash(node5, node6));
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 6)), calculated_root);

   tree.append(node7);
   calculated_root = hash(first_four_tree, hash(hash(node5, node6), node7));
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 7)), calculated_root);

   tree.append(node8);
   auto next_four_tree = hash(hash(node5, node6), hash(node7, node8));
   calculated_root = hash(first_four_tree, next_four_tree);
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 8)), calculated_root);

   tree.append(node9);
   calculated_root = hash(hash(first_four_tree, next_four_tree), node9);
   BOOST_CHECK_EQUAL(tree.get_root(), calculated_root);
   BOOST_CHECK_EQUAL(calculate_merkle(std::span(first, 9)), calculated_root);
}

BOOST_AUTO_TEST_CASE(consistency_over_large_range) {
   constexpr size_t num_digests = 1001ull;

   const std::vector<digest_type> digests = create_test_digests(num_digests);
   for (size_t i=1; i<num_digests; ++i) {
      incremental_merkle_tree tree;
      for (size_t j=0; j<i; ++j)
         tree.append(digests[j]);
      BOOST_CHECK_EQUAL(tree.num_digests_appended(), i);
      BOOST_CHECK_EQUAL(calculate_merkle(std::span(digests.begin(), i)), tree.get_root());
   }
}

// calculate_merkle dispatches to a multi-threaded implementation at two
// thresholds (detail::calculate_merkle_pow2):
//   size >= 256  -> 2 threads
//   size >= 2048 -> 4 threads
// Output must be byte-identical to the single-threaded path and stable across
// repeated invocations. Consensus safety depends on this: every node that
// computes a merkle root over the same input must get the same bytes regardless
// of how the work is partitioned across threads.
BOOST_AUTO_TEST_CASE(async_matches_sequential_and_is_reproducible) {
   // Sizes chosen to span both async thresholds and boundary cases: just below
   // 256, exactly at 256, between the two thresholds, exactly at 2048, and well
   // past the 4-thread threshold.
   const std::vector<size_t> sizes = {
      255, 256, 257,    // 2-thread boundary
      512, 1024,        // within 2-thread range
      2047, 2048, 2049, // 4-thread boundary
      4096, 8192        // well past
   };

   // Build the largest vector once; slice with std::span to exercise each size.
   const std::vector<digest_type> digests = create_test_digests(sizes.back());

   for (size_t n : sizes) {
      auto input = std::span(digests.begin(), n);

      // Cross-check async calculate_merkle against the purely-sequential
      // incremental_merkle_tree to pin down the expected byte pattern.
      incremental_merkle_tree tree;
      for (size_t j = 0; j < n; ++j) tree.append(digests[j]);
      const digest_type expected = tree.get_root();

      const digest_type async_first = calculate_merkle(input);
      BOOST_CHECK_MESSAGE(async_first == expected,
         "async calculate_merkle diverged from sequential tree at size " << n);

      // Reproducibility: re-running the same computation must return the same
      // bytes. 10 iterations is enough to surface any thread-race that alters
      // output (probabilistically; if races existed, they'd show up reliably
      // at these sizes since the thread pool schedules all slices concurrently).
      for (int i = 0; i < 10; ++i) {
         BOOST_CHECK_MESSAGE(calculate_merkle(input) == expected,
            "calculate_merkle not reproducible at size " << n << " on iteration " << i);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
