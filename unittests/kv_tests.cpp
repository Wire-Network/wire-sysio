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

   const uint16_t users_tid    = compute_table_id("users");
   const uint16_t users_idx0   = compute_table_id("users.byname");

   // kv_index_object references a primary row by chainbase id, so create the
   // primary kv_objects first and thread their ids into the secondary rows.
   const auto& alice_row = db.create<kv_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_tid;
      o.key.assign("\x00\x01", 2);
      o.value.assign("alice_val", 9);
   });
   const auto& bob_row = db.create<kv_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_tid;
      o.key.assign("\x00\x02", 2);
      o.value.assign("bob_val", 7);
   });

   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx0;
      o.sec_key.assign("alice", 5);
      o.primary_id = alice_row.id;
   });

   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx0;
      o.sec_key.assign("bob", 3);
      o.primary_id = bob_row.id;
   });

   // Find by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(
      name("test"), users_idx0, std::string_view("alice", 5)));
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key.data(), itr->sec_key.size()), "alice");
   BOOST_CHECK(itr->primary_id == alice_row.id);

   // Verify ordering: alice < bob
   ++itr;
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key.data(), itr->sec_key.size()), "bob");
   BOOST_CHECK(itr->primary_id == bob_row.id);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_basic) {
   // Primary and secondary iterators now live in independent pools.  Both pools
   // start at slot index 0 and do not share the free-list; a handle namespace
   // on top (see kv_make_secondary_handle) distinguishes them externally.
   kv_primary_iterator_pool primary;
   kv_secondary_iterator_pool secondary;

   // Allocate primary — returns raw slot index (no tag bit).
   uint32_t h1 = primary.allocate(uint16_t(0), "test"_n, "prefix", 6);
   BOOST_CHECK_EQUAL(h1, 0u);
   BOOST_CHECK(!kv_handle_is_secondary(h1));
   auto& slot1 = primary.get(h1);
   BOOST_CHECK_EQUAL(slot1.code, "test"_n);

   // Allocate secondary — pool returns a raw index; apply_context tags it
   // with the secondary handle bit before handing back to the contract.
   uint32_t s_idx = secondary.allocate("test"_n, uint16_t(100));
   BOOST_CHECK_EQUAL(s_idx, 0u);
   uint32_t h2 = kv_make_secondary_handle(s_idx);
   BOOST_CHECK(kv_handle_is_secondary(h2));
   BOOST_CHECK_EQUAL(kv_handle_slot_index(h2), 0u);
   auto& slot2 = secondary.get(s_idx);
   BOOST_CHECK_EQUAL(slot2.code, "test"_n);

   // Release and reuse within each pool independently.
   primary.release(h1);
   uint32_t h3 = primary.allocate(uint16_t(0), "other"_n, "", 0);
   BOOST_CHECK_EQUAL(h3, 0u); // reuses slot 0 of the primary pool

   secondary.release(s_idx);
   primary.release(h3);
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_exhaustion) {
   kv_primary_iterator_pool primary;
   kv_secondary_iterator_pool secondary;

   // Fill both pools — each independently sized to max_kv_iterators.
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      primary.allocate(uint16_t(0), "test"_n, "", 0);
      secondary.allocate("test"_n, uint16_t(100));
   }

   // One more primary allocation throws — tests that the primary pool
   // caps independently of the secondary pool.
   BOOST_CHECK_THROW(
      primary.allocate(uint16_t(0), "test"_n, "", 0),
      kv_iterator_limit_exceeded
   );

   // Same for secondary.
   BOOST_CHECK_THROW(
      secondary.allocate("test"_n, uint16_t(100)),
      kv_iterator_limit_exceeded
   );

   // Releasing a primary slot frees a primary handle only; secondary is
   // still saturated.
   primary.release(5);
   uint32_t h = primary.allocate(uint16_t(0), "test"_n, "", 0);
   BOOST_CHECK_EQUAL(h, 5u);
   BOOST_CHECK_THROW(
      secondary.allocate("test"_n, uint16_t(100)),
      kv_iterator_limit_exceeded
   );
}

BOOST_AUTO_TEST_CASE(kv_handle_encoding_layout) {
   // Pin the consensus-observable handle encoding.  Any change to this
   // layout is a protocol change and should fail this test loudly.
   BOOST_CHECK_EQUAL(kv_handle_slot_mask,     0x000003FFu);
   BOOST_CHECK_EQUAL(kv_secondary_handle_tag, 0x00010000u);

   // Reserved mask is every bit outside of slot+tag (within 32 bits).
   BOOST_CHECK_EQUAL(kv_handle_reserved_mask,
                     static_cast<uint32_t>(~(kv_handle_slot_mask | kv_secondary_handle_tag)));

   // Well-formed primary handle: only slot bits may be set.
   uint32_t primary_handle = 5u;
   BOOST_CHECK((primary_handle & kv_handle_reserved_mask) == 0);
   BOOST_CHECK(!kv_handle_is_secondary(primary_handle));
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(primary_handle));

   // Well-formed secondary handle: slot bits + tag bit.
   uint32_t secondary_handle = kv_make_secondary_handle(5u);
   BOOST_CHECK_EQUAL(secondary_handle, 0x00010005u);
   BOOST_CHECK((secondary_handle & kv_handle_reserved_mask) == 0);
   BOOST_CHECK(kv_handle_is_secondary(secondary_handle));
   BOOST_CHECK_EQUAL(kv_handle_slot_index(secondary_handle), 5u);
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(secondary_handle));

   // Any bit inside the reserved mask must be rejected.  Walk each reserved
   // bit individually to catch a future layout change that accidentally
   // shrinks the reserved set.
   for (uint32_t bit = 0; bit < 32; ++bit) {
      uint32_t probe = 1u << bit;
      if ((probe & kv_handle_reserved_mask) == 0) continue;
      BOOST_CHECK_THROW(kv_handle_check_reserved_zero(probe), kv_invalid_iterator);
   }

   // Handle values in int32_t form must remain non-negative; bit 31 is
   // reserved (and catches the "negative means not found" intrinsic
   // contract at the encoding layer).
   BOOST_CHECK((kv_handle_reserved_mask & 0x80000000u) != 0);
}

BOOST_AUTO_TEST_SUITE_END()
