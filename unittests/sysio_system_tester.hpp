#pragma once

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

namespace sysio_system {

class sysio_system_tester : public validating_tester {
public:

   sysio_system_tester()
   : sysio_system_tester([](validating_tester& ) {}){}

   template<typename Lambda>
   sysio_system_tester(Lambda setup) {
      setup(*this);

      produce_blocks( 2 );

      create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
               "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n, "sysio.rex"_n });

      produce_blocks( 100 );

      set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );
      set_abi( "sysio.token"_n, test_contracts::sysio_token_abi() );
      set_privileged("sysio.token"_n);

      {
         const auto* accnt = control->find_account_metadata( "sysio.token"_n );
         BOOST_REQUIRE(accnt != nullptr);
         abi_def abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
         token_abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
      }

      create_currency( "sysio.token"_n, sysio::chain::config::system_account_name, core_from_string("10000000000.0000") );
      issue(sysio::chain::config::system_account_name,      core_from_string("1000000000.0000"));
      BOOST_REQUIRE_EQUAL( core_from_string("1000000000.0000"), get_balance( name("sysio") ) );

      set_code( sysio::chain::config::system_account_name, test_contracts::sysio_system_wasm() );
      set_abi( sysio::chain::config::system_account_name, test_contracts::sysio_system_abi() );

      base_tester::push_action(sysio::chain::config::system_account_name, "init"_n,
                            sysio::chain::config::system_account_name,  mutable_variant_object()
                            ("version", 0)
                            ("core", symbol(CORE_SYMBOL).to_string()));

      {
         const auto* accnt = control->find_account_metadata( sysio::chain::config::system_account_name );
         BOOST_REQUIRE(accnt != nullptr);
         abi_def abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
         abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
      }

      produce_blocks();

      create_account( "alice1111111"_n, sysio::chain::config::system_account_name, false, false, false, false );
      create_account( "bob111111111"_n, sysio::chain::config::system_account_name, false, false, false, false );
      create_account( "carol1111111"_n, sysio::chain::config::system_account_name, false, false, false, false );

      BOOST_REQUIRE_EQUAL( core_from_string("1000000000.0000"),
            get_balance(name("sysio")) + get_balance(name("sysio.ramfee")) + get_balance(name("sysio.stake")) + get_balance(name("sysio.ram")) );
   }

   action_result open( account_name  owner,
                       const string& symbolname,
                       account_name  ram_payer    ) {
      return push_action( ram_payer, "open"_n, mvo()
                          ( "owner", owner )
                          ( "symbol", symbolname )
                          ( "ram_payer", ram_payer )
         );
   }

   transaction_trace_ptr setup_producer_accounts( const std::vector<account_name>& accounts ) {
      account_name creator(sysio::chain::config::system_account_name);
      signed_transaction trx;
      set_transaction_headers(trx);

      for (const auto& a: accounts) {
         authority owner_auth( get_public_key( a, "owner" ) );
         trx.actions.emplace_back( vector<permission_level>{{creator,sysio::chain::config::active_name}},
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
         act.account = sysio::chain::config::system_account_name;
         act.name = name;
         act.data = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function( abi_serializer_max_time ) );

         return base_tester::push_action( std::move(act), auth ? signer.to_uint64_t() :
                                                signer == "bob111111111"_n ? "alice1111111"_n.to_uint64_t() : "bob111111111"_n.to_uint64_t() );
   }

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
         ("max_inline_action_size", 512*1024 + n)
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
      return time_point_sec( control->head().block_time() ).sec_since_epoch();
   }

   asset get_balance( const account_name& act ) {
      vector<char> data = get_row_by_account( "sysio.token"_n, act, "accounts"_n, name(symbol(CORE_SYMBOL).to_symbol_code().value) );
      return data.empty() ? asset(0, symbol(CORE_SYMBOL)) : token_abi_ser.binary_to_variant("account", data, abi_serializer::create_yield_function( abi_serializer_max_time ))["balance"].as<asset>();
   }

   fc::variant get_producer_info( const account_name& act ) {
      vector<char> data = get_row_by_account( sysio::chain::config::system_account_name, sysio::chain::config::system_account_name, "producers"_n, act );
      return abi_ser.binary_to_variant( "producer_info", data, abi_serializer::create_yield_function( abi_serializer_max_time ) );
   }

   void create_currency( name contract, name manager, asset maxsupply ) {
      auto act =  mutable_variant_object()
         ("issuer",       manager )
         ("maximum_supply", maxsupply );

      base_tester::push_action(contract, "create"_n,
         {permission_level(manager, config::sysio_payer_name), permission_level(contract, sysio::chain::config::active_name), permission_level(manager, sysio::chain::config::active_name)},
         act );
   }

   void issue( name to, const asset& amount, name manager = sysio::chain::config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "issue"_n,
         {permission_level(manager, config::sysio_payer_name), permission_level(manager, sysio::chain::config::active_name)},
         mutable_variant_object()
                                ("to",      to )
                                ("quantity", amount )
                                ("memo", "")
                                );
   }
   void transfer( name from, name to, const asset& amount, name manager = sysio::chain::config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "transfer"_n,
         {permission_level(manager, config::sysio_payer_name), permission_level(manager, sysio::chain::config::active_name)},
         mutable_variant_object()
                                ("from",    from)
                                ("to",      to )
                                ("quantity", amount)
                                ("memo", "")
                                );
   }

   fc::variant get_stats( const string& symbolname ) {
      auto symb = sysio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;
      vector<char> data = get_row_by_account( "sysio.token"_n, name(symbol_code), "stat"_n, name(symbol_code) );
      return data.empty() ? fc::variant() : token_abi_ser.binary_to_variant( "currency_stats", data, abi_serializer::create_yield_function( abi_serializer_max_time ) );
   }

   asset get_token_supply() {
      return get_stats("4," CORE_SYMBOL_NAME)["supply"].as<asset>();
   }

   fc::variant get_global_state() {
      vector<char> data = get_row_by_account( sysio::chain::config::system_account_name, sysio::chain::config::system_account_name, "global"_n, "global"_n );
      if (data.empty()) std::cout << "\nData is empty\n" << std::endl;
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "sysio_global_state", data, abi_serializer::create_yield_function( abi_serializer_max_time ) );

   }

   abi_serializer initialize_multisig() {
      abi_serializer msig_abi_ser;
      {
         create_account( "sysio.msig"_n, sysio::chain::config::system_account_name, false, false, false, false );
         produce_block();

         set_privileged("sysio.msig"_n);
         set_code( "sysio.msig"_n, test_contracts::sysio_msig_wasm() );
         set_abi( "sysio.msig"_n, test_contracts::sysio_msig_abi() );

         produce_blocks();
         const auto* accnt = control->find_account_metadata( "sysio.msig"_n );
         BOOST_REQUIRE(accnt != nullptr);
         abi_def msig_abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, msig_abi), true);
         msig_abi_ser.set_abi(std::move(msig_abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
      }
      return msig_abi_ser;
   }

   abi_serializer abi_ser;
   abi_serializer token_abi_ser;
};

inline uint64_t M( const string& sys_str ) {
   return core_from_string( sys_str ).get_amount();
}

}
