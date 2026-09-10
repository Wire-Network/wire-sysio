#include <sysio/chain/abi_serializer.hpp>
#include <sysio/testing/tester.hpp>


#include <fc/variant_object.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <contracts.hpp>
#include <cmath>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;
using namespace std;
using namespace boost::multiprecision;

using mvo = fc::mutable_variant_object;

class sysio_swap_tester : public tester {
public:

    sysio_swap_tester() {
        produce_blocks( 2 );

        create_accounts( { "alice"_n, "bob"_n, "carol"_n, "sysio.token"_n, "sysio.swap"_n,
          "wevotethefee"_n, "badtoken"_n, "anothertoken"_n } );
        produce_blocks( 2 );

        // sysio.token bills every row to the system account (ram_payer = "sysio"),
        // which only a privileged contract may do -- every account hosting the
        // token WASM needs the flag, exactly as sysio.token_tests does.
        set_code( "sysio.token"_n, contracts::token_wasm() );
        set_abi( "sysio.token"_n, contracts::token_abi().data() );
        set_privileged( "sysio.token"_n );
        set_code( "anothertoken"_n, contracts::token_wasm() );
        set_abi( "anothertoken"_n, contracts::token_abi().data() );
        set_privileged( "anothertoken"_n );

        set_code( "sysio.swap"_n, contracts::swap_wasm() );
        set_abi( "sysio.swap"_n, contracts::swap_abi().data() );

        set_code( "carol"_n, contracts::token_wasm() );
        set_abi( "carol"_n, contracts::token_abi().data() );
        set_privileged( "carol"_n );

        set_code( "badtoken"_n, contracts::util::badtoken_wasm() );
        set_abi( "badtoken"_n, contracts::util::badtoken_abi().data() );

        set_code( "wevotethefee"_n, contracts::util::wevotethefee_wasm() );
        set_abi( "wevotethefee"_n, contracts::util::wevotethefee_abi().data() );
        
        produce_blocks();

        const auto* accnt1 = control->find_account_metadata( "sysio.token"_n );
        abi_def abi1;
        BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt1->abi, abi1), true);
        abi_ser.set_abi(abi1, abi_serializer::create_yield_function(abi_serializer_max_time));
    }

    fc::variant get_balance( name smartctr, name user, name table, int64_t id, string struc) 
    {
        vector<char> data = get_row_by_account( smartctr, user, table, name(id) );
        return data.empty() ? fc::variant() : abi_ser.binary_to_variant( struc, data, abi_serializer::create_yield_function(abi_serializer_max_time));
    }

    action_result push_action( const account_name& smartctr, const account_name& signer, const action_name &name, const variant_object &data ) {
        string action_type_name = abi_ser.get_action_type(name);

        action act;
        act.account = smartctr;
        act.name    = name;
        act.data    = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function(abi_serializer_max_time));

        return base_tester::push_action( std::move(act), signer.to_uint64_t() );
    }
    action_result create( name contract, account_name issuer, asset maximum_supply ) {
        return push_action( contract, contract, "create"_n, mvo()
            ( "issuer", issuer)
            ( "maximum_supply", maximum_supply)
        );
    }
    action_result issue( name contract, account_name issuer, account_name to, asset quantity, string memo ) {
        return push_action( contract, issuer, "issue"_n, mvo()
            ( "to", to)
            ( "quantity", quantity)
            ( "memo", memo)
        );
    }
    action_result transfer( name contract, account_name from,
                    account_name to,
                    asset        quantity,
                    string       memo ) {
        return push_action( contract, from, "transfer"_n, mvo()
            ( "from", from)
            ( "to", to)
            ( "quantity", quantity)
            ( "memo", memo)
        );
    }
    action_result open( name owner, symbol sym, name ram_payer ) {
        return push_action( "sysio.swap"_n, owner, "open"_n, mvo()
            ( "owner", owner)
            ( "symbol", sym)
            ( "ram_payer", ram_payer)
        );
    }
    action_result openext( name user, name payer, extended_symbol ext_symbol ) {
        return push_action( "sysio.swap"_n, payer, "openext"_n, mvo()
            ( "user", user)
            ( "payer", payer)
            ( "ext_symbol", ext_symbol)
        );
    }
    action_result closeext ( const name user, const name to, const extended_symbol ext_symbol ){ 
        return push_action( "sysio.swap"_n, user, "closeext"_n, mvo()
            ( "user", user )
            ( "to", to )
            ( "ext_symbol", ext_symbol )
            ( "memo", "" )
        );
    }
    action_result withdraw(name user, name to, extended_asset to_withdraw) {
        return push_action( "sysio.swap"_n, user, "withdraw"_n, mvo()
          ( "user", user )
          ( "to", to )
          ( "to_withdraw", to_withdraw )
          ( "memo", "" )
        );
    }
    action_result inittoken( name user, symbol new_symbol, extended_asset initial_pool1,
      extended_asset initial_pool2, int initial_fee, name fee_contract){
        // inittoken bills the new stat row to `user`, so the action must carry the
        // user's sysio.payer permission in addition to the two active authorities.
        std::vector<permission_level> auths{ {user, config::sysio_payer_name},
                                             {user, config::active_name},
                                             {"sysio.swap"_n, config::active_name} };
        try {
        sysio::testing::base_tester::push_action( "sysio.swap"_n, "inittoken"_n, auths, mvo()
          ( "user", user)
          ("new_symbol", new_symbol)
          ("initial_pool1", initial_pool1)
          ("initial_pool2", initial_pool2) 
          ("initial_fee", initial_fee)
          ("fee_contract", fee_contract)
        , 100);
      } catch (const fc::exception& ex) {
         edump((ex.to_detail_string()));
         return error(ex.top_message());
      }
      return success();
    }
    action_result addliquidity(name user, asset to_buy, asset max_asset1, asset max_asset2) {
        return push_action( "sysio.swap"_n, user, "addliquidity"_n, mvo()
          ( "user", user )
          ( "to_buy", to_buy )
          ( "max_asset1", max_asset1)
          ( "max_asset2", max_asset2)
        );
    }
    action_result remliquidity(name user, asset to_sell,
      asset min_asset1, asset min_asset2) {
        return push_action( "sysio.swap"_n, user, "remliquidity"_n, mvo()
          ( "user", user )
          ( "to_sell", to_sell )
          ( "min_asset1", min_asset1)
          ( "min_asset2", min_asset2)
        );
    }
    action_result exchange( name user, symbol_code pair_token, extended_asset ext_asset_in, asset min_expected ) {
        return push_action( "sysio.swap"_n, user, "exchange"_n, mvo()
          ( "user", user )
          ( "pair_token", pair_token )
          ( "ext_asset_in", ext_asset_in )
          ( "min_expected", min_expected )
        );
    }
    action_result changefee( symbol_code pair_token, int newfee ) {
        return push_action( "sysio.swap"_n, "wevotethefee"_n, "changefee"_n, mvo()
          ( "pair_token", pair_token )
          ( "newfee", newfee )
        );
    }

    action_result openfeetable( name user, symbol_code pair_token ) {
        return push_action( "wevotethefee"_n, user, "openfeetable"_n, mvo()
          ( "user", user )
          ( "pair_token", pair_token )
        );
    }
    action_result closefeetable( symbol_code pair_token ) {
        return push_action( "wevotethefee"_n, "wevotethefee"_n, "closefeetable"_n, mvo()
          ( "pair_token", pair_token )
        );
    }
    action_result votefee( name user, symbol_code pair_token, int fee_voted ) {
        return push_action( "wevotethefee"_n, user, "votefee"_n, mvo()
          ( "user", user )
          ( "pair_token", pair_token )
          ( "fee_voted", fee_voted )
        );
    }
    action_result closevote( name user, symbol_code pair_token ) {
        return push_action( "wevotethefee"_n, user, "closevote"_n, mvo()
          ( "user", user )
          ( "pair_token", pair_token )
        );
    }
    action_result updatefee( name user, symbol_code pair_token ) {
      return push_action( "wevotethefee"_n, user, "updatefee"_n, mvo()
        ( "pair_token", pair_token )
      );
    }

    int64_t balance(name user, int64_t id) {
        auto _balance = get_balance("sysio.swap"_n, user, "evodexacnts"_n, id, "evodexaccount" );
        return to_int(fc::json::to_string(_balance["balance"]["quantity"], 
          fc::time_point(fc::time_point::now() + abi_serializer_max_time) ));
    }
    int64_t tok_balance(name user, int64_t id){
        auto _balance = get_balance("sysio.swap"_n, user, "accounts"_n, id, "account" );
        return to_int(fc::json::to_string(_balance["balance"], 
          fc::time_point(fc::time_point::now() + abi_serializer_max_time) ));
    }
    int64_t token_balance(name contract, name user, int64_t id){
        auto _balance = get_balance(contract, user, "accounts"_n, id, "account" );
        return to_int(fc::json::to_string(_balance["balance"], 
          fc::time_point(fc::time_point::now() + abi_serializer_max_time) ));
    }
    int64_t to_int(string in) {
        auto sub = in.substr(1,in.length()-2);
        return asset::from_string(sub).get_amount();
    }
    vector <int64_t> system_balance(int64_t id){
        auto sys_balance_json = get_balance("sysio.swap"_n, name(id), "stat"_n, id, "currency_stats" );   
        auto saldo1 = to_int(fc::json::to_string(sys_balance_json["pool1"]["quantity"], 
          fc::time_point(fc::time_point::now() + abi_serializer_max_time) ));
        auto saldo2 = to_int(fc::json::to_string(sys_balance_json["pool2"]["quantity"], 
          fc::time_point(fc::time_point::now() + abi_serializer_max_time) ));
        auto minted = to_int(fc::json::to_string(sys_balance_json["supply"], 
          fc::time_point(fc::time_point::now() + abi_serializer_max_time) ));
        vector <int64_t> ans = {saldo1, saldo2, minted};
        return ans;
    }
    bool is_increasing(vector <int64_t> v, vector <int64_t> w){
        using wide = boost::multiprecision::int256_t;
        wide x = wide(v.at(0)) * wide(v.at(1)) * wide(w.at(2)) * wide(w.at(2));
        wide y = wide(w.at(0)) * wide(w.at(1)) * wide(v.at(2)) * wide(v.at(2));
        return x <= y;
    }
    vector <int64_t> total(){
        auto EVO_value = symbol::from_string("4,EVO").to_symbol_code().value;
        auto ETUSD_value = symbol::from_string("3,ETUSD").to_symbol_code().value;
        int64_t total_eos =  balance("alice"_n, 0) + balance("bob"_n, 0) 
          + system_balance(EVO_value).at(0) + system_balance(ETUSD_value).at(0);
        int64_t total_voice = balance("alice"_n, 1) + balance("bob"_n, 1) 
          + system_balance(EVO_value).at(1);
        int64_t total_tusd = balance("alice"_n, 2) + balance("bob"_n, 2)
          + system_balance(ETUSD_value).at(1);
        vector <int64_t> ans = {total_eos, total_voice, total_tusd};
        return ans;
    }
    void create_tokens_and_issue() {
        BOOST_REQUIRE_EQUAL( success(), create( "sysio.token"_n, "alice"_n, asset::from_string("461168601842738.7903 EOS") ) );
        BOOST_REQUIRE_EQUAL( success(), create( "anothertoken"_n, "bob"_n, asset::from_string("461168601842738.7903 VOICE") ) );
        BOOST_REQUIRE_EQUAL( success(), create( "sysio.token"_n, "alice"_n, asset::from_string("46116860184273879.03 TUSD") ) );
        BOOST_REQUIRE_EQUAL( success(), issue( "sysio.token"_n, "alice"_n, "alice"_n, asset::from_string("461168601842738.7903 EOS"), "") );
        BOOST_REQUIRE_EQUAL( success(), issue( "anothertoken"_n, "bob"_n, "bob"_n, asset::from_string("461168601842738.7903 VOICE"), "") );
        BOOST_REQUIRE_EQUAL( success(), issue( "sysio.token"_n, "alice"_n, "alice"_n, asset::from_string("46116860184273879.03 TUSD"), "") );
    }
    void many_openext() {
        BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, extended_symbol{symbol::from_string("4,EOS"), "sysio.token"_n}) );
        BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, extended_symbol{symbol::from_string("4,VOICE"), "anothertoken"_n}) );
        BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, extended_symbol{symbol::from_string("2,TUSD"), "sysio.token"_n}) );
        BOOST_REQUIRE_EQUAL( success(), openext( "bob"_n, "alice"_n, extended_symbol{symbol::from_string("4,EOS"), "sysio.token"_n}) );
        BOOST_REQUIRE_EQUAL( success(), openext( "bob"_n, "alice"_n, extended_symbol{symbol::from_string("4,VOICE"), "anothertoken"_n}) );
        BOOST_REQUIRE_EQUAL( success(), openext( "bob"_n, "alice"_n, extended_symbol{symbol::from_string("2,TUSD"), "sysio.token"_n}) );
    }
    void many_transfer() {
        BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("461000000000000.0000 EOS"), "") );
        BOOST_REQUIRE_EQUAL( success(), transfer( "anothertoken"_n, "bob"_n, "sysio.swap"_n, asset::from_string("461168601842738.7000 VOICE"), "deposit to: alice") );
        // 0.0902, not bob's full 0.0903 remainder: memoexchange_test first sends
        // 0.0001 VOICE bob -> alice, so 0.0903 overdraws there (upstream ignored
        // that failure silently; this fixture asserts every setup step).
        BOOST_REQUIRE_EQUAL( success(), transfer( "anothertoken"_n, "bob"_n, "sysio.swap"_n, asset::from_string("0.0902 VOICE"), "this goes to Bob") );
        BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("45000000000000000.00 TUSD"), "") );
        BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("300000000000000.00 TUSD"), "deposit to: bob") );
    }
    void prepare_carol_token() {
        create( "carol"_n, "carol"_n, asset::from_string("4.0000 EOS") );
        issue( "carol"_n, "carol"_n, "carol"_n, asset::from_string("4.0000 EOS"), "");
        create( "carol"_n, "carol"_n, asset::from_string("1.0000 VOICE") );
        issue( "carol"_n, "carol"_n, "carol"_n, asset::from_string("1.0000 VOICE"), "");
    }
    abi_serializer abi_ser;
};

