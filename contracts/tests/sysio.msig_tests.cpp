#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/wast_to_wasm.hpp>

#include <fc/variant_object.hpp>
#include "contracts.hpp"
#include "test_symbol.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

class sysio_msig_tester : public tester {
public:
   sysio_msig_tester() {
      create_accounts( { "sysio.msig"_n, "alice"_n, "bob"_n, "carol"_n } );
      produce_block();

      set_code( "sysio.msig"_n, contracts::msig_wasm() );
      set_abi( "sysio.msig"_n, contracts::msig_abi().data() );
      set_privileged("sysio.msig"_n);

      produce_blocks();
      const auto* accnt = control->find_account_metadata( "sysio.msig"_n );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   void create_currency( name contract, name manager, asset maxsupply ) {
      auto act =  mutable_variant_object()
         ("issuer",       manager )
         ("maximum_supply", maxsupply );

      base_tester::push_action(contract, "create"_n, contract, act );
   }
   void issue( name to, const asset& amount, name manager = config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "issue"_n, manager, mutable_variant_object()
                                ("to",      to )
                                ("quantity", amount )
                                ("memo", "")
                                );
   }
   void transfer( name from, name to, const string& amount, name manager = config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "transfer"_n, manager, mutable_variant_object()
                                ("from",    from)
                                ("to",      to )
                                ("quantity", asset::from_string(amount) )
                                ("memo", "")
                                );
   }
   asset get_balance( const account_name& act ) {
      //return get_currency_balance( config::system_account_name, symbol(CORE_SYMBOL), act );
      //temporary code. current get_currency_balancy uses table name "accounts"_n from currency.h
      //generic_currency table name is "account"_n.
      const auto& db  = control->db();
      share_type result = 0;

      auto key = sysio::chain::make_kv_scoped_key(act, symbol(CORE_SYM).to_symbol_code());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple("sysio.token"_n, sysio::chain::compute_table_id("accounts"_n.to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         // balance is the first field in the serialization
         fc::datastream<const char *> ds(it->value.data(), it->value.size());
         fc::raw::unpack(ds, result);
      }
      return asset( result, symbol(CORE_SYM) );
   }

   transaction_trace_ptr push_action( const account_name& signer, const action_name& name, const variant_object& data, bool auth = true ) {
      vector<account_name> accounts;
      if( auth )
         accounts.push_back( signer );
      auto trace = base_tester::push_action( "sysio.msig"_n, name, accounts, data );
      produce_block();
      BOOST_REQUIRE_EQUAL( true, chain_has_transaction(trace->id) );
      return trace;

      /*
         string action_type_name = abi_ser.get_action_type(name);

         action act;
         act.account = "sysio.msig"_n;
         act.name = name;
         act.data = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         //std::cout << "test:\n" << fc::to_hex(act.data.data(), act.data.size()) << " size = " << act.data.size() << std::endl;

         return base_tester::push_action( std::move(act), auth ? uint64_t(signer) : 0 );
      */
   }

   transaction reqauth( account_name from, const vector<permission_level>& auths, const fc::microseconds& max_serialization_time );

   void check_traces(transaction_trace_ptr trace, std::vector<std::map<std::string, name>> res);

   abi_serializer abi_ser;
};

transaction sysio_msig_tester::reqauth( account_name from, const vector<permission_level>& auths, const fc::microseconds& max_serialization_time ) {
   fc::variants v;
   for ( auto& level : auths ) {
      v.push_back(fc::mutable_variant_object()
                  ("actor", level.actor)
                  ("permission", level.permission)
      );
   }
   fc::variant pretty_trx = fc::mutable_variant_object()
      ("expiration", "2025-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", 0)
      ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::system_account_name))
               ("name", "reqauth")
               ("authorization", v)
               ("data", fc::mutable_variant_object() ("from", from) )
               })
      );
   transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function(max_serialization_time));
   return trx;
}

