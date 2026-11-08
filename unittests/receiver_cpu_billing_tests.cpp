#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/merkle.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>

#include <numeric>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;

namespace {

constexpr account_name token_account   = "sysio.token"_n;
constexpr account_name msig_account    = "sysio.msig"_n;
constexpr account_name system_receiver = "sysio.cpurcv"_n; ///< cpu_receiver_test on a privileged sysio account
constexpr account_name alice           = "alice"_n;        ///< plain account that issues the token
constexpr account_name bob             = "bob"_n;          ///< plain account
constexpr account_name carol           = "carol"_n;        ///< plain account given no CPU and no NET
constexpr account_name game            = "game"_n;         ///< contracts running cpu_receiver_test
constexpr account_name winner          = "winner"_n;
constexpr account_name relay           = "relay"_n;
constexpr account_name idle            = "idle"_n;         ///< contract given no CPU

constexpr const char* max_supply = "1000000.0000 TKN";
constexpr const char* issued     = "2000.0000 TKN";
constexpr const char* funding    = "1000.0000 TKN"; ///< what the game starts with
constexpr const char* amount     = "1.0000 TKN";
constexpr const char* spin_memo  = "spin:20000";
constexpr uint32_t    spin_count = 20000;
constexpr int64_t     cpu_weight = 10'000'000'000; ///< ample CPU for the accounts that run or pay for code
constexpr uint32_t    proposal_expiration_sec  = 3600;
constexpr uint32_t    high_min_trx_cpu_us      = 20'000;
constexpr uint8_t     small_max_cpu_usage_ms   = 5;
constexpr const char* receipt_has_extra_entry  = "is more than the expected";
constexpr const char* receipt_mismatch         = "receipt does not match";
constexpr const char* account_cpu_limit_reached = "reached account cpu limit";
constexpr const char* spun                     = "spun"; ///< what cpu_receiver_test::spin prints
constexpr int64_t     beyond_any_leeway_us     = 10'000'000; ///< more than the subjective CPU leeway
constexpr uint32_t    pending_bill_lifetime_sec = 3600;

using usage_window = resource_limits::usage_accumulator;

bool same_window( const usage_window& a, const usage_window& b ) {
   return a.last_ordinal == b.last_ordinal && a.value_ex == b.value_ex && a.consumed == b.consumed;
}

uint64_t total( const std::vector<uint32_t>& entries ) {
   return std::accumulate( entries.begin(), entries.end(), uint64_t{ 0 } );
}

/// Contracts reached through system contracts pay CPU for their own code; see transaction_context::begin_frame.
struct receiver_cpu_tester : validating_tester {
   receiver_cpu_tester() {
      create_accounts( { token_account, msig_account, system_receiver, alice, bob, carol, game, winner, relay, idle } );
      produce_block();

      set_code( token_account, test_contracts::sysio_token_wasm() );
      set_abi( token_account, test_contracts::sysio_token_abi() );
      set_privileged( token_account );
      set_code( msig_account, test_contracts::sysio_msig_wasm() );
      set_abi( msig_account, test_contracts::sysio_msig_abi() );
      set_privileged( msig_account );
      for( auto contract : { system_receiver, game, winner, relay, idle } ) {
         set_code( contract, test_contracts::cpu_receiver_test_wasm() );
         set_abi( contract, test_contracts::cpu_receiver_test_abi() );
      }
      set_privileged( system_receiver );
      for( auto account : { alice, game, winner, relay } )
         set_cpu_weight( account, cpu_weight );
      set_cpu_weight( idle, 0 );
      produce_block();

      push_action( token_account, "create"_n, token_account,
                   mvo()( "issuer", alice )( "maximum_supply", asset::from_string( max_supply ) ) );
      push_action( token_account, "issue"_n, alice,
                   mvo()( "to", alice )( "quantity", asset::from_string( issued ) )( "memo", "" ) );
      push_action( token_account, "transfer"_n, alice,
                   mvo()( "from", alice )( "to", game )( "quantity", asset::from_string( funding ) )( "memo", "" ) );
      produce_block();
   }

