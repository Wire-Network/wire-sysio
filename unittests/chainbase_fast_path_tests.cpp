// Compile-time verification that every chainbase-backed production table's
// secondary-index `boost::multi_index::member<>` extractor passes
// chainbase::detail::fast_path_eligible_v. If a future table change replaces
// a trivially-copyable field with a shallow/aliasing or non-trivial type,
// one of these static_asserts will fail at build time, preventing a silent
// loss of the post_modify fast path (or worse, a consensus-divergent
// aliasing hazard if the exclusion trait misses a new aliasing type).
//
// Only tables registered via CHAINBASE_SET_INDEX_TYPE are covered here;
// unapplied_transaction_queue / vote_processor / wasm_interface_private use
// plain boost::multi_index_container (no undo_index, no fast path).
//
// Primary (by_id) indexes are intentionally not asserted: modify() never
// mutates the id field and post_modify starts at N=1, so by_id eligibility
// does not affect the fast path. Composite-key indexes are intentionally
// not eligible and always take the full walk (see
// test_composite_key_modify in libraries/chaindb/test/undo_index.cpp).

#include <chainbase/undo_index.hpp>

#include <sysio/chain/account_object.hpp>
#include <sysio/chain/resource_limits_private.hpp>

#include <boost/multi_index/member.hpp>

#include <boost/test/unit_test.hpp>

namespace {
   namespace bmi = boost::multi_index;
   using chainbase::detail::fast_path_eligible_v;

   using sysio::chain::account_object;
   using sysio::chain::account_metadata_object;
   using sysio::chain::account_name;

   static_assert(fast_path_eligible_v<bmi::member<account_object,          account_name, &account_object::name>>);
   static_assert(fast_path_eligible_v<bmi::member<account_metadata_object, account_name, &account_metadata_object::name>>);

   using sysio::chain::resource_limits::resource_object;
   using sysio::chain::resource_limits::resource_pending_object;

   static_assert(fast_path_eligible_v<bmi::member<resource_object,         account_name, &resource_object::owner>>);
   static_assert(fast_path_eligible_v<bmi::member<resource_pending_object, account_name, &resource_pending_object::owner>>);
}

BOOST_AUTO_TEST_SUITE(chainbase_fast_path_tests)

// Single runtime test case so Boost.Test records something; the real work
// is the static_asserts above, which fire at build time.
BOOST_AUTO_TEST_CASE(verify_production_extractors_eligible) {
   BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()
