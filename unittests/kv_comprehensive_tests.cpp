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
#include <sysio/chain/abi_serializer.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mutable_variant_object = fc::mutable_variant_object;

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
         o.key.assign("mykey", 5);
         o.value.assign("myvalue", 7);
      });
      session.push();
   }

   // Verify data is readable
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("persist"), config::kv_format_raw, std::string_view("mykey", 5)));
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
         o.key.assign("tempkey", 7);
         o.value.assign("tempval", 7);
      });

      // Data exists within session
      auto itr = idx.find(boost::make_tuple(name("undotest"), config::kv_format_raw, std::string_view("tempkey", 7)));
      BOOST_REQUIRE(itr != idx.end());

      session.undo();
   }

   // Data reverted after undo
   auto itr = idx.find(boost::make_tuple(name("undotest"), config::kv_format_raw, std::string_view("tempkey", 7)));
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
      o.key.assign("k", 1);
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
      o.key.assign("", 0);
      o.value.assign("notempty", 8);
   });

   // Empty value
   db.create<kv_object>([](auto& o) {
      o.code = "edge"_n;
      o.key.assign("haskey", 6);
      o.value.assign("", 0);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr1 = idx.find(boost::make_tuple(name("edge"), config::kv_format_raw, std::string_view("", 0)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->value.size(), 8u);

   auto itr2 = idx.find(boost::make_tuple(name("edge"), config::kv_format_raw, std::string_view("haskey", 6)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->value.size(), 0u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_max_key_size) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // 24-byte key (standard kv_key_size)
   std::string std_key(24, 'A');
   db.create<kv_object>([&](auto& o) {
      o.code = "maxkey"_n;
      o.key.assign(std_key.data(), std_key.size());
      o.value.assign("v", 1);
   });

   // Large key (256 bytes = default max, on-chain config can raise up to 1024)
   std::string max_key(256, 'B');
   db.create<kv_object>([&](auto& o) {
      o.code = "maxkey"_n;
      o.key.assign(max_key.data(), max_key.size());
      o.value.assign("v", 1);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr1 = idx.find(boost::make_tuple(name("maxkey"), config::kv_format_raw, std::string_view(std_key)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->key.size(), chain::kv_key_size);

   auto itr2 = idx.find(boost::make_tuple(name("maxkey"), config::kv_format_raw, std::string_view(max_key)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->key.size(), 256u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_cross_contract_isolation) {
   validating_tester t;
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Two contracts store same key
   db.create<kv_object>([](auto& o) {
      o.code = "contract.a"_n;
      o.key.assign("shared", 6);
      o.value.assign("from_a", 6);
   });
   db.create<kv_object>([](auto& o) {
      o.code = "contract.b"_n;
      o.key.assign("shared", 6);
      o.value.assign("from_b", 6);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();

   auto itr_a = idx.find(boost::make_tuple(name("contract.a"), config::kv_format_raw, std::string_view("shared", 6)));
   auto itr_b = idx.find(boost::make_tuple(name("contract.b"), config::kv_format_raw, std::string_view("shared", 6)));

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
      o.key.assign(key, chain::kv_key_size);
      o.value.assign("testvalue", 9);
   });

   // Serialize using the SHiP serializer
   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("sysio.token"), config::kv_format_standard, std::string_view(key, chain::kv_key_size)));
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
      o.key.assign("short", 5);
      o.value.assign("data", 4);
   });

   auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(name("rawcontract"), config::kv_format_raw, std::string_view("short", 5)));
   BOOST_REQUIRE(itr != idx.end());

   // Serialize — should use the contract_row_kv_v0 path {code, payer, key, value}
   fc::datastream<size_t> ps;
   fc::raw::pack(ps, make_history_serial_wrapper(db, *itr));
   size_t packed_size = ps.tellp();
   BOOST_CHECK(packed_size > 0);

   std::vector<char> buf(packed_size);
   fc::datastream<char*> ds(buf.data(), buf.size());
   fc::raw::pack(ds, make_history_serial_wrapper(db, *itr));

   fc::datastream<const char*> rds(buf.data(), buf.size());

   fc::unsigned_int struct_version;
   uint64_t code, payer;
   fc::raw::unpack(rds, struct_version);
   fc::raw::unpack(rds, code);
   fc::raw::unpack(rds, payer);

   BOOST_CHECK_EQUAL(struct_version.value, 0u);
   BOOST_CHECK_EQUAL(code, name("rawcontract").to_uint64_t());
   BOOST_CHECK_EQUAL(payer, 0u); // default-constructed payer

   // Unpack key bytes
   std::vector<char> key_bytes;
   fc::raw::unpack(rds, key_bytes);
   BOOST_CHECK_EQUAL(key_bytes.size(), 5u);
   BOOST_CHECK(std::string(key_bytes.data(), key_bytes.size()) == "short");

   // Unpack value bytes
   std::vector<char> value_bytes;
   fc::raw::unpack(rds, value_bytes);
   BOOST_CHECK_EQUAL(value_bytes.size(), 4u);
   BOOST_CHECK(std::string(value_bytes.data(), value_bytes.size()) == "data");

   BOOST_TEST_MESSAGE("SHiP delta: non-24-byte key serialized as contract_row_kv_v0 {code, payer, key, value}");

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
         o.sec_key.assign(sk.data(), sk.size());
         o.pri_key.assign(pk.data(), pk.size());
      });
   }

   // Verify iteration is sorted by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(name("sectest"), name("mytable"), uint8_t(0)));

   std::vector<uint64_t> actual_order;
   while (itr != sec_idx.end() && itr->code == name("sectest")) {
      auto decode = [](const char* data, size_t size) -> uint64_t {
         uint64_t v = 0;
         for (size_t i = 0; i < size && i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(data[i]);
         return v;
      };
      actual_order.push_back(decode(itr->sec_key.data(), itr->sec_key.size()));
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
      o.key_format = 1;
      o.key.assign(key, chain::kv_key_size);
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

// ========== kv::raw_table Tests (format=0, BE keys) ==========

BOOST_AUTO_TEST_CASE(kv_map_store_and_get) {
   validating_tester t;
   t.produce_block();

   t.create_accounts({"kvmap"_n});
   t.produce_block();
   t.set_code("kvmap"_n, test_contracts::test_kv_map_wasm());
   t.set_abi("kvmap"_n, test_contracts::test_kv_map_abi().c_str());
   t.produce_block();

   // Store an entry
   BOOST_CHECK_NO_THROW(t.push_action("kvmap"_n, "put"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)("payload", "hello")("amount", 100)));

   // Retrieve it
   BOOST_CHECK_NO_THROW(t.push_action("kvmap"_n, "get"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)));

   // Non-existent key should fail
   BOOST_CHECK_THROW(t.push_action("kvmap"_n, "get"_n, "kvmap"_n,
      mutable_variant_object()("region", "eu-west")("id", 99)), fc::exception);
}

BOOST_AUTO_TEST_CASE(kv_map_erase) {
   validating_tester t;
   t.produce_block();

   t.create_accounts({"kvmap"_n});
   t.produce_block();
   t.set_code("kvmap"_n, test_contracts::test_kv_map_wasm());
   t.set_abi("kvmap"_n, test_contracts::test_kv_map_abi().c_str());
   t.produce_block();

   t.push_action("kvmap"_n, "put"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)("payload", "hello")("amount", 100));

   // Erase
   BOOST_CHECK_NO_THROW(t.push_action("kvmap"_n, "erase"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)));

   // Should be gone
   BOOST_CHECK_THROW(t.push_action("kvmap"_n, "get"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)), fc::exception);
}

