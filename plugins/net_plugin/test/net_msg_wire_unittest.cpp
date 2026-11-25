#include "../src/trx_admission.hpp"

#include <boost/test/unit_test.hpp>

#include <sysio/net_plugin/protocol.hpp>
#include <sysio/net_plugin/buffer_factory.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/vote_message.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/network/message_buffer.hpp>

#include <atomic>
#include <stdexcept>
#include <string>

using namespace sysio;
using namespace sysio::chain;

namespace {

struct test_trx_connection : std::enable_shared_from_this<test_trx_connection> {
   std::atomic<uint32_t> trx_in_progress_size{0};
};

struct trx_admission_fixture {
   std::shared_ptr<test_trx_connection> conn = std::make_shared<test_trx_connection>();
   uint32_t drops = 0;
   uint32_t submissions = 0;
   packed_transaction_ptr dropped_trx;
   packed_transaction_ptr submitted_trx;
   next_function<transaction_trace_ptr> completion;

   template <typename Stream>
   void receive(fc::bounded_datastream<Stream>& frame, const transaction_id_type& id) {
      net_utils::process_trx_message_body(
         frame, id, conn->trx_in_progress_size,
         [&](uint32_t, const packed_transaction_ptr& trx) {
            BOOST_CHECK_EQUAL(frame.remaining(), 0u);
            ++drops;
            dropped_trx = trx;
         },
         [](const transaction_id_type&) { BOOST_FAIL("unexpected transaction ID mismatch"); },
         [&](const packed_transaction_ptr& trx, uint32_t trx_size) {
            BOOST_CHECK_EQUAL(frame.remaining(), 0u);
            ++submissions;
            submitted_trx = trx;
            completion = net_utils::track_trx_in_progress(*conn, trx_size, [](const auto&) {});
         });
   }
};

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

// Helper: create a vote_message with a random BLS key.
vote_message make_vote_message(const block_id_type& bid = block_id_type(), bool strong = true) {
   auto bls_priv = fc::crypto::bls::private_key::generate();
   auto bls_pub = bls_priv.get_public_key();
   auto sig = bls_priv.sign_sha256( fc::sha256::hash("test_vote") );

   vote_message vm;
   vm.block_id = bid;
   vm.strong = strong;
   vm.finalizer_key = bls_pub;
   vm.sig = sig;
   return vm;
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

// A zlib-compressed transaction is rejected when its payload is unpacked; the peeked ID, outside the payload, still
// reads.
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
   BOOST_CHECK_THROW( fc::raw::unpack( peek_ds, unpacked_pt ), tx_compression_not_allowed );
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

BOOST_FIXTURE_TEST_SUITE(trx_admission, trx_admission_fixture)

BOOST_AUTO_TEST_CASE(saturated_peer_skips_body_and_resumes_after_completion) {
   const auto trx = make_packed_trx();
   const auto trx_size = static_cast<uint32_t>(trx.get_estimated_size());
   const auto limit = net_utils::def_max_trx_in_progress_size;
   conn->trx_in_progress_size = limit - trx_size + 1;
   auto prior_completion = net_utils::track_trx_in_progress(*conn, trx_size, [](const auto&) {});
   BOOST_REQUIRE_EQUAL(conn->trx_in_progress_size.load(), limit + 1);

   // Both saturated bodies are too short to unpack. The following body must remain readable.
   constexpr uint32_t truncated_body_size = 1;
   constexpr uint32_t buffer_chunk_size = 64;
   auto next_body = fc::raw::pack(trx);
   std::vector<char> wire(2 * truncated_body_size, 0);
   wire.insert(wire.end(), next_body.begin(), next_body.end());
   fc::message_buffer<buffer_chunk_size> buffer;
   buffer.add_space(wire.size());
   BOOST_REQUIRE_EQUAL(boost::asio::buffer_copy(buffer.get_buffer_sequence_for_boost_async_read(),
                                                boost::asio::buffer(wire)), wire.size());
   buffer.advance_write_ptr(wire.size());
   auto stream = buffer.create_datastream();
   fc::bounded_datastream over_limit_frame(stream, truncated_body_size);
   receive(over_limit_frame, trx.id());
   BOOST_CHECK_EQUAL(drops, 1u);
   BOOST_CHECK(!dropped_trx);
   BOOST_CHECK_EQUAL(submissions, 0u);
   BOOST_REQUIRE_EQUAL(buffer.bytes_to_read(), truncated_body_size + next_body.size());

   // Simulate completion freeing one byte and leaving the balance exactly at the cap.
   --conn->trx_in_progress_size;
   fc::bounded_datastream at_limit_frame(stream, truncated_body_size);
   receive(at_limit_frame, trx.id());
   BOOST_CHECK_EQUAL(drops, 2u);
   BOOST_CHECK(!dropped_trx);
   BOOST_CHECK_EQUAL(submissions, 0u);
   BOOST_REQUIRE_EQUAL(buffer.bytes_to_read(), next_body.size());

   // Completion releases the charge even if a copy of the one-shot callback outlives invocation.
   const auto retained_completion = prior_completion;
   prior_completion(transaction_trace_ptr{});
   BOOST_CHECK_EQUAL(conn->trx_in_progress_size.load(), limit - trx_size);
   fc::bounded_datastream next_frame(stream, next_body.size());
   receive(next_frame, trx.id());
   BOOST_CHECK_EQUAL(drops, 2u);
   BOOST_CHECK_EQUAL(submissions, 1u);
   BOOST_REQUIRE(submitted_trx);
   BOOST_CHECK(submitted_trx->id() == trx.id());
   BOOST_CHECK_EQUAL(conn->trx_in_progress_size.load(), limit);
   BOOST_CHECK_EQUAL(buffer.bytes_to_read(), 0u);
}

BOOST_AUTO_TEST_CASE(transaction_one_byte_over_remaining_budget_skips_submission) {
   const auto trx = make_packed_trx();
   const auto trx_size = static_cast<uint32_t>(trx.get_estimated_size());
   const auto limit = net_utils::def_max_trx_in_progress_size;
   conn->trx_in_progress_size = limit - trx_size + 1;
   const auto wire = fc::raw::pack(trx);
   fc::datastream<const char*> stream(wire.data(), wire.size());
   fc::bounded_datastream frame(stream, wire.size());
   receive(frame, trx.id());
   BOOST_CHECK_EQUAL(drops, 1u);
   BOOST_REQUIRE(dropped_trx);
   BOOST_CHECK(dropped_trx->id() == trx.id());
   BOOST_CHECK_EQUAL(submissions, 0u);
   BOOST_CHECK_EQUAL(conn->trx_in_progress_size.load(), limit - trx_size + 1);
}

BOOST_AUTO_TEST_CASE(submission_failure_releases_the_transaction_charge) {
   const auto trx_size = static_cast<uint32_t>(make_packed_trx().get_estimated_size());
   conn->trx_in_progress_size = trx_size;
   const auto submit = [&](next_function<transaction_trace_ptr>) {
      BOOST_CHECK_EQUAL(conn->trx_in_progress_size.load(), 2 * trx_size);
      throw std::runtime_error("submission failure");
   };
   BOOST_CHECK_THROW(submit(net_utils::track_trx_in_progress(*conn, trx_size, [](const auto&) {})), std::runtime_error);
   BOOST_CHECK_EQUAL(conn->trx_in_progress_size.load(), trx_size);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(net_message_session_gate)

/// Verifies that only handshake/auth control traffic is accepted before session establishment.
BOOST_AUTO_TEST_CASE(pre_session_control_messages_are_limited) {
   BOOST_CHECK( is_pre_session_control_message( msg_type_t::handshake_message ) );
   BOOST_CHECK( is_pre_session_control_message( msg_type_t::go_away_message ) );
   BOOST_CHECK( is_pre_session_control_message( msg_type_t::time_message ) );
   BOOST_CHECK( is_pre_session_control_message( msg_type_t::peer_auth_message ) );
}

/// Verifies that block-sync and broadcast traffic waits for handshake/auth completion.
BOOST_AUTO_TEST_CASE(pre_session_rejects_expensive_messages) {
   const msg_type_t rejected_messages[] = {
      msg_type_t::peer_status_notice,
      msg_type_t::block_request_message,
      msg_type_t::sync_request_message,
      msg_type_t::signed_block,
      msg_type_t::transaction_message,
      msg_type_t::vote_message,
      msg_type_t::block_nack_message,
      msg_type_t::block_nack_request_message,
      msg_type_t::block_notice_message,
      msg_type_t::gossip_bp_peers_message,
      msg_type_t::transaction_notice_message,
      msg_type_t::unknown,
   };

   for( const auto msg : rejected_messages ) {
      BOOST_CHECK( !is_pre_session_control_message( msg ) );
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(vote_msg_wire)

// Verify vote_message wire format: [which][vote_id (32 bytes)][vote_message ...]
// and that the ID can be peeked from the wire bytes without full deserialization.
BOOST_AUTO_TEST_CASE(test_vote_message_wire_roundtrip) {
   auto vm = make_vote_message();
   const auto expected_vid = compute_vote_id(vm);

   constexpr uint32_t vote_which = to_index(msg_type_t::vote_message);

   // Build wire bytes: [which][vote_id][vote_message]
   const uint32_t which_size = fc::raw::pack_size( unsigned_int( vote_which ) );
   const uint32_t id_size = fc::raw::pack_size( expected_vid );
   const uint32_t msg_size = fc::raw::pack_size( vm );
   const uint32_t total = which_size + id_size + msg_size;

   std::vector<char> wire(total);
   fc::datastream<char*> out_ds( wire.data(), wire.size() );
   fc::raw::pack( out_ds, unsigned_int( vote_which ) );
   fc::raw::pack( out_ds, expected_vid );
   fc::raw::pack( out_ds, vm );

   // Peek: read which + vote_id without consuming the full payload.
   fc::datastream<const char*> peek_ds( wire.data(), wire.size() );
   unsigned_int which{};
   fc::raw::unpack( peek_ds, which );
   BOOST_CHECK_EQUAL( which.value, vote_which );

   vote_id_type peeked_vid;
   fc::raw::unpack( peek_ds, peeked_vid );
   BOOST_CHECK( peeked_vid == expected_vid );

   // Full unpack of the remaining vote_message.
   vote_message unpacked_vm;
   fc::raw::unpack( peek_ds, unpacked_vm );
   BOOST_CHECK( unpacked_vm.block_id == vm.block_id );
   BOOST_CHECK_EQUAL( unpacked_vm.strong, vm.strong );
   BOOST_CHECK( compute_vote_id(unpacked_vm) == expected_vid );
   BOOST_CHECK_EQUAL( peek_ds.remaining(), 0u );
}

// Verify compute_vote_id is deterministic and varies with inputs.
BOOST_AUTO_TEST_CASE(test_vote_id_determinism) {
   auto vm1 = make_vote_message();
   auto vm2 = make_vote_message(); // different key

   // Same message => same ID.
   BOOST_CHECK( compute_vote_id(vm1) == compute_vote_id(vm1) );

   // Different finalizer_key => different ID.
   BOOST_CHECK( compute_vote_id(vm1) != compute_vote_id(vm2) );

   // Different strong flag => different ID.
   auto vm1_weak = vm1;
   vm1_weak.strong = !vm1.strong;
   BOOST_CHECK( compute_vote_id(vm1) != compute_vote_id(vm1_weak) );

   // Different block_id => different ID.
   auto vm1_diff_block = vm1;
   vm1_diff_block.block_id = block_id_type( fc::sha256::hash("other_block") );
   BOOST_CHECK( compute_vote_id(vm1) != compute_vote_id(vm1_diff_block) );

   // Different signature, same (block_id, strong, key) => same ID (sig excluded).
   auto vm1_diff_sig = vm1;
   auto other_priv = fc::crypto::bls::private_key::generate();
   vm1_diff_sig.sig = other_priv.sign_sha256( fc::sha256::hash("different") );
   BOOST_CHECK( compute_vote_id(vm1) == compute_vote_id(vm1_diff_sig) );
}

// Verify vote_buffer_factory produces a send buffer whose payload matches the wire format.
BOOST_AUTO_TEST_CASE(test_vote_buffer_factory_format) {
   auto vm = make_vote_message();
   const auto expected_vid = compute_vote_id(vm);

   vote_buffer_factory factory;
   const auto& sb = factory.get_send_buffer( vm );

   // Send buffer layout: [4-byte size header][payload...]
   BOOST_REQUIRE( sb->size() > message_header_size );

   // Read the size header.
   uint32_t payload_size = 0;
   memcpy( &payload_size, sb->data(), sizeof(payload_size) );
   BOOST_CHECK_EQUAL( payload_size, sb->size() - message_header_size );

   // Parse the payload: [which][vote_id][vote_message]
   fc::datastream<const char*> ds( sb->data() + message_header_size, payload_size );
   unsigned_int which{};
   fc::raw::unpack( ds, which );
   BOOST_CHECK_EQUAL( which.value, to_index(msg_type_t::vote_message) );

   vote_id_type wire_vid;
   fc::raw::unpack( ds, wire_vid );
   BOOST_CHECK( wire_vid == expected_vid );

   vote_message unpacked_vm;
   fc::raw::unpack( ds, unpacked_vm );
   BOOST_CHECK( compute_vote_id(unpacked_vm) == expected_vid );
   BOOST_CHECK_EQUAL( ds.remaining(), 0u );
}

BOOST_AUTO_TEST_SUITE_END()
