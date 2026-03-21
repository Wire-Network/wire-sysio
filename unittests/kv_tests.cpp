#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/kv_context.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(kv_tests)

// Direct chainbase tests for KV objects (no WASM needed)

BOOST_AUTO_TEST_CASE(kv_object_crud) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // CREATE
   const auto& obj = db.create<kv_object>([](auto& o) {
      o.code = "test"_n;
      o.key_assign("hello", 5);
      o.value.assign("world", 5);
   });

   BOOST_CHECK_EQUAL(obj.code, "test"_n);
   BOOST_CHECK_EQUAL(obj.key_size, 5);
   BOOST_CHECK_EQUAL(obj.key_view(), std::string_view("hello", 5));
   BOOST_CHECK_EQUAL(std::string_view(obj.value.data(), obj.value.size()), "world");

   // READ via index
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("test"), std::string_view("hello", 5)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->value.data(), itr->value.size()), "world");

   // UPDATE
   db.modify(*itr, [](auto& o) {
      o.value.assign("updated", 7);
   });
   BOOST_CHECK_EQUAL(std::string_view(itr->value.data(), itr->value.size()), "updated");

   // DELETE
   db.remove(*itr);
   auto itr2 = idx.find(boost::make_tuple(name("test"), std::string_view("hello", 5)));
   BOOST_CHECK(itr2 == idx.end());

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_object_ordering) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Insert keys out of order
   auto make_key = [](uint64_t v) {
      char buf[8];
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
      return std::string(buf, 8);
   };

   std::vector<uint64_t> values = {50, 10, 90, 30, 70, 1, 100};
   for (auto v : values) {
      auto k = make_key(v);
      db.create<kv_object>([&](auto& o) {
         o.code = "order"_n;
         o.key_assign(k.data(), k.size());
         o.value.assign("x", 1);
      });
   }

   // Verify iteration is in sorted order
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.lower_bound(boost::make_tuple(name("order")));

   std::sort(values.begin(), values.end());
   size_t i = 0;
   while (itr != idx.end() && itr->code == name("order")) {
      auto expected = make_key(values[i]);
      BOOST_CHECK_EQUAL(itr->key_view(), std::string_view(expected.data(), 8));
      ++itr;
      ++i;
   }
   BOOST_CHECK_EQUAL(i, values.size());

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_key_size_limits) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // SSO path (8 bytes)
   {
      db.create<kv_object>([](auto& o) {
         o.code = "limits"_n;
         o.key_assign("12345678", 8);
         o.value.assign("v", 1);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), std::string_view("12345678", 8)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_size, 8);
   }

   // SSO path (24 bytes, max inline)
   {
      std::string key24(24, 'A');
      db.create<kv_object>([&](auto& o) {
         o.code = "limits"_n;
         o.key_assign(key24.data(), key24.size());
         o.value.assign("v", 1);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), std::string_view(key24)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_size, 24);
   }

   // Heap path (32 bytes, exceeds SSO capacity)
   {
      std::string key32(32, 'B');
      db.create<kv_object>([&](auto& o) {
         o.code = "limits"_n;
         o.key_assign(key32.data(), key32.size());
         o.value.assign("v", 1);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), std::string_view(key32)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_size, 32);
   }

   // Empty key
   {
      db.create<kv_object>([](auto& o) {
         o.code = "limits"_n;
         o.key_assign("", 0);
         o.value.assign("empty", 5);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), std::string_view("", 0)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_size, 0);
   }

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_index_object_crud) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Create secondary index entry
   db.create<kv_index_object>([](auto& o) {
      o.code = "test"_n;
      o.table = "users"_n;
      o.index_id = 0;
      o.sec_key_assign("alice", 5);
      o.pri_key_assign("\x00\x01", 2);
   });

   db.create<kv_index_object>([](auto& o) {
      o.code = "test"_n;
      o.table = "users"_n;
      o.index_id = 0;
      o.sec_key_assign("bob", 3);
      o.pri_key_assign("\x00\x02", 2);
   });

   // Find by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(
      name("test"), name("users"), uint8_t(0), std::string_view("alice", 5)));
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key_data(), itr->sec_key_size), "alice");

   // Verify ordering: alice < bob
   ++itr;
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key_data(), itr->sec_key_size), "bob");

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_basic) {
   kv_iterator_pool pool;

   // Allocate primary
   uint32_t h1 = pool.allocate_primary("test"_n, "prefix", 6);
   BOOST_CHECK_EQUAL(h1, 0u);
   auto& slot1 = pool.get(h1);
   BOOST_CHECK(slot1.is_primary);
   BOOST_CHECK_EQUAL(slot1.code, "test"_n);

   // Allocate secondary
   uint32_t h2 = pool.allocate_secondary("test"_n, "table"_n, 1);
   BOOST_CHECK_EQUAL(h2, 1u);
   auto& slot2 = pool.get(h2);
   BOOST_CHECK(!slot2.is_primary);

   // Release and reuse
   pool.release(h1);
   uint32_t h3 = pool.allocate_primary("other"_n, "", 0);
   BOOST_CHECK_EQUAL(h3, 0u); // reuses slot 0

   pool.release(h2);
   pool.release(h3);
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_exhaustion) {
   kv_iterator_pool pool;

   // Allocate all 16 slots
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      pool.allocate_primary("test"_n, "", 0);
   }

   // 17th should throw
   BOOST_CHECK_THROW(
      pool.allocate_primary("test"_n, "", 0),
      kv_iterator_limit_exceeded
   );

   // Release one and try again
   pool.release(5);
   uint32_t h = pool.allocate_primary("test"_n, "", 0);
   BOOST_CHECK_EQUAL(h, 5u);
}

BOOST_AUTO_TEST_SUITE_END()