void sysio_msig_tester::check_traces(transaction_trace_ptr trace, std::vector<std::map<std::string, name>> res) {

   BOOST_REQUIRE( bool(trace) );
   BOOST_REQUIRE( !!trace->receipt );
   BOOST_REQUIRE_EQUAL( res.size(), trace->action_traces.size() );

   for (size_t i = 0; i < res.size(); i++) {
      auto cur_action = trace->action_traces.at(i);
      BOOST_REQUIRE_EQUAL( res[i]["receiver"], cur_action.receiver );
      BOOST_REQUIRE_EQUAL( res[i]["act_name"], cur_action.act.name );
   }
}

BOOST_AUTO_TEST_SUITE(sysio_msig_tests)

BOOST_FIXTURE_TEST_CASE( propose_approve_execute, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, abi_serializer_max_time );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", vector<permission_level>{{ "alice"_n, config::active_name }})
   );

   //fail to execute before approval
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "exec"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("executer",      "alice")
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );

   //approve and execute
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );

   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                             ("proposer",      "alice")
                                             ("proposal_name", "first")
                                             ("executer",      "alice")
   );

   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "reqauth"_n}}
                        } );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( propose_approve_unapprove, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, abi_serializer_max_time );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", vector<permission_level>{{ "alice"_n, config::active_name }})
   );

   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );

   push_action( "alice"_n, "unapprove"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );

   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "exec"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("executer",      "alice")
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( propose_approve_by_two, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, vector<permission_level>{ { "alice"_n, config::active_name }, { "bob"_n, config::active_name } }, abi_serializer_max_time );
   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", vector<permission_level>{ { "alice"_n, config::active_name }, { "bob"_n, config::active_name } })
   );

   //approve by alice
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );

   //fail because approval by bob is missing

   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "exec"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("executer",      "alice")
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );

   //approve by bob and execute
   push_action( "bob"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "bob"_n, config::active_name })
   );

   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                            ("proposer",      "alice")
                                            ("proposal_name", "first")
                                            ("executer",      "alice")
   );

   check_traces( trace, {
                     {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                     {{"receiver", config::system_account_name}, {"act_name", "reqauth"_n}}
                     } );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( propose_with_wrong_requested_auth, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, vector<permission_level>{ { "alice"_n, config::active_name },  { "bob"_n, config::active_name } }, abi_serializer_max_time );
   //try with not enough requested auth
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "propose"_n, mvo()
                                             ("proposer",      "alice")
                                             ("proposal_name", "third")
                                             ("trx",           trx)
                                             ("requested", vector<permission_level>{ { "alice"_n, config::active_name } } )
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( big_transaction, sysio_msig_tester ) try {
   //change `default_max_inline_action_size` to 512 KB
   sysio::chain::chain_config params = control->get_global_properties().configuration;
   params.max_inline_action_size = 512 * 1024;
   base_tester::push_action( config::system_account_name, "setparams"_n, config::system_account_name, mutable_variant_object()
                              ("params", params) );

   produce_blocks();

   vector<permission_level> perm = { { "alice"_n, config::active_name }, { "bob"_n, config::active_name } };
   auto wasm = contracts::system_wasm();

   fc::variant pretty_trx = fc::mutable_variant_object()
      ("expiration", "2025-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", 0)
      ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::system_account_name))
               ("name", "setcode")
               ("authorization", perm)
               ("data", fc::mutable_variant_object()
                ("account", "alice")
                ("vmtype", 0)
                ("vmversion", 0)
                ("code", bytes( wasm.begin(), wasm.end() ))
               )
               })
      );

   transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function(abi_serializer_max_time));

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", perm)
   );

   //approve by alice
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );
   //approve by bob and execute
   push_action( "bob"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "bob"_n, config::active_name })
   );

   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                            ("proposer",      "alice")
                                            ("proposal_name", "first")
                                            ("executer",      "alice")
   );

   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "setcode"_n}}
                        } );
} FC_LOG_AND_RETHROW()



