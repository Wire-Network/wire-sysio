#include <boost/test/unit_test.hpp>

#include <sysio/net_plugin/protocol.hpp>
#include <sysio/net_plugin/buffer_factory.hpp>
#include <sysio/chain/transaction.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/private_key.hpp>
#include <string>

using namespace sysio;
using namespace sysio::chain;

namespace {

// Helper: create a minimal signed_transaction and return a packed_transaction.
packed_transaction make_packed_trx(packed_transaction::compression_type compression =
                                      packed_transaction::compression_type::none,
                                   unsigned num_sigs = 1) {
   signed_transaction strx;
   strx.actions.emplace_back( action{ {}, name{"test"_n}, name{"act"_n}, bytes{} } );
   strx.expiration = fc::time_point_sec{ fc::time_point::now() + fc::seconds(30) };
   strx.ref_block_num = 1;
   strx.ref_block_prefix = 2;

   for (unsigned i = 0; i < num_sigs; ++i) {
      auto priv = fc::crypto::private_key::generate();
      strx.signatures.push_back( priv.sign( fc::sha256::hash( std::to_string(i) ) ) );
   }
   return packed_transaction{ strx, compression };
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(trx_msg_wire)

// Verify transaction_message wire format: [which][trx_id (32 bytes)][packed_transaction ...]
// and that the ID can be peeked from the wire bytes without full deserialization.
BOOST_AUTO_TEST_CASE(test_transaction_message_wire_roundtrip) {
   auto pt = make_packed_trx();
   const auto expected_id = pt.id();

   // Serialize as transaction_message variant (same as buffer_factory does for the payload after the header).
   constexpr uint32_t trx_msg_which = to_index(msg_type_t::transaction_message);

   // Build wire bytes: [which][trx_id][packed_transaction]
   const uint32_t which_size = fc::raw::pack_size( unsigned_int( trx_msg_which ) );
   const uint32_t id_size = fc::raw::pack_size( expected_id );
   const uint32_t trx_size = fc::raw::pack_size( pt );
   const uint32_t total = which_size + id_size + trx_size;

   std::vector<char> wire(total);
   fc::datastream<char*> out_ds( wire.data(), wire.size() );
   fc::raw::pack( out_ds, unsigned_int( trx_msg_which ) );
   fc::raw::pack( out_ds, expected_id );
   fc::raw::pack( out_ds, pt );

   // Peek: read which + trx_id without consuming the full payload.
   fc::datastream<const char*> peek_ds( wire.data(), wire.size() );
   unsigned_int which{};
   fc::raw::unpack( peek_ds, which );
   BOOST_CHECK_EQUAL( which.value, trx_msg_which );

   transaction_id_type peeked_id;
   fc::raw::unpack( peek_ds, peeked_id );
   BOOST_CHECK( peeked_id == expected_id );

   // Full unpack of the remaining packed_transaction.
   packed_transaction unpacked_pt;
   fc::raw::unpack( peek_ds, unpacked_pt );
   BOOST_CHECK( unpacked_pt.id() == expected_id );
}

// Same test with zlib compression — the peeked ID must still work
// because the ID is outside the compressed payload.
BOOST_AUTO_TEST_CASE(test_transaction_message_zlib) {
   auto pt = make_packed_trx( packed_transaction::compression_type::zlib );
   const auto expected_id = pt.id();

   constexpr uint32_t trx_msg_which = to_index(msg_type_t::transaction_message);

   std::vector<char> wire;
   {
      const uint32_t which_size = fc::raw::pack_size( unsigned_int( trx_msg_which ) );
      const uint32_t id_size = fc::raw::pack_size( expected_id );
      const uint32_t trx_size = fc::raw::pack_size( pt );
      wire.resize( which_size + id_size + trx_size );
      fc::datastream<char*> out_ds( wire.data(), wire.size() );
      fc::raw::pack( out_ds, unsigned_int( trx_msg_which ) );
      fc::raw::pack( out_ds, expected_id );
      fc::raw::pack( out_ds, pt );
   }

   // Peek ID.
   fc::datastream<const char*> peek_ds( wire.data(), wire.size() );
   unsigned_int which{};
   fc::raw::unpack( peek_ds, which );
   transaction_id_type peeked_id;
   fc::raw::unpack( peek_ds, peeked_id );
   BOOST_CHECK( peeked_id == expected_id );

   // Full unpack.
   packed_transaction unpacked_pt;
   fc::raw::unpack( peek_ds, unpacked_pt );
   BOOST_CHECK( unpacked_pt.id() == expected_id );
}

// Multiple k1 signatures — verify larger payload doesn't affect peek.
BOOST_AUTO_TEST_CASE(test_transaction_message_multiple_sigs) {
   auto pt = make_packed_trx( packed_transaction::compression_type::none, 3 );
   const auto expected_id = pt.id();

   constexpr uint32_t trx_msg_which = to_index(msg_type_t::transaction_message);

   std::vector<char> wire;
   {
      const uint32_t which_size = fc::raw::pack_size( unsigned_int( trx_msg_which ) );
      const uint32_t id_size = fc::raw::pack_size( expected_id );
      const uint32_t trx_size = fc::raw::pack_size( pt );
      wire.resize( which_size + id_size + trx_size );
      fc::datastream<char*> out_ds( wire.data(), wire.size() );
      fc::raw::pack( out_ds, unsigned_int( trx_msg_which ) );
      fc::raw::pack( out_ds, expected_id );
      fc::raw::pack( out_ds, pt );
   }

   // Peek ID.
   fc::datastream<const char*> peek_ds( wire.data(), wire.size() );
   unsigned_int which{};
   fc::raw::unpack( peek_ds, which );
   transaction_id_type peeked_id;
   fc::raw::unpack( peek_ds, peeked_id );
   BOOST_CHECK( peeked_id == expected_id );

   // Full unpack.
   packed_transaction unpacked_pt;
   fc::raw::unpack( peek_ds, unpacked_pt );
   BOOST_CHECK( unpacked_pt.id() == expected_id );
}

// Verify trx_buffer_factory produces a send buffer whose payload matches the transaction_message wire format.
BOOST_AUTO_TEST_CASE(test_trx_buffer_factory_format) {
   auto pt = make_packed_trx();
   auto ptr = std::make_shared<packed_transaction>( std::move(pt) );
   const auto expected_id = ptr->id();

   trx_buffer_factory factory;
   const auto& sb = factory.get_send_buffer( ptr );

   // Send buffer layout: [4-byte size header][payload...]
   BOOST_REQUIRE( sb->size() > message_header_size );

   // Read the size header.
   uint32_t payload_size = 0;
   memcpy( &payload_size, sb->data(), sizeof(payload_size) );
   BOOST_CHECK_EQUAL( payload_size, sb->size() - message_header_size );

   // Parse the payload: [which][trx_id][packed_transaction]
   fc::datastream<const char*> ds( sb->data() + message_header_size, payload_size );
   unsigned_int which{};
   fc::raw::unpack( ds, which );
   BOOST_CHECK_EQUAL( which.value, to_index(msg_type_t::transaction_message) );

   transaction_id_type wire_id;
   fc::raw::unpack( ds, wire_id );
   BOOST_CHECK( wire_id == expected_id );

   packed_transaction unpacked_pt;
   fc::raw::unpack( ds, unpacked_pt );
   BOOST_CHECK( unpacked_pt.id() == expected_id );
   BOOST_CHECK_EQUAL( ds.remaining(), 0u );
}

BOOST_AUTO_TEST_SUITE_END()