static symbol EVO4 = symbol::from_string("4,EVO");
static symbol ETUSD3 = symbol::from_string("3,ETUSD");
static symbol EOS4 = symbol::from_string("4,EOS");
static symbol VOICE4 = symbol::from_string("4,VOICE");
static symbol TUSD2 = symbol::from_string("2,TUSD");

static symbol_code EVO = EVO4.to_symbol_code();
static symbol_code ETUSD = ETUSD3.to_symbol_code();
static symbol_code EOS = EOS4.to_symbol_code();
static symbol_code VOICE = VOICE4.to_symbol_code();
static symbol_code TUSD = TUSD2.to_symbol_code();

extended_asset extend(asset to_extend) {
  if (to_extend.symbol_name() == "VOICE") {
    return extended_asset{to_extend, "anothertoken"_n};  
  } else {
    return extended_asset{to_extend, "sysio.token"_n};
  }
}

BOOST_AUTO_TEST_SUITE(sysio_swap_tests)

BOOST_FIXTURE_TEST_CASE( add_rem_liquidity, sysio_swap_tester ) try {
    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);

    create_tokens_and_issue();
    transfer( "anothertoken"_n, "bob"_n, "alice"_n, asset::from_string("500000000.0000 VOICE"), "");

    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));

    many_openext();

    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("10000000.0000 EOS"), "");
    transfer( "anothertoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("200000000.0000 VOICE"), "");
    
    inittoken( "alice"_n, EVO4,
      extend(asset::from_string("1000000.0000 EOS")), 
      extend(asset::from_string("100000000.0000 VOICE")), 10, "wevotethefee"_n);

    auto alice_evo_balance = get_balance("sysio.swap"_n, "alice"_n, "accounts"_n, EVO.value, "account");
    auto bal = mvo() ("balance", asset::from_string("10000000.0000 EVO"));
    BOOST_REQUIRE_EQUAL( fc::json::to_string(alice_evo_balance, fc::time_point(fc::time_point::now() + abi_serializer_max_time) ), 
    fc::json::to_string(bal, fc::time_point(fc::time_point::now() + abi_serializer_max_time) ) );

// ADDLIQUIDITY
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "addliquidity"_n, mvo()
          ( "user", "alice"_n)( "to_buy", asset::from_string("1 EVO"))
          ( "max_asset1", asset::from_string("1 EOS") )
          ( "max_asset2", asset::from_string("1 VOICE")) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("to_buy amount must be positive"), 
      addliquidity( "alice"_n, asset::from_string("-5.0000 EVO"), 
      asset::from_string("0.5000 EOS"), asset::from_string("5000.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"), 
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"), 
      asset::from_string("-0.3000 EOS"), asset::from_string("30.0000 NOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"), 
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"), 
      asset::from_string("0.3000 EOS"), asset::from_string("-30.0000 NOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"), 
      addliquidity( "alice"_n, asset::from_string("5.0000 EVO"), 
      asset::from_string("0.5000 EOS"), asset::from_string("5000.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"), 
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"), 
      asset::from_string("0.0000 EOS"), asset::from_string("20.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), 
      addliquidity( "alice"_n, asset::from_string("2.0000 EMMO"), 
      asset::from_string("0.0000 EOS"), asset::from_string("20.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("incorrect symbol"), 
      addliquidity( "alice"_n, asset::from_string("3.0000 EVO"), 
      asset::from_string("0.3000 ECOS"), asset::from_string("30.0001 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient funds"), 
      addliquidity( "alice"_n, asset::from_string("1000000000000.0000 EVO"), 
      asset::from_string("1000000000000.0000 EOS"), asset::from_string("20000000000000.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( success(), 
      addliquidity( "alice"_n, asset::from_string("50.0000 EVO"), 
      asset::from_string("5.0050 EOS"), asset::from_string("500.5000 VOICE") )
    );
    produce_blocks();

// REMLIQUIDITY
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "remliquidity"_n, mvo()
          ( "user", "alice"_n)( "to_sell", asset::from_string("1 EVO"))
          ( "min_asset1", asset::from_string("1 EOS") )
          ( "min_asset2", asset::from_string("1 VOICE")) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("to_sell amount must be positive"), 
      remliquidity( "alice"_n, asset::from_string("-5.0000 EVO"), 
      asset::from_string("0.5000 EOS"), asset::from_string("5000.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"), 
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"), 
      asset::from_string("-0.3000 EOS"), asset::from_string("30.0001 NOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"), 
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"), 
      asset::from_string("0.3000 EOS"), asset::from_string("-30.0001 NOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"), 
      remliquidity( "alice"_n, asset::from_string("1.0000 EVO"), 
      asset::from_string("0.1001 EOS"), asset::from_string("10.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"), 
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"), 
      asset::from_string("0.3000 EOS"), asset::from_string("30.0001 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("incorrect symbol"), 
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"), 
      asset::from_string("0.3000 EOS"), asset::from_string("30.0001 NOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("overdrawn balance"), 
      remliquidity( "alice"_n, asset::from_string("1000000000000.0000 EVO"), 
      asset::from_string("0.0000 EOS"), asset::from_string("0.0000 VOICE")));

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("computation overflow"), 
      addliquidity( "alice"_n, asset::from_string("46116860184273.8791 EVO"), 
      asset::from_string("1.0000 EOS"), asset::from_string("1.0000 VOICE")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("computation underflow"), 
      remliquidity( "alice"_n, asset::from_string("46116860184273.8791 EVO"), 
      asset::from_string("1.0000 EOS"), asset::from_string("1.0000 VOICE")));

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("the pool cannot be left empty"), 
      remliquidity( "alice"_n, asset::from_string("10000050.0000 EVO"),
      asset::from_string("0.0001 EOS"), asset::from_string("0.0001 VOICE") )
    );
/*    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), 
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"), 
      asset::from_string("0.0000 EOS"), asset::from_string("20.0000 VOICE"))
    );*/
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( exchange_action, sysio_swap_tester ) try {
    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);
    create_tokens_and_issue();
    transfer( "anothertoken"_n, "bob"_n, "alice"_n, asset::from_string("500000000.0000 VOICE"), "");
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    many_openext();
    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("10000000.0000 EOS"), "");
    transfer( "anothertoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("200000000.0000 VOICE"), "");
    inittoken( "alice"_n, EVO4,
      extend(asset::from_string("1000000.0000 EOS")),
      extend(asset::from_string("100000000.0000 VOICE")), 10, "wevotethefee"_n);
    addliquidity( "alice"_n, asset::from_string("50.0000 EVO"), 
      asset::from_string("5.0050 EOS"), asset::from_string("500.5000 VOICE") );
    remliquidity( "alice"_n, asset::from_string("17.1872 EVO"),
      asset::from_string("0.0000 EOS"), asset::from_string("0.0000 VOICE") );

// EXCHANGE
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "exchange"_n, mvo()
          ( "user", "alice"_n)( "pair_token", EVO )
          ( "ext_asset_in", extend(asset::from_string("1 EOS")) )
          ( "min_expected", asset::from_string("1 VOICE")) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg(
      "ext_asset_in must be nonzero and min_expected must have same sign or be zero"), 
      exchange( "alice"_n, EVO, extend(asset::from_string("2.0000 VOICE")), 
      asset::from_string("-0.1000 EOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg(
      "ext_asset_in must be nonzero and min_expected must have same sign or be zero"), 
      exchange( "alice"_n, EVO, extend(asset::from_string("-2.0000 RICE")), 
      asset::from_string("0.1000 REOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg(
      "ext_asset_in must be nonzero and min_expected must have same sign or be zero"), 
      exchange( "alice"_n, EVO, extend(asset::from_string("0.0000 EOS")), 
      asset::from_string("-0.1000 VOICE")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("invalid parameters"), 
      exchange( "alice"_n, EVO, extend(asset::from_string("-1000004.0000 EOS")),
      asset::from_string("-0.0001 VOICE")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("invalid parameters"), 
      exchange( "alice"_n, EVO, extend(asset::from_string("-100000328.6280 VOICE")), 
      asset::from_string("0.0000 EOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), 
      exchange( "alice"_n, TUSD, extend(asset::from_string("-8.0000 VOICE")), 
      asset::from_string("0.0000 EOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"), 
      exchange( "alice"_n, EVO, extend(asset::from_string("4.000 EOS")), 
      asset::from_string("10.0000 VOICE")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"), 
      exchange( "alice"_n, EVO, extended_asset{asset::from_string("4.0000 EOS"), "another"_n}, 
      asset::from_string("10.0000 VOICE")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"), 
      exchange( "alice"_n, EVO, extended_asset{asset::from_string("1.0000 VOICE"), "another"_n}, 
      asset::from_string("1.0000 EOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      exchange( "alice"_n, EVO, extend(asset::from_string("4.0000 EOS")),
      asset::from_string("400.0000 VOICE")) );

    exchange( "alice"_n, EVO, extend(asset::from_string("4.0000 EOS")), asset::from_string("10.0000 VOICE"));
    exchange( "alice"_n, EVO, extend(asset::from_string("0.1000 EOS")), asset::from_string("4.8500 VOICE"));
    exchange( "alice"_n, EVO, extend(asset::from_string("0.0001 EOS")), asset::from_string("0.0009 VOICE"));

    vector <int64_t> expected_system_balance = {10000073819, 999999185799, 100000328128};
    BOOST_REQUIRE_EQUAL(expected_system_balance == system_balance(EVO.value), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,0), 89999926181);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,1), 1000000814201);
 
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 50) );

    addliquidity( "alice"_n, asset::from_string("50.0000 EVO"),
      asset::from_string("10000000.0000 EOS"), asset::from_string("10000000.0000 VOICE") );

    expected_system_balance = {10000123826, 1000004186279, 100000828128};
    BOOST_REQUIRE_EQUAL(expected_system_balance == system_balance(EVO.value), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,0), 89999876174);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,1), 999995813721);
 
    BOOST_REQUIRE_EQUAL( success(),
      exchange( "alice"_n, EVO, extend(asset::from_string("-4.0000 EOS")), 
                              asset::from_string("-401.9984 VOICE")) );
    expected_system_balance = {10000083826, 1000008206263, 100000828128};
    BOOST_REQUIRE_EQUAL(expected_system_balance == system_balance(EVO.value), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,0), 89999916174);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,1), 999991793737);

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( increasing_poolvalue, sysio_swap_tester) try {
    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);

    create_tokens_and_issue();
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    many_openext(); 
    many_transfer();
    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, 
    asset::from_string("168601842738.7903 EOS"), "");

    inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("23058430092.1369 EOS")),
      extend(asset::from_string("96116860184.2738 VOICE")), 10, "wevotethefee"_n);

    inittoken( "alice"_n, ETUSD3, 
      extend(asset::from_string("10000000000.0000 EOS")),
      extend(asset::from_string("9911686018427.38 TUSD")), 10, "wevotethefee"_n);

    auto old_total = total();
    auto old_vec = system_balance(EVO.value);
    auto old_alice_bal_0 = balance("alice"_n,0);
    auto old_alice_bal_1 = balance("alice"_n,1);

    BOOST_REQUIRE_EQUAL(success(), 
      exchange( "alice"_n, EVO, extend(asset::from_string("4.0000 EOS")),
      asset::from_string("1.0000 VOICE") ));
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,0) - old_alice_bal_0, -40000);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,1) - old_alice_bal_1, 166569);

    old_total = total();
    old_vec = system_balance(EVO.value);
    old_alice_bal_0 = balance("alice"_n, 0);
    old_alice_bal_1 = balance("alice"_n, 1);
    addliquidity( "alice"_n, asset::from_string("0.0001 EVO"), 
      asset::from_string("10000000.0000 EOS"), asset::from_string("10000000.0000 VOICE") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,0) - old_alice_bal_0, -2);
    BOOST_REQUIRE_EQUAL(balance("alice"_n,1) - old_alice_bal_1, -4);

    produce_blocks();

    old_total = total();
    old_vec = system_balance(EVO.value);
    old_alice_bal_0 = balance("alice"_n, 0);
    old_alice_bal_1 = balance("alice"_n, 1);
    remliquidity( "alice"_n, asset::from_string("0.0001 EVO"),
      asset::from_string("0.0000 EOS"), asset::from_string("0.0000 VOICE") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, 0) - old_alice_bal_0, 0);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, 1) - old_alice_bal_1, 2);

    old_total = total();
    old_vec = system_balance(ETUSD.value);
    BOOST_REQUIRE_EQUAL(success(), 
      exchange( "bob"_n, ETUSD, 
      extend(asset::from_string("3000000000000.00 TUSD")),
      asset::from_string("40000000.0000 EOS") ) );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(ETUSD.value)), true);

    old_total = total();    
    old_vec = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL(success(), 
      exchange( "bob"_n, EVO, extend(asset::from_string("4000.0000 EOS")), 
      asset::from_string("1.0000 VOICE") ) );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);

    old_total = total();
    BOOST_REQUIRE_EQUAL( success(), withdraw( "bob"_n, "bob"_n, 
      extend(asset::from_string("0.0001 EOS"))) );
    BOOST_REQUIRE_EQUAL(old_total == total(), false);

    old_total = total();
    old_vec = system_balance(EVO.value);
    addliquidity( "alice"_n, asset::from_string("150000000000000.0000 EVO"), 
       asset::from_string("400000000000000.0000 EOS"), asset::from_string("400000000000000.0000 VOICE") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);

    old_total = total(); 
    old_vec = system_balance(EVO.value);
    exchange( "alice"_n, EVO, extend(asset::from_string("387592687324317.3478 EOS")), 
      asset::from_string("0.0001 VOICE") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    
    old_total = total(); 
    old_vec = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( success(), exchange( "alice"_n, EVO, 
      extend(asset::from_string("-1.0000 EOS")), asset::from_string("-0.1069 VOICE")) );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);

    old_total = total(); 
    old_vec = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( success(), exchange( "bob"_n, EVO, 
      extend(asset::from_string("-12.0001 VOICE")), asset::from_string("-122.0329 EOS")) );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( memoexchange_test, sysio_swap_tester ) try {
    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);

    create_tokens_and_issue();
    prepare_carol_token();
    transfer( "anothertoken"_n, "bob"_n, "alice"_n, asset::from_string("0.0001 VOICE"), "");

    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));

    many_openext();
    many_transfer();

    BOOST_REQUIRE_EQUAL( success(),
      inittoken( "alice"_n, EVO4,
        extend(asset::from_string("23058430092.1369 EOS")),
        extend(asset::from_string("96116860184.2738 VOICE")), 10, "wevotethefee"_n) );

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"), 
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"), 
      "exchange: EVO, 166536 VOICE") );

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"), 
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"), 
      "exchange: EVO, 16.6570 VOICE") );

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"), 
      transfer( "carol"_n, "carol"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"), 
      "exchange: EVO, 16.6569 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"), 
      transfer( "carol"_n, "carol"_n, "sysio.swap"_n, asset::from_string("1.0000 VOICE"), 
      "exchange: EVO, 0.0001 EOS") );

    int64_t pre_eos_balance = token_balance("sysio.token"_n, "alice"_n, EOS.value);
    int64_t pre_voice_balance = token_balance("anothertoken"_n, "alice"_n, VOICE.value);
    BOOST_REQUIRE_EQUAL( success(), 
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"), 
      "exchange: EVO, 16.6569 VOICE") );
    BOOST_REQUIRE_EQUAL( pre_eos_balance - 40000, token_balance("sysio.token"_n, "alice"_n, EOS.value) );
    BOOST_REQUIRE_EQUAL( pre_voice_balance + 166569, token_balance("anothertoken"_n, "alice"_n, VOICE.value) );

    inittoken( "alice"_n, ETUSD3,
      extend(asset::from_string("10000000000.0000 EOS")),
      extend(asset::from_string("9911686018427.38 TUSD")), 10, "wevotethefee"_n);

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"), 
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n,
        asset::from_string("400000.00 TUSD"), "exchange: ETUSD, 403.1605 EOS") );

    pre_eos_balance = token_balance("sysio.token"_n, "alice"_n, EOS.value);
    int64_t pre_tusd_balance = token_balance("sysio.token"_n, "alice"_n, TUSD.value);
    BOOST_REQUIRE_EQUAL( success(), 
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("400000.00 TUSD"), 
      "exchange: ETUSD, 403.1604 EOS") );

    BOOST_REQUIRE_EQUAL( pre_tusd_balance - 40000000, 
      token_balance("sysio.token"_n, "alice"_n, TUSD.value) );
    BOOST_REQUIRE_EQUAL( pre_eos_balance + 4031604, 
      token_balance("sysio.token"_n, "alice"_n, EOS.value) );

    auto old_vec = system_balance(EVO.value);
    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"), 
      "exchange: EVO, 10000 VOICE, nothing to say");
    auto new_vec = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, new_vec), true);

    old_vec = system_balance(ETUSD.value);
    transfer( "sysio.token"_n, "bob"_n, "sysio.swap"_n, 
      asset::from_string("30000000000000000.00 TUSD"), "exchange: ETUSD, 40000000000000 EOS,");
    new_vec = system_balance(ETUSD.value);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, new_vec), true);
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( the_other_actions, sysio_swap_tester ) try {

    create_tokens_and_issue();
    transfer( "anothertoken"_n, "bob"_n, "alice"_n, asset::from_string("500000000.0000 VOICE"), "");

    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));

    // add_signed_ext_balance
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("0.1000 EOS"), "") );

    // OPENEXT
    BOOST_REQUIRE_EQUAL( error("missing authority of natalia"), 
      push_action( "sysio.swap"_n, "bob"_n, "openext"_n, mvo()
        ( "user", "bob"_n)( "payer", "natalia"_n )
        ( "ext_symbol", extended_symbol{VOICE4, "anothertoken"_n} ))
    );
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, 
      extended_symbol{EOS4, "sysio.token"_n}) );
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, 
      extended_symbol{VOICE4, "anothertoken"_n}) );
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, 
      extended_symbol{VOICE4, "anothertoken"_n}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg( "user account does not exist"), 
      openext( "cat"_n, "alice"_n, extended_symbol{VOICE4, "anothertoken"_n}) );
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, 
      extended_symbol{VOICE4, "anothertoken"_n}) );

    // ONTRANSFER
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("This transfer is not for sysio.swap"), 
      transfer( "badtoken"_n, "alice"_n, "bob"_n, asset::from_string("1000.0000 EOS"), ""));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("quantity must be positive"), 
      transfer( "badtoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("-1000.0000 EOS"), ""));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Donation not accepted"), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, 
      asset::from_string("1000.0000 EOS"), "deposit to: sysio.swap") );
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, 
      asset::from_string("1000.0000 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), transfer( "anothertoken"_n, "alice"_n, "sysio.swap"_n,
      asset::from_string("20000.0000 VOICE"), "") );

    // WITHDRAW
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "withdraw"_n, mvo()
        ("user", "alice"_n) ("to", "natalia"_n)
        ("to_withdraw", extend(asset::from_string("1.0000 EOS"))) ("memo", "") )
    ); 
    BOOST_REQUIRE_EQUAL( success(), withdraw( "alice"_n, "bob"_n,
      extend(asset::from_string("999.9998 EOS")) ) );
    BOOST_REQUIRE_EQUAL( 9999998, token_balance( "sysio.token"_n, "bob"_n, EOS.value ));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("quantity must be positive"), withdraw( "alice"_n, "bob"_n,
      extend(asset::from_string("-0.0001 EOS")) ));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient funds"), withdraw( "alice"_n, "bob"_n,
      extend(asset::from_string("0.0003 EOS")) ));

    // INITTOKEN
    BOOST_REQUIRE_EQUAL( error("missing authority of sysio.swap"), 
      push_action( "sysio.swap"_n, "alice"_n, "inittoken"_n, mvo()
        ("user", "alice"_n) ("new_symbol", EVO4)
        ("initial_pool1", extend(asset::from_string("1.0000 EOS")))
        ("initial_pool2", extend(asset::from_string("1.0000 ECO")))
        ("initial_fee", 1) ("fee_contract", "carol"_n) )
    );
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "inittoken"_n, mvo()
        ("user", "alice"_n) ("new_symbol", EVO4)
        ("initial_pool1", extend(asset::from_string("1.0000 EOS")))
        ("initial_pool2", extend(asset::from_string("1.0000 ECO")))
        ("initial_fee", 1) ("fee_contract", "carol"_n) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Both assets must be positive"), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("-0.0001 EOS")),
      extend(asset::from_string("0.1000 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Both assets must be positive"), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("-0.1000 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Initial amounts must be less than 10^15"), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("100000000000.0001 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Initial amounts must be less than 10^15"), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("100000000000.0001 EOS")),
      extend(asset::from_string("1.0001 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended symbols must be different"), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.1000 EOS")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient funds"), 
      inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0003 EOS")), 
      extend(asset::from_string("0.1000 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL(  wasm_assert_msg("insufficient funds"),
      inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0002 EOS")), 
      extend(asset::from_string("100000000.0000 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("new_symbol precision must be (precision1 + precision2) / 2"),
      inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.100 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.1000 VOICE")), 10, "wevotethefee"_n) );
    produce_blocks();
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token symbol already exists"), inittoken( "alice"_n, EVO4, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.1000 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("initial_fee must be 10"), inittoken( "alice"_n, ETUSD3, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.10 TUSD")), 501, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee_contract must be wevotethefee"), 
      inittoken( "alice"_n, ETUSD3, 
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.10 ETUSD")), 10, "alice"_n) );
// The assert "the pool is already indexed" is tested in "indextable" test case.

  // TRANSFER
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer"),
      transfer("sysio.swap"_n, "alice"_n, "sysio.swap"_n, asset::from_string("0.0010 EVO"), "")
    );

  // CLOSEEXT
    BOOST_REQUIRE_EQUAL( error("missing authority of natalia"), 
      push_action( "sysio.swap"_n, "bob"_n, "closeext"_n, mvo()
        ("user", "natalia"_n)("ext_symbol", extended_symbol{EVO4, "sysio.token"_n})
        ("to", "alice"_n)("memo", "") )
    );
    BOOST_REQUIRE_EQUAL( success(), 
      closeext( "alice"_n, "alice"_n, extended_symbol{VOICE4, "anothertoken"_n}) );
    BOOST_REQUIRE_EQUAL( success(), 
      closeext( "alice"_n, "bob"_n, extended_symbol{EOS4, "sysio.token"_n} ) );
    BOOST_REQUIRE_EQUAL( 9999999, token_balance( "sysio.token"_n, "bob"_n, EOS.value ));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("User does not have such token"), 
      closeext( "alice"_n, "bob"_n, extended_symbol{EOS4, "sysio.token"_n} ) );

  // CHANGEFEE
    BOOST_REQUIRE_EQUAL( error("missing authority of wevotethefee"), 
      push_action( "sysio.swap"_n, "bob"_n, "changefee"_n, 
        mvo() ("pair_token", EVO) ("newfee", 50) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), changefee(EOS, 500));
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 50) );

  // Notifications to fee_contract 

    set_code( "wevotethefee"_n, contracts::util::badtoken_wasm() );
    set_abi( "wevotethefee"_n, contracts::util::badtoken_abi().data() );
    many_openext();
    transfer( "sysio.token"_n, "bob"_n, "sysio.swap"_n, asset::from_string("0.0004 EOS"), "");
    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("0.04 TUSD"), "deposit to: bob");

    BOOST_REQUIRE_EQUAL( success(), inittoken( "bob"_n, ETUSD3, 
      extend(asset::from_string("0.0002 EOS")),
      extend(asset::from_string("0.02 TUSD")), 10, "wevotethefee"_n ) ); 
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("notification received"), 
      transfer( "sysio.swap"_n, "bob"_n, "alice"_n, asset::from_string("0.001 ETUSD"), "") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("notification received"), 
      addliquidity( "bob"_n, asset::from_string("0.001 ETUSD"), 
      asset::from_string("0.0002 EOS"), asset::from_string("0.02 TUSD")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("notification received"), 
      remliquidity( "bob"_n, asset::from_string("0.001 ETUSD"), 
      asset::from_string("0.0001 EOS"), asset::from_string("0.01 TUSD")));

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( we_vote_the_fee, sysio_swap_tester ) try {

    create_tokens_and_issue();
    transfer( "anothertoken"_n, "bob"_n, "alice"_n, asset::from_string("500000000.0000 VOICE"), "");

    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));

    many_openext();

    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("10000000.0000 EOS"), "");
    transfer( "anothertoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("200000000.0000 VOICE"), "");
    
    inittoken( "alice"_n, EVO4,
      extend(asset::from_string("1000000.0000 EOS")), 
      extend(asset::from_string("100000000.0000 VOICE")), 10, "wevotethefee"_n);

    const auto* accnt3 = control->find_account_metadata( "wevotethefee"_n );
    abi_def abi_wevote;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt3->abi, abi_wevote), true);

