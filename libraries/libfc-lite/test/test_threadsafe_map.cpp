#include <boost/test/unit_test.hpp>

#include <fc-lite/threadsafe_map.hpp>

#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace fc;

BOOST_AUTO_TEST_SUITE(threadsafe_map_tests)

// ==================== Ordered Map Tests ====================

BOOST_AUTO_TEST_CASE(ordered_basic_write_and_read) {
   threadsafe_map<int, std::string> map;

   // Write some values
   {
      auto w = map.writeable();
      w[1] = "one";
      w[2] = "two";
      w[3] = "three";
   }

   // Read them back
   {
      auto r = map.readable();
      BOOST_CHECK_EQUAL(r.size(), 3u);
      BOOST_CHECK_EQUAL(r[1], "one");
      BOOST_CHECK_EQUAL(r[2], "two");
      BOOST_CHECK_EQUAL(r[3], "three");
   }
};

BOOST_AUTO_TEST_CASE(ordered_read_view_contains) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[10] = "ten";
   }

   auto r = map.readable();
   BOOST_CHECK(r.contains(10));
   BOOST_CHECK(!r.contains(20));
};

BOOST_AUTO_TEST_CASE(ordered_read_view_at_throws) {
   threadsafe_map<int, std::string> map;

   auto r = map.readable();
   BOOST_CHECK_THROW(r.at(999), std::out_of_range);
};

BOOST_AUTO_TEST_CASE(ordered_read_view_get_and_get_copy) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[1] = "value";
   }

   auto r = map.readable();
   BOOST_CHECK_EQUAL(r.get(1), "value");
   BOOST_CHECK_EQUAL(r.get_copy(1), "value");
};

BOOST_AUTO_TEST_CASE(ordered_read_view_empty_and_size) {
   threadsafe_map<int, std::string> map;

   {
      auto r = map.readable();
      BOOST_CHECK(r.empty());
      BOOST_CHECK_EQUAL(r.size(), 0u);
   }

   {
      auto w = map.writeable();
      w[1] = "one";
      w[2] = "two";
   }

   {
      auto r = map.readable();
      BOOST_CHECK(!r.empty());
      BOOST_CHECK_EQUAL(r.size(), 2u);
   }
};

BOOST_AUTO_TEST_CASE(ordered_read_view_iteration) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[1] = "a";
      w[2] = "b";
      w[3] = "c";
   }

   auto r = map.readable();
   std::vector<int> keys;
   for (auto it = r.begin(); it != r.end(); ++it) {
      keys.push_back(it->first);
   }
   // std::map maintains sorted order
   BOOST_CHECK_EQUAL(keys.size(), 3u);
   BOOST_CHECK_EQUAL(keys[0], 1);
   BOOST_CHECK_EQUAL(keys[1], 2);
   BOOST_CHECK_EQUAL(keys[2], 3);
};

BOOST_AUTO_TEST_CASE(ordered_read_view_map_access) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[5] = "five";
   }

   auto r = map.readable();
   const auto& underlying = r.map();
   BOOST_CHECK_EQUAL(underlying.size(), 1u);
   BOOST_CHECK(underlying.contains(5));
};

BOOST_AUTO_TEST_CASE(ordered_write_view_try_emplace) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      auto [it1, inserted1] = w.try_emplace(1, "one");
      BOOST_CHECK(inserted1);
      BOOST_CHECK_EQUAL(it1->second, "one");

      // Try to emplace again - should not insert
      auto [it2, inserted2] = w.try_emplace(1, "ONE");
      BOOST_CHECK(!inserted2);
      BOOST_CHECK_EQUAL(it2->second, "one"); // Original value retained
   }
};

BOOST_AUTO_TEST_CASE(ordered_write_view_erase) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[1] = "one";
      w[2] = "two";
   }

   {
      auto w = map.writeable();
      BOOST_CHECK(w.erase(1));
      BOOST_CHECK(!w.erase(999)); // Non-existent key
      BOOST_CHECK(!w.contains(1));
      BOOST_CHECK(w.contains(2));
   }
};