BOOST_FIXTURE_TEST_CASE( update_system_contract_all_approve, sysio_msig_tester ) try {

   // required to set up the link between (sysio active) and (sysio.prods active)
   //
   //                  sysio active
   //                       |
   //             sysio.prods active (2/3 threshold)
   //             /         |        \             <--- implicitly updated in onblock action
   // alice active     bob active   carol active

   set_authority(
      config::system_account_name,
      config::active_name,
      authority( 1,
                 vector<key_weight>{{get_private_key(config::system_account_name, "active").get_public_key(), 1}},
                 vector<permission_level_weight>{{{"sysio.prods"_n, config::active_name}, 1}}
      ),
      config::owner_name,
      {{config::system_account_name, config::active_name}},
      {get_private_key(config::system_account_name, "active")}
   );

   set_producers( {"alice"_n,"bob"_n,"carol"_n} );
   produce_blocks(50);

   create_accounts( { "sysio.token"_n } );
   set_code( "sysio.token"_n, contracts::token_wasm() );
   set_abi( "sysio.token"_n, contracts::token_abi().data() );
   set_privileged("sysio.token"_n);

   create_currency( "sysio.token"_n, config::system_account_name, core_sym::from_string("10000000000.0000") );
   issue(config::system_account_name, core_sym::from_string("1000000000.0000"));
   BOOST_REQUIRE_EQUAL( core_sym::from_string("1000000000.0000"),
                        get_balance(config::system_account_name) + get_balance("sysio.ramfee"_n) + get_balance("sysio.stake"_n) + get_balance("sysio.ram"_n) );

   set_code( config::system_account_name, contracts::system_wasm() );
   set_abi( config::system_account_name, contracts::system_abi().data() );
   base_tester::push_action( config::system_account_name, "init"_n,
                             config::system_account_name,  mutable_variant_object()
                              ("version", 0)
                              ("core", CORE_SYM_STR)
   );
   produce_blocks();
   create_account( "alice1111111"_n, "sysio"_n );
   create_account( "bob111111111"_n, "sysio"_n );
   create_account( "carol1111111"_n, "sysio"_n );

   BOOST_REQUIRE_EQUAL( core_sym::from_string("1000000000.0000"),
                        get_balance(config::system_account_name) + get_balance("sysio.ramfee"_n) + get_balance("sysio.stake"_n) + get_balance("sysio.ram"_n) );

   vector<permission_level> perm = { { "alice"_n, config::active_name }, { "bob"_n, config::active_name },
      {"carol"_n, config::active_name} };

   vector<permission_level> action_perm = {{"sysio"_n, config::active_name}};

   auto wasm = contracts::util::reject_all_wasm();

   fc::variant pretty_trx = fc::mutable_variant_object()
      ("expiration", "2025-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", 0)
      ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::system_account_name))
               ("name", "setcode")
               ("authorization", action_perm)
               ("data", fc::mutable_variant_object()
                ("account", name(config::system_account_name))
                ("vmtype", 0)
                ("vmversion", 0)
                ("code", bytes( wasm.begin(), wasm.end() ))
               )
               })
      );

   transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function(abi_serializer_max_time));

   // propose action
   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", perm)
   );

   //approve by alice
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );
   //approve by bob
   push_action( "bob"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "bob"_n, config::active_name })
   );
   //approve by carol
   push_action( "carol"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "carol"_n, config::active_name })
   );

   // execute by alice to replace the sysio system contract
   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                             ("proposer",      "alice")
                                             ("proposal_name", "first")
                                             ("executer",      "alice")
   );

   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "setcode"_n}}
                        } );

   // can't create account because system contract was replaced by the reject_all contract
   BOOST_REQUIRE_EXCEPTION( create_account( "alice1111112"_n, "sysio"_n ),
                            sysio_assert_message_exception, sysio_assert_message_is("rejecting all actions")

   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( update_system_contract_major_approve, sysio_msig_tester ) try {

   // set up the link between (sysio active) and (sysio.prods active)
   set_authority(
      config::system_account_name,
      config::active_name,
      authority( 1,
                 vector<key_weight>{{get_private_key(config::system_account_name, "active").get_public_key(), 1}},
                 vector<permission_level_weight>{{{"sysio.prods"_n, config::active_name}, 1}}
      ),
      config::owner_name,
      {{config::system_account_name, config::active_name}},
      {get_private_key(config::system_account_name, "active")}
   );

   create_accounts( { "apple"_n } );
   set_producers( {"alice"_n,"bob"_n,"carol"_n, "apple"_n} );
   produce_blocks(50);

   create_accounts( { "sysio.token"_n } );
   set_code( "sysio.token"_n, contracts::token_wasm() );
   set_abi( "sysio.token"_n, contracts::token_abi().data() );
   set_privileged("sysio.token"_n);

   create_currency( "sysio.token"_n, config::system_account_name, core_sym::from_string("10000000000.0000") );
   issue(config::system_account_name, core_sym::from_string("1000000000.0000"));
   BOOST_REQUIRE_EQUAL( core_sym::from_string("1000000000.0000"), get_balance( config::system_account_name ) );

   set_code( config::system_account_name, contracts::system_wasm() );
   set_abi( config::system_account_name, contracts::system_abi().data() );
   base_tester::push_action( config::system_account_name, "init"_n,
                             config::system_account_name,  mutable_variant_object()
                                 ("version", 0)
                                 ("core", CORE_SYM_STR)
   );
   produce_blocks();

   create_account( "alice1111111"_n, "sysio"_n );
   create_account( "bob111111111"_n, "sysio"_n );
   create_account( "carol1111111"_n, "sysio"_n );

   BOOST_REQUIRE_EQUAL( core_sym::from_string("1000000000.0000"),
                        get_balance(config::system_account_name) + get_balance("sysio.ramfee"_n) + get_balance("sysio.stake"_n) + get_balance("sysio.ram"_n) );

   vector<permission_level> perm = { { "alice"_n, config::active_name }, { "bob"_n, config::active_name },
      {"carol"_n, config::active_name}, {"apple"_n, config::active_name}};

   vector<permission_level> action_perm = {{"sysio"_n, config::active_name}};

   auto wasm = contracts::util::reject_all_wasm();

   fc::variant pretty_trx = fc::mutable_variant_object()
      ("expiration", "2025-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", 0)
      ("actions", fc::variants({
            fc::mutable_variant_object()
               ("account", name(config::system_account_name))
               ("name", "setcode")
               ("authorization", action_perm)
               ("data", fc::mutable_variant_object()
                ("account", name(config::system_account_name))
                ("vmtype", 0)
                ("vmversion", 0)
                ("code", bytes( wasm.begin(), wasm.end() ))
               )
               })
      );

   transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer::create_yield_function(abi_serializer_max_time));

   // propose action
   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", perm)
   );

   //approve by alice
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );
   //approve by bob
   push_action( "bob"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "bob"_n, config::active_name })
   );

   // not enough approvers
   BOOST_REQUIRE_EXCEPTION(
      push_action( "alice"_n, "exec"_n, mvo()
                     ("proposer",      "alice")
                     ("proposal_name", "first")
                     ("executer",      "alice")
      ),
      sysio_assert_message_exception, sysio_assert_message_is("transaction authorization failed")
   );

   //approve by apple
   push_action( "apple"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "apple"_n, config::active_name })
   );
   // execute by another producer different from proposer
   transaction_trace_ptr trace = push_action( "apple"_n, "exec"_n, mvo()
                                             ("proposer",      "alice")
                                             ("proposal_name", "first")
                                             ("executer",      "apple")
   );

   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "setcode"_n}}
                        } );

   // can't create account because system contract was replaced by the reject_all contract
   BOOST_REQUIRE_EXCEPTION( create_account( "alice1111112"_n, "sysio"_n ),
                            sysio_assert_message_exception, sysio_assert_message_is("rejecting all actions")

   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( propose_approve_invalidate, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, abi_serializer_max_time );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", vector<permission_level>{{ "alice"_n, config::active_name }})
   );

   //fail to execute before approval
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "exec"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("executer",      "alice")
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );

   //approve
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );

   //invalidate
   push_action( "alice"_n, "invalidate"_n, mvo()
                  ("account",      "alice")
   );

   //fail to execute after invalidation
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "exec"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("executer",      "alice")
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( propose_invalidate_approve, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, abi_serializer_max_time );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", vector<permission_level>{{ "alice"_n, config::active_name }})
   );

   //fail to execute before approval
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "exec"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("executer",      "alice")
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("transaction authorization failed")
   );

   //invalidate
   push_action( "alice"_n, "invalidate"_n, mvo()
                  ("account",      "alice")
   );

   //approve
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );

   transaction_trace_ptr trace = push_action( "bob"_n, "exec"_n, mvo()
                                            ("proposer",      "alice")
                                            ("proposal_name", "first")
                                            ("executer",      "bob")
   );

   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "reqauth"_n}}
                        } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( approve_with_hash, sysio_msig_tester ) try {
   auto trx = reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, abi_serializer_max_time );
   auto trx_hash = fc::sha256::hash( trx );
   auto not_trx_hash = fc::sha256::hash( trx_hash );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx)
                  ("requested", vector<permission_level>{{ "alice"_n, config::active_name }})
   );

   //fail to approve with incorrect hash
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "approve"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("level",         permission_level{ "alice"_n, config::active_name })
                                          ("proposal_hash", not_trx_hash)
                            ),
                            sysio::chain::crypto_api_exception,
                            fc_exception_message_is("hash mismatch")
   );

   //approve and execute
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("level",         permission_level{ "alice"_n, config::active_name })
                  ("proposal_hash", trx_hash)
   );

   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                            ("proposer",      "alice")
                                            ("proposal_name", "first")
                                            ("executer",      "alice")
   );
   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "reqauth"_n}}
                        } );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( switch_proposal_and_fail_approve_with_hash, sysio_msig_tester ) try {
   auto trx1 = reqauth( "alice"_n, {permission_level{"alice"_n, config::active_name}}, abi_serializer_max_time );
   auto trx1_hash = fc::sha256::hash( trx1 );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx1)
                  ("requested", vector<permission_level>{{ "alice"_n, config::active_name }})
   );

   auto trx2 = reqauth( "alice"_n,
                       { permission_level{"alice"_n, config::active_name},
                         permission_level{"alice"_n, config::owner_name}  },
                       abi_serializer_max_time );

   push_action( "alice"_n, "cancel"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("canceler",       "alice")
   );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "first")
                  ("trx",           trx2)
                  ("requested", vector<permission_level>{ { "alice"_n, config::active_name },
                                                          { "alice"_n, config::owner_name } })
   );

   //fail to approve with hash meant for old proposal
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "approve"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "first")
                                          ("level",         permission_level{ "alice"_n, config::active_name })
                                          ("proposal_hash", trx1_hash)
                            ),
                            sysio::chain::crypto_api_exception,
                            fc_exception_message_is("hash mismatch")
   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( sendinline, sysio_msig_tester ) try {
   create_accounts( {"sendinline"_n} );
   set_code( "sendinline"_n, system_contracts::testing::test_contracts::sendinline_wasm() );
   set_abi( "sendinline"_n, system_contracts::testing::test_contracts::sendinline_abi().data() );

   create_accounts( {"wrongcon"_n} );
   set_code( "wrongcon"_n, system_contracts::testing::test_contracts::sendinline_wasm() );
   set_abi( "wrongcon"_n, system_contracts::testing::test_contracts::sendinline_abi().data() );
   produce_blocks();

   action act = get_action( config::system_account_name, "reqauth"_n, {}, mvo()("from", "alice"));

   BOOST_REQUIRE_EXCEPTION( base_tester::push_action( "sendinline"_n, "send"_n, "bob"_n, mvo()
                                                       ("contract", "sysio")
                                                       ("action_name", "reqauth")
                                                       ("auths", std::vector<permission_level>{ {"alice"_n, config::active_name} })
                                                       ("payload", act.data)
                          ),
                          unsatisfied_authorization,
                          fc_exception_message_starts_with("transaction declares authority")
   );

   base_tester::push_action(config::system_account_name, "updateauth"_n, "alice"_n, mvo()
                              ("account", "alice")
                              ("permission", "perm")
                              ("parent", "active")
                              ("auth",  authority{ 1, {}, {permission_level_weight{ {"sendinline"_n, config::active_name}, 1}} })
   );
   produce_blocks();

   base_tester::push_action( config::system_account_name, "linkauth"_n, "alice"_n, mvo()
                              ("account", "alice")
                              ("code", "sysio")
                              ("type", "reqauth")
                              ("requirement", "perm")
   );
   produce_blocks();

   transaction_trace_ptr trace = base_tester::push_action( "sendinline"_n, "send"_n, "bob"_n, mvo()
                                                            ("contract", "sysio")
                                                            ("action_name", "reqauth")
                                                            ("auths", std::vector<permission_level>{ {"alice"_n, "perm"_n} })
                                                            ("payload", act.data)
   );
   check_traces( trace, {
                        {{"receiver", "sendinline"_n}, {"act_name", "send"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "reqauth"_n}}
                        } );

   produce_blocks();

   action approve_act = get_action("sysio.msig"_n, "approve"_n, {}, mvo()
                                    ("proposer", "bob")
                                    ("proposal_name", "first")
                                    ("level", permission_level{"sendinline"_n, config::active_name})
   );

   transaction trx = reqauth( "alice"_n, {permission_level{"alice"_n, "perm"_n}}, abi_serializer_max_time );

   base_tester::push_action( "sysio.msig"_n, "propose"_n, "bob"_n, mvo()
                              ("proposer", "bob")
                              ("proposal_name", "first")
                              ("trx", trx)
                              ("requested", std::vector<permission_level>{{ "sendinline"_n, config::active_name}})
   );
   produce_blocks();

   base_tester::push_action( "sendinline"_n, "send"_n, "bob"_n, mvo()
                              ("contract", "sysio.msig")
                              ("action_name", "approve")
                              ("auths", std::vector<permission_level>{{"sendinline"_n, config::active_name}})
                              ("payload", approve_act.data)
   );
   produce_blocks();

   trace = base_tester::push_action( "sysio.msig"_n, "exec"_n, "bob"_n, mvo()
                                          ("proposer", "bob")
                                          ("proposal_name", "first")
                                          ("executer", "bob")
   );

   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n}, {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name}, {"act_name", "reqauth"_n}}
                        } );

} FC_LOG_AND_RETHROW()


