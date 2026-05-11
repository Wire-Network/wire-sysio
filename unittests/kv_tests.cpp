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

// Verify that db.modify() correctly rebalances AVL trees when a composite
// index key field (sec_key) changes, as an alternative to remove+create.
BOOST_AUTO_TEST_CASE(kv_index_modify_rekeys_correctly) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   const uint16_t users_idx = compute_table_id("users.byname");

   // Create three entries: alice, bob, charlie
   const auto& alice = db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx;
      o.sec_key.assign("alice", 5);
      o.pri_key.assign("\x00\x01", 2);
   });
   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx;
      o.sec_key.assign("bob", 3);
      o.pri_key.assign("\x00\x02", 2);
   });
   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx;
      o.sec_key.assign("charlie", 7);
      o.pri_key.assign("\x00\x03", 2);
   });

   // Modify alice's sec_key to "zebra" -- should move to end of ordering
   db.modify(alice, [](auto& o) {
      o.sec_key.assign("zebra", 5);
   });

   // Verify ordering in by_code_table_id_seckey: bob < charlie < zebra
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(name("test"), users_idx));
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(itr->sec_key_view(), "bob");
   ++itr;
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(itr->sec_key_view(), "charlie");
   ++itr;
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(itr->sec_key_view(), "zebra");
   // Verify pri_key is preserved
   BOOST_CHECK_EQUAL(itr->pri_key_view(), std::string_view("\x00\x01", 2));

   // Lookup by full composite key (sec_key + pri_key) finds the modified object.
   auto pitr = sec_idx.find(boost::make_tuple(
      name("test"), users_idx,
      std::string_view("zebra", 5),
      std::string_view("\x00\x01", 2)));
   BOOST_REQUIRE(pitr != sec_idx.end());
   BOOST_CHECK_EQUAL(pitr->sec_key_view(), "zebra");

   // Old sec_key no longer resolves.
   auto old_itr = sec_idx.find(boost::make_tuple(
      name("test"), users_idx,
      std::string_view("alice", 5),
      std::string_view("\x00\x01", 2)));
   BOOST_CHECK(old_itr == sec_idx.end());

   // Verify undo restores original ordering
   session.undo();

   auto& sec_idx2 = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr2 = sec_idx2.lower_bound(boost::make_tuple(name("test"), users_idx));
   // After undo, all 3 entries should be gone (session created them all)
   BOOST_CHECK(itr2 == sec_idx2.end() || itr2->code != name("test"));
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_basic) {
   kv_primary_iterator_pool prim_pool;
   kv_secondary_iterator_pool sec_pool;

   // Allocate primary -- returns a raw slot index (no tag).
   uint32_t h1 = prim_pool.allocate(uint16_t(0), "test"_n, "prefix", 6);
   BOOST_CHECK_EQUAL(h1, 0u);
   BOOST_CHECK(!kv_handle_is_secondary(h1));
   auto& slot1 = prim_pool.get(kv_handle_slot_index(h1));
   BOOST_CHECK_EQUAL(slot1.code, "test"_n);
   BOOST_CHECK_EQUAL(slot1.prefix.size(), 6u);

   // Allocate secondary -- slot index is wrapped with the tag bit.
   uint32_t s2 = sec_pool.allocate("test"_n, uint16_t(100));
   uint32_t h2 = kv_make_secondary_handle(s2);
   BOOST_CHECK(kv_handle_is_secondary(h2));
   BOOST_CHECK_EQUAL(kv_handle_slot_index(h2), s2);
   auto& slot2 = sec_pool.get(kv_handle_slot_index(h2));
   BOOST_CHECK_EQUAL(slot2.code, "test"_n);

   // Release and reuse from each pool independently.
   prim_pool.release(kv_handle_slot_index(h1));
   uint32_t h3 = prim_pool.allocate(uint16_t(0), "other"_n, "", 0);
   BOOST_CHECK_EQUAL(h3, 0u); // reuses slot 0

   sec_pool.release(kv_handle_slot_index(h2));
   prim_pool.release(kv_handle_slot_index(h3));
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_independent_exhaustion) {
   kv_primary_iterator_pool prim_pool;
   kv_secondary_iterator_pool sec_pool;

   // Exhausting one pool must not consume slots in the other.
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      prim_pool.allocate(uint16_t(0), "test"_n, "", 0);
   }
   BOOST_CHECK_THROW(
      prim_pool.allocate(uint16_t(0), "test"_n, "", 0),
      kv_iterator_limit_exceeded
   );

   // Secondary pool is still empty -- can allocate the full budget.
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      sec_pool.allocate("test"_n, uint16_t(0));
   }
   BOOST_CHECK_THROW(
      sec_pool.allocate("test"_n, uint16_t(0)),
      kv_iterator_limit_exceeded
   );

   // Release one slot in each pool and verify reuse.
   prim_pool.release(5);
   BOOST_CHECK_EQUAL(prim_pool.allocate(uint16_t(0), "test"_n, "", 0), 5u);
   sec_pool.release(7);
   BOOST_CHECK_EQUAL(sec_pool.allocate("test"_n, uint16_t(0)), 7u);
}