// Start wevotethefee actions. Testing checks and basic functions.
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    auto ISNT = symbol::from_string("4,ISNT").to_symbol_code();
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("pair_token balance does not exist"), 
      votefee("alice"_n, ISNT, 10) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("pair_token balance does not exist"), 
      votefee("bob"_n, EVO, 30) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("fee table nonexistent, run openfeetable"), votefee("alice"_n, EVO, 30));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("fee table nonexistent, run openfeetable"), updatefee("alice"_n, EVO));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("table does not exist"), closefeetable(EVO));
    BOOST_REQUIRE_EQUAL(success(), openfeetable("alice"_n, EVO));
    BOOST_REQUIRE_EQUAL(success(), closefeetable(EVO));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("fee table nonexistent, run openfeetable"), votefee("alice"_n, EVO, 30));
    BOOST_REQUIRE_EQUAL(success(), openfeetable("alice"_n, EVO));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("already opened"), openfeetable("alice"_n, EVO));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("user is not voting"), closevote("alice"_n, EVO));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("only values between 10 and 100 are allowed"), votefee("alice"_n, EVO, 101));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("only values between 10 and 100 are allowed"), votefee("alice"_n, EVO, -10));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("only values between 10 and 100 are allowed"), votefee("alice"_n, EVO, 9));
    BOOST_REQUIRE_EQUAL(success(), votefee("alice"_n, EVO, 30));
    BOOST_REQUIRE_EQUAL(success(), updatefee("alice"_n, EVO));
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("table of votes is not empty"), closefeetable(EVO));

    auto evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(100000000000, evo_votes[8]);

    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    auto evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(30, evo_stats["fee"]);

