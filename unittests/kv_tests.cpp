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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // CREATE
   const auto& obj = db.create<kv_object>([](auto& o) {
      o.code = "test"_n;
      o.key.assign("hello", 5);
      o.value.assign("world", 5);
   });

   BOOST_CHECK_EQUAL(obj.code, "test"_n);
   BOOST_CHECK_EQUAL(obj.key.size(), 5u);
   BOOST_CHECK_EQUAL(obj.key_view(), std::string_view("hello", 5));
   BOOST_CHECK_EQUAL(std::string_view(obj.value.data(), obj.value.size()), "world");

   // READ via index
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("test"), uint16_t(0), std::string_view("hello", 5)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->value.data(), itr->value.size()), "world");

   // UPDATE
   db.modify(*itr, [](auto& o) {
      o.value.assign("updated", 7);
   });
   BOOST_CHECK_EQUAL(std::string_view(itr->value.data(), itr->value.size()), "updated");

   // DELETE
   db.remove(*itr);
   auto itr2 = idx.find(boost::make_tuple(name("test"), uint16_t(0), std::string_view("hello", 5)));
   BOOST_CHECK(itr2 == idx.end());

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_object_ordering) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
         o.key.assign(k.data(), k.size());
         o.value.assign("x", 1);
      });
   }

   // Verify iteration is in sorted order
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.lower_bound(boost::make_tuple(name("order"), uint16_t(0)));

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

BOOST_AUTO_TEST_CASE(kv_table_id_isolation) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Insert two rows with same (code, key) but different table_ids
   db.create<kv_object>([](auto& o) {
      o.code = "iso"_n;
      o.table_id = 100;
      o.key.assign("samekey", 7);
      o.value.assign("val_a", 5);
   });
   db.create<kv_object>([](auto& o) {
      o.code = "iso"_n;
      o.table_id = 200;
      o.key.assign("samekey", 7);
      o.value.assign("val_b", 5);
   });

   // Both should coexist — lookup each independently
   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr_a = idx.find(boost::make_tuple(name("iso"), uint16_t(100), std::string_view("samekey", 7)));
   BOOST_REQUIRE(itr_a != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr_a->value.data(), itr_a->value.size()), "val_a");
   BOOST_CHECK_EQUAL(itr_a->table_id, 100);

   auto itr_b = idx.find(boost::make_tuple(name("iso"), uint16_t(200), std::string_view("samekey", 7)));
   BOOST_REQUIRE(itr_b != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr_b->value.data(), itr_b->value.size()), "val_b");
   BOOST_CHECK_EQUAL(itr_b->table_id, 200);

   // Iteration within table_id=100 should not see table_id=200's row
   auto itr_100 = idx.lower_bound(boost::make_tuple(name("iso"), uint16_t(100)));
   int count_100 = 0;
   while (itr_100 != idx.end() && itr_100->code == name("iso") && itr_100->table_id == 100) {
      ++count_100;
      ++itr_100;
   }
   BOOST_CHECK_EQUAL(count_100, 1);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_key_size_limits) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Small key (8 bytes)
   {
      db.create<kv_object>([](auto& o) {
         o.code = "limits"_n;
         o.key.assign("12345678", 8);
         o.value.assign("v", 1);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view("12345678", 8)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key.size(), 8u);
   }

   // Standard key (24 bytes = kv_key_size)
   {
      std::string key24(24, 'A');
      db.create<kv_object>([&](auto& o) {
         o.code = "limits"_n;
         o.key.assign(key24.data(), key24.size());
         o.value.assign("v", 1);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view(key24)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key.size(), chain::kv_key_size);
   }

   // Large key (32 bytes)
   {
      std::string key32(32, 'B');
      db.create<kv_object>([&](auto& o) {
         o.code = "limits"_n;
         o.key.assign(key32.data(), key32.size());
         o.value.assign("v", 1);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view(key32)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key.size(), 32u);
   }

   // Empty key
   {
      db.create<kv_object>([](auto& o) {
         o.code = "limits"_n;
         o.key.assign("", 0);
         o.value.assign("empty", 5);
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view("", 0)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key.size(), 0u);
   }

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_index_object_crud) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Create secondary index entries with table_id
   const uint16_t users_idx0 = compute_table_id("users.byname");
   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx0;
      o.sec_key.assign("alice", 5);
      o.pri_key.assign("\x00\x01", 2);
   });

   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx0;
      o.sec_key.assign("bob", 3);
      o.pri_key.assign("\x00\x02", 2);
   });

   // Find by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(
      name("test"), users_idx0, std::string_view("alice", 5)));
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key.data(), itr->sec_key.size()), "alice");

   // Verify ordering: alice < bob
   ++itr;
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key.data(), itr->sec_key.size()), "bob");

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_basic) {
   kv_iterator_pool pool;

   // Allocate primary
   uint32_t h1 = pool.allocate_primary(uint16_t(0), "test"_n, "prefix", 6);
   BOOST_CHECK_EQUAL(h1, 0u);
   auto& slot1 = pool.get(h1);
   BOOST_CHECK(slot1.is_primary);
   BOOST_CHECK_EQUAL(slot1.code, "test"_n);

   // Allocate secondary
   uint32_t h2 = pool.allocate_secondary("test"_n, uint16_t(100));
   BOOST_CHECK_EQUAL(h2, 1u);
   auto& slot2 = pool.get(h2);
   BOOST_CHECK(!slot2.is_primary);

   // Release and reuse
   pool.release(h1);
   uint32_t h3 = pool.allocate_primary(uint16_t(0), "other"_n, "", 0);
   BOOST_CHECK_EQUAL(h3, 0u); // reuses slot 0

   pool.release(h2);
   pool.release(h3);
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_exhaustion) {
   kv_iterator_pool pool;

   // Allocate all 16 slots
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      pool.allocate_primary(uint16_t(0), "test"_n, "", 0);
   }

   // 17th should throw
   BOOST_CHECK_THROW(
      pool.allocate_primary(uint16_t(0), "test"_n, "", 0),
      kv_iterator_limit_exceeded
   );

   // Release one and try again
   pool.release(5);
   uint32_t h = pool.allocate_primary(uint16_t(0), "test"_n, "", 0);
   BOOST_CHECK_EQUAL(h, 5u);
}

BOOST_AUTO_TEST_SUITE_END()
