#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include <random>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;

using mvo = fc::mutable_variant_object;

namespace {

/// sysio.msig and sysio.wrap reject packed transaction bytes that are not the canonical encoding of what they decode to.
constexpr auto non_canonical_message = "transaction is not canonically serialized";
/// The dispatcher rejects action data that has bytes after the arguments the action reads.
constexpr auto trailing_bytes_message = "action data has bytes left after the arguments";
/// The contract decoder rejects bytes that end before the value it is reading does.
constexpr auto read_past_end_message = "datastream attempted to read past the end";
/// The contract decoder rejects bytes that end before the single byte it is reading.
constexpr auto read_byte_past_end_message = "get";
/// The node's check_transaction_authorization rejects bytes left after the transaction it decodes.
constexpr auto node_trailing_bytes_message = "trailing bytes after the packed argument";

/**
 * True when the contract refused bytes that are not one canonically encoded transaction: its decode ran out of data,
 * or the round-trip comparison failed. Which of the two comes first depends on how the toolchain the contract was
 * built with reads variable-length integers, so tests that must hold for either accept both.
 */
bool is_non_canonical_rejection( const sysio_assert_message_exception& e ) {
   for( const char* message : { non_canonical_message, read_past_end_message, read_byte_past_end_message } ) {
      if( sysio_assert_message_is( message )( e ) ) return true;
   }
   return false;
}

constexpr name msig_account = "sysio.msig"_n;

/// Random byte-level edits to a packed transaction: overwrite, bit flip, insert, insert continuation bytes,
/// erase, append, and set a continuation bit.
std::vector<char> mutate_packed_trx( const std::vector<char>& base, std::mt19937& rng ) {
   constexpr size_t edit_kinds    = 7;
   constexpr size_t max_rounds    = 3;
   constexpr size_t max_run       = 6;
   constexpr size_t max_appended  = 4;
   constexpr size_t byte_values   = 256;
   constexpr size_t bits_per_byte = 8;
   constexpr char   continuation  = '\x80';

   auto out  = base;
   auto pick = [&]( size_t bound ) { return std::uniform_int_distribution<size_t>( 0, bound - 1 )( rng ); };
   const size_t rounds = 1 + pick( max_rounds );
   for( size_t r = 0; r < rounds; ++r ) {
      switch( pick( edit_kinds ) ) {
         case 0: out[pick( out.size() )] = char( pick( byte_values ) ); break;
         case 1: out[pick( out.size() )] ^= char( 1u << pick( bits_per_byte ) ); break;
         case 2: out.insert( out.begin() + pick( out.size() + 1 ), char( pick( byte_values ) ) ); break;
         case 3: out.insert( out.begin() + pick( out.size() + 1 ), 1 + pick( max_run ), continuation ); break;
         case 4: if( out.size() > 1 ) out.erase( out.begin() + pick( out.size() ) ); break;
         case 5: out.insert( out.end(), 1 + pick( max_appended ), char( pick( byte_values ) ) ); break;
         case 6: out[pick( out.size() )] |= continuation; break;
      }
   }
   return out;
}

} // namespace

/**
 * Proposes, approves and executes raw packed transactions through sysio.msig as one account, `packer`.
 *
 * `packer@packer` is linked to `packer::packer` and to `sysio.msig::approve`, so one key can request and grant two
 * distinct permission levels.
 */
class sysio_msig_canonical_tester : public tester {
public:
   static constexpr name packer = "packer"_n;

   sysio_msig_canonical_tester() {
      create_accounts( { msig_account, packer, "alice"_n } );
      produce_block();
      deploy_msig( contracts::msig_wasm(), contracts::msig_abi() );
      set_privileged( msig_account );
      set_authority( packer, packer, authority( get_public_key( packer, "active" ) ), config::active_name );
      link_authority( packer, packer, packer, packer );
      link_authority( packer, msig_account, packer, "approve"_n );
      produce_block();
   }

   void deploy_msig( const std::vector<uint8_t>& wasm, const std::vector<char>& abi ) {
      set_code( msig_account, wasm );
      set_abi( msig_account, abi.data() );
      produce_blocks();
   }

   time_point_sec packed_expiration() const {
      constexpr auto lifetime = fc::seconds( 1000 );
      return time_point_sec( control->pending_block_time() + lifetime );
   }