BOOST_AUTO_TEST_CASE(kv_iterator_handle_encoding) {
   // Round-trip: tag a slot index, recover it via the helpers.
   for (uint32_t slot : {uint32_t{0}, uint32_t{1}, uint32_t{17}, uint32_t{1023}}) {
      uint32_t handle = kv_make_secondary_handle(slot);
      BOOST_CHECK(kv_handle_is_secondary(handle));
      BOOST_CHECK_EQUAL(kv_handle_slot_index(handle), slot);
      // Reserved bits 10..15 and 17..31 must all be zero in a freshly-encoded handle.
      BOOST_CHECK_EQUAL(handle & ~(kv_handle_slot_index_mask | kv_secondary_handle_tag),
                        0u);
   }

   // Primary handles carry no tag.
   BOOST_CHECK(!kv_handle_is_secondary(0u));
   BOOST_CHECK(!kv_handle_is_secondary(1u));
   BOOST_CHECK(!kv_handle_is_secondary(1023u));

   // Reserved-bit guard: a fabricated handle with any reserved bit set throws.
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(0u));
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(1023u));
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(kv_make_secondary_handle(5u)));
   BOOST_CHECK_THROW(kv_handle_check_reserved_zero(0x00000400u), kv_invalid_iterator); // bit 10
   BOOST_CHECK_THROW(kv_handle_check_reserved_zero(0x00020000u), kv_invalid_iterator); // bit 17
   BOOST_CHECK_THROW(kv_handle_check_reserved_zero(0x40000000u), kv_invalid_iterator); // bit 30
}

// kv_idx_update uses db.modify, which preserves the chainbase id but can move
// the object's sort position. Verify that invalidate_cache clears cached_id
// only on the matching secondary slot and leaves stored key bytes and status
// intact for the slow re-seek path.
BOOST_AUTO_TEST_CASE(kv_secondary_iterator_pool_invalidate_cache) {
   kv_secondary_iterator_pool pool;

   const uint16_t users_idx       = compute_table_id("users.byname");
   const uint16_t users_idx_other = compute_table_id("users.byage");
   const uint16_t things_idx      = compute_table_id("things.byname");

   uint32_t h_sec          = pool.allocate("test"_n, users_idx);
   uint32_t h_other_idx_a  = pool.allocate("test"_n, things_idx);
   uint32_t h_other_idx_b  = pool.allocate("test"_n, users_idx_other);
   uint32_t h_other_code   = pool.allocate("alt"_n,  users_idx);
   uint32_t h_other_id     = pool.allocate("test"_n, users_idx);

   const int64_t target_id = 42;
   const int64_t other_id  = 99;

   auto seed = [](kv_secondary_slot& s, int64_t id) {
      s.status = kv_it_stat::iterator_ok;
      s.current_sec_key.assign({'a','l','i','c','e'});
      s.current_pri_key.assign({'\x00','\x01'});
      s.cached_id = id;
   };

   seed(pool.get(h_sec),          target_id);
   seed(pool.get(h_other_idx_a),  target_id);
   seed(pool.get(h_other_idx_b),  target_id);
   seed(pool.get(h_other_code),   target_id);
   seed(pool.get(h_other_id),     other_id);

   pool.invalidate_cache("test"_n, users_idx, target_id);

   // Matching slot: cached_id cleared, key bytes and status preserved.
   const auto& matched = pool.get(h_sec);
   BOOST_CHECK_EQUAL(matched.cached_id, -1);
   BOOST_CHECK(matched.status == kv_it_stat::iterator_ok);
   BOOST_CHECK_EQUAL(matched.current_sec_key.size(), 5u);
   BOOST_CHECK_EQUAL(matched.current_pri_key.size(), 2u);

   // Slots that differ in any of code/table_id/id are untouched.
   BOOST_CHECK_EQUAL(pool.get(h_other_idx_a).cached_id, target_id);
   BOOST_CHECK_EQUAL(pool.get(h_other_idx_b).cached_id, target_id);
   BOOST_CHECK_EQUAL(pool.get(h_other_code).cached_id,  target_id);
   BOOST_CHECK_EQUAL(pool.get(h_other_id).cached_id,    other_id);
}

BOOST_AUTO_TEST_SUITE_END()
