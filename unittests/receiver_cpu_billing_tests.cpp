#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/merkle.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/chain/wasm_interface.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/scoped_exit.hpp>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <string>
#include <thread>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;

namespace {

constexpr account_name token_account   = "sysio.token"_n;
constexpr account_name msig_account    = "sysio.msig"_n;
constexpr account_name system_receiver = "sysio.cpurcv"_n; ///< cpu_receiver_test on a privileged sysio account
constexpr account_name unprivileged    = "sysio.unpriv"_n; ///< cpu_receiver_test on a sysio account not privileged
constexpr account_name alice           = "alice"_n;        ///< plain account that issues the token
constexpr account_name bob             = "bob"_n;          ///< plain account
constexpr account_name carol           = "carol"_n;        ///< plain account given no CPU and no NET
constexpr account_name game            = "game"_n;         ///< contracts running cpu_receiver_test
constexpr account_name winner          = "winner"_n;
constexpr account_name relay           = "relay"_n;
constexpr account_name idle            = "idle"_n;         ///< contract given no CPU
constexpr account_name cold            = "coldload"_n;     ///< runs a large module once, in one test
constexpr action_name  setalimits      = "setalimits"_n;   ///< sysio.bios: sets an account's resource limits
constexpr action_name  setparams       = "setparams"_n;    ///< sysio.bios: sets the chain parameters

constexpr const char* max_supply = "1000000.0000 TKN";
constexpr const char* issued     = "2000.0000 TKN";
constexpr const char* funding    = "1000.0000 TKN"; ///< what the game starts with
constexpr const char* amount     = "1.0000 TKN";
constexpr const char* bob_funding = "100.0000 TKN"; ///< what bob sends from in the many-notifications test
const symbol          token_symbol{ 4, "TKN" };
constexpr const char* spin_memo  = "spin:20000";
constexpr uint32_t    spin_count = 20000;
constexpr int64_t     cpu_weight = 10'000'000'000; ///< ample CPU for the accounts that run or pay for code

// A handler either stays inside the stipend or is measured to run well past it.
/// The notify_cpu_stipend the fixture sets: well above a no-op handler on every runtime, so that a frame inside or
/// past it does not depend on the host's speed.
constexpr int64_t     stipend_us         = 1000;
constexpr int64_t     long_frame_us      = 4 * stipend_us; ///< long_spins() keeps a frame busy at least this long
constexpr uint32_t    first_long_spins   = 1000;
constexpr uint32_t    max_spin_doublings = 21; ///< first_long_spins doubled this often still fits in uint32_t
constexpr uint32_t    calibration_runs   = 3;  ///< runs of each loop count long_spins() measures
constexpr uint32_t    short_spin_count   = 10;
constexpr uint32_t    many_notifications = 50;
constexpr auto        frame_pause        = std::chrono::milliseconds( 50 ); ///< a pause inside a frame: 50 stipends
constexpr uint8_t     short_trx_max_cpu_ms = 20; ///< a transaction limit that frame_pause runs past
constexpr uint8_t     paused_trx_max_cpu_ms = 100; ///< the transaction's limit when a pause outlasts the block
constexpr auto        paused_block_time  = std::chrono::milliseconds( 200 ); ///< that push's block deadline
constexpr auto        block_pause        = std::chrono::milliseconds( 400 ); ///< a pause past that block deadline
constexpr const char* spin_command       = "spin:";     ///< cpu_receiver_test memo: loop n times
constexpr const char* spinsend_command   = "spinsend:"; ///< loop n times and send itself an inline spin of n

constexpr uint32_t    proposal_expiration_sec  = 3600;
constexpr uint32_t    high_min_trx_cpu_us      = 20'000;
constexpr uint8_t     small_max_cpu_usage_ms   = 5;
constexpr const char* receipt_has_extra_entry  = "is more than the expected";
constexpr const char* receipt_mismatch         = "receipt does not match";
constexpr const char* account_cpu_limit_reached = "reached account cpu limit";
constexpr const char* idle_over_its_limit      = "billable CPU time for the account idle";
constexpr const char* relay_over_its_limit     = "billable CPU time for the account relay";
constexpr const char* stipend_over_trx_limit   = "notify cpu stipend cannot exceed max transaction cpu usage";
constexpr const char* idle_over_greylisted_limit = "greylisted billable CPU time for the account idle";
constexpr const char* greylisted_frame_stopped = "greylisted transaction";
constexpr const char* trx_max_cpu_reached      = "reached trx specified max_cpu_usage_ms";
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

/// The most a notified contract's entry can hold for frames that ran `elapsed_us` in all: the stipend covers the rest.
/// A frame's CPU is within its elapsed time, so a frame inside the stipend leaves an entry of 0.
uint32_t most_past_stipend( int64_t elapsed_us, int64_t stipend = stipend_us ) {
   return static_cast<uint32_t>( std::max<int64_t>( elapsed_us - stipend, 0 ) );
}

/// Contracts reached through system contracts pay CPU for their own code past the stipend of a notification; see
/// transaction_context::begin_frame.
struct receiver_cpu_tester : validating_tester {
   receiver_cpu_tester() {
      create_accounts(
         { token_account, msig_account, system_receiver, unprivileged, alice, bob, carol, game, winner, relay, idle } );
      produce_block();

      set_code( token_account, test_contracts::sysio_token_wasm() );
      set_abi( token_account, test_contracts::sysio_token_abi() );
      set_privileged( token_account );
      set_code( msig_account, test_contracts::sysio_msig_wasm() );
      set_abi( msig_account, test_contracts::sysio_msig_abi() );
      set_privileged( msig_account );
      for( auto contract : { system_receiver, unprivileged, game, winner, relay, idle } ) {
         set_code( contract, test_contracts::cpu_receiver_test_wasm() );
         set_abi( contract, test_contracts::cpu_receiver_test_abi() );
      }
      set_privileged( system_receiver );
      for( auto account : { alice, game, winner, relay, unprivileged } )
         set_cpu_weight( account, cpu_weight );
      set_cpu_weight( idle, 0 );
      genesis_stipend = control->get_global_properties().configuration.notify_cpu_stipend;
      set_notify_cpu_stipend( stipend_us );
      produce_block();

      push_action( token_account, "create"_n, token_account,
                   mvo()( "issuer", alice )( "maximum_supply", asset::from_string( max_supply ) ) );
      push_action( token_account, "issue"_n, alice,
                   mvo()( "to", alice )( "quantity", asset::from_string( issued ) )( "memo", "" ) );
      push_action( token_account, "transfer"_n, alice,
                   mvo()( "from", alice )( "to", game )( "quantity", asset::from_string( funding ) )( "memo", "" ) );
      produce_block();
   }