BOOST_AUTO_TEST_CASE(kv_map_iteration) {
   validating_tester t;
   t.produce_block();

   t.create_accounts({"kvmap"_n});
   t.produce_block();
   t.set_code("kvmap"_n, test_contracts::test_kv_map_wasm());
   t.set_abi("kvmap"_n, test_contracts::test_kv_map_abi().c_str());
   t.produce_block();

   // Store multiple entries
   t.push_action("kvmap"_n, "put"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)("payload", "a")("amount", 10));
   t.push_action("kvmap"_n, "put"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 2)("payload", "b")("amount", 20));
   t.push_action("kvmap"_n, "put"_n, "kvmap"_n,
      mutable_variant_object()("region", "eu-west")("id", 1)("payload", "c")("amount", 30));

   // count action iterates and asserts > 0
   BOOST_CHECK_NO_THROW(t.push_action("kvmap"_n, "count"_n, "kvmap"_n,
      mutable_variant_object()));
}

// int64_t key ordering: negatives must sort before positives
BOOST_AUTO_TEST_CASE(kv_map_signed_key_ordering) {
   validating_tester t;
   t.produce_block();

   t.create_accounts({"kvmap"_n});
   t.produce_block();
   t.set_code("kvmap"_n, test_contracts::test_kv_map_wasm());
   t.set_abi("kvmap"_n, test_contracts::test_kv_map_abi().c_str());
   t.produce_block();

   BOOST_CHECK_NO_THROW(t.push_action("kvmap"_n, "chkintorder"_n, "kvmap"_n,
      mutable_variant_object()));
}

BOOST_AUTO_TEST_CASE(kv_map_abi_key_metadata) {
   // Verify the contract ABI has key_names/key_types from [[sysio::kv_key]]
   auto abi_str = test_contracts::test_kv_map_abi();
   auto abi_var = fc::json::from_string(abi_str);
   auto abi = abi_var.as<abi_def>();

   // Find the "geodata" table
   bool found = false;
   for (const auto& tbl : abi.tables) {
      if (tbl.name == "geodata"_n) {
         found = true;

         // Verify key_names from the my_key struct
         BOOST_REQUIRE_EQUAL(tbl.key_names.size(), 2u);
         BOOST_CHECK_EQUAL(tbl.key_names[0], "region");
         BOOST_CHECK_EQUAL(tbl.key_names[1], "id");

         // Verify key_types
         BOOST_REQUIRE_EQUAL(tbl.key_types.size(), 2u);
         BOOST_CHECK_EQUAL(tbl.key_types[0], "string");
         BOOST_CHECK_EQUAL(tbl.key_types[1], "uint64");

         // Verify value type
         BOOST_CHECK_EQUAL(tbl.type, "my_value");
         break;
      }
   }
   BOOST_CHECK(found);

   // Verify my_key struct is also in the ABI
   bool key_struct_found = false;
   for (const auto& s : abi.structs) {
      if (s.name == "my_key") {
         key_struct_found = true;
         BOOST_REQUIRE_EQUAL(s.fields.size(), 2u);
         BOOST_CHECK_EQUAL(s.fields[0].name, "region");
         BOOST_CHECK_EQUAL(s.fields[0].type, "string");
         BOOST_CHECK_EQUAL(s.fields[1].name, "id");
         BOOST_CHECK_EQUAL(s.fields[1].type, "uint64");
         break;
      }
   }
   BOOST_CHECK(key_struct_found);
}

BOOST_AUTO_TEST_SUITE_END()