   /// `num_actions` actions on `packer`, each authorized by `packer@active` and carrying `data_size` bytes.
   transaction packer_trx( size_t num_actions = 1, size_t data_size = 0 ) const {
      transaction trx;
      trx.expiration = packed_expiration();
      for( size_t i = 0; i < num_actions; ++i ) {
         trx.actions.emplace_back( vector<permission_level>{{ packer, config::active_name }}, packer, numbered_name( i ),
                                   bytes( data_size, char( i + 1 ) ) );
      }
      return trx;
   }

   /**
    * Packs `trx` field by field. With `pad_index` set, the variable-length integer at that position is written with
    * `pad` extra continuation bytes that leave its value unchanged. `num_varuints` receives the count of those integers.
    */
   static std::vector<char> pack_trx( const transaction& trx, int pad_index = -1, size_t pad = 0, int* num_varuints = nullptr ) {
      std::vector<char> out;
      int index = 0;
      auto put_raw = [&]( const auto& v ) { auto b = fc::raw::pack( v ); out.insert( out.end(), b.begin(), b.end() ); };
      auto put_varuint = [&]( uint32_t v ) {
         const size_t extra = ( index++ == pad_index ) ? pad : 0;
         do {
            uint8_t b = v & 0x7f;
            v >>= 7;
            if( v || extra ) b |= 0x80;
            out.push_back( char(b) );
         } while( v );
         for( size_t i = 0; i < extra; ++i )
            out.push_back( i + 1 < extra ? '\x80' : '\x00' );
      };
      auto put_actions = [&]( const vector<action>& acts ) {
         put_varuint( acts.size() );
         for( const auto& act : acts ) {
            put_raw( act.account );
            put_raw( act.name );
            put_varuint( act.authorization.size() );
            for( const auto& level : act.authorization ) put_raw( level );
            put_varuint( act.data.size() );
            out.insert( out.end(), act.data.begin(), act.data.end() );
         }
      };
      put_raw( trx.expiration );
      put_raw( trx.ref_block_num );
      put_raw( trx.ref_block_prefix );
      put_varuint( trx.max_net_usage_words.value );
      put_raw( trx.max_cpu_usage_ms );
      put_varuint( trx.delay_sec.value );
      put_actions( trx.context_free_actions );
      put_actions( trx.actions );
      put_varuint( 0 );
      if( num_varuints ) *num_varuints = index;
      return out;
   }

   /// Bytes the node reads as one action, `outer`, while carrying `inner` after it in a non-canonical layout.
   std::vector<char> nested_trx( const action& outer, const action& inner ) const {
      const std::vector<char> zero = { '\x80', '\x80', '\x80', '\x80', '\x80' };
      const std::vector<char> one  = { '\x81', '\x80', '\x80', '\x80', '\x80' };
      std::vector<char> out;
      auto put     = [&]( const std::vector<char>& v ) { out.insert( out.end(), v.begin(), v.end() ); };
      auto put_raw = [&]( const auto& v ) { put( fc::raw::pack( v ) ); };

      put_raw( packed_expiration() );
      put_raw( uint16_t(0) );
      put_raw( uint32_t(0) );
      put( zero );
      out.push_back( '\x80' );
      put( zero );
      put( zero );
      put( one );
      put_raw( outer.account );
      put_raw( outer.name );
      put( one );
      put_raw( outer.authorization.at(0) );
      put( zero );
      put( zero );
      out.insert( out.end(), 4, '\0' );
      put_raw( std::vector<action>{ inner } );
      out.push_back( 0 );
      return out;
   }

   /// `{packer, sysio.payer}, {packer, active}`: packer proposes and pays for the propose transaction.
   static vector<permission_level> payer_auths() {
      return { { packer, config::sysio_payer_name }, { packer, config::active_name } };
   }

   transaction_trace_ptr propose_packed( name proposal_name, const vector<permission_level>& requested,
                                         const std::vector<char>& packed_trx ) {
      bytes data = fc::raw::pack( packer );
      auto append = [&]( const std::vector<char>& v ) { data.insert( data.end(), v.begin(), v.end() ); };
      append( fc::raw::pack( proposal_name ) );
      append( fc::raw::pack( requested ) );
      append( packed_trx );

      signed_transaction trx;
      trx.actions.emplace_back( payer_auths(), msig_account, "propose"_n, std::move(data) );
      set_transaction_headers( trx );
      trx.sign( get_private_key( packer, "active" ), control->get_chain_id() );
      return push_transaction( trx );
   }

