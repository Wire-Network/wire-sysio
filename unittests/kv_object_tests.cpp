#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/kv_context.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

// These tests write kv_object and kv_index_object straight into chainbase, with no contract and no intrinsic in the way.
// They pin the object types, their comparators and chainbase. They reach no host code: no limit, no status, no billing,
// no iterator. kv_intrinsic_probe_tests, kv_api_tests and kv_tests are where the host is under test.
BOOST_AUTO_TEST_SUITE(kv_chainbase_object_tests)

/// Longer than chainbase's small size allocator takes, which is 1024 bytes. Its pools never give memory back, so only a
/// blob this long shows a leak in get_free_memory.
constexpr size_t unpooled_key_size = 2048;

BOOST_AUTO_TEST_CASE(kv_object_crud) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // CREATE
   const auto& obj = db.create<kv_object>([](auto& o) {
      o.code = "test"_n;
      o.set_key("hello", 5);
      o.set_value({"world", 5});
   });

   BOOST_CHECK_EQUAL(obj.code, "test"_n);
   BOOST_CHECK_EQUAL(obj.key_view().size(), 5u);
   BOOST_CHECK_EQUAL(obj.key_view(), std::string_view("hello", 5));
   BOOST_CHECK_EQUAL(obj.value_view(), "world");

   // READ via index
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("test"), uint16_t(0), std::string_view("hello", 5)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(itr->value_view(), "world");

   // UPDATE
   db.modify(*itr, [](auto& o) {
      o.set_value({"updated", 7});
   });
   BOOST_CHECK_EQUAL(itr->value_view(), "updated");

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
         o.set_key(k.data(), k.size());
         o.set_value({"x", 1});
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

BOOST_AUTO_TEST_CASE(kv_object_index_separates_table_ids) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Insert two rows with same (code, key) but different table_ids
   db.create<kv_object>([](auto& o) {
      o.code = "iso"_n;
      o.table_id = 100;
      o.set_key("samekey", 7);
      o.set_value({"val_a", 5});
   });
   db.create<kv_object>([](auto& o) {
      o.code = "iso"_n;
      o.table_id = 200;
      o.set_key("samekey", 7);
      o.set_value({"val_b", 5});
   });

   // Both should coexist — lookup each independently
   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr_a = idx.find(boost::make_tuple(name("iso"), uint16_t(100), std::string_view("samekey", 7)));
   BOOST_REQUIRE(itr_a != idx.end());
   BOOST_CHECK_EQUAL(itr_a->value_view(), "val_a");
   BOOST_CHECK_EQUAL(itr_a->table_id, 100);

   auto itr_b = idx.find(boost::make_tuple(name("iso"), uint16_t(200), std::string_view("samekey", 7)));
   BOOST_REQUIRE(itr_b != idx.end());
   BOOST_CHECK_EQUAL(itr_b->value_view(), "val_b");
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

BOOST_AUTO_TEST_CASE(kv_object_holds_keys_of_several_sizes) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Small key (8 bytes)
   {
      db.create<kv_object>([](auto& o) {
         o.code = "limits"_n;
         o.set_key("12345678", 8);
         o.set_value({"v", 1});
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view("12345678", 8)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_view().size(), 8u);
   }

   // 24-byte key
   {
      std::string key24(24, 'A');
      db.create<kv_object>([&](auto& o) {
         o.code = "limits"_n;
         o.set_key(key24.data(), key24.size());
         o.set_value({"v", 1});
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view(key24)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_view().size(), key24.size());
   }

   // Large key (32 bytes)
   {
      std::string key32(32, 'B');
      db.create<kv_object>([&](auto& o) {
         o.code = "limits"_n;
         o.set_key(key32.data(), key32.size());
         o.set_value({"v", 1});
      });
      auto& idx = db.get_index<kv_index, by_code_key>();
      auto itr = idx.find(boost::make_tuple(name("limits"), uint16_t(0), std::string_view(key32)));
      BOOST_REQUIRE(itr != idx.end());
      BOOST_CHECK_EQUAL(itr->key_view().size(), 32u);
   }

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_index_object_crud) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Create secondary index entries with table_id
   const uint16_t users_idx0 = compute_string_table_id("users.byname");
   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx0;
      o.set_sec_key("alice", 5);
      o.set_pri_key("\x00\x01", 2);
   });

   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx0;
      o.set_sec_key("bob", 3);
      o.set_pri_key("\x00\x02", 2);
   });

   // Find by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(
      name("test"), users_idx0, std::string_view("alice", 5)));
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key_view().data(), itr->sec_key_view().size()), "alice");

   // Verify ordering: alice < bob
   ++itr;
   BOOST_REQUIRE(itr != sec_idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->sec_key_view().data(), itr->sec_key_view().size()), "bob");

   session.undo();
}

