#include <boost/test/unit_test.hpp>

#include <sysio/net_plugin/trx_dedup.hpp>
#include <sysio/net_plugin/protocol.hpp>

#include <sysio/chain/transaction.hpp>

#include <fc/crypto/private_key.hpp>
#include <fc/io/raw.hpp>


using namespace sysio::chain;
using namespace fc;

namespace {

// Serialize a packed_transaction to wire bytes as it appears in a net_message:
// unsigned_int(variant_index for packed_transaction) + fc::raw::pack(pt)
std::vector<char> to_wire_bytes(const packed_transaction& pt) {
   constexpr uint32_t pt_index = fc::get_index<sysio::net_message, packed_transaction>();
   auto packed = fc::raw::pack(pt);
   std::vector<char> wire;
   wire.reserve(5 + packed.size()); // max varint + packed data
   // Pack the variant index
   auto which_packed = fc::raw::pack(fc::unsigned_int(pt_index));
   wire.insert(wire.end(), which_packed.begin(), which_packed.end());
   wire.insert(wire.end(), packed.begin(), packed.end());
   return wire;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(early_dedup_tests)

// Test: single k1 signature, compression none
BOOST_AUTO_TEST_CASE(test_early_dedup_k1_signature) {
   auto priv_key = crypto::private_key::generate();

   signed_transaction strx;
   strx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(300));
   strx.ref_block_num = 1;
   strx.ref_block_prefix = 100;
   strx.actions.emplace_back(
      action{vector<permission_level>{{name("test"), name("active")}}, name("sysio"), name("noop"), {}});
   auto digest = strx.sig_digest(chain_id_type::empty_chain_id());
   strx.signatures.push_back(priv_key.sign(digest));

   packed_transaction pt(strx, packed_transaction::compression_type::none);

   auto wire = to_wire_bytes(pt);
   fc::datastream<const char*> ds(wire.data(), wire.size());

   auto result = sysio::parse_trx_dedup_info(ds);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(result->first, pt.id());
   BOOST_CHECK(result->second == pt.expiration());
}

// Test: multiple k1 signatures
BOOST_AUTO_TEST_CASE(test_early_dedup_multiple_signatures) {
   auto key1 = crypto::private_key::generate();
   auto key2 = crypto::private_key::generate();
   auto key3 = crypto::private_key::generate();

   signed_transaction strx;
   strx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(600));
   strx.ref_block_num = 42;
   strx.ref_block_prefix = 9999;
   strx.actions.emplace_back(
      action{vector<permission_level>{{name("alice"), name("active")}, {name("bob"), name("active")}},
             name("sysio.token"), name("transfer"), {1, 2, 3, 4}});
   auto digest = strx.sig_digest(chain_id_type::empty_chain_id());
   strx.signatures.push_back(key1.sign(digest));
   strx.signatures.push_back(key2.sign(digest));
   strx.signatures.push_back(key3.sign(digest));

   packed_transaction pt(strx, packed_transaction::compression_type::none);

   auto wire = to_wire_bytes(pt);
   fc::datastream<const char*> ds(wire.data(), wire.size());

   auto result = sysio::parse_trx_dedup_info(ds);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(result->first, pt.id());
   BOOST_CHECK(result->second == pt.expiration());
}

// Test: webauthn signature (variant index 2) causes fallback
BOOST_AUTO_TEST_CASE(test_early_dedup_webauthn_fallback) {
   // Construct wire bytes manually with a webauthn variant index
   constexpr uint32_t pt_index = fc::get_index<sysio::net_message, packed_transaction>();

   std::vector<char> wire;
   {
      fc::datastream<size_t> sz;
      fc::raw::pack(sz, fc::unsigned_int(pt_index)); // which
      fc::raw::pack(sz, fc::unsigned_int(1));         // sig_count = 1
      fc::raw::pack(sz, fc::unsigned_int(2));         // variant index 2 = webauthn
      wire.resize(sz.tellp());
   }
   {
      fc::datastream<char*> out(wire.data(), wire.size());
      fc::raw::pack(out, fc::unsigned_int(pt_index));
      fc::raw::pack(out, fc::unsigned_int(1));
      fc::raw::pack(out, fc::unsigned_int(2));
   }

   fc::datastream<const char*> ds(wire.data(), wire.size());
   auto result = sysio::parse_trx_dedup_info(ds);
   BOOST_CHECK(!result.has_value());
}

// Test: zlib compression causes fallback
BOOST_AUTO_TEST_CASE(test_early_dedup_zlib_fallback) {
   auto priv_key = crypto::private_key::generate();

   signed_transaction strx;
   strx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(300));
   strx.ref_block_num = 1;
   strx.ref_block_prefix = 100;
   strx.actions.emplace_back(
      action{vector<permission_level>{{name("test"), name("active")}}, name("sysio"), name("noop"), {}});
   auto digest = strx.sig_digest(chain_id_type::empty_chain_id());
   strx.signatures.push_back(priv_key.sign(digest));

   packed_transaction pt(strx, packed_transaction::compression_type::zlib);

   auto wire = to_wire_bytes(pt);
   fc::datastream<const char*> ds(wire.data(), wire.size());

   auto result = sysio::parse_trx_dedup_info(ds);
   BOOST_CHECK(!result.has_value());
}

// Test: zero signatures (unusual but valid wire format)
BOOST_AUTO_TEST_CASE(test_early_dedup_zero_signatures) {
   signed_transaction strx;
   strx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(300));
   strx.ref_block_num = 1;
   strx.ref_block_prefix = 100;
   strx.actions.emplace_back(
      action{vector<permission_level>{{name("test"), name("active")}}, name("sysio"), name("noop"), {}});
   // No signatures

   packed_transaction pt(strx, packed_transaction::compression_type::none);

   auto wire = to_wire_bytes(pt);
   fc::datastream<const char*> ds(wire.data(), wire.size());

   auto result = sysio::parse_trx_dedup_info(ds);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(result->first, pt.id());
   BOOST_CHECK(result->second == pt.expiration());
}

BOOST_AUTO_TEST_SUITE_END()