   transaction_trace_ptr approve_packed( name proposal_name, const vector<permission_level>& levels ) {
      signed_transaction trx;
      for( const auto& level : levels ) {
         trx.actions.push_back( get_action( msig_account, "approve"_n, { level }, mvo()
                        ("proposer",      packer)
                        ("proposal_name", proposal_name)
                        ("level",         level)
         ) );
      }
      set_transaction_headers( trx );
      trx.sign( get_private_key( packer, "active" ), control->get_chain_id() );
      return push_transaction( trx );
   }

   transaction_trace_ptr exec_packed( name proposal_name ) {
      return base_tester::push_action( msig_account, "exec"_n, packer, mvo()
                     ("proposer",      packer)
                     ("proposal_name", proposal_name)
                     ("executer",      packer)
      );
   }

   transaction_trace_ptr cancel_packed( name proposal_name ) {
      return base_tester::push_action( msig_account, "cancel"_n, packer, mvo()
                     ("proposer",      packer)
                     ("proposal_name", proposal_name)
                     ("canceler",      packer)
      );
   }

   /// True when `packed_trx` is exactly the node's own encoding of what the node decodes from it.
   static bool is_canonical( const std::vector<char>& packed_trx ) {
      try {
         return fc::raw::pack( fc::raw::unpack<transaction>( packed_trx ) ) == packed_trx;
      } catch( const fc::exception& ) {
         return false;
      } catch( const std::exception& ) {
         return false;
      }
   }

   /// The actions exec sent inline equal the actions the node decodes from `packed_trx`, in order.
   static void require_sent_actions( const transaction_trace_ptr& trace, const std::vector<char>& packed_trx ) {
      const auto authorized = fc::raw::unpack<transaction>( packed_trx );
      std::vector<action> sent;
      for( const auto& at : trace->action_traces ) {
         if( at.creator_action_ordinal.value == 1 && at.receiver == at.act.account ) sent.push_back( at.act );
      }
      BOOST_REQUIRE_EQUAL( sent.size(), authorized.actions.size() );
      for( size_t i = 0; i < sent.size(); ++i ) {
         BOOST_REQUIRE_EQUAL( sent[i].account, authorized.actions[i].account );
         BOOST_REQUIRE_EQUAL( sent[i].name, authorized.actions[i].name );
         BOOST_REQUIRE( sent[i].authorization == authorized.actions[i].authorization );
         BOOST_REQUIRE( sent[i].data == authorized.actions[i].data );
      }
   }

   /// A distinct eight-letter name per `n`.
   static name numbered_name( uint64_t n ) {
      static const char charset[] = "abcdefghijklmnopqrstuvwxyz";
      constexpr size_t letters = 26;
      std::string str( 8, 'a' );
      for( size_t i = 0; i < str.size(); ++i, n /= letters ) str[i] = charset[n % letters];
      return name( str );
   }
};

BOOST_AUTO_TEST_SUITE(sysio_msig_canonical_tests)

// The test encoder writes exactly what the node writes when nothing is padded.
BOOST_FIXTURE_TEST_CASE( packer_encoding_matches_node, sysio_msig_canonical_tester ) try {
   for( size_t num_actions : { 0, 1, 3 } ) {
      for( size_t data_size : { 0, 127, 128, 16383, 16384 } ) {
         auto trx = packer_trx( num_actions, data_size );
         trx.max_net_usage_words = 0xffffffffu;
         BOOST_REQUIRE( pack_trx( trx ) == fc::raw::pack( trx ) );
      }
   }
} FC_LOG_AND_RETHROW()

// Canonical encodings at every variable-length integer width are proposed, approved and executed as the node reads them.
BOOST_FIXTURE_TEST_CASE( canonical_proposals_execute, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, config::active_name };
   uint64_t n = 0;
   for( uint32_t max_net : { 0u, 0x7fu, 0x80u, 0x3fffu, 0x4000u, 0x1fffffu, 0x200000u, 0xfffffffu, 0x10000000u, 0xffffffffu } ) {
      for( size_t num_actions : { 1, 3 } ) {
         for( size_t data_size : { 0, 127, 128, 16383, 16384 } ) {
            auto trx = packer_trx( num_actions, data_size );
            trx.max_net_usage_words = max_net;
            trx.ref_block_num = 0x8080;
            trx.ref_block_prefix = 0x80808080u;
            trx.max_cpu_usage_ms = 0x80;
            const auto packed = fc::raw::pack( trx );
            const name proposal_name = numbered_name( n++ );
            propose_packed( proposal_name, { level }, packed );
            approve_packed( proposal_name, { level } );
            require_sent_actions( exec_packed( proposal_name ), packed );
            produce_block();
         }
      }
   }
} FC_LOG_AND_RETHROW()

