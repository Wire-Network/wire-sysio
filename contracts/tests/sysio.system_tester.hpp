#pragma once

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>
#include "contracts.hpp"
#include "test_symbol.hpp"

#include <fc/variant_object.hpp>
#include <fstream>

using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

#ifndef TESTER
#ifdef NON_VALIDATING_TEST
#define TESTER tester
#else
#define TESTER validating_tester
#endif
#endif

namespace sysio_system {


class sysio_system_tester : public TESTER {
public:

   void basic_setup() {
      produce_blocks( 2 );

      create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
               "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n, "sysio.rex"_n });


      produce_blocks( 100 );
      set_code( "sysio.token"_n, contracts::token_wasm());
      set_abi( "sysio.token"_n, contracts::token_abi().data() );
      set_privileged("sysio.token"_n);

      {
         const auto* accnt = control->find_account_metadata( "sysio.token"_n );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
         token_abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));
      }
   }

   void create_core_token( symbol core_symbol = symbol{CORE_SYM} ) {
      FC_ASSERT( core_symbol.decimals() == 4, "create_core_token assumes core token has 4 digits of precision" );
      create_currency( "sysio.token"_n, config::system_account_name, asset(100000000000000, core_symbol) );
      issue( asset(10000000000000, core_symbol) );
      BOOST_REQUIRE_EQUAL( asset(10000000000000, core_symbol), get_balance( "sysio", core_symbol ) );
   }

   void deploy_contract( bool call_init = true ) {
      set_code( config::system_account_name, contracts::system_wasm() );
      set_abi( config::system_account_name, contracts::system_abi().data() );
      if( call_init ) {
         base_tester::push_action(config::system_account_name, "init"_n,
                                               config::system_account_name,  mutable_variant_object()
                                               ("version", 0)
                                               ("core", CORE_SYM_STR)
         );
      }

      {
         const auto* accnt = control->find_account_metadata( config::system_account_name );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
         abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));
      }
   }

   void remaining_setup() {
      produce_blocks();

      create_account( "alice1111111"_n, config::system_account_name, false, false, false, false );
      create_account( "bob111111111"_n, config::system_account_name, false, false, false, false );
      create_account( "carol1111111"_n, config::system_account_name, false, false, false, false );

      BOOST_REQUIRE_EQUAL( core_sym::from_string("1000000000.0000"), get_balance("sysio")  + get_balance("sysio.ramfee") + get_balance("sysio.stake") + get_balance("sysio.ram") );
   }

   enum class setup_level {
      none,
      minimal,
      core_token,
      deploy_contract,
      full
   };

   sysio_system_tester( setup_level l = setup_level::full ) {
      if( l == setup_level::none ) return;

      basic_setup();
      if( l == setup_level::minimal ) return;

      create_core_token();
      if( l == setup_level::core_token ) return;

      initialize_multisig();
      deploy_contract();
      if( l == setup_level::deploy_contract ) return;

      remaining_setup();
   }

   template<typename Lambda>
   sysio_system_tester(Lambda setup) {
      setup(*this);

      basic_setup();
      create_core_token();
      deploy_contract();
      remaining_setup();
   }


   transaction_trace_ptr setup_producer_accounts( const std::vector<account_name>& accounts )
   {
      account_name creator(config::system_account_name);
      signed_transaction trx;
      set_transaction_headers(trx);

      for (const auto& a: accounts) {
         authority owner_auth( get_public_key( a, "owner" ) );
         trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                   newaccount{
                                         .creator  = creator,
                                         .name     = a,
                                         .owner    = owner_auth,
                                         .active   = authority( get_public_key( a, "active" ) )
                                         });

      }

      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), control->get_chain_id()  );
      return push_transaction( trx );
   }

   action_result push_action( const account_name& signer, const action_name &name, const variant_object &data, bool auth = true ) {
         string action_type_name = abi_ser.get_action_type(name);

         action act;
         act.account = config::system_account_name;
         act.name = name;
         act.data = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function(abi_serializer_max_time) );

         return base_tester::push_contract_paid_action( std::move(act), (auth ? signer : signer == "bob111111111"_n ? "alice1111111"_n : "bob111111111"_n).to_uint64_t() );
   }

   int64_t get_net_limit( account_name a ) {
      int64_t ram_bytes = 0, net = 0, cpu = 0;
      control->get_resource_limits_manager().get_account_limits( a, ram_bytes, net, cpu );
      return net;
   };

   int64_t get_cpu_limit( account_name a ) {
      int64_t ram_bytes = 0, net = 0, cpu = 0;
      control->get_resource_limits_manager().get_account_limits( a, ram_bytes, net, cpu );
      return cpu;
   };

   static fc::variant_object producer_parameters_example( int n ) {
      return mutable_variant_object()
         ("max_block_net_usage", 10000000 + n )
         ("target_block_net_usage_pct", 10 + n )
         ("max_transaction_net_usage", 1000000 + n )
         ("base_per_transaction_net_usage", 100 + n)
         ("net_usage_leeway", 500 + n )
         ("context_free_discount_net_usage_num", 1 + n )
         ("context_free_discount_net_usage_den", 100 + n )
         ("max_block_cpu_usage", 10000000 + n )
         ("target_block_cpu_usage_pct", 10 + n )
         ("max_transaction_cpu_usage", 1000000 + n )
         ("min_transaction_cpu_usage", 100 + n )
         ("max_transaction_lifetime", 3600 + n)
         ("deferred_trx_expiration_window", 600 + n)
         ("max_transaction_delay", 10*86400+n)
         ("max_inline_action_size", 4096 + n)
         ("max_inline_action_depth", 4 + n)
         ("max_authority_depth", 6 + n)
         ("max_ram_size", (n % 10 + 1) * 1024 * 1024)
         ("ram_reserve_ratio", 100 + n);
   }

   action_result regproducer( const account_name& acnt, int params_fixture = 1 ) {
      action_result r = push_action( acnt, "regproducer"_n, mvo()
                          ("producer",  acnt )
                          ("producer_key", get_public_key( acnt, "active" ) )
                          ("url", "" )
                          ("location", 0 )
      );
      BOOST_REQUIRE_EQUAL( success(), r);
      return r;
   }

   uint32_t last_block_time() const {
      return time_point_sec( control->head_block_time() ).sec_since_epoch();
   }

   asset get_balance( const account_name& act, symbol balance_symbol = symbol{CORE_SYM} ) {
      vector<char> data = get_row_by_account( "sysio.token"_n, act, "accounts"_n, account_name(balance_symbol.to_symbol_code().value) );
      return data.empty() ? asset(0, balance_symbol) : token_abi_ser.binary_to_variant("account", data, abi_serializer::create_yield_function(abi_serializer_max_time))["balance"].as<asset>();
   }

   asset get_balance( std::string_view act, symbol balance_symbol = symbol{CORE_SYM} ) {
      return get_balance( account_name(act), balance_symbol );
   }

   fc::variant get_producer_info( const account_name& act ) {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, "producers"_n, act );
      return abi_ser.binary_to_variant( "producer_info", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
   }
   fc::variant get_producer_info( std::string_view act ) {
      return get_producer_info( account_name(act) );
   }

   void create_currency( name contract, name manager, asset maxsupply ) {
      auto act =  mutable_variant_object()
         ("issuer",       manager )
         ("maximum_supply", maxsupply );

      base_tester::push_action(contract, "create"_n, contract, act );
   }

   void issue( const asset& amount, const name& manager = config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "issue"_n, manager, mutable_variant_object()
                                ("to",       manager )
                                ("quantity", amount )
                                ("memo",     "")
                                );
   }

   void transfer( const name& from, const name& to, const asset& amount, const name& manager = config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "transfer"_n, manager, mutable_variant_object()
                                ("from",    from)
                                ("to",      to )
                                ("quantity", amount)
                                ("memo", "")
                                );
   }

   void transfer( const name& from, std::string_view to, const asset& amount, const name& manager = config::system_account_name ) {
      transfer( from, name(to), amount, manager );
   }

   void transfer( std::string_view from, std::string_view to, const asset& amount, std::string_view manager ) {
      transfer( name(from), name(to), amount, name(manager) );
   }

   void transfer( std::string_view from, std::string_view to, const asset& amount ) {
      transfer( name(from), name(to), amount );
   }

   void issue_and_transfer( const name& to, const asset& amount, const name& manager = config::system_account_name ) {
      signed_transaction trx;
      trx.actions.emplace_back( get_action( "sysio.token"_n, "issue"_n,
                                            vector<permission_level>{{manager, config::active_name}},
                                            mutable_variant_object()
                                            ("to",       manager )
                                            ("quantity", amount )
                                            ("memo",     "")
                                            )
                                );
      if ( to != manager ) {
         trx.actions.emplace_back( get_action( "sysio.token"_n, "transfer"_n,
                                               vector<permission_level>{{manager, config::active_name}},
                                               mutable_variant_object()
                                               ("from",     manager)
                                               ("to",       to )
                                               ("quantity", amount )
                                               ("memo",     "")
                                               )
                                   );
      }
      set_transaction_headers( trx );
      trx.sign( get_private_key( manager, "active" ), control->get_chain_id()  );
      push_transaction( trx );
   }

   void issue_and_transfer( std::string_view to, const asset& amount, std::string_view manager ) {
      issue_and_transfer( name(to), amount, name(manager) );
   }

   void issue_and_transfer( std::string_view to, const asset& amount, const name& manager ) {
      issue_and_transfer( name(to), amount, manager);
   }

   void issue_and_transfer( std::string_view to, const asset& amount ) {
      issue_and_transfer( name(to), amount );
   }

   double stake2votes( asset stake ) {
      auto now = control->pending_block_time().time_since_epoch().count() / 1000000;
      return stake.get_amount() * pow(2, int64_t((now - (config::block_timestamp_epoch / 1000)) / (86400 * 7))/ double(52) ); // 52 week periods (i.e. ~years)
   }

   double stake2votes( const string& s ) {
      return stake2votes( core_sym::from_string(s) );
   }

   fc::variant get_stats( const string& symbolname ) {
      auto symb = sysio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;
      vector<char> data = get_row_by_account( "sysio.token"_n, name(symbol_code), "stat"_n, account_name(symbol_code) );
      return data.empty() ? fc::variant() : token_abi_ser.binary_to_variant( "currency_stats", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   asset get_token_supply() {
      return get_stats("4," CORE_SYM_NAME)["supply"].as<asset>();
   }

   uint64_t microseconds_since_epoch_of_iso_string( const fc::variant& v ) {
      return static_cast<uint64_t>( time_point::from_iso_string( v.as_string() ).time_since_epoch().count() );
   }

   fc::variant get_global_state() {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, "global"_n, "global"_n );
      if (data.empty()) std::cout << "\nData is empty\n" << std::endl;
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "sysio_global_state", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

   void initialize_multisig() {
      create_account( "sysio.msig"_n, config::system_account_name );
      produce_block();

      set_code( "sysio.msig"_n, contracts::msig_wasm() );
      set_abi( "sysio.msig"_n, contracts::msig_abi().data() );
      set_privileged( "sysio.msig"_n );

      produce_blocks();
      const auto* accnt = control->find_account_metadata( "sysio.msig"_n );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def msig_abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, msig_abi), true);
      msig_abi_ser.set_abi(msig_abi, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   vector<name> active_producers() {
      //stake more than 15% of total SYS supply to activate chain
      transfer( "sysio"_n, "alice1111111"_n, core_sym::from_string("650000000.0000"), config::system_account_name );
      // TODO: do the equivalent with ROA
      // BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111"_n, "alice1111111"_n, core_sym::from_string("300000000.0000"), core_sym::from_string("300000000.0000") ) );

      // create accounts {defproducera, defproducerb, ..., defproducerz} and register as producers
      std::vector<account_name> producer_names;
      {
         producer_names.reserve('z' - 'a' + 1);
         const std::string root("defproducer");
         for ( char c = 'a'; c < 'a'+21; ++c ) {
            producer_names.emplace_back(root + std::string(1, c));
         }
         setup_producer_accounts(producer_names);
         std::vector<legacy::producer_key> schedule;
         for (const auto& p: producer_names) {
            BOOST_REQUIRE_EQUAL( success(), regproducer(p) );
            auto key = get_public_key( p, "active" );
            schedule.push_back(legacy::producer_key{p, key});
         }
         auto trace = TESTER::push_action(config::system_account_name, "setprodkeys"_n, config::system_account_name, mvo()("schedule", schedule));
         BOOST_REQUIRE(!!trace->receipt);
      }
      produce_blocks( 250);

      auto trace_auth = TESTER::push_action(config::system_account_name, updateauth::get_name(), config::system_account_name, mvo()
                                            ("account", name(config::system_account_name).to_string())
                                            ("permission", name(config::active_name).to_string())
                                            ("parent", name(config::owner_name).to_string())
                                            ("auth",  authority(1, {key_weight{get_public_key( config::system_account_name, "active" ), 1}}, {
                                                  permission_level_weight{{config::system_account_name, config::sysio_code_name}, 1},
                                                     permission_level_weight{{config::producers_account_name,  config::active_name}, 1}
                                               }
                                            ))
      );
      BOOST_REQUIRE(!!trace_auth->receipt);

      produce_blocks( 250 );

      auto producer_keys = control->head_block_state()->active_schedule.producers;
      BOOST_REQUIRE_EQUAL( 21, producer_keys.size() );
      BOOST_REQUIRE_EQUAL( name("defproducera"), producer_keys[0].producer_name );

      return producer_names;
   }


   abi_serializer abi_ser;
   abi_serializer token_abi_ser;
   abi_serializer msig_abi_ser;
};

inline uint64_t M( const string& sys_str ) {
   return core_sym::from_string( sys_str ).get_amount();
}

}