BOOST_AUTO_TEST_CASE(ordered_write_view_clear) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[1] = "one";
      w[2] = "two";
      w[3] = "three";
   }

   {
      auto w = map.writeable();
      w.clear();
   }

   auto r = map.readable();
   BOOST_CHECK(r.empty());
};

BOOST_AUTO_TEST_CASE(ordered_write_view_with_lock) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w.with_lock([](auto& m) {
         m[1] = "one";
         m[2] = "two";
      });
   }

   auto r = map.readable();
   BOOST_CHECK_EQUAL(r.size(), 2u);
   BOOST_CHECK_EQUAL(r[1], "one");
   BOOST_CHECK_EQUAL(r[2], "two");
};

BOOST_AUTO_TEST_CASE(ordered_write_view_map_access) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      auto& underlying = w.map();
      underlying[100] = "hundred";
   }

   auto r = map.readable();
   BOOST_CHECK_EQUAL(r[100], "hundred");
};

BOOST_AUTO_TEST_CASE(ordered_write_view_contains_at_get) {
   threadsafe_map<int, std::string> map;

   {
      auto w = map.writeable();
      w[1] = "one";

      BOOST_CHECK(w.contains(1));
      BOOST_CHECK(!w.contains(2));
      BOOST_CHECK_EQUAL(w.at(1), "one");
      BOOST_CHECK_EQUAL(w.get(1), "one");
      BOOST_CHECK_EQUAL(w.get_copy(1), "one");
      BOOST_CHECK_THROW(w.at(999), std::out_of_range);
   }
};

// ==================== Unordered Map Tests ====================

BOOST_AUTO_TEST_CASE(unordered_basic_write_and_read) {
   threadsafe_map<int, std::string, std::unordered_map<int, std::string>> map;

   {
      auto w = map.writeable();
      w[1] = "one";
      w[2] = "two";
      w[3] = "three";
   }

   {
      auto r = map.readable();
      BOOST_CHECK_EQUAL(r.size(), 3u);
      BOOST_CHECK_EQUAL(r[1], "one");
      BOOST_CHECK_EQUAL(r[2], "two");
      BOOST_CHECK_EQUAL(r[3], "three");
   }
};

BOOST_AUTO_TEST_CASE(unordered_read_view_contains) {
   threadsafe_map<std::string, int, std::unordered_map<std::string, int>> map;

   {
      auto w = map.writeable();
      w["key"] = 42;
   }

   auto r = map.readable();
   BOOST_CHECK(r.contains("key"));
   BOOST_CHECK(!r.contains("nonexistent"));
};

BOOST_AUTO_TEST_CASE(unordered_read_view_at_throws) {
   threadsafe_map<std::string, int, std::unordered_map<std::string, int>> map;

   auto r = map.readable();
   BOOST_CHECK_THROW(r.at("missing"), std::out_of_range);
};

BOOST_AUTO_TEST_CASE(unordered_read_view_empty_and_size) {
   threadsafe_map<std::string, int, std::unordered_map<std::string, int>> map;

   {
      auto r = map.readable();
      BOOST_CHECK(r.empty());
      BOOST_CHECK_EQUAL(r.size(), 0u);
   }

   {
      auto w = map.writeable();
      w["a"] = 1;
      w["b"] = 2;
      w["c"] = 3;
   }

   {
      auto r = map.readable();
      BOOST_CHECK(!r.empty());
      BOOST_CHECK_EQUAL(r.size(), 3u);
   }
};

BOOST_AUTO_TEST_CASE(unordered_write_view_try_emplace) {
   threadsafe_map<std::string, int, std::unordered_map<std::string, int>> map;

   {
      auto w = map.writeable();
      auto [it1, inserted1] = w.try_emplace("key", 100);
      BOOST_CHECK(inserted1);
      BOOST_CHECK_EQUAL(it1->second, 100);

      auto [it2, inserted2] = w.try_emplace("key", 200);
      BOOST_CHECK(!inserted2);
      BOOST_CHECK_EQUAL(it2->second, 100);
   }
};