// A variable-length integer written longer than needed is rejected at every position in the transaction.
BOOST_FIXTURE_TEST_CASE( propose_rejects_padded_integer, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, config::active_name };
   const auto trx = packer_trx( 2, 200 );
   int num_varuints = 0;
   pack_trx( trx, -1, 0, &num_varuints );
   BOOST_REQUIRE_EQUAL( num_varuints, 9 );

   uint64_t n = 0;
   for( int index = 0; index < num_varuints; ++index ) {
      for( size_t pad : { 1, 2, 3, 4, 5, 9, 40 } ) {
         const auto packed = pack_trx( trx, index, pad );
         BOOST_REQUIRE( !is_canonical( packed ) );
         BOOST_REQUIRE_EXCEPTION( propose_packed( numbered_name( n++ ), { level }, packed ),
                                  sysio_assert_message_exception,
                                  is_non_canonical_rejection );
      }
      produce_block();
   }
} FC_LOG_AND_RETHROW()

// Bits above 32 in the last byte of a five-byte integer are rejected.
BOOST_FIXTURE_TEST_CASE( propose_rejects_unused_integer_bits, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, config::active_name };
   auto trx = packer_trx();
   trx.max_net_usage_words = 0xffffffffu;
   auto packed = fc::raw::pack( trx );
   constexpr size_t last_byte_of_max_net = 14;
   BOOST_REQUIRE_EQUAL( uint8_t(packed[last_byte_of_max_net]), 0x0f );

   uint64_t n = 0;
   for( uint8_t last : { 0x1f, 0x2f, 0x4f, 0x7f } ) {
      packed[last_byte_of_max_net] = char(last);
      BOOST_REQUIRE_EXCEPTION( propose_packed( numbered_name( n++ ), { level }, packed ),
                               sysio_assert_message_exception,
                               sysio_assert_message_is( non_canonical_message ) );
   }
   packed[last_byte_of_max_net] = 0x0f;
   propose_packed( numbered_name( n++ ), { level }, packed );
} FC_LOG_AND_RETHROW()

// Bytes after the transaction are rejected, and every truncation fails.
BOOST_FIXTURE_TEST_CASE( propose_rejects_wrong_length, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, config::active_name };
   const auto packed = fc::raw::pack( packer_trx( 2, 50 ) );

   uint64_t n = 0;
   for( size_t extra : { 1, 2, 8, 200 } ) {
      for( char fill : { '\0', '\x01', '\x80', '\xff' } ) {
         auto longer = packed;
         longer.insert( longer.end(), extra, fill );
         BOOST_REQUIRE_EXCEPTION( propose_packed( numbered_name( n++ ), { level }, longer ),
                                  sysio_assert_message_exception,
                                  sysio_assert_message_is( trailing_bytes_message ) );
      }
   }
   for( size_t size = 0; size < packed.size(); ++size ) {
      const std::vector<char> shorter( packed.begin(), packed.begin() + size );
      BOOST_REQUIRE_THROW( propose_packed( numbered_name( n++ ), { level }, shorter ), fc::exception );
      if( size % 20 == 0 ) produce_block();
   }
   propose_packed( numbered_name( n++ ), { level }, packed );
} FC_LOG_AND_RETHROW()

// A layout the node reads as one action on packer, and that is not canonical, is rejected at propose.
BOOST_FIXTURE_TEST_CASE( propose_rejects_nested_layout, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, packer };
   const action outer( vector<permission_level>{ level }, packer, packer, bytes{} );
   const action inner( vector<permission_level>{{ "alice"_n, config::active_name }}, config::system_account_name, "reqauth"_n,
                       fc::raw::pack( "alice"_n ) );
   const auto packed = nested_trx( outer, inner );

   const auto authorized = fc::raw::unpack<transaction>( packed );
   BOOST_REQUIRE_EQUAL( authorized.actions.size(), 1u );
   BOOST_REQUIRE_EQUAL( authorized.actions[0].account, packer );
   BOOST_REQUIRE( authorized.actions[0].authorization == vector<permission_level>{ level } );
   BOOST_REQUIRE( !is_canonical( packed ) );

   BOOST_REQUIRE_EXCEPTION( propose_packed( "nested"_n, { level }, packed ),
                            sysio_assert_message_exception,
                            is_non_canonical_rejection );
} FC_LOG_AND_RETHROW()

