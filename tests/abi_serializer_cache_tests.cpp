#include <boost/test/unit_test.hpp>

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain_plugin/abi_serializer_cache.hpp>

#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sysio;
using namespace sysio::chain;
using sysio::chain_apis::abi_serializer_cache;

namespace {

/// Budget generous enough that no build in these tests runs out of time.
constexpr fc::microseconds ample_budget = fc::seconds(30);

/// Packed ABI declaring one struct named `struct_name` with a field of type `field_type`.
bytes packed_abi(const std::string& struct_name, const std::string& field_type = "uint8") {
   abi_def abi;
   abi.version = "sysio::abi/1.0";
   abi.structs.emplace_back(struct_name, "", std::vector<field_def>{{"v", field_type}});
   return fc::raw::pack(abi);
}

/// Bytes a cache is charged for holding `abi`.
size_t charge_of(const bytes& abi) {
   return abi.size() + abi_serializer_cache::entry_overhead_bytes;
}

} // namespace

namespace sysio::chain_apis {

/// Stores build outcomes directly, in the order concurrent builds of the same bytes might finish.
struct abi_serializer_cache_test_access {
   using entry = abi_serializer_cache::entry;

   /// The outcome of a build of `abi` that ran out of `budget` at `failed_at`.
   static entry timed_out(const bytes& abi, const fc::microseconds& budget, const fc::time_point& failed_at) {
      entry outcome;
      outcome.error        = abi_serialization_deadline_exception(FC_LOG_MESSAGE(error, "out of time"))
                                .dynamic_copy_exception();
      outcome.timed_out    = true;
      outcome.build_budget = budget;
      outcome.failed_at    = failed_at;
      outcome.charge       = charge_of(abi);
      return outcome;
   }

   /// Stores `outcome` for `abi` and returns what `cache` then reports for it.
   static abi_serializer_ptr store(abi_serializer_cache& cache, const bytes& abi, const entry& outcome) {
      return abi_serializer_cache::result_of(cache.store(fc::sha256::hash(abi.data(), abi.size()), outcome));
   }
};

} // namespace sysio::chain_apis

using sysio::chain_apis::abi_serializer_cache_test_access;

BOOST_AUTO_TEST_SUITE(abi_serializer_cache_tests)

/// A second request for the same bytes gets the serializer the first built; other bytes get their own.
BOOST_AUTO_TEST_CASE(hit_and_miss) try {
   abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   const bytes a = packed_abi("a");
   const bytes b = packed_abi("b");

   const abi_serializer_ptr first = cache.get(a, ample_budget);
   BOOST_REQUIRE(first);
   BOOST_CHECK(first->is_struct("a"));
   BOOST_CHECK(cache.get(a, ample_budget) == first);
   BOOST_CHECK_EQUAL(cache.builds(), 1u);

   const abi_serializer_ptr other = cache.get(b, ample_budget);
   BOOST_REQUIRE(other);
   BOOST_CHECK(other != first);
   BOOST_CHECK(other->is_struct("b"));
   BOOST_CHECK_EQUAL(cache.builds(), 2u);
   BOOST_CHECK_EQUAL(cache.size(), 2u);
   BOOST_CHECK_EQUAL(cache.charged_bytes(), charge_of(a) + charge_of(b));

   BOOST_CHECK(!cache.get(bytes{}, ample_budget));
   BOOST_CHECK_EQUAL(cache.builds(), 2u);
} FC_LOG_AND_RETHROW()

/// The least recently used entry goes first once the bytes charged pass the bound; one larger than the bound is
/// built but never kept.
BOOST_AUTO_TEST_CASE(evicts_least_recently_used_by_bytes) try {
   const bytes a = packed_abi("a");
   const bytes b = packed_abi("b");
   const bytes c = packed_abi("c");
   abi_serializer_cache cache(charge_of(a) + charge_of(b));

   const abi_serializer_ptr kept = cache.get(a, ample_budget);
   cache.get(b, ample_budget);
   BOOST_CHECK(cache.get(a, ample_budget) == kept); // a is now the most recently used
   cache.get(c, ample_budget);                     // evicts b
   BOOST_CHECK_EQUAL(cache.size(), 2u);
   BOOST_CHECK_LE(cache.charged_bytes(), charge_of(a) + charge_of(b));
   BOOST_CHECK_EQUAL(cache.builds(), 3u);

   BOOST_CHECK(cache.get(a, ample_budget) == kept);
   BOOST_CHECK_EQUAL(cache.builds(), 3u);
   cache.get(b, ample_budget);
   BOOST_CHECK_EQUAL(cache.builds(), 4u);

   abi_serializer_cache tiny(charge_of(a) - 1);
   BOOST_CHECK(tiny.get(a, ample_budget));
   BOOST_CHECK(tiny.get(a, ample_budget));
   BOOST_CHECK_EQUAL(tiny.builds(), 2u);
   BOOST_CHECK_EQUAL(tiny.size(), 0u);
   BOOST_CHECK_EQUAL(tiny.charged_bytes(), 0u);
} FC_LOG_AND_RETHROW()

