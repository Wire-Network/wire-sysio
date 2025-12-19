#include <boost/test/unit_test.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/variant_object.hpp>
#include <contracts.hpp>
#include <test_contracts.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/log/logger.hpp>
#include <cstdlib>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::chain_apis;
using namespace sysio::testing;
using namespace sysio::chain_apis;
using namespace fc;

using mvo = fc::mutable_variant_object;

class chain_plugin_tester : public validating_tester {
public:

    action_result push_action( const account_name& signer, const action_name &name, const variant_object &data, bool auth = true ) {
         string action_type_name = abi_ser.get_action_type(name);

         action act({}, sysio::chain::config::system_account_name, name,
                    abi_ser.variant_to_binary(action_type_name, data,
                                              abi_serializer::create_yield_function(abi_serializer_max_time)));

         return base_tester::push_action( std::move(act), (auth ? signer : signer == "bob111111111"_n ? "alice1111111"_n : "bob111111111"_n).to_uint64_t() );
    }

    void transfer( name from, name to, const asset& amount, name manager = sysio::chain::config::system_account_name ) {
      base_tester::push_action( "sysio.token"_n, "transfer"_n, manager, mutable_variant_object()
                                ("from",    from)
                                ("to",      to )
                                ("quantity", amount)
                                ("memo", "")
                                );
    }

    asset get_balance( const account_name& act ) {
      vector<char> data = get_row_by_account( "sysio.token"_n, act, "accounts"_n, name(symbol(CORE_SYMBOL).to_symbol_code().value) );
      return data.empty() ? asset(0, symbol(CORE_SYMBOL)) : token_abi_ser.binary_to_variant("account", data, abi_serializer::create_yield_function( abi_serializer_max_time ))["balance"].as<asset>();
    }

    void create_currency( name contract, name manager, asset maxsupply ) {
        auto act =  mutable_variant_object()
                ("issuer",       manager )
                ("maximum_supply", maxsupply );

        base_tester::push_action(contract, "create"_n, contract, act );
    }

    void issue( name to, const asset& amount, name manager = sysio::chain::config::system_account_name ) {
        base_tester::push_action( "sysio.token"_n, "issue"_n, manager, mutable_variant_object()
                ("to",      to )
                ("quantity", amount )
                ("memo", "")
        );
    }
    void setup_system_accounts(){
       create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
                         "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n, "sysio.rex"_n });

       set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );
       set_abi( "sysio.token"_n, test_contracts::sysio_token_abi() );
       set_privileged("sysio.token"_n);

       {
           const auto& accnt = control->find_account_metadata( "sysio.token"_n );
           FC_ASSERT(accnt != nullptr, "sysio.token account metadata not found");
           abi_def abi;
           BOOST_CHECK_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
           token_abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
       }

       create_currency( "sysio.token"_n, sysio::chain::config::system_account_name, core_from_string("10000000000.0000") );
       issue(sysio::chain::config::system_account_name,      core_from_string("1000000000.0000"));
       BOOST_CHECK_EQUAL( core_from_string("1000000000.0000"), get_balance( name("sysio") ) );

       set_code( sysio::chain::config::system_account_name, test_contracts::sysio_system_wasm() );
       set_abi( sysio::chain::config::system_account_name, test_contracts::sysio_system_abi() );

       base_tester::push_action(sysio::chain::config::system_account_name, "init"_n,
                                sysio::chain::config::system_account_name,  mutable_variant_object()
                                        ("version", 0)
                                        ("core", symbol(CORE_SYMBOL).to_string()));

       {
           const auto& accnt = control->find_account_metadata( sysio::chain::config::system_account_name );
           FC_ASSERT(accnt != nullptr, "sysio account metadata not found");
           abi_def abi;
           BOOST_CHECK_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
           abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
       }

    }

    read_only::get_account_results get_account_info(const account_name acct){
       auto account_object = control->get_account(acct);
       read_only::get_account_params params = { account_object.name };
       std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
       chain_apis::read_only plugin(*(control.get()), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
       auto res =   plugin.get_account(params, fc::time_point::maximum())();
       BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(res));
       return std::get<chain_apis::read_only::get_account_results>(std::move(res));
    }

   abi_serializer abi_ser;
   abi_serializer token_abi_ser;
};

BOOST_AUTO_TEST_SUITE(test_chain_plugin_tests)

BOOST_FIXTURE_TEST_CASE(account_results_total_resources_test, chain_plugin_tester) { try {
    produce_blocks(10);
    setup_system_accounts();
    produce_blocks();
    // creates account with ROA policy values
    //   ("net_weight", "0.0010 SYS")
    //   ("cpu_weight", "0.0010 SYS")
    //   ("ram_weight", "4.0000 SYS")
    create_account("alice1111111"_n, sysio::chain::config::system_account_name);
    transfer(name("sysio"), name("alice1111111"), core_from_string("650000000.0000"), name("sysio") );

    read_only::get_account_results results = get_account_info(name("alice1111111"));
    BOOST_CHECK(results.total_resources.get_type() != fc::variant::type_id::null_type);
    BOOST_CHECK_EQUAL(core_from_string("0.0010"), results.total_resources["net_weight"].as<asset>());
    BOOST_CHECK_EQUAL(core_from_string("0.0010"), results.total_resources["cpu_weight"].as<asset>());
    BOOST_CHECK_EQUAL(results.total_resources["ram_bytes"].as_int64(), 4161144); // 40000*104+newaccount_ram(1144)

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