// Leave vote table with zero votes, fee value remains unchanged
    transfer( "sysio.swap"_n, "alice"_n, "bob"_n, asset::from_string("10000000.0000 EVO"), "");
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(30, evo_stats["fee"]);
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(0, evo_votes[5]);
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    transfer( "sysio.swap"_n, "bob"_n, "alice"_n, asset::from_string("10000000.0000 EVO"), "");

// Test authorizations
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    BOOST_REQUIRE_EQUAL( error("missing authority of bob"),
      push_action( "wevotethefee"_n, "alice"_n, "votefee"_n, 
        mvo()( "user", "bob"_n )( "pair_token", EVO )( "fee_voted", "10" ) )
    );
    BOOST_REQUIRE_EQUAL( error("missing authority of bob"),
      push_action( "wevotethefee"_n, "alice"_n, "closevote"_n, 
        mvo()( "user", "bob"_n )( "pair_token", EVO ) )
    );

// Vote balance change after transfer
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    BOOST_REQUIRE_EQUAL(success(), 
      transfer( "sysio.swap"_n, "alice"_n, "bob"_n, asset::from_string("100.0000 EVO"), ""));

    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    BOOST_REQUIRE_EQUAL(success(), votefee("bob"_n, EVO, 100));
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(99999000000, evo_votes[8]);
    BOOST_REQUIRE_EQUAL(1000000, evo_votes[11]);

