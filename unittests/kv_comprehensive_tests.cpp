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

// ========== RAM Accounting Tests ==========

BOOST_AUTO_TEST_CASE(kv_ram_billing) {
   BOOST_CHECK_EQUAL(config::billable_size_v<kv_object>, 112u);
   BOOST_CHECK_EQUAL(config::billable_size_v<kv_index_object>, 112u);
}

// ========== Edge Case Tests ==========

// ========== SHiP ABI Translation Tests ==========

// Helper: serialize a kv_row via SHiP history_serial_wrapper, then deserialize
// and verify contract_row_kv_v0 {code, payer, table_id, key, value}.
static void verify_ship_kv_roundtrip(const chainbase::database& db, const kv_row& obj,
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
   // All KV rows serialize as contract_row_kv_v0 {code, payer, table_id, key, value}
   // regardless of key size or content. Verify with multiple key shapes.
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());
   auto& store = const_cast<kv_store&>(t.control->kv());

   auto session = db.start_undo_session(true);

   // --- Case 1: short key (5 bytes) ---
   const uint16_t tid_short = compute_string_table_id("geodata");
   const kv_row obj_short = *store.insert_row("rawcontract"_n, tid_short, "rawcontract"_n, std::string_view("short", 5),
                                              std::string_view("data", 4));
   verify_ship_kv_roundtrip(db, obj_short, tid_short, "short", 5, "data", 4);

   // --- Case 2: 24-byte key (old standard layout, now just opaque bytes) ---
   auto encode_be64 = [](char* buf, uint64_t v) {
      for (int i = 7; i >= 0; --i) { buf[i] = static_cast<char>(v & 0xFF); v >>= 8; }
   };
   char key24[24];
   encode_be64(key24,      name("accounts").to_uint64_t());
   encode_be64(key24 + 8,  name("alice").to_uint64_t());
   encode_be64(key24 + 16, 1234567890ULL);

   const uint16_t tid_accts = compute_string_table_id("accounts");
   const kv_row obj_24 = *store.insert_row("sysio.token"_n, tid_accts, "sysio.token"_n, std::string_view(key24, 24),
                                           std::string_view("testvalue", 9));
   verify_ship_kv_roundtrip(db, obj_24, tid_accts, key24, 24, "testvalue", 9);

   // --- Case 3: 16-byte key (new standard scope+pk layout) ---
   char key16[16];
   encode_be64(key16,     name("alice").to_uint64_t());
   encode_be64(key16 + 8, 42ULL);

   const kv_row obj_16 = *store.insert_row("sysio.token"_n, tid_accts, "sysio.token"_n, std::string_view(key16, 16),
                                           std::string_view("balance", 7));
   verify_ship_kv_roundtrip(db, obj_16, tid_accts, key16, 16, "balance", 7);

   // --- Case 4: empty value ---
   const uint16_t tid_flags = compute_string_table_id("flags");
   const kv_row obj_empty =
      *store.insert_row("flagtest"_n, tid_flags, "flagtest"_n, std::string_view("present", 7), std::string_view());
   verify_ship_kv_roundtrip(db, obj_empty, tid_flags, "present", 7, nullptr, 0);

   // --- Case 5: large key (256 bytes) ---
   std::string big_key(256, '\xAB');
   const uint16_t tid_big = compute_string_table_id("bigkeys");
   const kv_row obj_big = *store.insert_row("bigtest"_n, tid_big, "bigtest"_n, big_key, std::string_view("v", 1));
   verify_ship_kv_roundtrip(db, obj_big, tid_big, big_key.data(), 256, "v", 1);

   session.undo();
}

// ========== Secondary Index Tests ==========

// ========== Tester KV Fallback Tests ==========

BOOST_AUTO_TEST_CASE(kv_get_row_by_id_fallback) {
   // Verify the tester's get_row_by_id finds data in KV storage
   validating_tester t( flat_set<account_name>(), nullptr, setup_policy::none );
   auto& db = const_cast<chainbase::database&>(t.control->db());

   auto session = db.start_undo_session(true);

   // Store data using new KV key encoding: [scope:8B][pk:8B] with table_id
   auto scoped_key = chain::make_kv_scoped_key("myscope"_n, 42);
   auto tid = chain::compute_table_id("mytable"_n.to_uint64_t());

   const_cast<kv_store&>(t.control->kv()).insert_row("mycode"_n, tid, {},
                                                     std::string_view(scoped_key.data, chain::kv_scoped_key_size),
                                                     "hello_kv");

   // Use tester's get_row_by_id — should find via KV fallback
   auto data = t.get_row_by_id("mycode"_n, "myscope"_n, "mytable"_n, 42);
   BOOST_CHECK_EQUAL(data.size(), 8u);
   BOOST_CHECK_EQUAL(std::string(data.data(), data.size()), "hello_kv");

   session.undo();
}

// ========== kv::raw_table Tests (BE keys) ==========

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
   BOOST_CHECK_EXCEPTION(t.push_action("kvmap"_n, "get"_n, "kvmap"_n,
      mutable_variant_object()("region", "eu-west")("id", 99)),
      sysio_assert_message_exception, sysio_assert_message_is("key not found"));
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
   BOOST_CHECK_EXCEPTION(t.push_action("kvmap"_n, "get"_n, "kvmap"_n,
      mutable_variant_object()("region", "us-east")("id", 1)),
      sysio_assert_message_exception, sysio_assert_message_is("key not found"));
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

BOOST_AUTO_TEST_SUITE_END()