// ---------------------------------------------------------------------------
// Chunked-storage tests for sysio.msig
//
// These exercise the path where a proposal's serialized inner transaction
// exceeds the per-row KV value limit (default 256 KiB) and the contract
// internally splits it across rows of the `propchunks` table. The chunk
// threshold inside the contract is `proposal_chunk_size = 200 * 1024`, so
// every test below builds an inner trx larger than that.
//
// We construct the large trx by stacking two `setcode` actions whose `code`
// field carries the full sysio.system wasm (~134 KiB). Two of those puts the
// serialized inner trx well above 200 KiB, while keeping each individual
// dispatched action under `max_inline_action_size` so `exec` succeeds.
// ---------------------------------------------------------------------------

namespace {
   // Build an inner transaction whose serialized form is larger than the contract's
   // chunk threshold. Two setcode actions to two different accounts, each carrying
   // the full sysio.system wasm. Total ≈ 270 KiB, which forces chunking.
   transaction build_chunking_trx(const vector<permission_level>& perm,
                                  sysio_msig_tester& t)
   {
      auto wasm = contracts::system_wasm();

      auto make_setcode = [&](const char* account_name) {
         return fc::mutable_variant_object()
            ("account", name(config::system_account_name))
            ("name",    "setcode")
            ("authorization", perm)
            ("data", fc::mutable_variant_object()
               ("account",   account_name)
               ("vmtype",    0)
               ("vmversion", 0)
               ("code",      bytes( wasm.begin(), wasm.end() )));
      };

      fc::variant pretty_trx = fc::mutable_variant_object()
         ("expiration", "2025-01-01T00:30")
         ("ref_block_num", 2)
         ("ref_block_prefix", 3)
         ("max_net_usage_words", 0)
         ("max_cpu_usage_ms", 0)
         ("delay_sec", 0)
         ("actions", fc::variants({ make_setcode("alice"), make_setcode("bob") }));

      transaction trx;
      abi_serializer::from_variant(pretty_trx, trx, t.get_resolver(),
                                   abi_serializer::create_yield_function(base_tester::abi_serializer_max_time));
      return trx;
   }
}