   /// Sets `account`'s NET and CPU weights through sysio.bios, keeping its RAM.
   void set_weights( account_name account, int64_t net_weight, int64_t cpu_weight ) {
      int64_t ram_bytes = 0, current_net_weight = 0, current_cpu_weight = 0;
      control->get_resource_limits_manager().get_account_limits( account, ram_bytes, current_net_weight,
                                                                 current_cpu_weight );
      push_action( config::system_account_name, "setalimits"_n, config::system_account_name,
                   mvo()( "account", account )( "ram_bytes", ram_bytes )( "net_weight", net_weight )
                        ( "cpu_weight", cpu_weight ) );
   }

   /// Sets `account`'s CPU weight, keeping its RAM and NET.
   void set_cpu_weight( account_name account, int64_t weight ) {
      int64_t ram_bytes = 0, net_weight = 0, current_cpu_weight = 0;
      control->get_resource_limits_manager().get_account_limits( account, ram_bytes, net_weight, current_cpu_weight );
      set_weights( account, net_weight, weight );
   }

   /// Authorizes `actor`, naming it the explicit payer too when asked.
   static vector<permission_level> auth( account_name actor, bool explicit_payer = false ) {
      if( explicit_payer )
         return { { actor, config::sysio_payer_name }, { actor, config::active_name } };
      return { { actor, config::active_name } };
   }

   /// Replaces the pending block with a speculative one, as a node that is not producing applies transactions.
   void start_speculative_block() {
      control->abort_block();
      control->start_block( control->head().block_time() + fc::microseconds( config::block_interval_us ), {},
                            controller::block_status::ephemeral );
   }

   signed_transaction make_trx( vector<action> actions ) {
      signed_transaction trx;
      trx.actions = std::move( actions );
      set_transaction_headers( trx );
      sign( trx );
      return trx;
   }

   void sign( signed_transaction& trx ) {
      flat_set<account_name> signers;
      for( const auto& act : trx.actions )
         for( const auto& level : act.authorization )
            if( level.permission != config::sysio_payer_name )
               signers.insert( level.actor );
      trx.signatures.clear();
      for( auto signer : signers )
         trx.sign( get_private_key( signer, "active" ), control->get_chain_id() );
   }

   action transfer_action( account_name from, account_name to, const char* quantity, const std::string& memo,
                           bool explicit_payer = false ) {
      return get_action( token_account, "transfer"_n, auth( from, explicit_payer ),
                         mvo()( "from", from )( "to", to )( "quantity", asset::from_string( quantity ) )
                              ( "memo", memo ) );
   }

   /// Pushes with CPU measured as a producer measures it, so the receipt is built here rather than given.
   transaction_trace_ptr push_measured( signed_transaction trx, fc::time_point deadline = fc::time_point::maximum() ) {
      return push_transaction( trx, deadline, 0 );
   }

   transaction_trace_ptr transfer( account_name from, account_name to, const char* quantity, const std::string& memo,
                                   bool explicit_payer = false ) {
      return push_measured( make_trx( { transfer_action( from, to, quantity, memo, explicit_payer ) } ) );
   }

   const resource_limits::resource_object& usage( const controller& c, account_name account ) const {
      return c.db().get<resource_limits::resource_object, resource_limits::by_owner>( account );
   }

   static std::vector<uint32_t> entries( const transaction_trace_ptr& trace ) {
      BOOST_REQUIRE( trace->receipt );
      std::vector<uint32_t> result;
      for( const auto& e : trace->receipt->cpu_usage_us )
         result.push_back( e.value );
      return result;
   }

   /// The trace of the `occurrence`-th frame that ran `receiver` for `act_name`.
   static const action_trace& frame( const transaction_trace_ptr& trace, account_name receiver, action_name act_name,
                                     size_t occurrence = 0 ) {
      for( const auto& at : trace->action_traces )
         if( at.receiver == receiver && at.act.name == act_name && occurrence-- == 0 )
            return at;
      BOOST_FAIL( "no frame " << occurrence << " for " << receiver << " " << act_name );
      return trace->action_traces.front();
   }