// The earlier contract cannot store that layout either: the node's authorization check refuses it at propose.
BOOST_FIXTURE_TEST_CASE( earlier_contract_refuses_nested_layout, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, packer };
   const action outer( vector<permission_level>{ level }, packer, packer, bytes{} );
   const action inner( vector<permission_level>{{ "alice"_n, config::active_name }}, config::system_account_name, "reqauth"_n,
                       fc::raw::pack( "alice"_n ) );

   deploy_msig( contracts::util::msig_wasm_old(), contracts::util::msig_abi_old() );
   BOOST_REQUIRE_EXCEPTION( propose_packed( "nested"_n, { level }, nested_trx( outer, inner ) ),
                            wasm_execution_error,
                            fc_exception_message_contains( node_trailing_bytes_message ) );
} FC_LOG_AND_RETHROW()

// Proposals with a padded integer, stored by the earlier contract at every integer position, cannot be executed after
// the upgrade, and can be cancelled.
BOOST_FIXTURE_TEST_CASE( exec_rejects_stored_padded_integer, sysio_msig_canonical_tester ) try {
   constexpr size_t pad = 1;
   const permission_level level{ packer, config::active_name };
   const auto trx = packer_trx( 2, 200 );
   int num_varuints = 0;
   pack_trx( trx, -1, 0, &num_varuints );

   deploy_msig( contracts::util::msig_wasm_old(), contracts::util::msig_abi_old() );
   for( int index = 0; index < num_varuints; ++index ) {
      const auto packed = pack_trx( trx, index, pad );
      BOOST_REQUIRE( !is_canonical( packed ) );
      propose_packed( numbered_name( index ), { level }, packed );
   }
   produce_block();

   deploy_msig( contracts::msig_wasm(), contracts::msig_abi() );
   for( int index = 0; index < num_varuints; ++index ) {
      const name proposal_name = numbered_name( index );
      approve_packed( proposal_name, { level } );
      BOOST_REQUIRE_EXCEPTION( exec_packed( proposal_name ),
                               sysio_assert_message_exception,
                               sysio_assert_message_is( non_canonical_message ) );
      cancel_packed( proposal_name );
   }
} FC_LOG_AND_RETHROW()

// Proposals at and above the chunked-storage size execute as the node reads them.
BOOST_FIXTURE_TEST_CASE( large_proposals_execute, sysio_msig_canonical_tester ) try {
   const permission_level level{ packer, config::active_name };
   uint64_t n = 0;
   for( size_t data_size : { 1000, 100000, 400000 } ) {
      const auto packed = fc::raw::pack( packer_trx( 1, data_size ) );
      const name proposal_name = numbered_name( n++ );
      const auto propose_trace = propose_packed( proposal_name, { level }, packed );
      approve_packed( proposal_name, { level } );
      const auto exec_trace = exec_packed( proposal_name );
      require_sent_actions( exec_trace, packed );
      BOOST_TEST_MESSAGE( "size " << packed.size() << " propose us " << propose_trace->elapsed.count()
                          << " exec us " << exec_trace->elapsed.count() );
      produce_block();
   }
} FC_LOG_AND_RETHROW()