BOOST_AUTO_TEST_CASE(unordered_write_view_erase_and_clear) {
   threadsafe_map<std::string, int, std::unordered_map<std::string, int>> map;

   {
      auto w = map.writeable();
      w["a"] = 1;
      w["b"] = 2;
      w["c"] = 3;
   }

   {
      auto w = map.writeable();
      BOOST_CHECK(w.erase("b"));
      BOOST_CHECK(!w.erase("nonexistent"));
   }

   {
      auto r = map.readable();
      BOOST_CHECK_EQUAL(r.size(), 2u);
      BOOST_CHECK(!r.contains("b"));
   }

   {
      auto w = map.writeable();
      w.clear();
   }

   {
      auto r = map.readable();
      BOOST_CHECK(r.empty());
   }
};

BOOST_AUTO_TEST_CASE(unordered_write_view_with_lock) {
   threadsafe_map<int, int, std::unordered_map<int, int>> map;

   {
      auto w = map.writeable();
      int sum = w.with_lock([](auto& m) {
         m[1] = 10;
         m[2] = 20;
         m[3] = 30;
         return m[1] + m[2] + m[3];
      });
      BOOST_CHECK_EQUAL(sum, 60);
   }

   auto r = map.readable();
   BOOST_CHECK_EQUAL(r.size(), 3u);
};

// ==================== Concurrency Tests ====================

BOOST_AUTO_TEST_CASE(concurrent_readers) {
   threadsafe_map<int, int> map;

   // Populate the map
   {
      auto w = map.writeable();
      for (int i = 0; i < 100; ++i) {
         w[i] = i * 10;
      }
   }

   // Multiple concurrent readers
   std::vector<std::thread> readers;
   std::atomic<int> success_count{0};

   for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&map, &success_count]() {
         for (int i = 0; i < 100; ++i) {
            auto r = map.readable();
            if (r.contains(i) && r[i] == i * 10) {
               ++success_count;
            }
         }
      });
   }

   for (auto& t : readers) {
      t.join();
   }

   BOOST_CHECK_EQUAL(success_count.load(), 400);
};

BOOST_AUTO_TEST_CASE(concurrent_writer_and_readers) {
   threadsafe_map<int, int> map;

   std::atomic<bool> done{false};
   std::atomic<int> read_count{0};

   // Writer thread
   std::thread writer([&map, &done]() {
      for (int i = 0; i < 100; ++i) {
         auto w = map.writeable();
         w[i] = i;
      }
      done = true;
   });

   // Reader threads
   std::vector<std::thread> readers;
   for (int t = 0; t < 2; ++t) {
      readers.emplace_back([&map, &done, &read_count]() {
         while (!done.load()) {
            auto r = map.readable();
            read_count += static_cast<int>(r.size());
         }
      });
   }

   writer.join();
   for (auto& t : readers) {
      t.join();
   }

   // Verify final state
   auto r = map.readable();
   BOOST_CHECK_EQUAL(r.size(), 100u);
   for (int i = 0; i < 100; ++i) {
      BOOST_CHECK_EQUAL(r[i], i);
   }
};

BOOST_AUTO_TEST_CASE(view_move_semantics) {
   threadsafe_map<int, std::string> map;

   // Test read_view move
   {
      auto r1 = map.readable();
      auto r2 = std::move(r1);
      BOOST_CHECK(r2.empty());
   }

   // Test write_view move
   {
      auto w1 = map.writeable();
      w1[1] = "one";
      auto w2 = std::move(w1);
      BOOST_CHECK(w2.contains(1));
   }

   auto r = map.readable();
   BOOST_CHECK_EQUAL(r[1], "one");
};

BOOST_AUTO_TEST_SUITE_END()