/// A failed build is cached and rethrown with its type, without building again.
BOOST_AUTO_TEST_CASE(caches_failures) try {
   abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   const bytes invalid = packed_abi("bad", "no_such_type");
   for (int attempt = 0; attempt < 2; ++attempt)
      BOOST_CHECK_THROW(cache.get(invalid, ample_budget), invalid_type_inside_abi);
   BOOST_CHECK_EQUAL(cache.builds(), 1u);

   const bytes garbage(64, char(0xff));
   for (int attempt = 0; attempt < 2; ++attempt)
      BOOST_CHECK_THROW(cache.get(garbage, ample_budget), fc::exception);
   BOOST_CHECK_EQUAL(cache.builds(), 2u);
   BOOST_CHECK_EQUAL(cache.size(), 2u);
} FC_LOG_AND_RETHROW()

/// A build that ran out of time is reused only by callers with no larger budget; a larger budget builds again and
/// replaces it.
BOOST_AUTO_TEST_CASE(timed_out_build_retried_with_larger_budget) try {
   abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   const bytes            a     = packed_abi("a");
   const fc::microseconds spent = fc::milliseconds(10);

   BOOST_CHECK_THROW(abi_serializer_cache_test_access::store(
                        cache, a, abi_serializer_cache_test_access::timed_out(a, spent, fc::time_point::now())),
                     abi_serialization_deadline_exception);
   for (int attempt = 0; attempt < 2; ++attempt)
      BOOST_CHECK_THROW(cache.get(a, spent), abi_serialization_deadline_exception);
   BOOST_CHECK_EQUAL(cache.builds(), 0u);

   const abi_serializer_ptr built = cache.get(a, ample_budget);
   BOOST_REQUIRE(built);
   BOOST_CHECK_EQUAL(cache.builds(), 1u);
   BOOST_CHECK(cache.get(a, fc::microseconds(0)) == built);
   BOOST_CHECK_EQUAL(cache.builds(), 1u);
   BOOST_CHECK_EQUAL(cache.size(), 1u);
} FC_LOG_AND_RETHROW()

/// A build that ran out of time and finished after another caller stored a serializer or a lasting failure for the
/// same bytes leaves that entry, and its caller gets what the entry holds.
BOOST_AUTO_TEST_CASE(late_time_out_keeps_stored_outcome) try {
   abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   const bytes a = packed_abi("a");
   const abi_serializer_ptr built = cache.get(a, ample_budget);
   BOOST_REQUIRE(built);

   const auto late = abi_serializer_cache_test_access::timed_out(a, fc::milliseconds(10), fc::time_point::now());
   BOOST_CHECK(abi_serializer_cache_test_access::store(cache, a, late) == built);
   BOOST_CHECK(cache.get(a, fc::milliseconds(10)) == built);

   const bytes invalid = packed_abi("bad", "no_such_type");
   BOOST_CHECK_THROW(cache.get(invalid, ample_budget), invalid_type_inside_abi);
   BOOST_CHECK_THROW(abi_serializer_cache_test_access::store(
                        cache, invalid,
                        abi_serializer_cache_test_access::timed_out(invalid, ample_budget, fc::time_point::now())),
                     invalid_type_inside_abi);
   BOOST_CHECK_THROW(cache.get(invalid, ample_budget), invalid_type_inside_abi);
   BOOST_CHECK_EQUAL(cache.builds(), 2u);
   BOOST_CHECK_EQUAL(cache.size(), 2u);
} FC_LOG_AND_RETHROW()

/// Of two builds that ran out of time, the one with the larger budget stays until it is due for a retry, so callers
/// with no more budget than it had reuse its failure.
BOOST_AUTO_TEST_CASE(time_out_with_larger_budget_stays_until_due) try {
   using access = abi_serializer_cache_test_access;
   abi_serializer_cache   cache(abi_serializer_cache::default_max_bytes);
   const bytes            a     = packed_abi("a");
   const fc::time_point   now   = fc::time_point::now();
   const fc::microseconds large = fc::milliseconds(100);
   const fc::microseconds small = fc::milliseconds(10);

   BOOST_CHECK_THROW(access::store(cache, a, access::timed_out(a, large, now)), abi_serialization_deadline_exception);
   BOOST_CHECK_THROW(access::store(cache, a, access::timed_out(a, small, now)), abi_serialization_deadline_exception);
   BOOST_CHECK_THROW(cache.get(a, fc::milliseconds(50)), abi_serialization_deadline_exception);
   BOOST_CHECK_EQUAL(cache.builds(), 0u); // the 100 ms time-out stayed: a 50 ms budget is no larger

   // A time-out due for a retry gives way even to one with a smaller budget.
   const fc::time_point long_ago = now - abi_serializer_cache::timed_out_retry_interval - fc::seconds(1);
   BOOST_CHECK_THROW(access::store(cache, a, access::timed_out(a, fc::seconds(1), long_ago)),
                     abi_serialization_deadline_exception);
   BOOST_CHECK_THROW(access::store(cache, a, access::timed_out(a, small, now)), abi_serialization_deadline_exception);
   BOOST_CHECK(cache.get(a, fc::milliseconds(50))); // larger than the 10 ms time-out now stored: built again
   BOOST_CHECK_EQUAL(cache.builds(), 1u);
   BOOST_CHECK_EQUAL(cache.size(), 1u);
   BOOST_CHECK_EQUAL(cache.charged_bytes(), charge_of(a));
} FC_LOG_AND_RETHROW()

