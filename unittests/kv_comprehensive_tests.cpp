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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
   auto itr = idx.find(boost::make_tuple(name("persist"), uint16_t(0), std::string_view("mykey", 5)));
   BOOST_REQUIRE(itr != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr->value.data(), itr->value.size()), "myvalue");
}

BOOST_AUTO_TEST_CASE(kv_data_reverts_on_undo) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
      auto itr = idx.find(boost::make_tuple(name("undotest"), uint16_t(0), std::string_view("tempkey", 7)));
      BOOST_REQUIRE(itr != idx.end());

      session.undo();
   }

   // Data reverted after undo
   auto itr = idx.find(boost::make_tuple(name("undotest"), uint16_t(0), std::string_view("tempkey", 7)));
   BOOST_CHECK(itr == idx.end());
}

// ========== RAM Accounting Tests ==========

BOOST_AUTO_TEST_CASE(kv_ram_billing) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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

   auto itr1 = idx.find(boost::make_tuple(name("edge"), uint16_t(0), std::string_view("", 0)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->value.size(), 8u);

   auto itr2 = idx.find(boost::make_tuple(name("edge"), uint16_t(0), std::string_view("haskey", 6)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->value.size(), 0u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_max_key_size) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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

   auto itr1 = idx.find(boost::make_tuple(name("maxkey"), uint16_t(0), std::string_view(std_key)));
   BOOST_REQUIRE(itr1 != idx.end());
   BOOST_CHECK_EQUAL(itr1->key.size(), chain::kv_key_size);

   auto itr2 = idx.find(boost::make_tuple(name("maxkey"), uint16_t(0), std::string_view(max_key)));
   BOOST_REQUIRE(itr2 != idx.end());
   BOOST_CHECK_EQUAL(itr2->key.size(), 256u);

   session.undo();
}

BOOST_AUTO_TEST_CASE(kv_cross_contract_isolation) {
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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

   auto itr_a = idx.find(boost::make_tuple(name("contract.a"), uint16_t(0), std::string_view("shared", 6)));
   auto itr_b = idx.find(boost::make_tuple(name("contract.b"), uint16_t(0), std::string_view("shared", 6)));

   BOOST_REQUIRE(itr_a != idx.end());
   BOOST_REQUIRE(itr_b != idx.end());
   BOOST_CHECK_EQUAL(std::string_view(itr_a->value.data(), itr_a->value.size()), "from_a");
   BOOST_CHECK_EQUAL(std::string_view(itr_b->value.data(), itr_b->value.size()), "from_b");

   session.undo();
}

// ========== SHiP ABI Translation Tests ==========

// Helper: serialize kv_object via SHiP history_serial_wrapper, then deserialize
// and verify contract_row_kv_v0 {code, payer, table_id, key, value}.
static void verify_ship_kv_roundtrip(const chainbase::database& db, const kv_object& obj,
                                     uint16_t expected_tid, const char* expected_key, size_t expected_key_len,
                                     const char* expected_val, size_t expected_val_len) {
   fc::datastream<size_t> ps;
   fc::raw::pack(ps, make_history_serial_wrapper(db, obj));
   std::vector<char> buf(ps.tellp());
   fc::datastream<char*> ds(buf.data(), buf.size());
   fc::raw::pack(ds, make_history_serial_wrapper(db, obj));

   fc::datastream<const char*> rds(buf.data(), buf.size());
   fc::unsigned_int struct_version;
   uint64_t code, payer;
   uint16_t table_id;
   fc::raw::unpack(rds, struct_version);
   fc::raw::unpack(rds, code);
   fc::raw::unpack(rds, payer);
   fc::raw::unpack(rds, table_id);

   BOOST_CHECK_EQUAL(struct_version.value, 0u);
   BOOST_CHECK_EQUAL(code, obj.code.to_uint64_t());
   BOOST_CHECK_EQUAL(payer, obj.payer.to_uint64_t());
   BOOST_CHECK_EQUAL(table_id, expected_tid);

   std::vector<char> key_bytes, value_bytes;
   fc::raw::unpack(rds, key_bytes);
   fc::raw::unpack(rds, value_bytes);
   BOOST_CHECK_EQUAL(key_bytes.size(), expected_key_len);
   if (expected_key_len > 0)
      BOOST_CHECK(memcmp(key_bytes.data(), expected_key, expected_key_len) == 0);
   BOOST_CHECK_EQUAL(value_bytes.size(), expected_val_len);
   if (expected_val_len > 0)
      BOOST_CHECK(memcmp(value_bytes.data(), expected_val, expected_val_len) == 0);
}

BOOST_AUTO_TEST_CASE(kv_ship_delta_format_contract_row_kv) {
   // All kv_objects serialize as contract_row_kv_v0 {code, payer, table_id, key, value}
   // regardless of key size or content. Verify with multiple key shapes.
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // --- Case 1: short key (5 bytes) ---
   const uint16_t tid_short = compute_table_id("geodata");
   const auto& obj_short = db.create<kv_object>([&](auto& o) {
      o.code = "rawcontract"_n;
      o.payer = "rawcontract"_n;
      o.table_id = tid_short;
      o.key.assign("short", 5);
      o.value.assign("data", 4);
   });
   verify_ship_kv_roundtrip(db, obj_short, tid_short, "short", 5, "data", 4);

   // --- Case 2: 24-byte key (old standard layout, now just opaque bytes) ---
   auto encode_be64 = [](char* buf, uint64_t v) {
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
   };
   char key24[24];
   encode_be64(key24,      name("accounts").to_uint64_t());
   encode_be64(key24 + 8,  name("alice").to_uint64_t());
   encode_be64(key24 + 16, 1234567890ULL);

   const uint16_t tid_accts = compute_table_id("accounts");
   const auto& obj_24 = db.create<kv_object>([&](auto& o) {
      o.code = "sysio.token"_n;
      o.payer = "sysio.token"_n;
      o.table_id = tid_accts;
      o.key.assign(key24, 24);
      o.value.assign("testvalue", 9);
   });
   verify_ship_kv_roundtrip(db, obj_24, tid_accts, key24, 24, "testvalue", 9);

   // --- Case 3: 16-byte key (new standard scope+pk layout) ---
   char key16[16];
   encode_be64(key16,     name("alice").to_uint64_t());
   encode_be64(key16 + 8, 42ULL);

   const auto& obj_16 = db.create<kv_object>([&](auto& o) {
      o.code = "sysio.token"_n;
      o.payer = "sysio.token"_n;
      o.table_id = tid_accts;
      o.key.assign(key16, 16);
      o.value.assign("balance", 7);
   });
   verify_ship_kv_roundtrip(db, obj_16, tid_accts, key16, 16, "balance", 7);

   // --- Case 4: empty value ---
   const uint16_t tid_flags = compute_table_id("flags");
   const auto& obj_empty = db.create<kv_object>([&](auto& o) {
      o.code = "flagtest"_n;
      o.payer = "flagtest"_n;
      o.table_id = tid_flags;
      o.key.assign("present", 7);
      // value left empty
   });
   verify_ship_kv_roundtrip(db, obj_empty, tid_flags, "present", 7, nullptr, 0);

   // --- Case 5: large key (256 bytes) ---
   std::string big_key(256, '\xAB');
   const uint16_t tid_big = compute_table_id("bigkeys");
   const auto& obj_big = db.create<kv_object>([&](auto& o) {
      o.code = "bigtest"_n;
      o.payer = "bigtest"_n;
      o.table_id = tid_big;
      o.key.assign(big_key.data(), big_key.size());
      o.value.assign("v", 1);
   });
   verify_ship_kv_roundtrip(db, obj_big, tid_big, big_key.data(), 256, "v", 1);

   session.undo();
}

// ========== Secondary Index Tests ==========

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
         o.table_id = compute_table_id("mytable.idx0");
         o.sec_key.assign(sk.data(), sk.size());
         o.pri_key.assign(pk.data(), pk.size());
      });
   }

   // Verify iteration is sorted by secondary key
   auto& sec_idx = db.get_index<kv_index_index, by_code_table_id_seckey>();
   auto itr = sec_idx.lower_bound(boost::make_tuple(name("sectest"), compute_table_id("mytable.idx0")));

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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Store data using new KV key encoding: [scope:8B][pk:8B] with table_id
   auto scoped_key = chain::make_kv_scoped_key("myscope"_n, 42);
   auto tid = chain::compute_table_id("mytable"_n.to_uint64_t());

   db.create<kv_object>([&](auto& o) {
      o.code = "mycode"_n;
      o.table_id = tid;
      o.key.assign(scoped_key.data, chain::kv_scoped_key_size);
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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
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
      if (tbl.name == "geodata") {
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
         o.key.assign(pk.data(), pk.size());
         o.value.assign("data", 4);
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
         o.sec_key.assign(sk.data(), sk.size());
         o.pri_key.assign(pk.data(), pk.size());
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
      found_ids.push_back(chain::kv_decode_be64(itr->pri_key.data()));
      ++itr;
   }

   BOOST_REQUIRE_EQUAL(found_ids.size(), 2u);
   BOOST_CHECK_EQUAL(found_ids[0], 1u);
   BOOST_CHECK_EQUAL(found_ids[1], 3u);

   session.undo();
}

BOOST_AUTO_TEST_SUITE_END()
