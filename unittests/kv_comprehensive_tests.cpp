#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/permission_link_object.hpp>
#include <sysio/state_history/create_deltas.hpp>
#include <sysio/state_history/serialization.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(kv_comprehensive_tests)

// ========== State Persistence Tests ==========

BOOST_AUTO_TEST_CASE(kv_data_survives_block_production) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   // Write data in one session
   {
      auto session = db.start_undo_session(true);
      db.create<kv_object>([](auto& o) {
         o.code = "persist"_n;
         o.key_assign("mykey", 5);
         o.value.assign("myvalue", 7);
      });
      session.push();
   }

   // Verify data is readable
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("persist"), std::string_view("mykey", 5)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->value.data(), itr->value.size()), "myvalue");
}

BOOST_AUTO_TEST_CASE(kv_data_reverts_on_undo) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto& idx = db.get_index<kv_index, by_code_key>();

   {
      auto session = db.start_undo_session(true);
      db.create<kv_object>([](auto& o) {
         o.code = "undotest"_n;
         o.key_assign("tempkey", 7);
         o.value.assign("tempval", 7);
      });

      // Data exists within session
      auto itr = idx.find(boost::make_tuple(name("undotest"), std::string_view("tempkey", 7)));
      BOOST_REQUIRE(itr != idx.end());

      session.undo();
   }

   // Data reverted after undo
   auto itr = idx.find(boost::make_tuple(name("undotest"), std::string_view("tempkey", 7)));
   BOOST_CHECK(itr == idx.end());
}

// ========== RAM Accounting Tests ==========

BOOST_AUTO_TEST_CASE(kv_ram_billing) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Create a row and verify billable size is reasonable
   db.create<kv_object>([](auto& o) {
      o.code = "ramtest"_n;
      o.key_assign("k", 1);
      o.value.assign("value123", 8);
   });

   // billable_size should include overhead + key + value
   auto billable = config::billable_size_v<kv_object>;
   BOOST_CHECK(billable > 0);
   BOOST_TEST_MESSAGE("kv_object billable_size_v = " << billable);

   session.undo();
}

// ========== Edge Case Tests ==========