// Randomly edited transactions: every accepted proposal is canonical and executes as the node reads it.
BOOST_FIXTURE_TEST_CASE( random_edits_propose, sysio_msig_canonical_tester ) try {
   constexpr uint64_t     cases        = 3000;
   constexpr uint64_t     unedited_one_in = 10;
   constexpr uint64_t     block_every  = 25;
   // Floors for this seed. The node and the dispatcher refuse most edits before the canonical check is reached.
   constexpr size_t       min_accepted      = 300;
   constexpr size_t       min_executed      = 300;
   constexpr size_t       min_not_canonical = 50;
   constexpr std::mt19937::result_type seed = 20261110;

   const permission_level active{ packer, config::active_name };
   const vector<permission_level> requested{ active, permission_level{ packer, packer } };

   std::mt19937 rng( seed );
   size_t accepted = 0, executed = 0, not_canonical = 0, trailing = 0, other = 0;
   for( uint64_t n = 0; n < cases; ++n ) {
      const auto base = fc::raw::pack( packer_trx( 1 + n % 3, n % 7 ) );
      const auto packed = ( n % unedited_one_in == 0 ) ? base : mutate_packed_trx( base, rng );
      const name proposal_name = numbered_name( n );
      const bool canonical = is_canonical( packed );

      bool ok = false;
      try {
         propose_packed( proposal_name, requested, packed );
         ok = true;
      } catch( const sysio_assert_message_exception& e ) {
         if( sysio_assert_message_is( non_canonical_message )( e ) ) ++not_canonical;
         else if( sysio_assert_message_is( trailing_bytes_message )( e ) ) ++trailing;
         else ++other;
      } catch( const fc::exception& ) {
         ++other;
      }

      if( ok ) {
         ++accepted;
         BOOST_REQUIRE( canonical );
         approve_packed( proposal_name, requested );
         try {
            require_sent_actions( exec_packed( proposal_name ), packed );
            ++executed;
         } catch( const fc::exception& ) {}
      } else if( canonical ) {
         const auto trx = fc::raw::unpack<transaction>( packed );
         bool expect_accept = trx.expiration >= time_point_sec( control->pending_block_time() )
                              && trx.context_free_actions.empty() && trx.transaction_extensions.empty()
                              && trx.delay_sec.value == 0;
         for( const auto& act : trx.actions )
            expect_accept = expect_accept && act.authorization == vector<permission_level>{ active };
         BOOST_REQUIRE( !expect_accept );
      }
      if( n % block_every == block_every - 1 ) produce_block();
   }
   BOOST_TEST_MESSAGE( "accepted " << accepted << " executed " << executed << " not canonical " << not_canonical
                       << " trailing " << trailing << " other " << other );
   BOOST_REQUIRE_GT( accepted, min_accepted );
   BOOST_REQUIRE_GT( executed, min_executed );
   BOOST_REQUIRE_GT( not_canonical, min_not_canonical );
} FC_LOG_AND_RETHROW()

// Randomly edited transactions stored by the earlier contract: after the upgrade, exactly the canonical ones execute.
BOOST_FIXTURE_TEST_CASE( random_edits_stored_before_upgrade, sysio_msig_canonical_tester ) try {
   constexpr uint64_t     cases           = 1500;
   constexpr uint64_t     unedited_one_in = 10;
   constexpr uint64_t     block_every     = 25;
   constexpr size_t       min_executed    = 100;
   // The node refuses most edited transactions at propose, so few non-canonical rows are stored for this seed;
   // exec_rejects_stored_padded_integer covers that path at every integer position.
   constexpr size_t       min_stored_not_canonical = 1;
   constexpr std::mt19937::result_type seed = 20261111;

   const permission_level active{ packer, config::active_name };
   const vector<permission_level> requested{ active, permission_level{ packer, packer } };

   deploy_msig( contracts::util::msig_wasm_old(), contracts::util::msig_abi_old() );

   std::mt19937 rng( seed );
   std::vector<std::pair<name, std::vector<char>>> stored;
   for( uint64_t n = 0; n < cases; ++n ) {
      const auto base = fc::raw::pack( packer_trx( 1 + n % 3, n % 7 ) );
      const auto packed = ( n % unedited_one_in == 0 ) ? base : mutate_packed_trx( base, rng );
      const name proposal_name = numbered_name( n );
      try {
         propose_packed( proposal_name, requested, packed );
         stored.emplace_back( proposal_name, packed );
      } catch( const fc::exception& ) {}
      if( n % block_every == block_every - 1 ) produce_block();
   }

   deploy_msig( contracts::msig_wasm(), contracts::msig_abi() );

   size_t executed = 0, not_canonical = 0, stored_not_canonical = 0;
   for( size_t i = 0; i < stored.size(); ++i ) {
      const auto& [proposal_name, packed] = stored[i];
      const bool canonical = is_canonical( packed );
      if( !canonical ) ++stored_not_canonical;
      try {
         approve_packed( proposal_name, requested );
         auto trace = exec_packed( proposal_name );
         BOOST_REQUIRE( canonical );
         require_sent_actions( trace, packed );
         ++executed;
      } catch( const sysio_assert_message_exception& e ) {
         const bool rejected_as_non_canonical = sysio_assert_message_is( non_canonical_message )( e );
         if( rejected_as_non_canonical ) ++not_canonical;
         BOOST_REQUIRE( !canonical || !rejected_as_non_canonical );
      } catch( const fc::exception& ) {}
      if( i % block_every == block_every - 1 ) produce_block();
   }
   BOOST_TEST_MESSAGE( "stored " << stored.size() << " stored not canonical " << stored_not_canonical
                       << " executed " << executed << " not canonical " << not_canonical );
   BOOST_REQUIRE_GT( executed, min_executed );
   BOOST_REQUIRE_GE( stored_not_canonical, min_stored_not_canonical );
   BOOST_REQUIRE_EQUAL( not_canonical, stored_not_canonical );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