// Verify that db.modify() correctly rebalances AVL trees when a composite
// index key field (sec_key) changes, as an alternative to remove+create.
BOOST_AUTO_TEST_CASE(kv_index_modify_rekeys_correctly) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   const uint16_t users_idx = compute_string_table_id("users.byname");

   // Create three entries: alice, bob, charlie
   const auto& alice = db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx;
      o.set_sec_key("alice", 5);
      o.set_pri_key("\x00\x01", 2);
   });
   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx;
      o.set_sec_key("bob", 3);
      o.set_pri_key("\x00\x02", 2);
   });
   db.create<kv_index_object>([&](auto& o) {
      o.code = "test"_n;
      o.table_id = users_idx;
      o.set_sec_key("charlie", 7);
      o.set_pri_key("\x00\x03", 2);
   });

   // Modify alice's sec_key to "zebra" -- should move to end of ordering
   db.modify(alice, [](auto& o) {
      o.set_sec_key("zebra", 5);
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

BOOST_AUTO_TEST_CASE(kv_object_survives_a_pushed_session) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   // Write data in one session
   {
      auto session = db.start_undo_session(true);
      db.create<kv_object>([](auto& o) {
         o.code = "persist"_n;
         o.set_key("mykey", 5);
         o.set_value({"myvalue", 7});
      });
      session.push();
   }

   // Verify data is readable
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("persist"), uint16_t(0), std::string_view("mykey", 5)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(itr->value_view(), "myvalue");
}

BOOST_AUTO_TEST_CASE(kv_object_reverts_on_chainbase_undo) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto& idx = db.get_index<kv_index, by_code_key>();

   {
      auto session = db.start_undo_session(true);
      db.create<kv_object>([](auto& o) {
         o.code = "undotest"_n;
         o.set_key("tempkey", 7);
         o.set_value({"tempval", 7});
      });

      // Data exists within session
      auto itr = idx.find(boost::make_tuple(name("undotest"), uint16_t(0), std::string_view("tempkey", 7)));
      BOOST_REQUIRE(itr != idx.end());

      session.undo();
   }

   // Data reverted after undo
   auto itr = idx.find(boost::make_tuple(name("undotest"), uint16_t(0), std::string_view("tempkey", 7)));
   BOOST_CHECK(itr == idx.end());
}

