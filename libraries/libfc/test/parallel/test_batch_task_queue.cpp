#include <algorithm>
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fc-test/wait_until.hpp>
#include <fc/exception/exception.hpp>
#include <fc/parallel/batch_task_queue.hpp>
#include <fc/scoped_exit.hpp>
#include <memory>
#include <mutex>
#include <numeric>
#include <semaphore>
#include <span>
#include <vector>

using namespace fc::parallel;
using fc::test::wait_until;

namespace {

constexpr std::size_t batch_limit = 100;
constexpr int item_count = 250;
/// Items pushed while the first batch (a single item) is held inside its callback.
constexpr std::size_t items_behind_held_batch = static_cast<std::size_t>(item_count - 1);
/// The held batch, then the items behind it in full batches plus one remainder.
constexpr std::size_t expected_batches = 1 + (items_behind_held_batch + batch_limit - 1) / batch_limit;
constexpr std::size_t pending_limit = 2;
/// Workers and item count for the multi-threaded case: enough of both that the drains genuinely overlap.
constexpr uint64_t multi_thread_workers = 4;
constexpr int multi_thread_item_count = 1000;

/// Batch sizes and the concatenated items a callback has seen, guarded for the worker thread.
struct batch_recorder {
   std::mutex mtx;
   std::vector<std::size_t> sizes;
   std::vector<int> items;
   std::atomic<int> delivered{0};