// Scenarios with many votes
    create_accounts( { "dan"_n, "eva"_n, "fran"_n});
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));    
    BOOST_REQUIRE_EQUAL(success(), transfer( "sysio.swap"_n, "alice"_n, "carol"_n, asset::from_string("2000.0000 EVO"), ""));
    transfer( "sysio.swap"_n, "alice"_n, "dan"_n, asset::from_string("400000.0000 EVO"), "");
    transfer( "sysio.swap"_n, "alice"_n, "eva"_n, asset::from_string("4240000.0000 EVO"), "");
    transfer( "sysio.swap"_n, "alice"_n, "fran"_n, asset::from_string("2500000.0000 EVO"), "");

    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    BOOST_REQUIRE_EQUAL(success(), votefee("carol"_n, EVO, 10));
    votefee("dan"_n, EVO, 10);
    votefee("eva"_n, EVO, 74);
    votefee("fran"_n, EVO, 73);

    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(75, evo_stats["fee"]);

    // Test updatefee when closing votes, then restore votes.
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    closevote( "alice"_n, EVO );
    closevote( "eva"_n, EVO );
    closevote( "fran"_n, EVO );
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(10, evo_stats["fee"]);
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    votefee("alice"_n, EVO, 30); 
    votefee("eva"_n, EVO, 75);
    votefee("fran"_n, EVO, 75);
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(75, evo_stats["fee"]);

    // Test fee modification when adding and removing liquidity
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    openext( "eva"_n, "eva"_n, extended_symbol{symbol::from_string("4,EOS"), "sysio.token"_n});
    openext( "eva"_n, "eva"_n, extended_symbol{symbol::from_string("4,VOICE"), "anothertoken"_n});
    push_action( "sysio.swap"_n, "eva"_n, "remliquidity"_n, mvo()
          ( "user", "eva"_n)( "to_sell", asset::from_string("4000000.0000 EVO"))
          ( "min_asset1", asset::from_string("1.0000 EOS") )
          ( "min_asset2", asset::from_string("1.0000 VOICE")) );
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(30, evo_stats["fee"]);

    BOOST_REQUIRE_EQUAL(success(), push_action( "sysio.swap"_n, "eva"_n, "addliquidity"_n, mvo()
          ( "user", "eva"_n)( "to_buy", asset::from_string("3900000.0000 EVO"))
          ( "max_asset1", asset::from_string("100000000000.0000 EOS") )
          ( "max_asset2", asset::from_string("100000000000.0000 VOICE")) )   );
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(75, evo_stats["fee"]);