// End-to-end: propose a >200 KiB inner trx, approve from both signers (one of
// them with the optional `proposal_hash` arg), then exec. This implicitly
// tests three load-bearing paths in one: (1) propose chunks the blob, (2)
// approve verifies the precomputed `trx_hash` for a chunked proposal — there
// is no inline `packed_transaction` to re-hash from, so this is the only path
// where the stored hash is load-bearing, (3) exec reassembles the chunks
// before parsing and dispatching the inline actions.
BOOST_FIXTURE_TEST_CASE( big_transaction_chunked, sysio_msig_tester ) try {
   vector<permission_level> perm = { { "alice"_n, config::active_name },
                                     { "bob"_n,   config::active_name } };
   transaction trx = build_chunking_trx(perm, *this);
   const auto trx_hash     = fc::sha256::hash( trx );
   const auto not_trx_hash = fc::sha256::hash( trx_hash );

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("trx",           trx)
                  ("requested",     perm)
   );

   // Wrong hash must be rejected against the contract's stored `trx_hash` —
   // exercises the chunked-path branch in approve.
   BOOST_REQUIRE_EXCEPTION( push_action( "alice"_n, "approve"_n, mvo()
                                          ("proposer",      "alice")
                                          ("proposal_name", "chunkprop")
                                          ("level",         permission_level{ "alice"_n, config::active_name })
                                          ("proposal_hash", not_trx_hash)
                            ),
                            sysio_assert_message_exception,
                            sysio_assert_message_is("hash provided does not match stored proposal trx_hash")
   );

   // Correct hash must succeed — proves the precomputed trx_hash is being
   // stored on the proposal row and read back equal to fc::sha256::hash(trx).
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("level",         permission_level{ "alice"_n, config::active_name })
                  ("proposal_hash", trx_hash)
   );
   push_action( "bob"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("level",         permission_level{ "bob"_n,   config::active_name })
   );

   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                               ("proposer",      "alice")
                                               ("proposal_name", "chunkprop")
                                               ("executer",      "alice")
   );

   // exec must dispatch both setcode actions in order — proves the contract
   // assembled the chunks back into a single buffer before parsing.
   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n},                  {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name},     {"act_name", "setcode"_n}},
                        {{"receiver", config::system_account_name},     {"act_name", "setcode"_n}}
                        } );
} FC_LOG_AND_RETHROW()

