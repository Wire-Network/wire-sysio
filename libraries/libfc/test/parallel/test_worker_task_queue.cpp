#include <boost/test/unit_test.hpp>

#include <fc/parallel/worker_task_queue.hpp>

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

using namespace fc::parallel;

BOOST_AUTO_TEST_SUITE(worker_task_queue_tests)

BOOST_AUTO_TEST_CASE(single_thread_processes_all_items) {
   constexpr int count = 100;
   std::atomic<int> processed{0};

   auto q = worker_task_queue<int>::create({.max_threads = 1}, [&](int&) { ++processed; });

   for (int i = 0; i < count; ++i)
      q->push(i);

   // give workers time to drain
   while (processed.load() < count)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->stop();
   BOOST_TEST(processed.load() == count);
}

BOOST_AUTO_TEST_CASE(multi_thread_processes_all_items) {
   constexpr int count = 1000;
   std::atomic<int> processed{0};

   auto q = worker_task_queue<int>::create({.max_threads = 4}, [&](int&) { ++processed; });

   for (int i = 0; i < count; ++i)
      q->push(i);

   while (processed.load() < count)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->stop();
   BOOST_TEST(processed.load() == count);
}

BOOST_AUTO_TEST_CASE(stop_is_idempotent) {
   auto q = worker_task_queue<int>::create({.max_threads = 2}, [](int&) {});
   q->stop();
   q->stop(); // must not hang or crash
   BOOST_TEST(!q->running());
}

BOOST_AUTO_TEST_CASE(destroy_is_alias_for_stop) {
   std::atomic<int> processed{0};
   auto q = worker_task_queue<int>::create({.max_threads = 1}, [&](int&) { ++processed; });
   q->push(42);

   while (processed.load() < 1)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->destroy();
   BOOST_TEST(!q->running());
   BOOST_TEST(processed.load() == 1);
}

BOOST_AUTO_TEST_CASE(push_after_stop_is_noop) {
   std::atomic<int> processed{0};
   auto q = worker_task_queue<int>::create({.max_threads = 1}, [&](int&) { ++processed; });
   q->stop();
   q->push(1);
   q->push(2);
   BOOST_TEST(processed.load() == 0);
   BOOST_TEST(q->size() == 0u);
}

BOOST_AUTO_TEST_CASE(skip_autostart_requires_manual_start) {
   std::atomic<int> processed{0};
   auto q = worker_task_queue<int>::create({.max_threads = 1, .skip_autostart = true},
                                           [&](int&) { ++processed; });
   BOOST_TEST(!q->running());

   q->push(10);
   BOOST_TEST(q->size() == 1u);

   q->start();
   BOOST_TEST(q->running());

   while (processed.load() < 1)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->stop();
   BOOST_TEST(processed.load() == 1);
}

BOOST_AUTO_TEST_CASE(callback_receives_correct_values) {
   std::atomic<int> sum{0};
   auto q = worker_task_queue<int>::create({.max_threads = 1}, [&](int& v) { sum += v; });

   q->push(10);
   q->push(20);
   q->push(30);

   while (sum.load() < 60)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->stop();
   BOOST_TEST(sum.load() == 60);
}

BOOST_AUTO_TEST_CASE(move_only_type) {
   struct move_only {
      int value;
      move_only(int v) : value(v) {}
      move_only(move_only&&) = default;
      move_only& operator=(move_only&&) = default;
      move_only(const move_only&)       = delete;
      move_only& operator=(const move_only&) = delete;
   };

   std::atomic<int> sum{0};
   auto q = worker_task_queue<move_only>::create({.max_threads = 2}, [&](move_only& m) { sum += m.value; });

   q->push(move_only{5});
   q->push(move_only{15});

   while (sum.load() < 20)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->stop();
   BOOST_TEST(sum.load() == 20);
}

BOOST_AUTO_TEST_CASE(concurrent_producers) {
   constexpr int per_producer = 500;
   constexpr int num_producers = 4;
   std::atomic<int> processed{0};

   auto q = worker_task_queue<int>::create({.max_threads = 2}, [&](int&) { ++processed; });

   std::vector<std::thread> producers;
   producers.reserve(num_producers);
   for (int p = 0; p < num_producers; ++p) {
      producers.emplace_back([&q]() {
         for (int i = 0; i < per_producer; ++i)
            q->push(i);
      });
   }
   for (auto& t : producers)
      t.join();

   while (processed.load() < per_producer * num_producers)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

   q->stop();
   BOOST_TEST(processed.load() == per_producer * num_producers);
}

BOOST_AUTO_TEST_SUITE_END()