   uint32_t genesis_stipend = 0; ///< notify_cpu_stipend at genesis, before the fixture sets stipend_us

   /// Sets chain_config::notify_cpu_stipend through sysio.bios setparams, keeping the other parameters.
   void set_notify_cpu_stipend( uint32_t stipend ) {
      auto cfg = control->get_global_properties().configuration;
      cfg.notify_cpu_stipend = stipend;
      push_action( config::system_account_name, setparams, config::system_account_name, mvo()( "params", cfg ) );
   }

   /// sysio.bios setalimits data giving `account` these NET and CPU weights, keeping its RAM.
   mvo weights( account_name account, int64_t net_weight, int64_t cpu_weight ) const {
      int64_t ram_bytes = 0, current_net_weight = 0, current_cpu_weight = 0;
      control->get_resource_limits_manager().get_account_limits( account, ram_bytes, current_net_weight,
                                                                 current_cpu_weight );
      return mvo()( "account", account )( "ram_bytes", ram_bytes )( "net_weight", net_weight )
                  ( "cpu_weight", cpu_weight );
   }

   int64_t net_weight_of( account_name account ) const {
      int64_t ram_bytes = 0, net_weight = 0, cpu_weight = 0;
      control->get_resource_limits_manager().get_account_limits( account, ram_bytes, net_weight, cpu_weight );
      return net_weight;
   }

   /// Sets `account`'s NET and CPU weights through sysio.bios, keeping its RAM.
   void set_weights( account_name account, int64_t net_weight, int64_t cpu_weight ) {
      push_action( config::system_account_name, setalimits, config::system_account_name,
                   weights( account, net_weight, cpu_weight ) );
   }

   /// Sets `account`'s CPU weight, keeping its RAM and NET.
   void set_cpu_weight( account_name account, int64_t weight ) {
      set_weights( account, net_weight_of( account ), weight );
   }