/// A build may take its own limit or the time left before the deadline, whichever is less, and none once the deadline
/// has passed.
BOOST_AUTO_TEST_CASE(build_budget_is_cut_short_by_deadline) try {
   const fc::time_point   now   = fc::time_point::now();
   const fc::microseconds limit = fc::milliseconds(100);

   BOOST_CHECK(abi_serializer_cache::build_budget(limit, fc::time_point::maximum(), now) == limit);
   BOOST_CHECK(abi_serializer_cache::build_budget(fc::microseconds::maximum(), fc::time_point::maximum(), now) ==
               fc::microseconds::maximum());
   BOOST_CHECK(abi_serializer_cache::build_budget(limit, now + fc::seconds(1), now) == limit);
   BOOST_CHECK(abi_serializer_cache::build_budget(limit, now + fc::milliseconds(40), now) == fc::milliseconds(40));
   BOOST_CHECK(abi_serializer_cache::build_budget(fc::microseconds::maximum(), now + fc::milliseconds(40), now) ==
               fc::milliseconds(40));
   BOOST_CHECK(abi_serializer_cache::build_budget(limit, now, now) == fc::microseconds(0));
   BOOST_CHECK(abi_serializer_cache::build_budget(limit, now - fc::seconds(1), now) == fc::microseconds(0));
   BOOST_CHECK(abi_serializer_cache::build_budget(fc::microseconds(0), now + fc::seconds(1), now) ==
               fc::microseconds(0));
} FC_LOG_AND_RETHROW()

/// A miss with no time left, whether its deadline has passed or its limit is zero, fails for want of time without
/// unpacking the ABI or storing anything; a serializer built earlier is still returned.
BOOST_AUTO_TEST_CASE(miss_without_budget_does_not_build) try {
   abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   const fc::time_point passed = fc::time_point::now() - fc::seconds(1);

   // Bytes that fail to unpack: the failure is the deadline's, so they were never unpacked.
   const bytes garbage(64, char(0xff));
   BOOST_CHECK_THROW(cache.get(garbage, ample_budget, passed), abi_serialization_deadline_exception);
   BOOST_CHECK_THROW(cache.get(garbage, fc::microseconds(0)), abi_serialization_deadline_exception);
   BOOST_CHECK_EQUAL(cache.builds(), 0u);
   BOOST_CHECK_EQUAL(cache.size(), 0u);
   BOOST_CHECK_THROW(cache.get(garbage, ample_budget), fc::exception);
   BOOST_CHECK_EQUAL(cache.builds(), 1u);

   const bytes a = packed_abi("a");
   BOOST_CHECK_THROW(cache.get(a, ample_budget, passed), abi_serialization_deadline_exception);
   const abi_serializer_ptr built = cache.get(a, ample_budget, fc::time_point::now() + ample_budget);
   BOOST_REQUIRE(built);
   BOOST_CHECK_EQUAL(cache.builds(), 2u);
   BOOST_CHECK(cache.get(a, ample_budget, passed) == built);
   BOOST_CHECK(cache.get(a, fc::microseconds(0)) == built);
   BOOST_CHECK_EQUAL(cache.builds(), 2u);
} FC_LOG_AND_RETHROW()

/// Threads asking for the same bytes at once all get a usable serializer, and later requests share one.
BOOST_AUTO_TEST_CASE(concurrent_requests) try {
   abi_serializer_cache cache(abi_serializer_cache::default_max_bytes);
   const bytes a = packed_abi("a");
   constexpr size_t thread_count = 8;

   std::vector<abi_serializer_ptr> results(thread_count);
   std::vector<std::thread>        threads;
   for (size_t i = 0; i < thread_count; ++i)
      threads.emplace_back([&cache, &a, &results, i] { results[i] = cache.get(a, ample_budget); });
   for (std::thread& t : threads)
      t.join();

   for (const abi_serializer_ptr& result : results) {
      BOOST_REQUIRE(result);
      BOOST_CHECK(result->is_struct("a"));
   }
   BOOST_CHECK_EQUAL(cache.size(), 1u);
   const abi_serializer_ptr shared = cache.get(a, ample_budget);
   BOOST_CHECK(cache.get(a, ample_budget) == shared);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