// Verifies the read-only `getproposal` action: it must reassemble the chunked
// blob and return a `proposal` struct whose `packed_transaction` byte-equals
// the original serialized inner trx the user passed to `propose`. The trace's
// `action_traces[0].return_value` is the packed `proposal` struct returned by
// the action; we ABI-decode it via the cached msig abi_serializer and pull
// out the `packed_transaction` field for the byte-level equality check.
BOOST_FIXTURE_TEST_CASE( getproposal_read_only_returns_assembled, sysio_msig_tester ) try {
   vector<permission_level> perm = { { "alice"_n, config::active_name },
                                     { "bob"_n,   config::active_name } };
   transaction trx = build_chunking_trx(perm, *this);
   const bytes original_packed = fc::raw::pack(trx);

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("trx",           trx)
                  ("requested",     perm)
   );

   // Build and push a read-only transaction that calls sysio.msig::getproposal.
   action getproposal_act;
   getproposal_act.account = "sysio.msig"_n;
   getproposal_act.name    = "getproposal"_n;
   getproposal_act.authorization = {};
   getproposal_act.data = abi_ser.variant_to_binary(
      "get_proposal",
      mvo()("proposer", "alice")("proposal_name", "chunkprop"),
      abi_serializer::create_yield_function(abi_serializer_max_time));

   signed_transaction ro_trx;
   ro_trx.actions.push_back(getproposal_act);
   set_transaction_headers(ro_trx);
   auto trace = push_transaction( ro_trx, fc::time_point::maximum(),
                                  DEFAULT_BILLED_CPU_TIME_US, false,
                                  transaction_metadata::trx_type::read_only );

   BOOST_REQUIRE( bool(trace) );
   BOOST_REQUIRE_EQUAL( trace->action_traces.size(), 1u );
   const auto& return_value = trace->action_traces[0].return_value;
   BOOST_REQUIRE( !return_value.empty() );

   // Decode the action_results entry "proposal" and pull out packed_transaction.
   fc::variant decoded = abi_ser.binary_to_variant(
      "proposal", return_value,
      abi_serializer::create_yield_function(abi_serializer_max_time));

   const auto& obj = decoded.get_object();
   const auto& packed_var = obj["packed_transaction"];
   const bytes returned_packed = packed_var.as<bytes>();

   // The reassembled blob must byte-equal what we originally passed to propose.
   BOOST_REQUIRE_EQUAL( returned_packed.size(), original_packed.size() );
   BOOST_REQUIRE( std::equal(returned_packed.begin(), returned_packed.end(), original_packed.begin()) );

   // chunk_count should be > 0 since the proposal exceeded the threshold.
   const uint32_t chunk_count = obj["chunk_count"].as<uint32_t>();
   BOOST_REQUIRE_GT( chunk_count, 0u );

   // total_size should match the original blob length.
   const uint32_t total_size = obj["total_size"].as<uint32_t>();
   BOOST_REQUIRE_EQUAL( total_size, original_packed.size() );
} FC_LOG_AND_RETHROW()