BOOST_AUTO_TEST_CASE(kv_object_holds_an_empty_value) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   db.create<kv_object>([](auto& o) {
      o.code = "edge"_n;
      o.set_key("haskey", 6);
      o.set_value({"", 0});
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr = idx.find(boost::make_tuple(name("edge"), uint16_t(0), std::string_view("haskey", 6)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(itr->value_view().size(), 0u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_object_holds_a_256_byte_key) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // 24-byte key
   std::string std_key(24, 'A');
   db.create<kv_object>([&](auto& o) {
      o.code = "maxkey"_n;
      o.set_key(std_key.data(), std_key.size());
      o.set_value({"v", 1});
   });

   // Large key (256 bytes = default max, on-chain config can raise up to 512)
   std::string max_key(256, 'B');
   db.create<kv_object>([&](auto& o) {
      o.code = "maxkey"_n;
      o.set_key(max_key.data(), max_key.size());
      o.set_value({"v", 1});
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr1 = idx.find(boost::make_tuple(name("maxkey"), uint16_t(0), std::string_view(std_key)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->key_view().size(), std_key.size());

   auto itr2 = idx.find(boost::make_tuple(name("maxkey"), uint16_t(0), std::string_view(max_key)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->key_view().size(), 256u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_object_index_separates_codes) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Two contracts store same key
   db.create<kv_object>([](auto& o) {
      o.code = "contract.a"_n;
      o.set_key("shared", 6);
      o.set_value({"from_a", 6});
   });
   db.create<kv_object>([](auto& o) {
      o.code = "contract.b"_n;
      o.set_key("shared", 6);
      o.set_value({"from_b", 6});
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr_a = idx.find(boost::make_tuple(name("contract.a"), uint16_t(0), std::string_view("shared", 6)));
   auto itr_b = idx.find(boost::make_tuple(name("contract.b"), uint16_t(0), std::string_view("shared", 6)));

   BOOST_REQUIRE(itr_a != idx.end());
   BOOST_REQUIRE(itr_b != idx.end());
   BOOST_CHECK_EQUAL(itr_a->value_view(), "from_a");
   BOOST_CHECK_EQUAL(itr_b->value_view(), "from_b");

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_secondary_index_ordering) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Store secondary index entries out of order
   auto make_sec = [](uint64_t v) {
      char buf[8];
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
      return std::string(buf, 8);
   };
   auto make_pri = [](uint64_t v) {
      char buf[8];
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
      return std::string(buf, 8);
   };

   std::vector<std::pair<uint64_t, uint64_t>> entries = {{50, 1}, {10, 2}, {90, 3}, {30, 4}};
   for (auto& [sec, pri] : entries) {
      auto sk = make_sec(sec);
      auto pk = make_pri(pri);
      db.create<kv_index_object>([&](auto& o) {
         o.code = "sectest"_n;
         o.table_id = compute_string_table_id("mytable.idx0");
         o.set_sec_key(sk.data(), sk.size());
         o.set_pri_key(pk.data(), pk.size());
      });
   }

   // Verify iteration is sorted by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(name("sectest"), compute_string_table_id("mytable.idx0")));

   std::vector<uint64_t> actual_order;
   while (itr != sec_idx.end() && itr->code == name("sectest")) {
      auto decode = [](const char* data, size_t size) -> uint64_t {
         uint64_t v = 0;
         for (size_t i = 0; i < size && i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(data[i]);
         return v;
      };
      actual_order.push_back(decode(itr->sec_key_view().data(), itr->sec_key_view().size()));
      ++itr;
   }

   BOOST_CHECK_EQUAL(actual_order.size(), 4u);
   BOOST_CHECK_EQUAL(actual_order[0], 10u);
   BOOST_CHECK_EQUAL(actual_order[1], 30u);
   BOOST_CHECK_EQUAL(actual_order[2], 50u);
   BOOST_CHECK_EQUAL(actual_order[3], 90u);

   session.undo();
}

// Test secondary index lookup via chainbase (simulates chain_plugin get_table_rows with index_name)
BOOST_AUTO_TEST_CASE(kv_secondary_index_lookup_by_table_id) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Simulate a kv::table with a secondary index:
   // Primary table: table_id = compute_table_id("users"_n)
   // Secondary index "byowner": sec_table_id = compute_sec_table_id("users"_n, "byowner"_n)
   const uint16_t pri_tid = chain::compute_table_id("users"_n.to_uint64_t());
   const uint16_t sec_tid = chain::compute_sec_table_id("users"_n.to_uint64_t(), "byowner"_n.to_uint64_t());

   // Insert primary rows: key=[id:8B BE], value=some data
   auto make_pri_key = [](uint64_t id) {
      char buf[8]; chain::kv_encode_be64(buf, id); return std::string(buf, 8);
   };

   for (uint64_t id : {1, 2, 3}) {
      auto pk = make_pri_key(id);
      db.create<kv_object>([&](auto& o) {
         o.code = "mycontract"_n;
         o.table_id = pri_tid;
         o.set_key(pk.data(), pk.size());
         o.set_value({"data", 4});
      });
   }

   // Insert secondary index entries: sec_key=[owner:8B BE], pri_key=[id:8B BE]
   auto make_owner_key = [](name owner) {
      char buf[8]; chain::kv_encode_be64(buf, owner.to_uint64_t()); return std::string(buf, 8);
   };

   struct sec_entry { uint64_t id; name owner; };
   std::vector<sec_entry> entries = {{1, "alice"_n}, {2, "bob"_n}, {3, "alice"_n}};
   for (auto& e : entries) {
      auto sk = make_owner_key(e.owner);
      auto pk = make_pri_key(e.id);
      db.create<kv_index_object>([&](auto& o) {
         o.code = "mycontract"_n;
         o.table_id = sec_tid;
         o.set_sec_key(sk.data(), sk.size());
         o.set_pri_key(pk.data(), pk.size());
      });
   }

   // Query secondary index by owner="alice" — should find entries with id=1 and id=3
   auto alice_key = make_owner_key("alice"_n);
   auto alice_sv = std::string_view(alice_key.data(), alice_key.size());

   const auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple("mycontract"_n, sec_tid, alice_sv));

   std::vector<uint64_t> found_ids;
   while (itr != sec_idx.end() && itr->code == "mycontract"_n && itr->table_id == sec_tid) {
      if (itr->sec_key_view() != alice_sv) break;
      // Look up primary row using raw pri_key bytes
      auto pri_sv = itr->pri_key_view();
      const auto& pri_idx = db.get_index<kv_index, by_code_key>();
      auto pri_itr = pri_idx.find(boost::make_tuple("mycontract"_n, pri_tid, pri_sv));
      BOOST_REQUIRE(pri_itr != pri_idx.end());
      found_ids.push_back(chain::kv_decode_be64(itr->pri_key_view().data()));
      ++itr;
   }

   BOOST_REQUIRE_EQUAL(found_ids.size(), 2u);
   BOOST_CHECK_EQUAL(found_ids[0], 1u);
   BOOST_CHECK_EQUAL(found_ids[1], 3u);

   session.undo();
}

// A key slot has no destructor, so a row releases its keys. A modifier that throws must not strand them, and the row
// that was being built never gets to chainbase's destroy.
BOOST_AUTO_TEST_CASE(create_that_throws_releases_its_keys) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());
   auto session = db.start_undo_session(true);

   struct create_failed {};
   const std::string key(unpooled_key_size, 'k');
   const auto failed_row = [&] {
      db.create<kv_object>([&](auto& o) {
         o.set_key(key.data(), key.size());
         throw create_failed{};
      });
   };
   const auto failed_entry = [&] {
      db.create<kv_index_object>([&](auto& o) {
         o.set_sec_key(key.data(), key.size());
         o.set_pri_key(key.data(), key.size());
         throw create_failed{};
      });
   };

   // The first pass takes its nodes from a new allocator batch, which stays allocated.
   BOOST_CHECK_THROW(failed_row(), create_failed);
   BOOST_CHECK_THROW(failed_entry(), create_failed);
   const size_t free_memory = db.get_free_memory();

   BOOST_CHECK_THROW(failed_row(), create_failed);
   BOOST_CHECK_THROW(failed_entry(), create_failed);
   BOOST_CHECK_EQUAL(db.get_free_memory(), free_memory);

   session.undo();
}

// The copy to the undo stack and the moves back are written by hand, and the form of each key follows from a size kept
// in the row. Every way back has to restore the bytes, and to leave no blob behind and none freed twice.
BOOST_AUTO_TEST_CASE(keys_keep_their_bytes_and_memory_through_undo) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());
   auto outer = db.start_undo_session(true);

   const std::string fits(kv_key_blob::inline_capacity, 's');
   const std::string spills(unpooled_key_size, 'l');
   const std::string taken(unpooled_key_size, 't');

   const auto walk = [&](const std::string& sec_key, const std::string& pri_key) {
      const auto make_entry = [&](const std::string& sec) -> const kv_index_object& {
         return db.create<kv_index_object>([&](auto& o) {
            o.code = "undokeys"_n;
            o.set_sec_key(sec.data(), sec.size());
            o.set_pri_key(pri_key.data(), pri_key.size());
         });
      };
      const auto& entry = make_entry(sec_key);
      const auto& other = make_entry(taken);
      const auto& row = db.create<kv_object>([&](auto& o) {
         o.code = "undokeys"_n;
         o.set_key(pri_key.data(), pri_key.size());
         o.set_value(sec_key);
      });
      const auto check_restored = [&] {
         BOOST_CHECK(entry.sec_key_view() == sec_key);
         BOOST_CHECK(entry.pri_key_view() == pri_key);
         BOOST_CHECK(row.key_view() == pri_key);
         BOOST_CHECK(row.value_view() == sec_key);
      };

      // Modify keeps a copy, which undo moves back. The new key has the other form from the old one.
      {
         auto session = db.start_undo_session(true);
         const std::string& rekey = sec_key == fits ? spills : fits;
         db.modify(entry, [&](auto& o) { o.set_sec_key(rekey.data(), rekey.size()); });
         BOOST_CHECK(entry.sec_key_view() == rekey);
         db.modify(row, [&](auto& o) { o.set_key(rekey.data(), rekey.size()); });
         BOOST_CHECK(row.key_view() == rekey);
         session.undo();
      }
      check_restored();

      // A modify that collides moves the copy back at once.
      {
         auto session = db.start_undo_session(true);
         BOOST_CHECK_THROW(db.modify(entry, [&](auto& o) { o.set_sec_key(taken.data(), taken.size()); }),
                           std::logic_error);
         check_restored();
         session.undo();
      }
      check_restored();

      // Undo puts a removed row back with its keys.
      {
         auto session = db.start_undo_session(true);
         db.remove(entry);
         db.remove(row);
         session.undo();
      }
      check_restored();

      // A squashed session drops its copy, and the row keeps the new key.
      {
         auto session = db.start_undo_session(true);
         db.modify(row, [&](auto& o) { o.set_key(taken.data(), taken.size()); });
         session.squash();
      }
      BOOST_CHECK(row.key_view() == taken);

      db.remove(entry);
      db.remove(other);
      db.remove(row);
   };

   for (const auto& [sec_key, pri_key] : {std::pair{&fits, &spills}, {&spills, &fits}, {&spills, &spills}}) {
      walk(*sec_key, *pri_key); // takes its nodes from new allocator batches, which stay allocated
      const size_t free_memory = db.get_free_memory();
      walk(*sec_key, *pri_key);
      BOOST_CHECK_EQUAL(db.get_free_memory(), free_memory);
   }

   outer.undo();
}

BOOST_AUTO_TEST_SUITE_END()
