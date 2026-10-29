#include <boost/test/unit_test.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/variant_object.hpp>
#include <contracts.hpp>
#include <test_contracts.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/log/logger.hpp>
#include <cstdlib>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::chain_apis;
using namespace sysio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

class chain_plugin_tester : public validating_tester {
public:

    action_result push_action( const account_name& signer, const action_name &name, const variant_object &data, bool auth = true ) {
         string action_type_name = abi_ser.get_action_type(name);

         action act({}, config::system_account_name, name,
                    abi_ser.variant_to_binary(action_type_name, data,
                                              abi_serializer::create_yield_function(abi_serializer_max_time)));

         return base_tester::push_action( std::move(act), (auth ? signer : signer == "bob111111111"_n ? "alice1111111"_n : "bob111111111"_n).to_uint64_t() );
    }

    void transfer( name from, name to, const asset& amount, name manager = config::system_account_name ) {
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

    void issue( name to, const asset& amount, name manager = config::system_account_name ) {
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

       create_currency( "sysio.token"_n, config::system_account_name, core_from_string("10000000000.0000") );
       issue(config::system_account_name,      core_from_string("1000000000.0000"));
       BOOST_CHECK_EQUAL( core_from_string("1000000000.0000"), get_balance( name("sysio") ) );

       set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
       set_abi( config::system_account_name, test_contracts::sysio_system_abi() );

       base_tester::push_action(config::system_account_name, "init"_n,
                                config::system_account_name,  mutable_variant_object()
                                        ("version", 0)
                                        ("core", symbol(CORE_SYMBOL).to_string()));

       {
           const auto& accnt = control->find_account_metadata( config::system_account_name );
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
    //   ("ram_weight", "10.0000 SYS")
    create_account("alice1111111"_n, config::system_account_name);
    transfer(name("sysio"), name("alice1111111"), core_from_string("650000000.0000"), name("sysio") );

    read_only::get_account_results results = get_account_info(name("alice1111111"));
    BOOST_CHECK(results.total_resources.get_type() != fc::variant::type_id::null_type);
    BOOST_CHECK_EQUAL(core_from_string("0.0010"), results.total_resources["net_weight"].as<asset>());
    BOOST_CHECK_EQUAL(core_from_string("0.0010"), results.total_resources["cpu_weight"].as<asset>());
    BOOST_CHECK_EQUAL(results.total_resources["ram_bytes"].as_int64(), 10401144); // 100000*104+newaccount_ram(1144)

} FC_LOG_AND_RETHROW() }

// Byte-identical compat between the variant-cb path (fc::variant +
// fc::json::to_string) and the streaming-cb path (fc::to_json_stream via
// reflector dispatch + per-type overrides for chain::name / asset / symbol).
// Pins the parity that get_account's add_api_stream registration depends on.
BOOST_FIXTURE_TEST_CASE(get_account_streaming_vs_variant_byte_identical, chain_plugin_tester) { try {
   produce_blocks(10);
   setup_system_accounts();
   produce_blocks();
   create_account("alice1111111"_n, config::system_account_name);
   transfer(name("sysio"), name("alice1111111"), core_from_string("650000000.0000"), name("sysio"));

   read_only::get_account_results results = get_account_info(name("alice1111111"));

   const std::string variant_path =
      fc::json::to_string(fc::variant(results), fc::time_point::maximum());
   const std::string stream_path = fc::to_json_string(results);

   BOOST_CHECK_EQUAL(variant_path, stream_path);
} FC_LOG_AND_RETHROW() }

// Byte-identical compat for the new get_block_stream direct path.  Builds a
// populated block (account creation + a transfer; both decode through the
// resolver-located sysio.token / system ABI), then compares
// `to_string(convert_block(block))` against the streaming
// `convert_block_stream(block, json_writer)` output.  Drives the
// abi_serializer::to_json_stream<signed_block> template -- any drift in the
// ABI-aware stream path's token order, quoting, or action data form would
// surface here.
BOOST_FIXTURE_TEST_CASE(get_block_streaming_vs_variant_byte_identical, chain_plugin_tester) { try {
   produce_blocks(10);
   setup_system_accounts();
   produce_blocks();
   create_account("alice1111111"_n, config::system_account_name);
   transfer(name("sysio"), name("alice1111111"), core_from_string("100.0000"), name("sysio"));
   produce_block();

   auto signed_block = control->fetch_block_by_number(control->head().block_num());
   BOOST_REQUIRE(signed_block);

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   read_only ro(*control, {}, {}, _tracked_votes,
                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   auto resolver_v = ro.get_block_serializers(signed_block, fc::microseconds::maximum());
   auto resolver_s = ro.get_block_serializers(signed_block, fc::microseconds::maximum());

   const fc::variant variant_block = ro.convert_block(signed_block, resolver_v);
   const std::string variant_path = fc::json::to_string(variant_block, fc::time_point::maximum());

   std::string stream_path;
   {
      fc::json_writer w(stream_path);
      ro.convert_block_stream(signed_block, resolver_s, w);
      BOOST_REQUIRE(w.balanced());
   }

   BOOST_CHECK_EQUAL(variant_path, stream_path);
} FC_LOG_AND_RETHROW() }

// get_finalizer_info absent-policy schema: both policy keys are ALWAYS present and emit an
// explicit JSON null when the policy does not exist (fc::nullable fields).  A fresh chain
// has an active policy (Savanna activates at genesis) and no pending policy, so this pins
// the disengaged shape end-to-end plus byte parity between the variant and streaming paths.
// A reflected std::optional would omit the pending key entirely -- the regression this
// guards against.
BOOST_FIXTURE_TEST_CASE(get_finalizer_info_absent_policy_emits_null, chain_plugin_tester) { try {
   produce_blocks(2);

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   read_only ro(*control, {}, {}, _tracked_votes,
                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   const read_only::get_finalizer_info_result result = ro.get_finalizer_info({}, fc::time_point::maximum());
   BOOST_REQUIRE(result.active_finalizer_policy.has_value());
   BOOST_REQUIRE(!result.pending_finalizer_policy.has_value());

   const std::string variant_path = fc::json::to_string(fc::variant(result), fc::time_point::maximum());
   const std::string stream_path  = fc::to_json_string(result);

   BOOST_CHECK_EQUAL(variant_path, stream_path);
   BOOST_CHECK(stream_path.find("\"pending_finalizer_policy\":null") != std::string::npos);
   BOOST_CHECK(stream_path.find("\"active_finalizer_policy\":{") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