// Cancel must erase every `propchunks` row associated with the proposal so
// the proposer is not billed for orphaned RAM and the same proposal_name can
// be reused. We verify by re-proposing under the same name after cancel: this
// would fail with "proposal with the same name exists" if the parent row was
// not removed, and would silently leave dead chunks if the chunk-cleanup were
// missing — exec on the second proposal would then read stale chunk data.
BOOST_FIXTURE_TEST_CASE( cancel_chunked_proposal_cleans_chunks, sysio_msig_tester ) try {
   vector<permission_level> perm = { { "alice"_n, config::active_name },
                                     { "bob"_n,   config::active_name } };
   transaction trx = build_chunking_trx(perm, *this);

   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("trx",           trx)
                  ("requested",     perm)
   );

   // Proposer can cancel their own proposal at any time without waiting for expiration.
   push_action( "alice"_n, "cancel"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("canceler",      "alice")
   );

   // Re-propose under the same name. Succeeds only if the parent proposal row
   // and all of its chunk rows were erased by cancel.
   push_action( "alice"_n, "propose"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("trx",           trx)
                  ("requested",     perm)
   );

   // And the new proposal must be exec-able end-to-end with no contamination
   // from the prior chunks. If chunk cleanup left orphans, the chunktable.emplace
   // calls inside the second propose would have asserted "key already exists".
   push_action( "alice"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("level",         permission_level{ "alice"_n, config::active_name })
   );
   push_action( "bob"_n, "approve"_n, mvo()
                  ("proposer",      "alice")
                  ("proposal_name", "chunkprop")
                  ("level",         permission_level{ "bob"_n,   config::active_name })
   );
   transaction_trace_ptr trace = push_action( "alice"_n, "exec"_n, mvo()
                                               ("proposer",      "alice")
                                               ("proposal_name", "chunkprop")
                                               ("executer",      "alice")
   );
   check_traces( trace, {
                        {{"receiver", "sysio.msig"_n},                  {"act_name", "exec"_n}},
                        {{"receiver", config::system_account_name},     {"act_name", "setcode"_n}},
                        {{"receiver", config::system_account_name},     {"act_name", "setcode"_n}}
                        } );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