   /// Each account's CPU window, to compare after a transaction.
   std::map<account_name, usage_window> cpu_windows( std::initializer_list<account_name> accounts ) const {
      std::map<account_name, usage_window> result;
      for( auto a : accounts )
         result.emplace( a, usage( *control, a ).cpu_usage );
      return result;
   }

   /// Billed accounts' CPU windows moved to the pending block; the others did not move.
   void check_billed( const std::map<account_name, usage_window>& before, const flat_set<account_name>& billed ) const {
      const uint32_t slot = control->pending_block_timestamp().slot;
      for( const auto& [account, w] : before ) {
         const auto& now = usage( *control, account ).cpu_usage;
         BOOST_TEST_CONTEXT( account ) {
            if( billed.contains( account ) ) {
               BOOST_TEST( now.last_ordinal == slot );
               BOOST_TEST( !same_window( now, w ) );
            } else {
               BOOST_TEST( same_window( now, w ) );
            }
         }
      }
   }

   /// After the validator applies the block, each account's CPU and NET windows match the producer's.
   void produce_and_check_validator( std::initializer_list<account_name> accounts ) {
      produce_block();
      for( auto a : accounts ) {
         const auto& p = usage( *control, a );
         const auto& v = usage( *validating_node, a );
         BOOST_TEST_CONTEXT( a << " on the validator" ) {
            BOOST_TEST( same_window( p.cpu_usage, v.cpu_usage ) );
            BOOST_TEST( same_window( p.net_usage, v.net_usage ) );
         }
      }
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(receiver_cpu_billing_tests)

BOOST_FIXTURE_TEST_CASE( transfer_between_plain_accounts_adds_no_entry, receiver_cpu_tester ) try {
   auto before = cpu_windows( { token_account, alice, bob } );
   auto trace  = transfer( alice, bob, amount, spin_memo );

   BOOST_TEST( entries( trace ).size() == 1u );
   BOOST_TEST( !frame( trace, bob, "transfer"_n ).cpu_usage_us );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, alice, bob } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( contract_pays_for_its_transfer_handler, receiver_cpu_tester ) try {
   auto before     = cpu_windows( { token_account, alice, game } );
   auto game_net   = usage( *control, game ).net_usage;
   auto trace      = transfer( alice, game, amount, spin_memo );

   const auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( trace->action_traces[0].cpu_usage_us->value == receipt[0] );
   const auto& handler = frame( trace, game, "transfer"_n );
   BOOST_REQUIRE( handler.cpu_usage_us );
   BOOST_TEST( handler.cpu_usage_us->value == receipt[1] );
   BOOST_TEST( receipt[1] > 0u );
   BOOST_TEST( trace->total_cpu_usage_us.value == total( receipt ) );
   BOOST_TEST( !frame( trace, alice, "transfer"_n ).cpu_usage_us ); // no code, so no entry
   check_billed( before, { token_account, game } );
   BOOST_TEST( same_window( usage( *control, game ).net_usage, game_net ) ); // NET stays with the input payer
   produce_and_check_validator( { token_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( authorizer_without_cpu_or_net_is_billed_nothing, receiver_cpu_tester ) try {
   transfer( alice, carol, amount, "" );
   set_weights( carol, 0, 0 );
   produce_block();
   int64_t ram_bytes = 0, net_weight = -1, cpu_weight = -1;
   control->get_resource_limits_manager().get_account_limits( carol, ram_bytes, net_weight, cpu_weight );
   BOOST_REQUIRE_EQUAL( net_weight, 0 );
   BOOST_REQUIRE_EQUAL( cpu_weight, 0 );

   // sysio.token pays for its transfer, CPU and NET, and the game for its handler, CPU only.
   auto before    = cpu_windows( { token_account, carol, game } );
   auto carol_net = usage( *control, carol ).net_usage;
   auto token_net = usage( *control, token_account ).net_usage;
   auto trace     = transfer( carol, game, amount, spin_memo );
   BOOST_TEST( entries( trace ).size() == 2u );
   check_billed( before, { token_account, game } );
   BOOST_TEST( same_window( usage( *control, carol ).net_usage, carol_net ) );
   BOOST_TEST( !same_window( usage( *control, token_account ).net_usage, token_net ) );
   produce_and_check_validator( { token_account, carol, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( contract_without_cpu_receives_only_from_an_explicit_payer, receiver_cpu_tester ) try {
   BOOST_CHECK_EXCEPTION( transfer( alice, idle, amount, spin_memo ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( "billable CPU time for the account idle" ) );

   auto before = cpu_windows( { token_account, alice, idle } );
   auto trace  = transfer( alice, idle, amount, spin_memo, true );
   BOOST_TEST( entries( trace ).size() == 1u );
   check_billed( before, { alice } );
   produce_and_check_validator( { token_account, alice, idle } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_over_its_limit_fails_on_account_cpu_limit, receiver_cpu_tester ) try {
   // A real deadline: the handler runs until idle's remaining CPU, which is tighter than the transaction's limit.
   const auto deadline = fc::time_point::now() + fc::seconds( 10 );
   BOOST_CHECK_EXCEPTION( push_measured( make_trx( { transfer_action( alice, idle, amount, "forever" ) } ), deadline ),
                          tx_cpu_usage_exceeded, fc_exception_message_contains( "reached account cpu limit" ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( game_pays_for_its_payout_and_the_winner_for_its_handler, receiver_cpu_tester ) try {
   // Called directly, the game is the input payer, so its transfer stays on the input entry.
   auto before = cpu_windows( { token_account, game, winner } );
   auto payout = get_action( game, "payout"_n, auth( game ),
                             mvo()( "to", winner )( "quantity", asset::from_string( amount ) )( "memo", spin_memo ) );
   auto trace  = push_measured( make_trx( { payout } ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( frame( trace, winner, "transfer"_n ).cpu_usage_us->value == receipt[1] );
   BOOST_TEST( !frame( trace, game, "transfer"_n ).cpu_usage_us ); // billed to the game already
   BOOST_TEST( !frame( trace, token_account, "transfer"_n ).cpu_usage_us );
   check_billed( before, { game, winner } );
   produce_and_check_validator( { token_account, game, winner } );

   // From the game's transfer handler: the game pays for its handler and its transfer, the winner for its handler.
   before  = cpu_windows( { token_account, alice, game, winner } );
   trace   = transfer( alice, game, amount, "pay:winner" );
   receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
   BOOST_TEST( trace->action_traces[0].cpu_usage_us->value == receipt[0] );
   BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == receipt[1] );
   BOOST_TEST( !frame( trace, game, "transfer"_n, 1 ).cpu_usage_us ); // notified of its own transfer
   BOOST_TEST( frame( trace, winner, "transfer"_n ).cpu_usage_us->value == receipt[2] );
   check_billed( before, { token_account, game, winner } );
   produce_and_check_validator( { token_account, alice, game, winner } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( forwarded_notification_bills_the_forwarder, receiver_cpu_tester ) try {
   auto before = cpu_windows( { token_account, relay, winner } );
   auto trace  = transfer( alice, relay, amount, "forward:winner" );

   BOOST_TEST( entries( trace ).size() == 2u );
   BOOST_TEST( frame( trace, relay, "transfer"_n ).cpu_usage_us.has_value() );
   BOOST_TEST( !frame( trace, winner, "transfer"_n ).cpu_usage_us );
   check_billed( before, { token_account, relay } );
   produce_and_check_validator( { token_account, relay, winner } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( explicit_payer_pays_for_everything_below_it, receiver_cpu_tester ) try {
   auto before = cpu_windows( { token_account, alice, game, winner } );
   auto trace  = transfer( alice, game, amount, "pay:winner", true );
   BOOST_TEST( entries( trace ).size() == 1u );
   check_billed( before, { alice } );
   produce_and_check_validator( { token_account, alice, game, winner } );

   // within the transaction's max_cpu_usage_ms
   auto trx = make_trx( { transfer_action( alice, game, amount, "forever", true ) } );
   trx.max_cpu_usage_ms = small_max_cpu_usage_ms;
   sign( trx );
   BOOST_CHECK_EXCEPTION( push_measured( trx, fc::time_point::now() + fc::seconds( 10 ) ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( "reached trx specified max_cpu_usage_ms" ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( msig_exec_bills_the_called_contract, receiver_cpu_tester ) try {
   transaction proposed;
   proposed.actions.emplace_back( get_action( game, "spin"_n, auth( game ), mvo()( "n", spin_count ) ) );
   set_transaction_headers( proposed, proposal_expiration_sec );
   push_action( msig_account, "propose"_n, alice,
                mvo()( "proposer", alice )( "proposal_name", "spin" )( "trx", proposed )
                     ( "requested", vector<permission_level>{ { game, config::active_name } } ) );
   push_action( msig_account, "approve"_n, game,
                mvo()( "proposer", alice )( "proposal_name", "spin" )
                     ( "level", permission_level{ game, config::active_name } ) );
   produce_block();

   auto before = cpu_windows( { msig_account, alice, game } );
   auto trace  = push_measured( make_trx( { get_action( msig_account, "exec"_n, auth( alice ),
                                                        mvo()( "proposer", alice )( "proposal_name", "spin" )
                                                             ( "executer", alice ) ) } ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( frame( trace, game, "spin"_n ).cpu_usage_us->value == receipt[1] );
   check_billed( before, { msig_account, game } );
   produce_and_check_validator( { msig_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( overhead_goes_on_input_entries_only, receiver_cpu_tester ) try {
   // A high minimum puts most of the transaction's CPU outside any action.
   auto cfg = control->get_global_properties().configuration;
   cfg.min_transaction_cpu_usage = high_min_trx_cpu_us;
   // measured, since the new minimum applies to this transaction already
   push_measured( make_trx( { get_action( config::system_account_name, "setparams"_n,
                                          auth( config::system_account_name ), mvo()( "params", cfg ) ) } ) );
   produce_block();

   auto trace   = transfer( alice, game, amount, "spin:10" );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( total( receipt ) >= high_min_trx_cpu_us );
   // the receiver entry is its frame's run alone, which the frame's elapsed covers
   BOOST_TEST( receipt[1] <= static_cast<uint64_t>( frame( trace, game, "transfer"_n ).elapsed.count() ) );
   BOOST_TEST( receipt[0] > receipt[1] );
   produce_and_check_validator( { token_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( subjective_billing_goes_to_the_first_authorizer, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();

   // A producer bills its own block's transactions objectively only.
   sub_bill.reset();
   sub_bill.disable_payer_billing( false );
   transfer( alice, game, amount, "pay:winner" );
   for( auto account : { alice, token_account, game, winner } )
      BOOST_TEST_CONTEXT( account ) {
         BOOST_TEST( sub_bill.get_subjective_bill( account, control->pending_block_time() ).count() == 0 );
      }
   produce_block();

   // Succeeding on a speculative block, the transaction is billed subjectively until a block includes it.
   for( bool payer_billing_disabled : { true, false } ) {
      BOOST_TEST_CONTEXT( "payer billing disabled " << payer_billing_disabled ) {
         sub_bill.reset();
         sub_bill.disable_payer_billing( payer_billing_disabled );
         start_speculative_block();
         const auto now = control->pending_block_time();
         auto trx     = make_trx( { transfer_action( alice, game, payer_billing_disabled ? "1.0000 TKN" : "2.0000 TKN",
                                                     "pay:winner" ) } );
         auto trace   = push_measured( trx );
         auto receipt = entries( trace );
         BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
         BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == trace->total_cpu_usage_us.value );
         // With payer billing, a contract billed for its own code is billed like any payer; the game is also the
         // inline transfer's authorizer, which bills nothing.
         const int64_t token_bill  = payer_billing_disabled ? 0 : receipt[0];
         const int64_t game_bill   = payer_billing_disabled ? 0 : receipt[1];
         const int64_t winner_bill = payer_billing_disabled ? 0 : receipt[2];
         BOOST_TEST( sub_bill.get_subjective_bill( token_account, now ).count() == token_bill );
         BOOST_TEST( sub_bill.get_subjective_bill( game, now ).count() == game_bill );
         BOOST_TEST( sub_bill.get_subjective_bill( winner, now ).count() == winner_bill );
         control->abort_block();

         // The block that includes the transaction ends all of its subjective bills.
         push_measured( trx );
         sub_bill.on_block( fc::logger::default_logger(), produce_block(), now );
         for( auto account : { alice, token_account, game, winner } )
            BOOST_TEST_CONTEXT( account ) { BOOST_TEST( sub_bill.get_subjective_bill( account, now ).count() == 0 ); }
      }
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_that_authorized_the_input_is_billed_once, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();

   // The game sends its own tokens, so sysio.token notifies it: it authorizes the input and pays for its handler.
   for( bool payer_billing_disabled : { true, false } ) {
      BOOST_TEST_CONTEXT( "payer billing disabled " << payer_billing_disabled ) {
         sub_bill.reset();
         sub_bill.disable_payer_billing( payer_billing_disabled );
         start_speculative_block();
         const auto now = control->pending_block_time();
         auto trace   = transfer( game, bob, payer_billing_disabled ? "1.0000 TKN" : "2.0000 TKN", "" );
         auto receipt = entries( trace );
         BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
         BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == receipt[1] );
         BOOST_TEST( sub_bill.get_subjective_bill( game, now ).count() == trace->total_cpu_usage_us.value );
         BOOST_TEST( sub_bill.get_subjective_bill( token_account, now ).count() ==
                     ( payer_billing_disabled ? 0 : receipt[0] ) );
         control->abort_block();
      }
   }
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( failed_transaction_bills_no_receiver, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();

   // Failing, the transaction keeps all of its CPU on the input actions, and no receiver is billed.
   for( bool payer_billing_disabled : { true, false } ) {
      BOOST_TEST_CONTEXT( "payer billing disabled " << payer_billing_disabled ) {
         sub_bill.reset();
         sub_bill.disable_payer_billing( payer_billing_disabled );
         auto trx = make_trx( { transfer_action( alice, game, payer_billing_disabled ? "1.0000 TKN" : "2.0000 TKN",
                                                 "pay:winner" ),
                                transfer_action( alice, bob, max_supply, "" ) } ); // more than alice holds
         BOOST_CHECK_THROW( push_measured( trx ), sysio_assert_message_exception );
         const auto now = control->pending_block_time();
         BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() > 0 );
         BOOST_TEST( ( sub_bill.get_subjective_bill( token_account, now ).count() > 0 ) == !payer_billing_disabled );
         BOOST_TEST( sub_bill.get_subjective_bill( game, now ).count() == 0 );
         BOOST_TEST( sub_bill.get_subjective_bill( winner, now ).count() == 0 );
      }
   }
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_subjective_bill_limits_its_frame, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();
   sub_bill.reset();
   transfer( alice, bob, amount, "" );
   const auto now = control->pending_block_time();
   const auto [game_cpu_us, greylisted, unlimited] = control->get_cpu_limit( game, block_timestamp_type( now ) );
   BOOST_REQUIRE( !unlimited );

   // CPU pending for the game from another transaction leaves it none, leeway included.
   const auto pending_id = transaction_id_type::hash( std::string( "pending" ) );
   sub_bill.subjective_bill( pending_id, fc::time_point_sec( now + fc::seconds( pending_bill_lifetime_sec ) ), {},
                             { { game, fc::microseconds( game_cpu_us + beyond_any_leeway_us ) } } );
   const auto deadline = fc::time_point::now() + fc::seconds( 10 );
   auto trx = make_trx( { transfer_action( alice, game, amount, spin_memo ) } );
   BOOST_CHECK_EXCEPTION( push_measured( trx, deadline ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( account_cpu_limit_reached ) );
   transfer( alice, bob, amount, "spin:1" ); // reaches no contract

   sub_bill.remove_subjective_billing( pending_id, 0 );
   BOOST_TEST( entries( push_measured( trx, deadline ) ).size() == 2u );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( read_only_transaction_skips_receiver_limits, receiver_cpu_tester ) try {
   auto before = cpu_windows( { system_receiver, idle } );
   signed_transaction trx;
   trx.actions.emplace_back( vector<permission_level>{}, system_receiver, "notify"_n, fc::raw::pack( idle ) );
   set_transaction_headers( trx );
   // idle has no CPU, but a read-only transaction is never billed or limited
   auto trace = push_transaction( trx, fc::time_point::now() + fc::seconds( 10 ), 0, false,
                                  transaction_metadata::trx_type::read_only );
   BOOST_TEST( entries( trace ).size() == 2u );
   BOOST_TEST( frame( trace, idle, "notify"_n ).cpu_usage_us.has_value() );
   check_billed( before, {} );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( explicit_receipt_entries_follow_the_billing_switches, receiver_cpu_tester ) try {
   // An explicitly billed push reads its receipt the way a validator does.
   auto push_explicit = [&]( const signed_transaction& trx, cpu_usage_t billed ) {
      auto ptrx  = std::make_shared<packed_transaction>( trx );
      auto meta  = transaction_metadata::start_recover_keys( ptrx, control->get_thread_pool(), control->get_chain_id(),
                                                             fc::microseconds::maximum(),
                                                             transaction_metadata::trx_type::input ).get();
      auto trace = control->test_push_transaction( meta, fc::time_point::maximum(), fc::microseconds::maximum(),
                                                   billed, true );
      if( trace->except_ptr )
         std::rethrow_exception( trace->except_ptr );
      return trace;
   };

   auto switching = make_trx( { transfer_action( alice, game, amount, spin_memo ) } );
   BOOST_CHECK_EXCEPTION( push_explicit( switching, { 500, 500, 500 } ), transaction_exception,
                          fc_exception_message_contains( receipt_has_extra_entry ) );

   auto plain = make_trx( { transfer_action( alice, bob, amount, "" ) } );
   BOOST_CHECK_EXCEPTION( push_explicit( plain, { 500, 500 } ), transaction_exception,
                          fc_exception_message_contains( receipt_has_extra_entry ) );
   BOOST_CHECK_EXCEPTION( push_explicit( plain, {} ), transaction_exception,
                          fc_exception_message_contains( "is less than the action count" ) );

   auto before = cpu_windows( { token_account, game } );
   auto trace  = push_explicit( switching, { 500, 700 } );
   BOOST_TEST( entries( trace ) == std::vector<uint32_t>( { 500, 700 } ), boost::test_tools::per_element() );
   BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == 700u );
   check_billed( before, { token_account, game } );
   produce_and_check_validator( { token_account, game } );

   // A receiver entry the receipt leaves out bills nothing, and the receipt built here gets it.
   before = cpu_windows( { token_account, game } );
   trace  = push_explicit( make_trx( { transfer_action( alice, game, amount, "spin:10" ) } ), { 500 } );
   BOOST_TEST( entries( trace ) == std::vector<uint32_t>( { 500, 0 } ), boost::test_tools::per_element() );
   BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == 0u );
   check_billed( before, { token_account, game } );
   produce_and_check_validator( { token_account, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( context_free_receiver_entry_is_the_same_under_light_validation, receiver_cpu_tester ) try {
   auto before = cpu_windows( { system_receiver, game } );
   auto trace  = push_measured( make_trx( { get_action( system_receiver, "cfspin"_n, auth( alice ),
                                                        mvo()( "to", game )( "n", spin_count ) ) } ) );
   const auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   const auto& spin = frame( trace, game, "spin"_n );
   BOOST_TEST( spin.context_free );
   BOOST_TEST( spin.console == spun );
   BOOST_TEST( spin.cpu_usage_us->value == receipt[1] );
   check_billed( before, { system_receiver, game } );
   produce_and_check_validator( { system_receiver, game } );

   // A node that skips context-free actions in this producer's blocks builds the same receipt and windows.
   fc::temp_directory dir;
   auto [cfg, genesis]  = tester::default_config( dir );
   cfg.trusted_producers = { control->head().producer() };
   tester light( cfg, genesis );
   transaction_trace_ptr light_trace;
   auto connection = light.control->applied_transaction().connect(
      [&]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> x ) {
         if( std::get<0>( x )->id == trace->id )
            light_trace = std::get<0>( x );
      } );
   for( uint32_t n = 2; n <= control->head().block_num(); ++n )
      light.push_block( control->fetch_block_by_number( n ) );
   BOOST_REQUIRE( light_trace && light_trace->receipt );
   BOOST_TEST( ( *light_trace->receipt == *trace->receipt ) );
   BOOST_TEST( frame( light_trace, game, "spin"_n ).console.empty() ); // not run
   for( auto a : { system_receiver, game } ) {
      BOOST_TEST_CONTEXT( a << " on the light node" ) {
         BOOST_TEST( same_window( usage( *control, a ).cpu_usage, usage( *light.control, a ).cpu_usage ) );
         BOOST_TEST( same_window( usage( *control, a ).net_usage, usage( *light.control, a ).net_usage ) );
      }
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( block_with_wrong_receipt_entry_count_is_rejected, receiver_cpu_tester ) try {
   transfer( alice, game, amount, spin_memo );
   transfer( alice, bob, amount, "" );
   auto block = produce_block();
   BOOST_REQUIRE_EQUAL( block->transactions.size(), 2u );
   BOOST_REQUIRE_EQUAL( block->transactions[0].cpu_usage_us.size(), 2u );
   BOOST_REQUIRE_EQUAL( block->transactions[1].cpu_usage_us.size(), 1u );

   // `block` with its receipts edited, re-merkled and re-signed.
   auto tamper = [&]( auto&& edit ) {
      auto copy = block->clone();
      edit( copy->transactions );
      deque<digest_type> digests;
      for( const auto& r : copy->transactions )
         digests.emplace_back( r.digest() );
      copy->transaction_mroot   = calculate_merkle( std::move( digests ) );
      copy->producer_signatures = { get_private_key( copy->producer, "active" ).sign( copy->calculate_id() ) };
      return signed_block::create_signed_block( std::move( copy ) );
   };
   // A node at the block before `block` rejects `bad`.
   auto other_node = [&]() {
      auto other = std::make_unique<tester>( setup_policy::none );
      for( uint32_t n = 2; n < block->block_num(); ++n )
         other->push_block( control->fetch_block_by_number( n ) );
      return other;
   };

   // Left out, the receiver entry bills nothing, so the receipt the node builds differs from the block's. Its CPU
   // moves onto the input entry, so the transaction still meets the CPU minimum.
   BOOST_CHECK_EXCEPTION( other_node()->push_block( tamper( []( auto& receipts ) {
                             auto& cpu   = receipts[0].cpu_usage_us;
                             cpu.front() = fc::unsigned_int( cpu.front().value + cpu.back().value );
                             cpu.pop_back(); } ) ),
                          block_validate_exception, fc_exception_message_contains( receipt_mismatch ) );
   BOOST_CHECK_EXCEPTION( other_node()->push_block( tamper( []( auto& receipts ) {
                             receipts[1].cpu_usage_us.push_back( fc::unsigned_int( 1 ) ); } ) ),
                          transaction_exception, fc_exception_message_contains( receipt_has_extra_entry ) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