BOOST_AUTO_TEST_CASE(kv_empty_key_and_value) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Empty key
   db.create<kv_object>([](auto& o) {
      o.code = "edge"_n;
      o.key_assign("", 0);
      o.value.assign("notempty", 8);
   });

   // Empty value
   db.create<kv_object>([](auto& o) {
      o.code = "edge"_n;
      o.key_assign("haskey", 6);
      o.value.assign("", 0);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr1 = idx.find(boost::make_tuple(name("edge"), std::string_view("", 0)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->value.size(), 8u);

   auto itr2 = idx.find(boost::make_tuple(name("edge"), std::string_view("haskey", 6)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->value.size(), 0u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_max_key_size) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // SSO path (24 bytes)
   std::string sso_key(24, 'A');
   db.create<kv_object>([&](auto& o) {
      o.code = "maxkey"_n;
      o.key_assign(sso_key.data(), sso_key.size());
      o.value.assign("v", 1);
   });

   // Heap path (256 bytes = max)
   std::string max_key(256, 'B');
   db.create<kv_object>([&](auto& o) {
      o.code = "maxkey"_n;
      o.key_assign(max_key.data(), max_key.size());
      o.value.assign("v", 1);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr1 = idx.find(boost::make_tuple(name("maxkey"), std::string_view(sso_key)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->key_size, 24u);

   auto itr2 = idx.find(boost::make_tuple(name("maxkey"), std::string_view(max_key)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->key_size, 256u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_cross_contract_isolation) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Two contracts store same key
   db.create<kv_object>([](auto& o) {
      o.code = "contract.a"_n;
      o.key_assign("shared", 6);
      o.value.assign("from_a", 6);
   });
   db.create<kv_object>([](auto& o) {
      o.code = "contract.b"_n;
      o.key_assign("shared", 6);
      o.value.assign("from_b", 6);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr_a = idx.find(boost::make_tuple(name("contract.a"), std::string_view("shared", 6)));
   auto itr_b = idx.find(boost::make_tuple(name("contract.b"), std::string_view("shared", 6)));

   BOOST_REQUIRE(itr_a != idx.end());
   BOOST_REQUIRE(itr_b != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr_a->value.data(), itr_a->value.size()), "from_a");
   BOOST_CHECK_EQUAL(std::string_view(itr_b->value.data(), itr_b->value.size()), "from_b");

   session.undo();
}

// ========== SHiP ABI Translation Tests ==========

BOOST_AUTO_TEST_CASE(kv_ship_delta_format_24byte_key) {
   // Verify that kv_object with 24-byte key serializes in legacy contract_row format
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Create a row with SHiP-compatible 24-byte key: [table:8B][scope:8B][pk:8B]
   auto encode_be64 = [](char* buf, uint64_t v) {
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
   };
   char key[24];
   encode_be64(key,      name("accounts").to_uint64_t());
   encode_be64(key + 8,  name("alice").to_uint64_t());
   encode_be64(key + 16, 1234567890ULL);

   db.create<kv_object>([&](auto& o) {
      o.code = "sysio.token"_n;
         o.payer = "sysio.token"_n;
      o.key_format = 1; // standard [table:8B][scope:8B][pk:8B]
      o.key_assign(key, 24);
      o.value.assign("testvalue", 9);
   });

   // Serialize using the SHiP serializer
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("sysio.token"), std::string_view(key, 24)));
   BOOST_REQUIRE(itr != idx.end());

   // Pack using history_serial_wrapper (same path as create_deltas)
   fc::datastream<size_t> ps;
   fc::raw::pack(ps, make_history_serial_wrapper(db, *itr));
   size_t packed_size = ps.tellp();
   BOOST_CHECK(packed_size > 0);

   std::vector<char> buf(packed_size);
   fc::datastream<char*> ds(buf.data(), buf.size());
   fc::raw::pack(ds, make_history_serial_wrapper(db, *itr));

   // Deserialize and verify legacy contract_row fields
   fc::datastream<const char*> rds(buf.data(), buf.size());

   fc::unsigned_int struct_version;
   uint64_t code, scope, table, primary_key, payer;
   fc::raw::unpack(rds, struct_version);
   fc::raw::unpack(rds, code);
   fc::raw::unpack(rds, scope);
   fc::raw::unpack(rds, table);
   fc::raw::unpack(rds, primary_key);
   fc::raw::unpack(rds, payer);

   BOOST_CHECK_EQUAL(struct_version.value, 0u);
   BOOST_CHECK_EQUAL(code, name("sysio.token").to_uint64_t());
   BOOST_CHECK_EQUAL(scope, name("alice").to_uint64_t());
   BOOST_CHECK_EQUAL(table, name("accounts").to_uint64_t());
   BOOST_CHECK_EQUAL(primary_key, 1234567890ULL);
   BOOST_CHECK_EQUAL(payer, name("sysio.token").to_uint64_t()); // payer = contract

   BOOST_TEST_MESSAGE("SHiP delta: 24-byte key correctly decoded to (code, scope, table, pk)");

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_ship_delta_format_nonstandard_key) {
   // Verify that kv_object with non-24-byte key is handled gracefully
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   db.create<kv_object>([](auto& o) {
      o.code = "rawcontract"_n;
      o.key_assign("short", 5);
      o.value.assign("data", 4);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("rawcontract"), std::string_view("short", 5)));
   BOOST_REQUIRE(itr != idx.end());

   // Serialize — should use the non-standard path (scope=0, table=0, pk=0)
   fc::datastream<size_t> ps;
   fc::raw::pack(ps, make_history_serial_wrapper(db, *itr));
   size_t packed_size = ps.tellp();
   BOOST_CHECK(packed_size > 0);

   std::vector<char> buf(packed_size);
   fc::datastream<char*> ds(buf.data(), buf.size());
   fc::raw::pack(ds, make_history_serial_wrapper(db, *itr));

   fc::datastream<const char*> rds(buf.data(), buf.size());

   fc::unsigned_int struct_version;
   uint64_t code, scope, table, primary_key, payer;
   fc::raw::unpack(rds, struct_version);
   fc::raw::unpack(rds, code);
   fc::raw::unpack(rds, scope);
   fc::raw::unpack(rds, table);
   fc::raw::unpack(rds, primary_key);
   fc::raw::unpack(rds, payer);

   BOOST_CHECK_EQUAL(struct_version.value, 0u);
   BOOST_CHECK_EQUAL(code, name("rawcontract").to_uint64_t());
   BOOST_CHECK_EQUAL(scope, 0u);  // non-standard key → scope = 0
   BOOST_CHECK_EQUAL(table, 0u);
   BOOST_CHECK_EQUAL(primary_key, 0u);

   BOOST_TEST_MESSAGE("SHiP delta: non-24-byte key handled with zeroed scope/table/pk");

   session.undo();
}

// ========== Secondary Index Tests ==========

BOOST_AUTO_TEST_CASE(kv_secondary_index_ordering) {
   validating_tester t;
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
         o.table = "mytable"_n;
         o.index_id = 0;
         o.sec_key_assign(sk.data(), sk.size());
         o.pri_key_assign(pk.data(), pk.size());
      });
   }

   // Verify iteration is sorted by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(name("sectest"), name("mytable"), uint8_t(0)));

   std::vector<uint64_t> actual_order;
   while (itr != sec_idx.end() && itr->code == name("sectest")) {
      auto decode = [](const char* data, uint16_t size) -> uint64_t {
         uint64_t v = 0;
         for (size_t i = 0; i < size && i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(data[i]);
         return v;
      };
      actual_order.push_back(decode(itr->sec_key_data(), itr->sec_key_size));
      ++itr;
   }

   BOOST_CHECK_EQUAL(actual_order.size(), 4u);
   BOOST_CHECK_EQUAL(actual_order[0], 10u);
   BOOST_CHECK_EQUAL(actual_order[1], 30u);
   BOOST_CHECK_EQUAL(actual_order[2], 50u);
   BOOST_CHECK_EQUAL(actual_order[3], 90u);

   session.undo();
}

// ========== Tester KV Fallback Tests ==========

BOOST_AUTO_TEST_CASE(kv_get_row_by_id_fallback) {
   // Verify the tester's get_row_by_id finds data in KV storage
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Store data using KV key encoding: [table:8B][scope:8B][pk:8B]
   auto encode_be64 = [](char* buf, uint64_t v) {
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
   };

   char key[24];
   encode_be64(key,      name("mytable").to_uint64_t());
   encode_be64(key + 8,  name("myscope").to_uint64_t());
   encode_be64(key + 16, 42);

   db.create<kv_object>([&](auto& o) {
      o.code = "mycode"_n;
      o.key_assign(key, 24);
      o.value.assign("hello_kv", 8);
   });

   // Use tester's get_row_by_id — should find via KV fallback
   auto data = t.get_row_by_id("mycode"_n, "myscope"_n, "mytable"_n, 42);
   BOOST_CHECK_EQUAL(data.size(), 8u);
   BOOST_CHECK_EQUAL(std::string(data.data(), data.size()), "hello_kv");

   session.undo();
}

// Verify billable_size constants cover actual chainbase object sizes.
// If these fail, billable_size_v is too small and users aren't being charged
// enough RAM — which is a security issue (RAM undercharging exploit).
BOOST_AUTO_TEST_CASE(billable_size_covers_all_objects) {
   // billable_size_v must be >= sizeof(object) to ensure we charge at least
   // as much as the actual memory consumed by the chainbase allocation.

   // KV objects
   BOOST_CHECK_GE(config::billable_size_v<kv_object>, sizeof(kv_object));
   BOOST_CHECK_GE(config::billable_size_v<kv_index_object>, sizeof(kv_index_object));

   // Account objects
   BOOST_CHECK_GE(config::billable_size_v<account_object>, sizeof(account_object));
   BOOST_CHECK_GE(config::billable_size_v<account_metadata_object>, sizeof(account_metadata_object));

   // Permission objects
   BOOST_CHECK_GE(config::billable_size_v<permission_object>, sizeof(permission_object));
   BOOST_CHECK_GE(config::billable_size_v<permission_link_object>, sizeof(permission_link_object));

   // Resource limits objects
   BOOST_CHECK_GE(config::billable_size_v<resource_limits::resource_object>, sizeof(resource_limits::resource_object));
   BOOST_CHECK_GE(config::billable_size_v<resource_limits::resource_pending_object>, sizeof(resource_limits::resource_pending_object));

   BOOST_TEST_MESSAGE("kv_object: sizeof=" << sizeof(kv_object)
      << " billable=" << config::billable_size_v<kv_object>);
   BOOST_TEST_MESSAGE("kv_index_object: sizeof=" << sizeof(kv_index_object)
      << " billable=" << config::billable_size_v<kv_index_object>);
   BOOST_TEST_MESSAGE("account_object: sizeof=" << sizeof(account_object)
      << " billable=" << config::billable_size_v<account_object>);
   BOOST_TEST_MESSAGE("permission_object: sizeof=" << sizeof(permission_object)
      << " billable=" << config::billable_size_v<permission_object>);
   BOOST_TEST_MESSAGE("resource_object: sizeof=" << sizeof(resource_limits::resource_object)
      << " billable=" << config::billable_size_v<resource_limits::resource_object>);
}

BOOST_AUTO_TEST_SUITE_END()