   /// The sysio.bios action that sets `account`'s CPU weight, keeping its RAM and NET.
   action cpu_weight_action( account_name account, int64_t weight ) {
      return get_action( config::system_account_name, setalimits, auth( config::system_account_name ),
                         weights( account, net_weight_of( account ), weight ) );
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

   /**
    * Pauses billing for `pause` at the start of every frame that runs `receiver`'s code, as loading or compiling a
    * module on its first run does, and counts the pauses in `pauses`. The hook stays until the returned guard goes.
    */
   auto pause_frames_of( account_name receiver, std::chrono::milliseconds pause, uint32_t& pauses ) {
      auto& substitute_apply = control->get_wasm_interface().substitute_apply;
      substitute_apply = [receiver, pause, &pauses]( const digest_type&, uint8_t, uint8_t, apply_context& context ) {
         if( context.get_receiver() == receiver ) {
            context.trx_context.pause_billing_timer();
            std::this_thread::sleep_for( pause );
            context.trx_context.resume_billing_timer();
            ++pauses;
         }
         return false; // the code runs as usual
      };
      return fc::make_scoped_exit( [&substitute_apply]() { substitute_apply = {}; } );
   }

   /// Pushes with `billed` as the receipt's CPU, read the way a validator reads a block's receipt.
   transaction_trace_ptr push_explicit( const signed_transaction& trx, cpu_usage_t billed ) {
      auto ptrx  = std::make_shared<packed_transaction>( trx );
      auto meta  = transaction_metadata::start_recover_keys( ptrx, control->get_thread_pool(), control->get_chain_id(),
                                                             fc::microseconds::maximum(),
                                                             transaction_metadata::trx_type::input ).get();
      auto trace = control->test_push_transaction( meta, fc::time_point::maximum(), fc::microseconds::maximum(),
                                                   std::move( billed ), true );
      if( trace->except_ptr )
         std::rethrow_exception( trace->except_ptr );
      return trace;
   }

   transaction_trace_ptr transfer( account_name from, account_name to, const char* quantity, const std::string& memo,
                                   bool explicit_payer = false ) {
      return push_measured( make_trx( { transfer_action( from, to, quantity, memo, explicit_payer ) } ) );
   }

   std::optional<uint32_t> long_spin_count; ///< long_spins(), once measured

   /**
    * A loop count that keeps cpu_receiver_test busy for at least long_frame_us, well past the stipend. Runtimes differ
    * by orders of magnitude, so it is measured: spins the game pays for directly, doubled until even the fastest of
    * calibration_runs runs takes that long. The measuring takes a good part of a block's CPU, so the block is produced
    * and the test's own transactions start in a new one.
    */
   uint32_t long_spins() {
      if( !long_spin_count ) {
         uint32_t n = first_long_spins;
         for( uint32_t i = 0; i <= max_spin_doublings && !long_spin_count; ++i, n *= 2 ) {
            // Being descheduled only adds to a run's elapsed time, so the fastest of a few runs is the loop's cost.
            int64_t fastest_us = std::numeric_limits<int64_t>::max();
            for( uint32_t run = 0; run < calibration_runs; ++run ) {
               auto spin  = get_action( game, "spin"_n, auth( game ), mvo()( "n", n + run ) ); // distinct transactions
               auto trace = push_measured( make_trx( { spin } ) );
               fastest_us = std::min( fastest_us, trace->action_traces[0].elapsed.count() );
            }
            if( fastest_us >= long_frame_us )
               long_spin_count = n;
         }
         BOOST_REQUIRE( long_spin_count );
         produce_block();
      }
      return *long_spin_count;
   }

   /**
    * How much of its two frames' elapsed time the game's entry leaves out, for a transfer whose handler spins `count`
    * times and sends itself an inline spin of `count`: the least of calibration_runs transfers, `count` raised by the
    * run so that the transactions differ. A frame's elapsed time and its CPU span the same run, so the host's speed and
    * load cancel; being descheduled outside a frame only adds to what is left out.
    */
   int64_t least_left_out_of_game_frames( uint32_t count ) {
      int64_t least = std::numeric_limits<int64_t>::max();
      for( uint32_t run = 0; run < calibration_runs; ++run ) {
         auto trace   = transfer( alice, game, amount, spinsend_command + std::to_string( count + run ) );
         auto receipt = entries( trace );
         BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
         const int64_t frames_us =
            frame( trace, game, "transfer"_n ).elapsed.count() + frame( trace, game, "spin"_n ).elapsed.count();
         least = std::min( least, frames_us - static_cast<int64_t>( receipt[1] ) );
      }
      return least;
   }

   /// A transfer memo that keeps cpu_receiver_test's handler busy well past the stipend.
   std::string long_spin_memo() { return spin_command + std::to_string( long_spins() ); }

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

BOOST_FIXTURE_TEST_CASE( notified_contract_pays_only_past_the_stipend, receiver_cpu_tester ) try {
   const auto memo = long_spin_memo();
   auto before     = cpu_windows( { token_account, alice, game } );
   auto game_net   = usage( *control, game ).net_usage;
   auto trace      = transfer( alice, game, amount, memo );

   const auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( trace->action_traces[0].cpu_usage_us->value == receipt[0] );
   const auto& handler = frame( trace, game, "transfer"_n );
   BOOST_REQUIRE( handler.cpu_usage_us );
   BOOST_TEST( handler.cpu_usage_us->value == receipt[1] );
   // sysio.token's entry carries the stipend, and the game's entry the rest of its handler.
   BOOST_REQUIRE( handler.elapsed.count() > stipend_us );
   BOOST_TEST( receipt[0] >= stipend_us );
   BOOST_TEST( receipt[1] > 0u );
   BOOST_TEST( receipt[1] <= most_past_stipend( handler.elapsed.count() ) );
   BOOST_TEST( trace->total_cpu_usage_us.value == total( receipt ) );
   BOOST_TEST( !frame( trace, alice, "transfer"_n ).cpu_usage_us ); // no code, so no entry
   check_billed( before, { token_account, game } );
   BOOST_TEST( same_window( usage( *control, game ).net_usage, game_net ) ); // NET stays with the input payer
   produce_and_check_validator( { token_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( notify_cpu_stipend_starts_at_100_us, receiver_cpu_tester ) try {
   BOOST_TEST( genesis_stipend == config::default_notify_cpu_stipend );
   BOOST_TEST( config::default_notify_cpu_stipend == 100u );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( stipend_follows_setparams, receiver_cpu_tester ) try {
   // A larger stipend leaves more of the handler on sysio.token's entry.
   constexpr int64_t larger = 2 * stipend_us;
   const auto memo = long_spin_memo();
   set_notify_cpu_stipend( larger );
   auto before  = cpu_windows( { token_account, alice, game } );
   auto trace   = transfer( alice, game, amount, memo );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   const auto& handler = frame( trace, game, "transfer"_n );
   BOOST_TEST( receipt[0] >= larger );
   BOOST_TEST( receipt[1] > 0u );
   BOOST_TEST( receipt[1] <= most_past_stipend( handler.elapsed.count(), larger ) );
   check_billed( before, { token_account, game } );
   produce_and_check_validator( { token_account, alice, game } );

   // 0 turns the stipend off: a handler that returns at once bills the game, and a contract with no CPU cannot
   // receive.
   set_notify_cpu_stipend( 0 );
   before  = cpu_windows( { token_account, alice, game } );
   trace   = transfer( alice, game, amount, "" );
   receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( receipt[1] > 0u );
   check_billed( before, { token_account, game } );
   BOOST_CHECK_EXCEPTION( transfer( alice, idle, amount, "" ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( idle_over_its_limit ) );
   produce_and_check_validator( { token_account, alice, game, idle } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( stipend_above_the_transaction_limit_is_rejected, receiver_cpu_tester ) try {
   const uint32_t max_trx_cpu = control->get_global_properties().configuration.max_transaction_cpu_usage;
   set_notify_cpu_stipend( max_trx_cpu );
   BOOST_CHECK_EXCEPTION( set_notify_cpu_stipend( max_trx_cpu + 1 ), action_validate_exception,
                          fc_exception_message_contains( stipend_over_trx_limit ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( authorizer_without_cpu_or_net_is_billed_nothing, receiver_cpu_tester ) try {
   transfer( alice, carol, amount, "" );
   set_weights( carol, 0, 0 );
   produce_block();
   int64_t ram_bytes = 0, net_weight = -1, cpu_weight = -1;
   control->get_resource_limits_manager().get_account_limits( carol, ram_bytes, net_weight, cpu_weight );
   BOOST_REQUIRE_EQUAL( net_weight, 0 );
   BOOST_REQUIRE_EQUAL( cpu_weight, 0 );

   // sysio.token pays for its transfer, CPU and NET, and the game for its handler past the stipend, CPU only.
   const auto memo = long_spin_memo();
   auto before    = cpu_windows( { token_account, carol, game } );
   auto carol_net = usage( *control, carol ).net_usage;
   auto token_net = usage( *control, token_account ).net_usage;
   auto trace     = transfer( carol, game, amount, memo );
   BOOST_TEST( entries( trace ).size() == 2u );
   check_billed( before, { token_account, game } );
   BOOST_TEST( same_window( usage( *control, carol ).net_usage, carol_net ) );
   BOOST_TEST( !same_window( usage( *control, token_account ).net_usage, token_net ) );
   produce_and_check_validator( { token_account, carol, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( contract_without_cpu_receives_within_the_stipend, receiver_cpu_tester ) try {
   // idle has no CPU. A handler the stipend covers costs it nothing: its entry is 0 and its CPU window stays put.
   auto before  = cpu_windows( { token_account, alice, idle } );
   auto trace   = transfer( alice, idle, amount, "" );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( receipt[1] == 0u );
   BOOST_TEST( frame( trace, idle, "transfer"_n ).cpu_usage_us->value == 0u );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, alice, idle } );

   // Past the stipend it owes CPU it does not have, so the transfer fails with the usual CPU error and moves nothing.
   const auto memo          = long_spin_memo();
   const auto alice_balance = get_currency_balance( token_account, token_symbol, alice );
   const auto idle_balance  = get_currency_balance( token_account, token_symbol, idle );
   BOOST_CHECK_EXCEPTION( transfer( alice, idle, amount, memo ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( idle_over_its_limit ) );
   BOOST_TEST( get_currency_balance( token_account, token_symbol, alice ) == alice_balance );
   BOOST_TEST( get_currency_balance( token_account, token_symbol, idle ) == idle_balance );

   // An explicit payer pays for all of it.
   before = cpu_windows( { token_account, alice, idle } );
   trace  = transfer( alice, idle, amount, memo, true );
   BOOST_TEST( entries( trace ).size() == 1u );
   check_billed( before, { alice } );
   produce_and_check_validator( { token_account, alice, idle } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( many_notifications_in_one_block_cost_the_contract_nothing, receiver_cpu_tester ) try {
   // relay returns at once on transfers it has nothing to do for. Many such transfers to it from other accounts in one
   // block bill it nothing, and its own actions in that block and the next still run.
   // Give this case the maximum permitted stipend: a successful notification stays within it even if descheduled.
   // Other cases exercise stipend exhaustion; here every zero-billing and unchanged-window assertion stays strict.
   set_notify_cpu_stipend( control->get_global_properties().configuration.max_transaction_cpu_usage );
   transfer( alice, bob, bob_funding, "" );
   produce_block();
   auto before = cpu_windows( { relay } );
   for( uint32_t k = 0; k < many_notifications; ++k ) {
      const auto quantity = asset( k + 1, token_symbol ).to_string(); // distinct transactions
      auto receipt = entries( transfer( k % 2 == 0 ? alice : bob, relay, quantity.c_str(), "" ) );
      BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
      BOOST_TEST( receipt[1] == 0u );
   }
   check_billed( before, {} );
   auto own_spin = [&]( uint32_t n ) {
      return push_measured( make_trx( { get_action( relay, "spin"_n, auth( relay ), mvo()( "n", n ) ) } ) );
   };
   BOOST_TEST( own_spin( short_spin_count )->action_traces[0].console == spun );
   produce_and_check_validator( { token_account, alice, bob, relay } );
   BOOST_TEST( own_spin( short_spin_count + 1 )->action_traces[0].console == spun );
   produce_and_check_validator( { token_account, alice, bob, relay } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( cold_module_load_takes_none_of_the_stipend, receiver_cpu_tester ) try {
   // A contract with no CPU is notified for the first time, and its first run loads a large module no one has run
   // before. Loading pauses billing: the frame's CPU and its deadline both leave the load out. With no leeway, the
   // handler, which has nothing to do for transfers, still fits the stipend, though the load takes far longer: tens of
   // milliseconds to compile or parse the module on each runtime, against a few hundred microseconds for a warm run on
   // the interpreter.
   create_accounts( { cold } );
   set_code( cold, test_contracts::sysio_system_wasm() ); // no transfer handler, and nothing else runs this code
   set_cpu_weight( cold, 0 );
   produce_block();
   transfer( alice, bob, amount, "" ); // warms everything but cold's module
   control->set_subjective_cpu_leeway( fc::microseconds( 0 ) );

   // A block deadline, as a producer and a speculative node push every transaction with, enables the billing pause.
   auto before  = cpu_windows( { token_account, cold } );
   auto trace   = push_measured( make_trx( { transfer_action( alice, cold, amount, "" ) } ),
                                 fc::time_point::now() + fc::seconds( 10 ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( receipt[1] == 0u );
   BOOST_TEST( frame( trace, cold, "transfer"_n ).elapsed.count() > stipend_us ); // the load ran past the stipend
   check_billed( before, { token_account } );
   control->set_subjective_cpu_leeway( fc::microseconds( config::default_subjective_cpu_leeway_us ) );
   produce_and_check_validator( { token_account, alice, cold } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( pause_in_a_frame_is_left_out_of_its_cpu_and_deadline, receiver_cpu_tester ) try {
   // A hook pauses a frame the way a first run's load does, the same on every runtime: first the input action's frame,
   // under a transaction limit shorter than the pause, then a notified contract's that has no CPU, with no leeway. Each
   // push succeeds only if the paused frame's deadline moves past the pause and its CPU leaves the pause out.
   transfer( alice, idle, amount, "" ); // loads the modules, so the hook's are the frames' only pauses
   produce_block();
   // A block deadline, as a producer and a speculative node push every transaction with, enables the billing pause.
   const auto    block_deadline = [] { return fc::time_point::now() + fc::seconds( 10 ); };
   const int64_t pause_us       = std::chrono::microseconds( frame_pause ).count();
   uint32_t      pauses         = 0;
   {
      auto hook = pause_frames_of( token_account, frame_pause, pauses );
      auto trx  = make_trx( { transfer_action( alice, idle, amount, "" ) } );
      trx.max_cpu_usage_ms = short_trx_max_cpu_ms;
      sign( trx );
      auto trace = push_measured( trx, block_deadline() );
      BOOST_TEST( pauses == 1u );
      auto receipt = entries( trace );
      BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
      BOOST_TEST( receipt[0] < pause_us );
      BOOST_TEST( frame( trace, token_account, "transfer"_n ).elapsed.count() >= pause_us ); // paused inside the frame
   }
   control->set_subjective_cpu_leeway( fc::microseconds( 0 ) );
   {
      auto hook   = pause_frames_of( idle, frame_pause, pauses );
      auto before = cpu_windows( { token_account, idle } );
      auto trace  = push_measured( make_trx( { transfer_action( alice, idle, amount, "" ) } ), block_deadline() );
      BOOST_TEST( pauses == 2u );
      auto receipt = entries( trace );
      BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
      BOOST_TEST( receipt[1] == 0u );
      BOOST_TEST( frame( trace, idle, "transfer"_n ).elapsed.count() >= pause_us ); // paused inside the frame
      check_billed( before, { token_account } );
   }
   control->set_subjective_cpu_leeway( fc::microseconds( config::default_subjective_cpu_leeway_us ) );
   produce_and_check_validator( { token_account, alice, idle } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( pause_past_the_block_deadline_stops_at_the_block_deadline, receiver_cpu_tester ) try {
   // A pause is left out of a frame's CPU but not out of the block's time. The transaction's limit is within the push's
   // block deadline, and a pause runs past both: in the input action's frame, then in a notified contract's. The
   // deadlines move past the pause, but not past the block deadline: the transaction stops there with
   // deadline_exception, which a producer retries in a later block, rather than failing on any account's CPU or the
   // transaction's limit.
   transfer( alice, idle, amount, "" ); // loads the modules, so the hook's are the frames' only pauses
   produce_block();
   uint32_t pauses = 0;
   for( auto paused : { token_account, idle } ) {
      BOOST_TEST_CONTEXT( "pausing " << paused ) {
         auto hook = pause_frames_of( paused, block_pause, pauses );
         auto trx  = make_trx( { transfer_action( alice, idle, amount, "" ) } );
         trx.max_cpu_usage_ms = paused_trx_max_cpu_ms;
         sign( trx );
         const auto block_deadline =
            fc::time_point::now() + fc::microseconds( std::chrono::microseconds( paused_block_time ).count() );
         BOOST_CHECK_THROW( push_measured( trx, block_deadline ), deadline_exception );
      }
      produce_block(); // the next transaction differs
   }
   BOOST_TEST( pauses == 2u );
   // Without the pause it runs.
   transfer( alice, idle, amount, "" );
   produce_and_check_validator( { token_account, alice, idle } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( zero_entry_leaves_the_contracts_cpu_window_alone, receiver_cpu_tester ) try {
   // relay spends CPU and then loses its weight, so its window holds more than its allowance of none. Billing it even
   // 0 us would fail the window check.
   push_measured( make_trx( { get_action( relay, "spin"_n, auth( relay ), mvo()( "n", long_spins() ) ) } ) );
   produce_block();
   set_cpu_weight( relay, 0 );
   produce_block();
   const auto [relay_cpu_us, greylisted, unlimited] =
      control->get_cpu_limit( relay, control->pending_block_timestamp() );
   BOOST_REQUIRE( !unlimited );
   BOOST_REQUIRE_EQUAL( relay_cpu_us, 0 );
   BOOST_REQUIRE( usage( *control, relay ).cpu_usage.value_ex > 0 );

   // A handler the stipend covers bills relay nothing, so its window is neither checked nor moved.
   auto before  = cpu_windows( { token_account, alice, relay } );
   auto trace   = transfer( alice, relay, amount, "" );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( receipt[1] == 0u );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, alice, relay } );

   // Past the stipend it is over its limit.
   BOOST_CHECK_EXCEPTION( transfer( alice, relay, amount, long_spin_memo() ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( relay_over_its_limit ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_over_its_limit_fails_on_account_cpu_limit, receiver_cpu_tester ) try {
   // A real deadline: the handler runs through its stipend and then idle's remaining CPU, which together are tighter
   // than the transaction's limit.
   const auto deadline = fc::time_point::now() + fc::seconds( 10 );
   BOOST_CHECK_EXCEPTION( push_measured( make_trx( { transfer_action( alice, idle, amount, "forever" ) } ), deadline ),
                          tx_cpu_usage_exceeded, fc_exception_message_contains( "reached account cpu limit" ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( game_pays_the_winners_stipend_and_the_winner_the_rest, receiver_cpu_tester ) try {
   // Called directly, the game is the input payer: its transfer stays on the input entry, which pays the winner's
   // stipend, and the winner pays for the rest of its handler.
   const auto memo = long_spin_memo();
   auto before = cpu_windows( { token_account, game, winner } );
   auto payout = get_action( game, "payout"_n, auth( game ),
                             mvo()( "to", winner )( "quantity", asset::from_string( amount ) )( "memo", memo ) );
   auto trace  = push_measured( make_trx( { payout } ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   const auto& winner_handler = frame( trace, winner, "transfer"_n );
   BOOST_TEST( winner_handler.cpu_usage_us->value == receipt[1] );
   BOOST_REQUIRE( winner_handler.elapsed.count() > stipend_us );
   BOOST_TEST( receipt[0] >= stipend_us );
   BOOST_TEST( receipt[1] > 0u );
   BOOST_TEST( receipt[1] <= most_past_stipend( winner_handler.elapsed.count() ) );
   BOOST_TEST( !frame( trace, game, "transfer"_n ).cpu_usage_us ); // billed to the game already
   BOOST_TEST( !frame( trace, token_account, "transfer"_n ).cpu_usage_us );
   check_billed( before, { game, winner } );
   produce_and_check_validator( { token_account, game, winner } );

   // From the game's transfer handler: the game's transfer notifies the winner, so the game's entry pays the winner's
   // stipend on top of the game's own code past its stipend.
   before  = cpu_windows( { token_account, alice, game, winner } );
   trace   = transfer( alice, game, amount, "pay:winner" );
   receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
   BOOST_TEST( trace->action_traces[0].cpu_usage_us->value == receipt[0] );
   BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == receipt[1] );
   BOOST_TEST( !frame( trace, game, "transfer"_n, 1 ).cpu_usage_us ); // notified of its own transfer
   const auto& paid = frame( trace, winner, "transfer"_n );
   BOOST_TEST( paid.cpu_usage_us->value == receipt[2] );
   BOOST_TEST( receipt[2] <= most_past_stipend( paid.elapsed.count() ) );
   // The game's frames: its handler, its transfer, and its notification of that transfer.
   const int64_t game_frames_us = frame( trace, game, "transfer"_n ).elapsed.count() +
                                  frame( trace, token_account, "transfer"_n, 1 ).elapsed.count() +
                                  frame( trace, game, "transfer"_n, 1 ).elapsed.count();
   BOOST_TEST( receipt[1] > 0u ); // the winner's stipend at least
   BOOST_TEST( receipt[1] <= most_past_stipend( game_frames_us ) + paid.elapsed.count() );
   flat_set<account_name> billed{ token_account, game };
   if( receipt[2] > 0 )
      billed.insert( winner );
   check_billed( before, billed );
   produce_and_check_validator( { token_account, alice, game, winner } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( forwarded_notification_bills_the_forwarder, receiver_cpu_tester ) try {
   auto before  = cpu_windows( { token_account, relay, winner } );
   auto trace   = transfer( alice, relay, amount, "forward:winner" );
   auto receipt = entries( trace );

   // The winner's handler bills the relay's entry and shares its stipend.
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   const auto& relay_handler = frame( trace, relay, "transfer"_n );
   const auto& forwarded     = frame( trace, winner, "transfer"_n );
   BOOST_TEST( relay_handler.cpu_usage_us->value == receipt[1] );
   BOOST_TEST( !forwarded.cpu_usage_us );
   BOOST_TEST( receipt[1] <= most_past_stipend( relay_handler.elapsed.count() + forwarded.elapsed.count() ) );
   flat_set<account_name> billed{ token_account };
   if( receipt[1] > 0 )
      billed.insert( relay );
   check_billed( before, billed );
   produce_and_check_validator( { token_account, relay, winner } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( stipend_is_shared_by_the_frames_billed_to_the_contract, receiver_cpu_tester ) try {
   // The game's handler spins and sends itself an inline spin: both frames bill its one entry, with one stipend.
   const uint32_t n = long_spins() / 2; // each frame runs about half of long_frame_us, well past the stipend
   auto before  = cpu_windows( { token_account, alice, game } );
   auto trace   = transfer( alice, game, amount, spinsend_command + std::to_string( n ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   const auto& handler = frame( trace, game, "transfer"_n );
   const auto& spin    = frame( trace, game, "spin"_n );
   BOOST_TEST( handler.cpu_usage_us->value == receipt[1] );
   BOOST_TEST( !spin.cpu_usage_us ); // on the handler's entry
   BOOST_TEST( spin.console == spun );
   BOOST_TEST( receipt[1] > 0u );
   BOOST_TEST( receipt[1] <= most_past_stipend( handler.elapsed.count() + spin.elapsed.count() ) );
   check_billed( before, { token_account, game } );
   produce_and_check_validator( { token_account, alice, game } );

   // The entry leaves out of its frames' elapsed time one stipend and the little time outside the frames. A stipend
   // for each frame would leave out a second one.
   BOOST_TEST( least_left_out_of_game_frames( n ) < stipend_us + stipend_us / 2 );
   produce_and_check_validator( { token_account, alice, game } );
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
                          fc_exception_message_contains( trx_max_cpu_reached ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( msig_exec_bills_the_called_contract_without_a_stipend, receiver_cpu_tester ) try {
   transaction proposed;
   proposed.actions.emplace_back( get_action( game, "spin"_n, auth( game ), mvo()( "n", short_spin_count ) ) );
   set_transaction_headers( proposed, proposal_expiration_sec );
   push_action( msig_account, "propose"_n, auth( alice, true ),
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
   // An action sent to the game, not a notification, bills it from the start, as calling it directly does.
   const auto& spin = frame( trace, game, "spin"_n );
   BOOST_TEST( spin.cpu_usage_us->value == receipt[1] );
   BOOST_TEST( receipt[1] > 0u );
   BOOST_TEST( receipt[1] <= spin.elapsed.count() );
   check_billed( before, { msig_account, game } );
   produce_and_check_validator( { msig_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( overhead_goes_on_input_entries_only, receiver_cpu_tester ) try {
   // A high minimum puts most of the transaction's CPU outside any action.
   auto cfg = control->get_global_properties().configuration;
   cfg.min_transaction_cpu_usage = high_min_trx_cpu_us;
   // measured, since the new minimum applies to this transaction already
   push_measured( make_trx( { get_action( config::system_account_name, setparams,
                                          auth( config::system_account_name ), mvo()( "params", cfg ) ) } ) );
   produce_block();

   auto trace   = transfer( alice, game, amount, "spin:10" );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( total( receipt ) >= high_min_trx_cpu_us );
   // the receiver entry is its frame's run past the stipend alone, which the frame's elapsed covers
   BOOST_TEST( receipt[1] <= most_past_stipend( frame( trace, game, "transfer"_n ).elapsed.count() ) );
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

BOOST_FIXTURE_TEST_CASE( transaction_failing_its_final_checks_bills_no_receiver, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();
   transaction_trace_ptr failed;
   auto connection = control->applied_transaction().connect(
      [&]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> x ) {
         if( std::get<0>( x )->except )
            failed = std::get<0>( x );
      } );

   // Both handlers run to the end, and then idle, which has no CPU and ran past its stipend, fails the final checks.
   // The transaction is billed as a failed one: its CPU stays on the input actions, and neither receiver is billed.
   const auto idle_memo = long_spin_memo();
   for( bool speculative : { false, true } ) {
      for( bool payer_billing_disabled : { true, false } ) {
         BOOST_TEST_CONTEXT( "speculative " << speculative << ", payer billing disabled " << payer_billing_disabled ) {
            sub_bill.reset();
            sub_bill.disable_payer_billing( payer_billing_disabled );
            if( speculative )
               start_speculative_block();
            const auto now = control->pending_block_time();
            failed.reset();
            auto trx = make_trx( { transfer_action( alice, game, amount, spin_memo ),
                                   transfer_action( alice, idle, amount, idle_memo ) } );
            BOOST_CHECK_EXCEPTION( push_measured( trx ), tx_cpu_usage_exceeded,
                                   fc_exception_message_contains( idle_over_its_limit ) );
            BOOST_REQUIRE( failed );
            for( auto receiver : { game, idle } )
               BOOST_TEST_CONTEXT( receiver ) {
                  BOOST_TEST( !frame( failed, receiver, "transfer"_n ).cpu_usage_us );
                  BOOST_TEST( sub_bill.get_subjective_bill( receiver, now ).count() == 0 );
               }
            BOOST_TEST( failed->total_cpu_usage_us.value == failed->action_traces[0].cpu_usage_us->value +
                                                           failed->action_traces[1].cpu_usage_us->value );
            BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() > 0 );
            BOOST_TEST( ( sub_bill.get_subjective_bill( token_account, now ).count() > 0 ) == !payer_billing_disabled );
            if( speculative )
               control->abort_block();
         }
      }
   }
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_limit_includes_cpu_granted_in_the_transaction, receiver_cpu_tester ) try {
   // idle has no CPU as sysio.token notifies it, and the same transaction grants it CPU, after its handler or before.
   // Its handler runs past the stipend, and the final checks read idle's limit as the transaction leaves it.
   const auto memo = long_spin_memo();
   for( bool grant_first : { false, true } ) {
      BOOST_TEST_CONTEXT( "grant first " << grant_first ) {
         set_cpu_weight( idle, 0 );
         produce_block();
         auto before = cpu_windows( { token_account, alice, idle } );
         vector<action> actions{ transfer_action( alice, idle, amount, memo ), cpu_weight_action( idle, cpu_weight ) };
         if( grant_first )
            std::ranges::reverse( actions );
         auto trace   = push_measured( make_trx( std::move( actions ) ) );
         auto receipt = entries( trace );
         BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
         BOOST_TEST( frame( trace, idle, "transfer"_n ).cpu_usage_us->value == receipt[2] );
         BOOST_TEST( receipt[2] > 0u );
         check_billed( before, { token_account, idle } );
         produce_and_check_validator( { token_account, alice, idle } );
      }
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_subjective_bill_limits_its_frame_past_the_stipend, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();
   sub_bill.reset();
   const auto memo = long_spin_memo();
   transfer( alice, bob, amount, "" );
   const auto now = control->pending_block_time();
   const auto [game_cpu_us, greylisted, unlimited] = control->get_cpu_limit( game, block_timestamp_type( now ) );
   BOOST_REQUIRE( !unlimited );

   // CPU pending for the game from another transaction leaves it none, leeway included: its frame stops when the
   // stipend runs out.
   const auto pending_id = transaction_id_type::hash( std::string( "pending" ) );
   sub_bill.subjective_bill( pending_id, fc::time_point_sec( now + fc::seconds( pending_bill_lifetime_sec ) ), {},
                             { { game, fc::microseconds( game_cpu_us + beyond_any_leeway_us ) } } );
   const auto deadline = fc::time_point::now() + fc::seconds( 10 );
   auto trx = make_trx( { transfer_action( alice, game, amount, memo ) } );
   BOOST_CHECK_EXCEPTION( push_measured( trx, deadline ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( account_cpu_limit_reached ) );
   transfer( alice, bob, amount, "spin:1" ); // reaches no contract
   // A handler within the stipend still runs, and bills the game nothing.
   auto receipt = entries( push_measured( make_trx( { transfer_action( alice, game, amount, "" ) } ), deadline ) );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( receipt[1] == 0u );

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

   // A receiver entry the receipt leaves out bills nothing, and the receipt built here gets it as 0, which leaves the
   // game's CPU window alone. The validator replays the block to the same windows.
   before = cpu_windows( { token_account, game } );
   trace  = push_explicit( make_trx( { transfer_action( alice, game, amount, "spin:10" ) } ), { 500 } );
   BOOST_TEST( entries( trace ) == std::vector<uint32_t>( { 500, 0 } ), boost::test_tools::per_element() );
   BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == 0u );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( validator_bills_the_producers_split, receiver_cpu_tester ) try {
   // A validator bills the receipt's entries as given and never applies the stipend itself: a split no node would
   // measure, with nothing on the receiver, still bills exactly as written.
   const auto memo = long_spin_memo();
   auto before = cpu_windows( { token_account, idle } );
   auto trace  = push_explicit( make_trx( { transfer_action( alice, idle, amount, memo ) } ), { 5'000, 0 } );
   BOOST_TEST( entries( trace ) == std::vector<uint32_t>( { 5'000, 0 } ), boost::test_tools::per_element() );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, alice, idle } );
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

BOOST_FIXTURE_TEST_CASE( only_a_privileged_sysio_account_is_a_system_receiver, receiver_cpu_tester ) try {
   // A privileged sysio.* contract is a system account: its handler stays on sysio.token's entry.
   auto before = cpu_windows( { token_account, system_receiver } );
   auto trace  = transfer( alice, system_receiver, amount, spin_memo );
   BOOST_TEST( entries( trace ).size() == 1u );
   BOOST_TEST( !frame( trace, system_receiver, "transfer"_n ).cpu_usage_us );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, system_receiver } );

   // The sysio prefix alone does not make one: an unprivileged sysio.* contract pays for its handler past the stipend.
   const auto memo = long_spin_memo();
   before      = cpu_windows( { token_account, unprivileged } );
   trace       = transfer( alice, unprivileged, amount, memo );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 2u );
   BOOST_TEST( frame( trace, unprivileged, "transfer"_n ).cpu_usage_us->value == receipt[1] );
   check_billed( before, { token_account, unprivileged } );
   produce_and_check_validator( { token_account, unprivileged } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( entries_follow_their_input_actions, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();
   transfer( alice, bob, "10.0000 TKN", "" );
   const auto memo = long_spin_memo();
   produce_block();

   // Two inputs with different authorizers each reach a contract. The receiver entries follow the input entries in
   // the order their frames started, and each is billed subjectively to its own input's first authorizer.
   for( bool payer_billing_disabled : { true, false } ) {
      BOOST_TEST_CONTEXT( "payer billing disabled " << payer_billing_disabled ) {
         sub_bill.reset();
         sub_bill.disable_payer_billing( payer_billing_disabled );
         start_speculative_block();
         const auto now      = control->pending_block_time();
         const auto* quantity = payer_billing_disabled ? "1.0000 TKN" : "2.0000 TKN";
         auto trace   = push_measured( make_trx( { transfer_action( alice, game, quantity, memo ),
                                                   transfer_action( bob, winner, quantity, memo ) } ) );
         auto receipt = entries( trace );
         BOOST_REQUIRE_EQUAL( receipt.size(), 4u );
         BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == receipt[2] );
         BOOST_TEST( frame( trace, winner, "transfer"_n ).cpu_usage_us->value == receipt[3] );
         BOOST_TEST( sub_bill.get_subjective_bill( alice, now ).count() == receipt[0] + receipt[2] );
         BOOST_TEST( sub_bill.get_subjective_bill( bob, now ).count() == receipt[1] + receipt[3] );
         BOOST_TEST( sub_bill.get_subjective_bill( token_account, now ).count() ==
                     ( payer_billing_disabled ? 0 : receipt[0] + receipt[1] ) );
         BOOST_TEST( sub_bill.get_subjective_bill( game, now ).count() == ( payer_billing_disabled ? 0 : receipt[2] ) );
         BOOST_TEST( sub_bill.get_subjective_bill( winner, now ).count() ==
                     ( payer_billing_disabled ? 0 : receipt[3] ) );
         control->abort_block();
      }
   }

   auto before = cpu_windows( { token_account, alice, bob, game, winner } );
   push_measured( make_trx( { transfer_action( alice, game, amount, memo ),
                              transfer_action( bob, winner, amount, memo ) } ) );
   check_billed( before, { token_account, game, winner } );
   produce_and_check_validator( { token_account, alice, bob, game, winner } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( contract_reached_twice_gets_an_entry_each_time, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();
   const auto memo = long_spin_memo();
   sub_bill.reset();
   sub_bill.disable_payer_billing( false );
   start_speculative_block();
   const auto now = control->pending_block_time();
   auto trx     = make_trx( { transfer_action( alice, game, amount, memo ),
                              transfer_action( alice, game, amount, memo ) } );
   auto trace   = push_measured( trx );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 4u );
   // Each notification is its own switch, with its own stipend.
   for( size_t occurrence : { 0u, 1u } ) {
      const auto& handler = frame( trace, game, "transfer"_n, occurrence );
      BOOST_TEST( handler.cpu_usage_us->value == receipt[2 + occurrence] );
      BOOST_TEST( receipt[2 + occurrence] > 0u );
      BOOST_TEST( receipt[2 + occurrence] <= most_past_stipend( handler.elapsed.count() ) );
   }
   BOOST_TEST( sub_bill.get_subjective_bill( game, now ).count() == receipt[2] + receipt[3] );
   control->abort_block();

   auto before = cpu_windows( { token_account, game } );
   push_measured( trx );
   check_billed( before, { token_account, game } );
   produce_and_check_validator( { token_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_that_pays_for_another_input_is_billed_once, receiver_cpu_tester ) try {
   auto& sub_bill = control->get_mutable_subjective_billing();
   const auto memo = long_spin_memo();
   sub_bill.reset();
   sub_bill.disable_payer_billing( false );
   start_speculative_block();
   const auto now = control->pending_block_time();
   // The game pays for its own spin, an input, and for its transfer handler past the stipend, a receiver entry.
   auto spin    = get_action( game, "spin"_n, auth( game ), mvo()( "n", spin_count ) );
   auto trx     = make_trx( { spin, transfer_action( alice, game, amount, memo ) } );
   auto trace   = push_measured( trx );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
   BOOST_TEST( frame( trace, game, "transfer"_n ).cpu_usage_us->value == receipt[2] );
   BOOST_TEST( receipt[2] > 0u );
   BOOST_TEST( sub_bill.get_subjective_bill( game, now ).count() == receipt[0] + receipt[2] );
   control->abort_block();

   // It pays NET for its input only, which the validator's windows confirm.
   auto before   = cpu_windows( { token_account, game } );
   auto game_net = usage( *control, game ).net_usage;
   push_measured( trx );
   check_billed( before, { token_account, game } );
   BOOST_TEST( !same_window( usage( *control, game ).net_usage, game_net ) );
   produce_and_check_validator( { token_account, alice, game } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( explicit_payer_covers_only_its_own_action, receiver_cpu_tester ) try {
   // alice pays explicitly for the first input and everything below it; the second input switches as usual.
   auto before  = cpu_windows( { token_account, alice, game, winner, relay } );
   auto trace   = push_measured( make_trx( { transfer_action( alice, game, amount, "pay:winner", true ),
                                             transfer_action( alice, relay, amount, "forward:winner" ) } ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
   BOOST_TEST( !frame( trace, game, "transfer"_n ).cpu_usage_us );
   BOOST_TEST( frame( trace, relay, "transfer"_n ).cpu_usage_us->value == receipt[2] );
   flat_set<account_name> billed{ token_account, alice };
   if( receipt[2] > 0 )
      billed.insert( relay );
   check_billed( before, billed );
   produce_and_check_validator( { token_account, alice, game, winner, relay } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_frame_stops_at_the_transaction_limit, receiver_cpu_tester ) try {
   // The game has ample CPU, so its frame stops at the transaction's max_cpu_usage_ms, reported as such.
   auto trx = make_trx( { transfer_action( alice, game, amount, "forever" ) } );
   trx.max_cpu_usage_ms = small_max_cpu_usage_ms;
   sign( trx );
   BOOST_CHECK_EXCEPTION( push_measured( trx, fc::time_point::now() + fc::seconds( 10 ) ), tx_cpu_usage_exceeded,
                          fc_exception_message_contains( trx_max_cpu_reached ) );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( greylisted_receiver_fails_as_greylisted, receiver_cpu_tester ) try {
   // On a speculative block, a greylisted contract with no CPU fails as greylisted past its stipend, both while its
   // frame runs and in the final checks.
   control->add_resource_greylist( idle );
   const auto memo = long_spin_memo();
   start_speculative_block();
   const auto deadline = fc::time_point::now() + fc::seconds( 10 );
   BOOST_CHECK_EXCEPTION( push_measured( make_trx( { transfer_action( alice, idle, amount, "forever" ) } ), deadline ),
                          greylist_cpu_usage_exceeded, fc_exception_message_contains( greylisted_frame_stopped ) );
   BOOST_CHECK_EXCEPTION( push_measured( make_trx( { transfer_action( alice, idle, amount, memo ) } ), deadline ),
                          greylist_cpu_usage_exceeded, fc_exception_message_contains( idle_over_greylisted_limit ) );
   control->abort_block();
   control->remove_resource_greylist( idle );
   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( receiver_code_is_read_as_its_frame_starts, receiver_cpu_tester ) try {
   const auto wasm = test_contracts::cpu_receiver_test_wasm();
   auto set_code_action = [&]( account_name account, bytes code ) {
      return action( auth( account ), setcode{ .account = account, .code = std::move( code ) } );
   };

   // Code set earlier in the transaction counts: bob, a plain account, pays for the handler it just got.
   const auto memo = long_spin_memo();
   set_cpu_weight( bob, cpu_weight );
   produce_block();
   auto before  = cpu_windows( { token_account, bob } );
   auto trace   = push_measured( make_trx( { set_code_action( bob, bytes( wasm.begin(), wasm.end() ) ),
                                             transfer_action( alice, bob, amount, memo ) } ) );
   auto receipt = entries( trace );
   BOOST_REQUIRE_EQUAL( receipt.size(), 3u );
   BOOST_TEST( frame( trace, bob, "transfer"_n ).cpu_usage_us->value == receipt[2] );
   check_billed( before, { token_account, bob } );
   produce_and_check_validator( { token_account, alice, bob } );

   // Code cleared earlier in the transaction counts too: relay has none left to pay for.
   before  = cpu_windows( { token_account, relay } );
   trace   = push_measured( make_trx( { set_code_action( relay, bytes{} ),
                                        transfer_action( alice, relay, amount, spin_memo ) } ) );
   receipt = entries( trace );
   BOOST_TEST( receipt.size() == 2u );
   BOOST_TEST( !frame( trace, relay, "transfer"_n ).cpu_usage_us );
   check_billed( before, { token_account } );
   produce_and_check_validator( { token_account, alice, relay } );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