   void record(std::span<int> batch) {
      std::lock_guard<std::mutex> lk(mtx);
      sizes.push_back(batch.size());
      items.insert(items.end(), batch.begin(), batch.end());
      delivered.fetch_add(static_cast<int>(batch.size()));
   }
};

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(batch_task_queue_tests)

// The worker drains what is queued when it wakes: a single item is delivered as a batch of one, not held
// until max_items_per_task items exist.
BOOST_AUTO_TEST_CASE(a_lone_item_is_delivered_without_waiting_for_a_full_batch) {
   batch_recorder seen;
   auto q = batch_task_queue<int>::create({.max_items_per_task = batch_limit},
                                          [&](std::span<int> batch) { seen.record(batch); });
   // The worker holds a shared_ptr to the queue, so a failed REQUIRE must still stop it before `seen` goes away.
   auto stop_on_exit = fc::make_scoped_exit([&] { q->stop(); });
   BOOST_REQUIRE(q->try_push(7));
   BOOST_REQUIRE(wait_until([&] { return seen.delivered.load() >= 1; }));
   q->stop();
   std::lock_guard<std::mutex> lk(seen.mtx);
   BOOST_REQUIRE_EQUAL(seen.sizes.size(), 1u);
   BOOST_CHECK_EQUAL(seen.sizes[0], 1u);
   BOOST_CHECK_EQUAL(seen.items[0], 7);
}

// While the callback is blocked, items accumulate; the following batches carry at most max_items_per_task
// each, in queue order, and a callback in progress does not consume pending capacity: with the limit set to
// the number of items pushed behind the held batch, every one of them is admitted and only the next is refused.
BOOST_AUTO_TEST_CASE(batches_are_bounded_by_max_items_per_task_and_preserve_order) {
   batch_recorder seen;
   std::binary_semaphore first_batch_started{0};
   std::binary_semaphore release_first_batch{0};
   std::atomic<bool> first{true};
   std::atomic<bool> released{false};
   auto q = batch_task_queue<int>::create(
      {.max_items_per_task = batch_limit, .max_pending_items = items_behind_held_batch}, [&](std::span<int> batch) {
         if (first.exchange(false)) {
            first_batch_started.release();
            release_first_batch.acquire();
         }
         seen.record(batch);
      });
   // A binary semaphore may be released only once. The guard runs on every exit, including a failed REQUIRE:
   // it releases the held callback, then stops the queue -- a join that is bounded only because the callback
   // was released. Dropping q alone would not do this: the worker holds a shared_ptr to the queue, so q's own
   // scope exit merely drops a reference.
   auto release_once = [&] {
      if (!released.exchange(true))
         release_first_batch.release();
   };
   auto release_and_stop_on_exit = fc::make_scoped_exit([&] {
      release_once();
      q->stop();
   });
   BOOST_REQUIRE(q->try_push(0));
   BOOST_REQUIRE(first_batch_started.try_acquire_for(fc::test::wait_budget));
   for (int i = 1; i < item_count; ++i)
      BOOST_REQUIRE(q->try_push(i));
   BOOST_CHECK_EQUAL(q->size(), items_behind_held_batch);
   BOOST_CHECK(!q->try_push(item_count)); // the pending limit is reached; the held item does not count
   release_once();
   BOOST_REQUIRE(wait_until([&] { return seen.delivered.load() >= item_count; }));
   q->stop();

   std::lock_guard<std::mutex> lk(seen.mtx);
   BOOST_REQUIRE_EQUAL(seen.items.size(), static_cast<std::size_t>(item_count));
   for (int i = 0; i < item_count; ++i)
      BOOST_CHECK_EQUAL(seen.items[i], i);
   // The worker was parked in the held callback while every other item was pushed, so the drains after its
   // release are exactly full batches followed by one remainder.
   BOOST_REQUIRE_EQUAL(seen.sizes.size(), expected_batches);
   BOOST_CHECK_EQUAL(seen.sizes.front(), 1u); // the batch that was in progress
   for (std::size_t i = 1; i + 1 < expected_batches; ++i)
      BOOST_CHECK_EQUAL(seen.sizes[i], batch_limit);
   BOOST_CHECK_EQUAL(seen.sizes.back(), items_behind_held_batch - (expected_batches - 2) * batch_limit);
}

BOOST_AUTO_TEST_CASE(bounded_queue_rejects_items_beyond_pending_limit) {
   auto q = batch_task_queue<int>::create(
      {.max_items_per_task = batch_limit, .skip_autostart = true, .max_pending_items = pending_limit},
      [](std::span<int>) {});
   BOOST_TEST(q->try_push(1));
   BOOST_TEST(q->try_push(2));
   BOOST_TEST(!q->try_push(3));
   BOOST_TEST(q->size() == pending_limit);
   q->stop();
}

// The span is a view of the worker's buffer: the callback owns whatever it moves out.
BOOST_AUTO_TEST_CASE(callback_may_move_items_out_of_the_span) {
   std::mutex mtx;
   std::vector<std::unique_ptr<int>> taken;
   std::atomic<int> delivered{0};
   auto q = batch_task_queue<std::unique_ptr<int>>::create({.max_items_per_task = batch_limit},
                                                           [&](std::span<std::unique_ptr<int>> batch) {
                                                              std::lock_guard<std::mutex> lk(mtx);
                                                              for (auto& item : batch)
                                                                 taken.push_back(std::move(item));
                                                              delivered.fetch_add(static_cast<int>(batch.size()));
                                                           });
   // The worker holds a shared_ptr to the queue, so a failed REQUIRE must still stop it before `taken` goes away.
   auto stop_on_exit = fc::make_scoped_exit([&] { q->stop(); });
   BOOST_REQUIRE(q->try_push(std::make_unique<int>(1)));
   BOOST_REQUIRE(q->try_push(std::make_unique<int>(2)));
   BOOST_REQUIRE(wait_until([&] { return delivered.load() >= 2; }));
   q->stop();
   std::lock_guard<std::mutex> lk(mtx);
   BOOST_REQUIRE_EQUAL(taken.size(), 2u);
   BOOST_CHECK_EQUAL(*taken[0] + *taken[1], 3);
}

// stop() joins without draining; discard_pending() reports what was left and empties the queue.
BOOST_AUTO_TEST_CASE(stop_leaves_pending_items_and_discard_pending_counts_them) {
   auto q =
      batch_task_queue<int>::create({.max_items_per_task = batch_limit, .skip_autostart = true}, [](std::span<int>) {});
   BOOST_REQUIRE(q->try_push(1));
   BOOST_REQUIRE(q->try_push(2));
   q->stop();
   BOOST_TEST(!q->running());
   BOOST_TEST(!q->try_push(3));
   BOOST_CHECK_EQUAL(q->size(), 2u);
   BOOST_CHECK_EQUAL(q->discard_pending(), 2u);
   BOOST_CHECK_EQUAL(q->size(), 0u);
   q->stop(); // idempotent
}

// A stopped queue stays stopped: start() afterwards neither runs a worker nor admits items.
BOOST_AUTO_TEST_CASE(start_after_stop_is_a_no_op) {
   auto q =
      batch_task_queue<int>::create({.max_items_per_task = batch_limit, .skip_autostart = true}, [](std::span<int>) {});
   q->stop();
   q->start();
   BOOST_TEST(!q->running());
   BOOST_TEST(!q->try_push(1));
   BOOST_CHECK_EQUAL(q->size(), 0u);
}

// Several workers drain the same queue at once, so neither the order across batches nor the partition into
// batches is specified -- what holds is that every admitted item reaches a callback exactly once.
BOOST_AUTO_TEST_CASE(multi_thread_delivers_every_item_exactly_once) {
   batch_recorder seen;
   auto q = batch_task_queue<int>::create({.max_threads = multi_thread_workers, .max_items_per_task = batch_limit},
                                          [&](std::span<int> batch) { seen.record(batch); });
   // Each worker holds a shared_ptr to the queue, so a failed REQUIRE must still stop them before `seen` goes away.
   auto stop_on_exit = fc::make_scoped_exit([&] { q->stop(); });
   for (int i = 0; i < multi_thread_item_count; ++i)
      BOOST_REQUIRE(q->try_push(i));
   BOOST_REQUIRE(wait_until([&] { return seen.delivered.load() >= multi_thread_item_count; }));
   q->stop();

   std::lock_guard<std::mutex> lk(seen.mtx);
   // Every value exactly once: sorting the concatenated items must reproduce the pushed sequence.
   BOOST_REQUIRE_EQUAL(seen.items.size(), static_cast<std::size_t>(multi_thread_item_count));
   std::vector<int> delivered = seen.items;
   std::sort(delivered.begin(), delivered.end());
   for (int i = 0; i < multi_thread_item_count; ++i)
      BOOST_CHECK_EQUAL(delivered[static_cast<std::size_t>(i)], i);
   // The recorded batch sizes account for those items and nothing else.
   BOOST_CHECK_EQUAL(std::accumulate(seen.sizes.begin(), seen.sizes.end(), std::size_t{0}),
                     static_cast<std::size_t>(multi_thread_item_count));
}

BOOST_AUTO_TEST_CASE(zero_max_items_per_task_is_rejected) {
   BOOST_CHECK_THROW(batch_task_queue<int>::create({.max_items_per_task = 0}, [](std::span<int>) {}), fc::exception);
}

/// A pool of zero threads is accepted by boost::asio and runs nothing, so create() rejects the count rather
/// than hand back a queue that reports itself running while it delivers no batch.
BOOST_AUTO_TEST_CASE(zero_max_threads_is_rejected) {
   BOOST_CHECK_THROW(
      batch_task_queue<int>::create({.max_threads = 0, .max_items_per_task = batch_limit}, [](std::span<int>) {}),
      fc::exception);
}

BOOST_AUTO_TEST_SUITE_END()