// median 10, due to clamp between 10 and 100
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    votefee("alice"_n, EVO, 10);
    votefee("dan"_n, EVO, 10);
    votefee("eva"_n, EVO, 10);
    votefee("fran"_n, EVO, 10);
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(98999000000, evo_votes[5]);
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(10, evo_stats["fee"]);

// check votes after transfer, remliquidity and addliquidity
    BOOST_REQUIRE_EQUAL(success(), 
      transfer( "sysio.swap"_n, "alice"_n, "bob"_n, asset::from_string("100.0000 EVO"), ""));
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
      evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(98998000000, evo_votes[5]);
    BOOST_REQUIRE_EQUAL(2000000, evo_votes[11]);

    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    push_action( "sysio.swap"_n, "eva"_n, "remliquidity"_n, mvo()
          ( "user", "eva"_n)( "to_sell", asset::from_string("10000.0000 EVO"))
          ( "min_asset1", asset::from_string("1.0000 EOS") )
          ( "min_asset2", asset::from_string("1.0000 VOICE")) );
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(98998000000 - 100000000, evo_votes[5]);

    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    push_action( "sysio.swap"_n, "eva"_n, "addliquidity"_n, mvo()
          ( "user", "eva"_n)( "to_buy", asset::from_string("100.0000 EVO"))
          ( "max_asset1", asset::from_string("10000000.0000 EOS") )
          ( "max_asset2", asset::from_string("10000000.0000 VOICE")) );
    abi_ser.set_abi(abi_wevote, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    BOOST_REQUIRE_EQUAL(98998000000 - 100000000 + 1000000, evo_votes[5]);

// median 100
    votefee("dan"_n, EVO, 100);
    votefee("eva"_n, EVO, 100);
    votefee("fran"_n, EVO, 100);
    evo_votes = get_balance("wevotethefee"_n, name(EVO.value), "feetable"_n, EVO.value,
      "feetable" )["votes"].get_array();
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    evo_stats = get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value, "currency_stats" );
    BOOST_REQUIRE_EQUAL(100, evo_stats["fee"]);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( indextable, sysio_swap_tester ) try {

    create_tokens_and_issue();
    transfer( "anothertoken"_n, "bob"_n, "alice"_n, asset::from_string("500000000.0000 VOICE"), "");
    const auto* accnt2 = control->find_account_metadata( "sysio.swap"_n );
    abi_def abi_evo;
    BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt2->abi, abi_evo), true);
    abi_ser.set_abi(abi_evo, abi_serializer::create_yield_function(abi_serializer_max_time));
    many_openext();
    transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("10000000.0000 EOS"), "");
    transfer( "anothertoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("200000000.0000 VOICE"), "");
       
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("token symbol does not exist"), 
      push_action( "sysio.swap"_n, "alice"_n, "indexpair"_n, 
            mvo() ( "user", "alice"_n ) ( "evo_symbol", EVO4 ) ) );  

    BOOST_REQUIRE_EQUAL(success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1000000.0000 EOS")), 
        extend(asset::from_string("100000000.0000 VOICE")), 10, "wevotethefee"_n) );

    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the pool is already indexed"), 
      push_action( "sysio.swap"_n, "alice"_n, "indexpair"_n, 
            mvo() ( "user", "alice"_n ) ( "evo_symbol", EVO4 ) ) );  

    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the pool is already indexed"), 
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.0000 EOS")), 
        extend(asset::from_string("1.0000 VOICE")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the pool is already indexed"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.0000 VOICE")), 
        extend(asset::from_string("1.0000 EOS")), 10, "wevotethefee"_n) );

    auto table = get_balance("sysio.swap"_n, "sysio.swap"_n, "evoindex"_n, EVO.value, "index_struct");
    // make256key(anothertoken, 4,VOICE, sysio.token, 4,EOS) as little-endian
    // words; upstream's constant encoded eosio.token in the third word.
    BOOST_REQUIRE_EQUAL(table["id_256"], "34e996aaf9a4153000004543494f5604c7b0ea033482a60000000000534f4504");
    BOOST_REQUIRE_EQUAL(table["evo_symbol"], "4,EVO");

    BOOST_REQUIRE_EQUAL(wasm_assert_msg("extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.00000 VOICE")), 
        extend(asset::from_string("1.0000 EOS")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.0000 VOICE")), 
        extend(asset::from_string("1.00000 EOS")), 10, "wevotethefee"_n) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer"),
        inittoken( "alice"_n, EOS4, extend(asset::from_string("1.0000 VOICE")), 
        extended_asset{asset::from_string("1.0000 EOS"), "anothertoken"_n}, 
        10, "wevotethefee"_n) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END() 