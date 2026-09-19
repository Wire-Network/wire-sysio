#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>


#include <fc/variant_object.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <contracts.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <sysio.opp.common/twap.hpp>
#include "twap_wide.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <set>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;
using namespace std;
using namespace boost::multiprecision;

using mvo = fc::mutable_variant_object;

// The shadow token's rows as sysio.opp.common/shadow_yield.hpp lays them out,
// spelled again here so the test reads them without that (CDT-only) header:
// this is the host-side pin of the layout the swap contract compiles against.
struct shadow_account_row {
    asset    balance;
    uint64_t index_checkpoint;
    uint64_t owed_wire;
};
FC_REFLECT( shadow_account_row, (balance)(index_checkpoint)(owed_wire) )
struct shadow_index_row {
    uint64_t index;
    uint64_t pot;
    uint64_t carry;
};
FC_REFLECT( shadow_index_row, (index)(pot)(carry) )

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

// The system token (the tests' stand-in for WIRE): the second leg of every
// pair. The shadow token and its yield pool trade against it.
static const extended_symbol WIRE{ EOS4, "sysio.token"_n };
static symbol SHD4 = symbol::from_string("4,SHD");
static symbol SHEO4 = symbol::from_string("4,SHEO");
static symbol_code SHD = SHD4.to_symbol_code();
static symbol_code SHEO = SHEO4.to_symbol_code();
static const extended_symbol SHADOW{ SHD4, "shadowtoken"_n };

class sysio_swap_tester : public tester {
public:

    // `configure` = false leaves the fee authority unset, for the test that pins
    // what pair creation does before deployment has run setconfig.
    explicit sysio_swap_tester( bool configure = true ) {
        produce_blocks( 2 );

        create_accounts( { "alice"_n, "bob"_n, "carol"_n, "sysio.token"_n, "sysio.swap"_n,
          "badtoken"_n, "anothertoken"_n, "shadowtoken"_n } );
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

        // The shadow token stand-in bills its rows to itself; no privilege needed.
        set_code( "shadowtoken"_n, contracts::util::shadowtoken_wasm() );
        set_abi( "shadowtoken"_n, contracts::util::shadowtoken_abi().data() );

        produce_blocks();

        // Deployment's configuration step: governance executes as sysio, and EOS
        // on sysio.token stands in for WIRE as the system token.
        if (configure) BOOST_REQUIRE_EQUAL( success(), setconfig( config::system_account_name ) );

        const auto* accnt1 = control->find_account_metadata( "sysio.token"_n );
        abi_def abi1;
        BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt1->abi, abi1), true);
        abi_ser.set_abi(abi1, abi_serializer::create_yield_function(abi_serializer_max_time));

        const auto* shadow_accnt = control->find_account_metadata( "shadowtoken"_n );
        abi_def shadow_abi;
        BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(shadow_accnt->abi, shadow_abi), true);
        shadow_abi_ser.set_abi(shadow_abi, abi_serializer::create_yield_function(abi_serializer_max_time));
    }

    // Push `act` on sysio.swap under exactly the given authorities, resolving the
    // action's type from the deployed ABI, and seal its block -- the same
    // one-action-per-block cadence as the fixture's plain push_action. Helpers
    // whose signer is not a plain user go through here.
    action_result push_swap_action( action_name act, std::vector<permission_level> auths, const variant_object& data ) {
        try {
            sysio::testing::base_tester::push_action( "sysio.swap"_n, act, auths, data, 100 );
        } catch (const fc::exception& ex) {
            return error(ex.top_message());
        }
        produce_block();
        return success();
    }
    action_result setconfig( name fee_authority, extended_symbol system_token = WIRE, name signer = "sysio.swap"_n ) {
        return push_swap_action( "setconfig"_n, { {signer, config::active_name} }, mvo()
          ( "fee_authority", fee_authority )
          ( "system_token", system_token )
        );
    }
    // A deposit of a token no pair has yet, made before the pair is created: the
    // transfer carries the contract's authority, the one that creates pairs.
    action_result seed_transfer( name contract, name from, asset quantity, string memo ) {
        try {
            base_tester::push_action( contract, "transfer"_n,
                { {from, config::active_name}, {"sysio.swap"_n, config::active_name} },
                mvo()( "from", from )( "to", "sysio.swap"_n )( "quantity", quantity )( "memo", memo ), 100 );
        } catch (const fc::exception& ex) {
            return error(ex.top_message());
        }
        produce_block();
        return success();
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
    // An empty `fee_authority` adopts the configured one (sysio in this fixture);
    // `locked_shares` is in units of the new symbol, none by default; no yield
    // leg makes a plain pool.
    action_result inittoken( name user, symbol new_symbol, extended_asset initial_pool1,
      extended_asset initial_pool2, int initial_fee, name fee_authority = name{}, int64_t locked_shares = 0,
      std::optional<extended_symbol> yield_leg = std::nullopt ){
        return inittoken( user, new_symbol, initial_pool1, initial_pool2, initial_fee, fee_authority,
                          asset( locked_shares, new_symbol ), yield_leg );
    }
    action_result inittoken( name user, symbol new_symbol, extended_asset initial_pool1,
      extended_asset initial_pool2, int initial_fee, name fee_authority, asset locked_shares,
      std::optional<extended_symbol> yield_leg = std::nullopt ){
        // inittoken bills the new rows to `user`, so the action must carry the
        // user's sysio.payer permission in addition to the two active authorities.
        return push_swap_action( "inittoken"_n,
          { {user, config::sysio_payer_name}, {user, config::active_name}, {"sysio.swap"_n, config::active_name} },
          mvo()
          ( "user", user)
          ("new_symbol", new_symbol)
          ("initial_pool1", initial_pool1)
          ("initial_pool2", initial_pool2)
          ("initial_fee", initial_fee)
          ("fee_authority", fee_authority)
          ("locked_shares", locked_shares)
          ("yield_leg", yield_leg)
        );
    }
    // Signed by the pair's fee authority, sysio unless overridden.
    action_result setyield( symbol_code pair_token, uint32_t conversion_horizon_sec, uint32_t depth_cap_bps,
                            int64_t clip_floor, name authority = config::system_account_name ) {
        return push_swap_action( "setyield"_n, { {authority, config::active_name} }, mvo()
          ( "pair_token", pair_token )
          ( "conversion_horizon_sec", conversion_horizon_sec )
          ( "depth_cap_bps", depth_cap_bps )
          ( "clip_floor", clip_floor )
        );
    }
    // The pair's stat row as a variant (abi_ser must hold the swap ABI).
    fc::variant pair_row( symbol_code pair_token ) {
        return get_balance( "sysio.swap"_n, name(pair_token.value), "stat"_n, pair_token.value, "currency_stats" );
    }
    // accrueyield needs no authorization; any account can foot the CPU.
    action_result accrueyield( symbol_code pair_token ) {
        return push_action( "sysio.swap"_n, "alice"_n, "accrueyield"_n, mvo()
          ( "pair_token", pair_token )
        );
    }
    // Raw bytes of the pending-payout receipt keyed by `contract`; empty when none.
    vector<char> pending_payout( name contract ) {
        return get_kv_row( "sysio.swap"_n, "yieldpayouts"_n, { contract.to_uint64_t() } );
    }
    // fundyield is the funder's own call; the fixture adds its payer permission,
    // which the row is billed to.
    action_result fundyield( name from, symbol_code pair_token, asset quantity ) {
        return push_action( "sysio.swap"_n, from, "fundyield"_n, mvo()
          ( "from", from )( "pair_token", pair_token )( "quantity", quantity )
        );
    }
    action_result cancelyield( name from, name signer ) {
        return push_swap_action( "cancelyield"_n, { {signer, config::active_name} }, mvo()( "from", from ) );
    }
    action_result cancelyield( name from ) { return cancelyield( from, from ); }
    int64_t ram_usage( name account ) const {
        return control->get_resource_limits_manager().get_account_ram_usage( account );
    }
    // The pending funding announced by `funder`, as a variant; null when none.
    fc::variant pending_funding( name funder ) {
        const auto data = get_kv_row( "sysio.swap"_n, "yieldfunds"_n, { funder.to_uint64_t() } );
        return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "fund_receipt", data,
                   abi_serializer::create_yield_function(abi_serializer_max_time) );
    }
    // Units of shadow queued in `pair`'s reservoir. Requires the row: only yield
    // pools have one.
    int64_t reservoir_of( symbol_code pair ) {
        const auto data = get_kv_row( "sysio.swap"_n, "reservoirs"_n, { pair.value } );
        BOOST_REQUIRE_MESSAGE( !data.empty(), "no reservoir row for " << pair );
        const auto row = abi_ser.binary_to_variant( "reservoir", data,
                             abi_serializer::create_yield_function(abi_serializer_max_time) );
        return to_int( fc::json::to_string( row["balance"]["quantity"],
                       fc::time_point(fc::time_point::now() + abi_serializer_max_time) ) );
    }
    bool has_reservoir( symbol_code pair ) {
        return !get_kv_row( "sysio.swap"_n, "reservoirs"_n, { pair.value } ).empty();
    }
    // The intended shape of a funding: the announcement and the shadow transfer
    // in ONE transaction, signed by the funder, as a contract would do it inline.
    void fund_yield_in_one_transaction( name from, symbol_code pair, asset quantity ) {
        signed_transaction trx;
        action announce;
        announce.account = "sysio.swap"_n;
        announce.name    = "fundyield"_n;
        // The announcement's row is billed to the funder, so it carries the payer
        // permission as well as the active one.
        announce.authorization = { {from, config::sysio_payer_name}, {from, config::active_name} };
        announce.data = abi_ser.variant_to_binary( "fundyield",
            mvo()( "from", from )( "pair_token", pair )( "quantity", quantity ),
            abi_serializer::create_yield_function(abi_serializer_max_time) );
        action deliver;
        deliver.account = "shadowtoken"_n;
        deliver.name    = "transfer"_n;
        deliver.authorization = { {from, config::active_name} };
        deliver.data = shadow_abi_ser.variant_to_binary( "transfer",
            mvo()( "from", from )( "to", "sysio.swap"_n )( "quantity", quantity )( "memo", "" ),
            abi_serializer::create_yield_function(abi_serializer_max_time) );
        trx.actions.emplace_back( std::move(announce) );
        trx.actions.emplace_back( std::move(deliver) );
        set_transaction_headers( trx );
        trx.sign( get_private_key( from, "active" ), control->get_chain_id() );
        push_transaction( trx );
        produce_block();
    }

    // --- The shadow token stand-in (contracts/test_contracts/shadowtoken) ---

    action_result push_shadow_action( name signer, action_name act, const variant_object& data ) {
        action a;
        a.account = "shadowtoken"_n;
        a.name    = act;
        a.data    = shadow_abi_ser.variant_to_binary( shadow_abi_ser.get_action_type(act), data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time) );
        return base_tester::push_action( std::move(a), signer.to_uint64_t() );
    }
    action_result shadow_create( name issuer, asset maximum_supply, extended_symbol wire ) {
        return push_shadow_action( "shadowtoken"_n, "create"_n, mvo()
          ( "issuer", issuer )( "maximum_supply", maximum_supply )
          ( "wire_contract", wire.contract )( "wire_symbol", wire.sym ) );
    }
    action_result shadow_issue( name issuer, name to, asset quantity ) {
        return push_shadow_action( issuer, "issue"_n, mvo()( "to", to )( "quantity", quantity )( "memo", "" ) );
    }
    action_result shadow_transfer( name from, name to, asset quantity, string memo ) {
        return push_shadow_action( from, "transfer"_n, mvo()
          ( "from", from )( "to", to )( "quantity", quantity )( "memo", memo ) );
    }
    // `from` donates `quantity` WIRE to `target`'s holders. The token moves the
    // WIRE by an inline transfer under `from`'s authority, so `from`'s active
    // must carry shadowtoken@sysio.code (see grant_shadow_code).
    action_result shadow_addyield( name from, asset quantity, symbol_code target ) {
        return push_shadow_action( from, "addyield"_n, mvo()
          ( "from", from )( "quantity", quantity )( "target", target ) );
    }
    action_result shadow_claim( name holder, symbol_code sym ) {
        return push_shadow_action( holder, "claim"_n, mvo()( "holder", holder )( "sym", sym ) );
    }
    // Let the shadow token act for `account`: its active keeps its key and gains
    // shadowtoken@sysio.code, the delegation addyield's inline transfer needs. A
    // contract account keeps its own sysio.code seat too (`keep_own_code`), which
    // its own inline actions need; the seats are listed in name order.
    void grant_shadow_code( name account, bool keep_own_code = false ) {
        vector<permission_level_weight> seats{ { { "shadowtoken"_n, config::sysio_code_name }, 1 } };
        if (keep_own_code) seats.push_back( { { account, config::sysio_code_name }, 1 } );
        std::sort( seats.begin(), seats.end() );
        set_authority( account, config::active_name,
            authority( 1, { key_weight{ get_public_key( account, "active" ), 1 } }, seats ),
            config::owner_name );
        produce_block();
    }
    // tickyield needs no authorization; any account can foot the CPU.
    action_result tickyield( symbol_code pair_token ) {
        return push_action( "sysio.swap"_n, "alice"_n, "tickyield"_n, mvo()
          ( "pair_token", pair_token )
        );
    }
    // Two ticks in ONE transaction, so no block time passes between them.
    void tick_twice_in_one_transaction( symbol_code pair ) {
        signed_transaction trx;
        for (int i = 0; i < 2; ++i) {
            action act;
            act.account = "sysio.swap"_n;
            act.name    = "tickyield"_n;
            act.authorization = { {"alice"_n, config::active_name} };
            act.data = abi_ser.variant_to_binary( "tickyield", mvo()( "pair_token", pair ),
                                                  abi_serializer::create_yield_function(abi_serializer_max_time) );
            trx.actions.emplace_back( std::move(act) );
        }
        set_transaction_headers( trx );
        trx.sign( get_private_key( "alice"_n, "active" ), control->get_chain_id() );
        push_transaction( trx );
        produce_block();
    }
    // The pair's tick clock, in microseconds since the epoch.
    int64_t last_tick_us( symbol_code pair ) {
        return fc::time_point::from_iso_string( pair_row( pair )["last_tick"].as_string() ).time_since_epoch().count();
    }
    // The shadow side the pair recorded at its last setyield or selling tick.
    int64_t last_tick_depth( symbol_code pair ) {
        return pair_row( pair )["last_tick_depth"].as_int64();
    }
    // `holder`'s row for `sym` on the shadow token (scope = holder, key = symbol code).
    shadow_account_row shadow_account( name holder, symbol_code sym ) {
        const auto data = get_kv_row( "shadowtoken"_n, "accounts"_n, { holder.to_uint64_t(), sym.value } );
        BOOST_REQUIRE_MESSAGE( !data.empty(), "no shadow row for " << holder << " " << sym );
        return fc::raw::unpack<shadow_account_row>( data );
    }
    // The distribution state of `sym`; all zero before the first addyield.
    shadow_index_row shadow_index( symbol_code sym ) {
        const auto data = get_kv_row( "shadowtoken"_n, "yieldidx"_n, { sym.value } );
        return data.empty() ? shadow_index_row{ 0, 0, 0 } : fc::raw::unpack<shadow_index_row>( data );
    }
    // A user's deposit of any extended symbol, resolved without `extend`.
    int64_t deposit_of( name user, const extended_symbol& ext );
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
    // Signed by the pair's fee authority: the configured sysio unless overridden.
    action_result changefee( symbol_code pair_token, int newfee, name authority = config::system_account_name ) {
        return push_swap_action( "changefee"_n, { {authority, config::active_name} }, mvo()
          ( "pair_token", pair_token )
          ( "newfee", newfee )
        );
    }
    // Two exchanges by `user` in ONE transaction, so no block time can pass
    // between them. (The fixture's push_action seals a block per action.)
    void exchange_twice_in_one_transaction( name user, symbol_code pair,
                                            extended_asset in_a, extended_asset in_b, symbol out_symbol ) {
        signed_transaction trx;
        for (const auto& in : { in_a, in_b }) {
            action act;
            act.account = "sysio.swap"_n;
            act.name    = "exchange"_n;
            act.authorization = { {user, config::sysio_payer_name}, {user, config::active_name} };
            act.data = abi_ser.variant_to_binary( "exchange", mvo()
                ( "user", user )( "pair_token", pair )( "ext_asset_in", in )( "min_expected", asset(0, out_symbol) ),
                abi_serializer::create_yield_function(abi_serializer_max_time) );
            trx.actions.emplace_back( std::move(act) );
        }
        set_transaction_headers( trx );
        trx.sign( get_private_key( user, "active" ), control->get_chain_id() );
        push_transaction( trx );
    }
    // sync needs no authorization; any account can foot the CPU.
    action_result sync( symbol_code pair_token ) {
        return push_action( "sysio.swap"_n, "alice"_n, "sync"_n, mvo()
          ( "pair_token", pair_token )
        );
    }
    // The pair's cumulative-price row, with the 256-bit accumulators widened and
    // the timestamp in microseconds since the epoch.
    struct accumulator_row {
        boost::multiprecision::uint256_t price1;
        boost::multiprecision::uint256_t price2;
        int64_t                          last_update_us;
    };
    accumulator_row price_accumulator( symbol_code pair ) {
        auto row = get_balance( "sysio.swap"_n, "sysio.swap"_n, "priceaccum"_n, pair.value, "price_accumulator" );
        BOOST_REQUIRE( !row.is_null() );
        // The ABI serializer decodes a `uint128` field into the variant's native
        // 128-bit slot; widen it through the shared helper.
        auto limb = [](const fc::variant& v) {
            BOOST_REQUIRE( v.is_uint128() );
            return twap_testing::wide( v.as_uint128() );
        };
        auto cumulative = [&](const fc::variant& c) { return (limb(c["hi"]) << 128) | limb(c["lo"]); };
        return { cumulative(row["price1"]), cumulative(row["price2"]),
                 fc::time_point::from_iso_string(row["last_update"].as_string()).time_since_epoch().count() };
    }


    // Raw KV read of a composite-keyed row: every key word big-endian, in order
    // (for a scoped table the scope is the first word). Empty when absent.
    vector<char> get_kv_row( name code, name table, std::initializer_list<uint64_t> key_words ) {
        std::string key;
        for (uint64_t word : key_words)
            for (int shift = 56; shift >= 0; shift -= 8) key.push_back( char((word >> shift) & 0xff) );
        const auto& kv_idx = control->db().get_index<chain::kv_index, chain::by_code_key>();
        const auto itr = kv_idx.find( boost::make_tuple( code, chain::compute_table_id(table.to_uint64_t()),
                                                         std::string_view(key) ) );
        if (itr == kv_idx.end()) return {};
        return vector<char>( itr->value.data(), itr->value.data() + itr->value.size() );
    }
    // A user's deposit of `sym`, with the token contract resolved as `extend` does:
    // the evodexacnts row keyed by the extended symbol, scoped by the user.
    // Declared here, defined after `extend`.
    int64_t balance( name user, symbol sym );
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
    // Every unit of EOS, VOICE and TUSD held by the contract for alice, bob and
    // the two pools. Declared here, defined after the file-scope symbols.
    vector <int64_t> total();
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
    // The deposits the pools are seeded from. EOS is the system token and is
    // always accepted; VOICE and TUSD have no pair yet, so their deposits carry
    // the contract's authority.
    void many_transfer() {
        BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("461000000000000.0000 EOS"), "") );
        BOOST_REQUIRE_EQUAL( success(), seed_transfer( "anothertoken"_n, "bob"_n, asset::from_string("461168601842738.7000 VOICE"), "deposit to: alice") );
        // 0.0902, not bob's full 0.0903 remainder: memoexchange_test first sends
        // 0.0001 VOICE bob -> alice, so 0.0903 overdraws there (upstream ignored
        // that failure silently; this fixture asserts every setup step).
        BOOST_REQUIRE_EQUAL( success(), seed_transfer( "anothertoken"_n, "bob"_n, asset::from_string("0.0902 VOICE"), "this goes to Bob") );
        BOOST_REQUIRE_EQUAL( success(), seed_transfer( "sysio.token"_n, "alice"_n, asset::from_string("45000000000000000.00 TUSD"), "") );
        BOOST_REQUIRE_EQUAL( success(), seed_transfer( "sysio.token"_n, "alice"_n, asset::from_string("300000000000000.00 TUSD"), "deposit to: bob") );
    }
    abi_def swap_abi_def() {
        const auto* accnt = control->find_account_metadata( "sysio.swap"_n );
        BOOST_REQUIRE( accnt != nullptr );
        abi_def abi;
        BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
        return abi;
    }
    // LP-token balance, 0 when the user never held any (no accounts row).
    int64_t lp_balance( name user, symbol_code pair_token ) {
        auto row = get_balance( "sysio.swap"_n, user, "accounts"_n, pair_token.value, "account" );
        return row.is_null() ? 0 : to_int( fc::json::to_string( row["balance"],
                 fc::time_point(fc::time_point::now() + abi_serializer_max_time) ) );
    }
    int64_t pool_fee( symbol_code pair_token ) {
        return get_balance( "sysio.swap"_n, name(pair_token.value), "stat"_n, pair_token.value,
                            "currency_stats" )["fee"].as_int64();
    }
    // The two-pool state the math tests start from: EVO = EOS/VOICE and
    // ETUSD = EOS/TUSD, both funded by alice, with abi_ser on the swap ABI.
    // Defined after the file-scope symbols and `extend` it uses.
    void setup_pools();
    // Push an exact-input swap with a zero slippage floor and return what the
    // pool paid out, measured from the pool rather than taken from any quote.
    // `out_leg` is the pool index (0 = pool1, 1 = pool2) of the received token.
    int64_t settle_swap( name user, symbol_code pair, asset in, symbol out_symbol, int out_leg );
    void prepare_carol_token() {
        create( "carol"_n, "carol"_n, asset::from_string("4.0000 EOS") );
        issue( "carol"_n, "carol"_n, "carol"_n, asset::from_string("4.0000 EOS"), "");
        create( "carol"_n, "carol"_n, asset::from_string("1.0000 VOICE") );
        issue( "carol"_n, "carol"_n, "carol"_n, asset::from_string("1.0000 VOICE"), "");
    }
    // The yield-pool state the accrual tests start from: SHD (the shadow, paying
    // its yield in EOS) issued to alice, bob holding EOS to donate and able to
    // route it through the shadow token, and SHEO = SHD/EOS created by alice as
    // a yield pool on SHD. Defined after the file-scope symbols it uses.
    void setup_yield_pool();
    abi_serializer abi_ser;
    abi_serializer shadow_abi_ser;
};

static extended_asset shd( int64_t units ) { return extended_asset{ asset( units, SHD4 ), "shadowtoken"_n }; }
// Seed of the yield pool and the shadow's total issuance: the pool holds a third
// of the supply, so every distribution splits 1:2 between the pool and alice.
static const int64_t YieldPoolShadow   = 1'000'000'0000;
static const int64_t YieldPoolWire     = 1'000'000'0000;
static const int64_t ShadowIssuance    = 3 * YieldPoolShadow;
static const int64_t BobDonationBudget = 10'000'0000;

extended_asset extend(asset to_extend) {
  if (to_extend.symbol_name() == "VOICE") {
    return extended_asset{to_extend, "anothertoken"_n};
  } else {
    return extended_asset{to_extend, "sysio.token"_n};
  }
}

// The constant product of a pair's two pools, as system_balance reports them.
static boost::multiprecision::int256_t wide_product( const vector<int64_t>& pool ) {
  return boost::multiprecision::int256_t( pool.at(0) ) * pool.at(1);
}

// Reference quotes for the rounding tests, written from the SPEC rather than
// copied from the contract. The curve goes the pool's way: what a user pays is
// rounded up, what a user receives is rounded down. The two fees differ: the
// swap fee taken off a quote is rounded DOWN (the depot-wide amm_math
// convention) but never below one unit when both the rate and the quote are
// nonzero (units are precision-relative, so a fee-free window is not dust),
// while the liquidity fee added on top of what a provider pays is rounded UP.
namespace reference {
   using wide = boost::multiprecision::int128_t;
   constexpr int64_t FeeDenominator = 10000;
   constexpr int64_t MinSwapFee = 1;

   int64_t ceil_div( wide a, wide b )  { return int64_t( (a + b - 1) / b ); }
   int64_t floor_div( wide a, wide b ) { return int64_t( a / b ); }
   int64_t swap_fee_on( int64_t amount, int fee ) {
      const int64_t floored = floor_div( wide(amount) * fee, FeeDenominator );
      return (amount > 0 && fee > 0) ? std::max( floored, MinSwapFee ) : floored;
   }
   int64_t liquidity_fee_on( int64_t amount, int fee ) { return ceil_div( wide(amount) * fee, FeeDenominator ); }
   // The proportional fee BEFORE the one-unit minimum is applied. Zero is what
   // makes MinSwapFee bind, and a clip whose output gets there pays far above
   // the pair's rate.
   int64_t proportional_fee_on( int64_t amount, int fee ) { return floor_div( wide(amount) * fee, FeeDenominator ); }
   // Least output at which the pair's own rate is the fee a trade pays: the
   // smallest amount whose proportional fee reaches a whole unit.
   int64_t min_fee_bearing_output( int fee ) { return fee > 0 ? FeeDenominator / fee : 1; }

   // Units of `pool_out` received for `amount_in` units of `pool_in`.
   int64_t receive( int64_t amount_in, int64_t pool_in, int64_t pool_out, int fee ) {
      const int64_t gross = floor_div( wide(amount_in) * pool_out, wide(pool_in) + amount_in );
      return gross - swap_fee_on( gross, fee );
   }
   // Units of one leg charged for `shares` new LP tokens (ADD_LIQUIDITY_FEE = 1).
   int64_t add_leg( int64_t shares, int64_t pool_leg, int64_t supply ) {
      const int64_t gross = ceil_div( wide(shares) * pool_leg, supply );
      return gross + liquidity_fee_on( gross, 1 );
   }
   // Units of one leg returned for burning `shares` LP tokens (no fee).
   int64_t remove_leg( int64_t shares, int64_t pool_leg, int64_t supply ) {
      return floor_div( wide(shares) * pool_leg, supply );
   }
}

// The amm_math composition sysio.swap implements, evaluated on the host with
// the SAME header the contract compiles against: the equal-weight
// constant-product kernel for the gross output, then the depot fee split
// against it (no underwriter share -- the fee stays in the pool), with the
// contract's one-unit minimum on top. `reference` is the spec written by hand;
// `model` is the library. A swap must agree with both.
namespace model {
   namespace amm = sysio::opp::amm;
   constexpr uint64_t CpWeightBps = amm::WEIGHT_TOTAL_BPS / 2;
   constexpr uint32_t NoUnderwriterShareBps = 0;
   constexpr uint64_t MinSwapFee = 1;

   int64_t gross( int64_t amount_in, int64_t pool_in, int64_t pool_out ) {
      return int64_t( amm::out_given_in( uint64_t(pool_in), CpWeightBps,
                                         uint64_t(pool_out), CpWeightBps,
                                         uint64_t(amount_in) ) );
   }
   int64_t receive( int64_t amount_in, int64_t pool_in, int64_t pool_out, int fee ) {
      const uint64_t g = uint64_t( gross( amount_in, pool_in, pool_out ) );
      uint64_t f = amm::split_wire_fee( g, uint32_t(fee), NoUnderwriterShareBps ).fee;
      if (fee > 0 && g > 0) f = std::max( f, MinSwapFee );
      return int64_t( g - f );
   }
}

// The cumulative-price spec, written by hand: a side's spot price is the other
// side's balance over its own, in Q64.64 rounded down, and an accumulator is
// the sum of that price times the microseconds it held.
namespace twap_reference {
   using boost::multiprecision::uint256_t;
   constexpr int PriceFractionBits = 64;
   uint256_t price_fp( int64_t numerator, int64_t denominator ) {
      return (uint256_t(numerator) << PriceFractionBits) / denominator;
   }
}

// The shadow yield spec (sysio.opp.common/shadow_yield.hpp), written by hand:
// a distribution advances the index by WIRE * SCALE / supply with the
// remainder carried into the next one, and a holder is owed its banked WIRE
// plus balance * (index - checkpoint) / SCALE, floored.
namespace yield_reference {
   using wide = boost::multiprecision::uint128_t;
   constexpr uint64_t Scale = 1'000'000'000'000;
   struct distribution { uint64_t index_delta; uint64_t carry; };
   distribution distribute( int64_t wire, int64_t supply, uint64_t carry_in ) {
      const wide total = wide(wire) * Scale + carry_in;
      return { uint64_t( total / supply ), uint64_t( total % supply ) };
   }
   int64_t owed( int64_t balance, uint64_t index, uint64_t checkpoint, uint64_t banked = 0 ) {
      return int64_t( banked + uint64_t( wide(balance) * (index - checkpoint) / Scale ) );
   }
   // One tick's clip: the reservoir's share of the horizon elapsed, FLOORED,
   // capped by `cap_bps` of the pool's shadow side and by what is queued. A
   // clip short of min(clip_floor, queued) is not sold at all, which this
   // reports as 0 -- the tick moves nothing and leaves its clock alone.
   constexpr int64_t MicrosecondsPerSecond = 1'000'000;
   constexpr int64_t BpsTotal = 10'000;
   int64_t clip_size( int64_t queued, int64_t elapsed_us, uint32_t horizon_sec, int64_t pool_shadow,
                      uint32_t cap_bps, int64_t clip_floor ) {
      const wide horizon_us = wide(horizon_sec) * MicrosecondsPerSecond;
      const wide by_time = ( wide(queued) * elapsed_us ) / horizon_us;
      const wide cap     = wide(pool_shadow) * cap_bps / BpsTotal;
      const wide clip    = std::min( { by_time, cap, wide(queued) } );
      return clip < std::min( wide(clip_floor), wide(queued) ) ? 0 : int64_t( clip );
   }
}

vector<int64_t> sysio_swap_tester::total() {
    // EOS, the system token, is the second leg of both pools.
    const int64_t total_eos   = balance("alice"_n, EOS4) + balance("bob"_n, EOS4)
                              + system_balance(EVO.value).at(1) + system_balance(ETUSD.value).at(1);
    const int64_t total_voice = balance("alice"_n, VOICE4) + balance("bob"_n, VOICE4)
                              + system_balance(EVO.value).at(0);
    const int64_t total_tusd  = balance("alice"_n, TUSD2) + balance("bob"_n, TUSD2)
                              + system_balance(ETUSD.value).at(0);
    return { total_eos, total_voice, total_tusd };
}

int64_t sysio_swap_tester::deposit_of( name user, const extended_symbol& ext ) {
    const auto data = get_kv_row( "sysio.swap"_n, "evodexacnts"_n,
                                  { user.to_uint64_t(), ext.contract.to_uint64_t(), ext.sym.value() } );
    BOOST_REQUIRE_MESSAGE( !data.empty(), "no deposit row for " << user << " " << ext.sym );
    const auto row = abi_ser.binary_to_variant( "evodex_account", data,
                                                abi_serializer::create_yield_function(abi_serializer_max_time) );
    return to_int( fc::json::to_string( row["balance"]["quantity"],
                   fc::time_point(fc::time_point::now() + abi_serializer_max_time) ) );
}

int64_t sysio_swap_tester::balance( name user, symbol sym ) {
    const extended_asset ext = extend( asset(0, sym) );
    return deposit_of( user, extended_symbol{ sym, ext.contract } );
}

void sysio_swap_tester::setup_yield_pool() {
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    many_transfer();
    BOOST_REQUIRE_EQUAL( success(), shadow_create( "alice"_n, asset( 100 * ShadowIssuance, SHD4 ), WIRE ) );
    BOOST_REQUIRE_EQUAL( success(), shadow_issue( "alice"_n, "alice"_n, asset( ShadowIssuance, SHD4 ) ) );
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "bob"_n, asset( BobDonationBudget, EOS4 ), "" ) );
    grant_shadow_code( "bob"_n );
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, SHADOW ) );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "shadowtoken"_n, "alice"_n, asset( YieldPoolShadow, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, SHEO4, shd( YieldPoolShadow ),
        extend( asset( YieldPoolWire, EOS4 ) ), 10, name{}, 0, SHADOW ) );
    // The pool holds exactly its seed, its reservoir exists and is empty, and
    // the shadow row was stamped at index 0.
    const auto pool = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( YieldPoolShadow, pool.at(0) );
    BOOST_REQUIRE_EQUAL( YieldPoolWire,   pool.at(1) );
    BOOST_REQUIRE_EQUAL( 0, reservoir_of( SHEO ) );
    const auto held = shadow_account( "sysio.swap"_n, SHD );
    BOOST_REQUIRE_EQUAL( YieldPoolShadow, held.balance.get_amount() );
    BOOST_REQUIRE_EQUAL( 0u, held.index_checkpoint );
    BOOST_REQUIRE_EQUAL( 0u, held.owed_wire );
}

int64_t sysio_swap_tester::settle_swap( name user, symbol_code pair, asset in, symbol out_symbol, int out_leg ) {
    const auto before = system_balance(pair.value);
    BOOST_REQUIRE_EQUAL( success(), exchange( user, pair, extend(in), asset(0, out_symbol) ) );
    const auto after = system_balance(pair.value);
    return before[out_leg] - after[out_leg];
}

void sysio_swap_tester::setup_pools() {
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    many_transfer();
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n,
                                              asset::from_string("168601842738.7903 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("96116860184.2738 VOICE")),
        extend(asset::from_string("23058430092.1369 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset::from_string("9911686018427.38 TUSD")),
        extend(asset::from_string("10000000000.0000 EOS")), 10, name{}) );
    // Seed supply is the exact integer root of the product (neither is a square).
    BOOST_REQUIRE_EQUAL( 470776369546600, system_balance(EVO.value).at(2) );
    BOOST_REQUIRE_EQUAL( 314828302705258, system_balance(ETUSD.value).at(2) );
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
    seed_transfer( "anothertoken"_n, "alice"_n, asset::from_string("200000000.0000 VOICE"), "");

    inittoken( "alice"_n, EVO4,
      extend(asset::from_string("100000000.0000 VOICE")),
      extend(asset::from_string("1000000.0000 EOS")), 10, name{});

    auto alice_evo_balance = get_balance("sysio.swap"_n, "alice"_n, "accounts"_n, EVO.value, "account");
    auto bal = mvo() ("balance", asset::from_string("10000000.0000 EVO"));
    BOOST_REQUIRE_EQUAL( fc::json::to_string(alice_evo_balance, fc::time_point(fc::time_point::now() + abi_serializer_max_time) ), 
    fc::json::to_string(bal, fc::time_point(fc::time_point::now() + abi_serializer_max_time) ) );

// ADDLIQUIDITY
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "addliquidity"_n, mvo()
          ( "user", "alice"_n)( "to_buy", asset::from_string("1 EVO"))
          ( "max_asset1", asset::from_string("1 VOICE") )
          ( "max_asset2", asset::from_string("1 EOS")) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("to_buy amount must be positive"),
      addliquidity( "alice"_n, asset::from_string("-5.0000 EVO"),
      asset::from_string("5000.0000 VOICE"), asset::from_string("0.5000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"),
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"),
      asset::from_string("-30.0000 NOICE"), asset::from_string("0.3000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"),
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"),
      asset::from_string("30.0000 NOICE"), asset::from_string("-0.3000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      addliquidity( "alice"_n, asset::from_string("5.0000 EVO"),
      asset::from_string("5000.0000 VOICE"), asset::from_string("0.5000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      addliquidity( "alice"_n, asset::from_string("2.0000 EVO"),
      asset::from_string("20.0000 VOICE"), asset::from_string("0.0000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"),
      addliquidity( "alice"_n, asset::from_string("2.0000 EMMO"),
      asset::from_string("20.0000 VOICE"), asset::from_string("0.0000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("incorrect symbol"),
      addliquidity( "alice"_n, asset::from_string("3.0000 EVO"),
      asset::from_string("30.0001 VOICE"), asset::from_string("0.3000 ECOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient funds"),
      addliquidity( "alice"_n, asset::from_string("1000000000000.0000 EVO"),
      asset::from_string("20000000000000.0000 VOICE"), asset::from_string("1000000000000.0000 EOS")));
    BOOST_REQUIRE_EQUAL( success(),
      addliquidity( "alice"_n, asset::from_string("50.0000 EVO"),
      asset::from_string("500.5000 VOICE"), asset::from_string("5.0050 EOS") )
    );
    produce_blocks();

// REMLIQUIDITY
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"),
      push_action( "sysio.swap"_n, "bob"_n, "remliquidity"_n, mvo()
          ( "user", "alice"_n)( "to_sell", asset::from_string("1 EVO"))
          ( "min_asset1", asset::from_string("1 VOICE") )
          ( "min_asset2", asset::from_string("1 EOS")) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("to_sell amount must be positive"),
      remliquidity( "alice"_n, asset::from_string("-5.0000 EVO"),
      asset::from_string("5000.0000 VOICE"), asset::from_string("0.5000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"),
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"),
      asset::from_string("-30.0001 NOICE"), asset::from_string("0.3000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("assets must be nonnegative"),
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"),
      asset::from_string("30.0001 NOICE"), asset::from_string("-0.3000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      remliquidity( "alice"_n, asset::from_string("1.0000 EVO"),
      asset::from_string("10.0000 VOICE"), asset::from_string("0.1001 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"),
      asset::from_string("30.0001 VOICE"), asset::from_string("0.3000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("incorrect symbol"),
      remliquidity( "alice"_n, asset::from_string("3.0000 EVO"),
      asset::from_string("30.0001 NOICE"), asset::from_string("0.3000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("overdrawn balance"),
      remliquidity( "alice"_n, asset::from_string("1000000000000.0000 EVO"),
      asset::from_string("0.0000 VOICE"), asset::from_string("0.0000 EOS")));

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("computation overflow"),
      addliquidity( "alice"_n, asset::from_string("46116860184273.8791 EVO"),
      asset::from_string("1.0000 VOICE"), asset::from_string("1.0000 EOS")));
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("computation underflow"),
      remliquidity( "alice"_n, asset::from_string("46116860184273.8791 EVO"),
      asset::from_string("1.0000 VOICE"), asset::from_string("1.0000 EOS")));

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("the pool cannot be left empty"),
      remliquidity( "alice"_n, asset::from_string("10000050.0000 EVO"),
      asset::from_string("0.0001 VOICE"), asset::from_string("0.0001 EOS") )
    );
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
    seed_transfer( "anothertoken"_n, "alice"_n, asset::from_string("200000000.0000 VOICE"), "");
    inittoken( "alice"_n, EVO4,
      extend(asset::from_string("100000000.0000 VOICE")),
      extend(asset::from_string("1000000.0000 EOS")), 10, name{});
    addliquidity( "alice"_n, asset::from_string("50.0000 EVO"),
      asset::from_string("500.5000 VOICE"), asset::from_string("5.0050 EOS") );
    remliquidity( "alice"_n, asset::from_string("17.1872 EVO"),
      asset::from_string("0.0000 VOICE"), asset::from_string("0.0000 EOS") );

// EXCHANGE
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), 
      push_action( "sysio.swap"_n, "bob"_n, "exchange"_n, mvo()
          ( "user", "alice"_n)( "pair_token", EVO )
          ( "ext_asset_in", extend(asset::from_string("1 EOS")) )
          ( "min_expected", asset::from_string("1 VOICE")) )
    );
    // Inputs are exact-in only: the amount must be positive and the slippage
    // floor nonnegative. The negative-amount (exact-output) form is retired.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("min_expected must be nonnegative"),
      exchange( "alice"_n, EVO, extend(asset::from_string("2.0000 VOICE")),
      asset::from_string("-0.1000 EOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"),
      exchange( "alice"_n, EVO, extend(asset::from_string("-2.0000 RICE")),
      asset::from_string("0.1000 REOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"),
      exchange( "alice"_n, EVO, extend(asset::from_string("0.0000 EOS")),
      asset::from_string("-0.1000 VOICE")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"),
      exchange( "alice"_n, EVO, extend(asset::from_string("-1000004.0000 EOS")),
      asset::from_string("-0.0001 VOICE")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"),
      exchange( "alice"_n, EVO, extend(asset::from_string("-100000328.6280 VOICE")),
      asset::from_string("0.0000 EOS")) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"),
      exchange( "alice"_n, TUSD, extend(asset::from_string("8.0000 VOICE")),
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

    // {VOICE pool, EOS pool, supply}
    vector <int64_t> expected_system_balance = {999999185797, 10000073819, 100000328128};
    BOOST_REQUIRE_EQUAL(expected_system_balance == system_balance(EVO.value), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, EOS4), 89999926181);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, VOICE4), 1000000814203);

    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 50) );

    addliquidity( "alice"_n, asset::from_string("50.0000 EVO"),
      asset::from_string("10000000.0000 VOICE"), asset::from_string("10000000.0000 EOS") );

    expected_system_balance = {1000004186277, 10000123826, 100000828128};
    BOOST_REQUIRE_EQUAL(expected_system_balance == system_balance(EVO.value), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, EOS4), 89999876174);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, VOICE4), 999995813723);
 
    // The retired exact-output form is refused and leaves every balance as it was.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"),
      exchange( "alice"_n, EVO, extend(asset::from_string("-4.0000 EOS")),
                              asset::from_string("-401.9984 VOICE")) );
    BOOST_REQUIRE_EQUAL(expected_system_balance == system_balance(EVO.value), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, EOS4), 89999876174);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, VOICE4), 999995813723);

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
      extend(asset::from_string("96116860184.2738 VOICE")),
      extend(asset::from_string("23058430092.1369 EOS")), 10, name{});

    inittoken( "alice"_n, ETUSD3,
      extend(asset::from_string("9911686018427.38 TUSD")),
      extend(asset::from_string("10000000000.0000 EOS")), 10, name{});

    auto old_total = total();
    auto old_vec = system_balance(EVO.value);
    auto old_alice_bal_0 = balance("alice"_n, EOS4);
    auto old_alice_bal_1 = balance("alice"_n, VOICE4);

    BOOST_REQUIRE_EQUAL(success(), 
      exchange( "alice"_n, EVO, extend(asset::from_string("4.0000 EOS")),
      asset::from_string("1.0000 VOICE") ));
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, EOS4) - old_alice_bal_0, -40000);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, VOICE4) - old_alice_bal_1, 166570);

    old_total = total();
    old_vec = system_balance(EVO.value);
    old_alice_bal_0 = balance("alice"_n, EOS4);
    old_alice_bal_1 = balance("alice"_n, VOICE4);
    addliquidity( "alice"_n, asset::from_string("0.0001 EVO"),
      asset::from_string("10000000.0000 VOICE"), asset::from_string("10000000.0000 EOS") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, EOS4) - old_alice_bal_0, -2);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, VOICE4) - old_alice_bal_1, -4);

    produce_blocks();

    old_total = total();
    old_vec = system_balance(EVO.value);
    old_alice_bal_0 = balance("alice"_n, EOS4);
    old_alice_bal_1 = balance("alice"_n, VOICE4);
    remliquidity( "alice"_n, asset::from_string("0.0001 EVO"),
      asset::from_string("0.0000 VOICE"), asset::from_string("0.0000 EOS") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, EOS4) - old_alice_bal_0, 0);
    BOOST_REQUIRE_EQUAL(balance("alice"_n, VOICE4) - old_alice_bal_1, 2);

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
       asset::from_string("400000000000000.0000 VOICE"), asset::from_string("400000000000000.0000 EOS") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);

    old_total = total(); 
    old_vec = system_balance(EVO.value);
    exchange( "alice"_n, EVO, extend(asset::from_string("387592687324317.3478 EOS")), 
      asset::from_string("0.0001 VOICE") );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(is_increasing(old_vec, system_balance(EVO.value)), true);
    
    // The retired exact-output form is refused and moves nothing.
    old_total = total();
    old_vec = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"), exchange( "alice"_n, EVO,
      extend(asset::from_string("-1.0000 EOS")), asset::from_string("-0.1069 VOICE")) );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(old_vec == system_balance(EVO.value), true);

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"), exchange( "bob"_n, EVO,
      extend(asset::from_string("-12.0001 VOICE")), asset::from_string("-122.0329 EOS")) );
    BOOST_REQUIRE_EQUAL(old_total == total(), true);
    BOOST_REQUIRE_EQUAL(old_vec == system_balance(EVO.value), true);
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
        extend(asset::from_string("96116860184.2738 VOICE")),
        extend(asset::from_string("23058430092.1369 EOS")), 10, name{}) );

    // The memo's amount is parsed with overflow-checked arithmetic: a digit string
    // past int64 aborts at the digit that overflows, a scaled integer part past
    // int64 aborts at the scaling, more than 18 decimals is refused, and a sign
    // is not a digit.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("amount too large"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 99999999999999999999 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("amount too large"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 9223372036854775808 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("amount too large"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 922337203685477580.8 VOICE") );
    // ...and one that fits int64 but not an asset is the asset's own refusal.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("magnitude of asset amount must be less than 2^62"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 922337203685477580.7 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("precision should be <= 18"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 1.0000000000000000000 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("invalid character"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, -1.0000 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Missing decimal fraction after decimal point"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 16. VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Asset's amount and symbol should be separated with space"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 16.6570VOICE") );

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol mismatch"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 166536 VOICE") );

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 16.6571 VOICE") );

    // Look-alike symbols from another contract are not the system token and are
    // the first leg of no pair: refused before the memo is even read.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token is not a leg of any pair"),
      transfer( "carol"_n, "carol"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 16.6569 VOICE") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token is not a leg of any pair"),
      transfer( "carol"_n, "carol"_n, "sysio.swap"_n, asset::from_string("1.0000 VOICE"),
      "exchange: EVO, 0.0001 EOS") );

    int64_t pre_eos_balance = token_balance("sysio.token"_n, "alice"_n, EOS.value);
    int64_t pre_voice_balance = token_balance("anothertoken"_n, "alice"_n, VOICE.value);
    BOOST_REQUIRE_EQUAL( success(),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("4.0000 EOS"),
      "exchange: EVO, 16.6570 VOICE") );
    BOOST_REQUIRE_EQUAL( pre_eos_balance - 40000, token_balance("sysio.token"_n, "alice"_n, EOS.value) );
    BOOST_REQUIRE_EQUAL( pre_voice_balance + 166570, token_balance("anothertoken"_n, "alice"_n, VOICE.value) );

    inittoken( "alice"_n, ETUSD3,
      extend(asset::from_string("9911686018427.38 TUSD")),
      extend(asset::from_string("10000000000.0000 EOS")), 10, name{});

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n,
        asset::from_string("400000.00 TUSD"), "exchange: ETUSD, 403.1606 EOS") );

    pre_eos_balance = token_balance("sysio.token"_n, "alice"_n, EOS.value);
    int64_t pre_tusd_balance = token_balance("sysio.token"_n, "alice"_n, TUSD.value);
    BOOST_REQUIRE_EQUAL( success(),
      transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("400000.00 TUSD"),
      "exchange: ETUSD, 403.1605 EOS") );

    BOOST_REQUIRE_EQUAL( pre_tusd_balance - 40000000,
      token_balance("sysio.token"_n, "alice"_n, TUSD.value) );
    BOOST_REQUIRE_EQUAL( pre_eos_balance + 4031605,
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
    // Only the system token and the first legs of existing pairs are accepted;
    // a first leg's seed gets in ahead of its pair only with the contract's authority.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token is not a leg of any pair"), transfer( "anothertoken"_n, "alice"_n, "sysio.swap"_n,
      asset::from_string("20000.0000 VOICE"), "") );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "anothertoken"_n, "alice"_n,
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
        ("initial_pool1", extend(asset::from_string("1.0000 ECO")))
        ("initial_pool2", extend(asset::from_string("1.0000 EOS")))
        ("initial_fee", 1) ("fee_authority", "carol"_n) ("locked_shares", asset::from_string("0.0000 EVO")) ("yield_leg", fc::variant()) )
    );
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"),
      push_action( "sysio.swap"_n, "bob"_n, "inittoken"_n, mvo()
        ("user", "alice"_n) ("new_symbol", EVO4)
        ("initial_pool1", extend(asset::from_string("1.0000 ECO")))
        ("initial_pool2", extend(asset::from_string("1.0000 EOS")))
        ("initial_fee", 1) ("fee_authority", "carol"_n) ("locked_shares", asset::from_string("0.0000 EVO")) ("yield_leg", fc::variant()) )
    );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Both assets must be positive"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("-0.1000 VOICE")),
      extend(asset::from_string("0.0001 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Both assets must be positive"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.1000 VOICE")),
      extend(asset::from_string("-0.0001 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Initial amounts must be less than 10^15"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("100000000000.0001 VOICE")),
      extend(asset::from_string("0.0001 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("Initial amounts must be less than 10^15"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("1.0001 VOICE")),
      extend(asset::from_string("100000000000.0001 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended symbols must be different"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.1000 EOS")),
      extend(asset::from_string("0.0001 EOS")), 10, name{}) );
    // The second leg is the system token, whichever way round the legs are given
    // and whatever contract a look-alike symbol comes from.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("the second leg must be the system token"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.0001 EOS")),
      extend(asset::from_string("0.1000 VOICE")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("the second leg must be the system token"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.1000 VOICE")),
      extended_asset{asset::from_string("0.0001 EOS"), "anothertoken"_n}, 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient funds"),
      inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.1000 VOICE")),
      extend(asset::from_string("0.0003 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL(  wasm_assert_msg("insufficient funds"),
      inittoken( "alice"_n, EVO4,
      extend(asset::from_string("100000000.0000 VOICE")),
      extend(asset::from_string("0.0002 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("new_symbol precision must be (precision1 + precision2) / 2"),
      inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.100 VOICE")),
      extend(asset::from_string("0.0001 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.1000 VOICE")),
      extend(asset::from_string("0.0001 EOS")), 10, name{}) );
    produce_blocks();
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token symbol already exists"), inittoken( "alice"_n, EVO4,
      extend(asset::from_string("0.1000 VOICE")),
      extend(asset::from_string("0.0001 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee out of range"), inittoken( "alice"_n, ETUSD3,
      extend(asset::from_string("0.10 TUSD")),
      extend(asset::from_string("0.0001 EOS")), 10000, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee out of range"), inittoken( "alice"_n, ETUSD3,
      extend(asset::from_string("0.10 TUSD")),
      extend(asset::from_string("0.0001 EOS")), -1, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee authority account does not exist"),
      inittoken( "alice"_n, ETUSD3,
      extend(asset::from_string("0.10 TUSD")),
      extend(asset::from_string("0.0001 EOS")), 10, "natalia"_n) );
// The assert "the pool is already indexed" is tested in "indextable" test case.
// The fee authority itself is exercised in "fee_authority_configuration".

  // TRANSFER: the pair token itself is the first leg of no pair, so it cannot
  // be deposited back into the contract.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token is not a leg of any pair"),
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
    BOOST_REQUIRE_EQUAL( error("missing authority of sysio"), changefee(EVO, 50, "bob"_n) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), changefee(EOS, 500));
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 50) );

} FC_LOG_AND_RETHROW()

// A fixture whose deployment step has NOT run: no fee authority, no system token.
struct sysio_swap_unconfigured_tester : public sysio_swap_tester {
    sysio_swap_unconfigured_tester() : sysio_swap_tester( false ) {}
};

BOOST_FIXTURE_TEST_CASE( nothing_works_before_setconfig, sysio_swap_unconfigured_tester ) try {
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    // Without a system token nothing can be deposited, with or without the
    // contract's authority, and no pair can be created, whoever its authority is.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("swap not configured"),
        transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("1.0000 EOS"), "") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("swap not configured"),
        seed_transfer( "anothertoken"_n, "bob"_n, asset::from_string("1.0000 VOICE"), "deposit to: alice") );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("swap not configured"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("swap not configured"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, "alice"_n) );
    // setconfig is the contract's own call, and checks what it is given.
    BOOST_REQUIRE_EQUAL( error("missing authority of sysio.swap"), setconfig( "alice"_n, WIRE, "alice"_n ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee authority account does not exist"), setconfig( "natalia"_n ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("system token contract does not exist"),
        setconfig( config::system_account_name, extended_symbol{ EOS4, "natalia"_n } ) );
    BOOST_REQUIRE_EQUAL( success(), setconfig( config::system_account_name ) );
    // Configured: the system token deposits freely, a first leg with the
    // contract's authority, and pairs form against the system token.
    many_transfer();
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, "alice"_n) );
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 25, "alice"_n) );
    BOOST_REQUIRE_EQUAL( 25, pool_fee(EVO) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset::from_string("1.00 TUSD")), extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( success(), changefee(ETUSD, 25) );
    // Once the pair exists its first leg deposits on its own.
    BOOST_REQUIRE_EQUAL( success(), transfer( "anothertoken"_n, "bob"_n, "sysio.swap"_n, asset::from_string("0.0001 VOICE"), "") );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( seed_locks_shares, sysio_swap_tester ) try {
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    many_transfer();
    const int64_t minted = 100'000'000'000;   // sqrt(1e10 * 1e12)
    const int64_t locked = 5'000'000;
    const auto lock_of = [&](int64_t units) { return asset(units, EVO4); };

    // The lock must be in the new symbol, nonnegative, and leave the creator something.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("locked_shares must be in new_symbol"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("100000000.0000 VOICE")), extend(asset::from_string("1000000.0000 EOS")),
        10, name{}, asset(locked, VOICE4) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("locked_shares must be nonnegative"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("100000000.0000 VOICE")), extend(asset::from_string("1000000.0000 EOS")),
        10, name{}, lock_of(-1) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("locked_shares must leave the creator at least one share"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("100000000.0000 VOICE")), extend(asset::from_string("1000000.0000 EOS")),
        10, name{}, lock_of(minted) ) );

    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("100000000.0000 VOICE")), extend(asset::from_string("1000000.0000 EOS")),
        10, name{}, lock_of(locked) ) );
    // The whole geometric mean is supply; the creator holds all of it but the lock.
    auto pool = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( minted, pool.at(2) );
    BOOST_REQUIRE_EQUAL( minted - locked, lp_balance("alice"_n, EVO) );
    BOOST_REQUIRE_EQUAL( lock_of(locked).to_string(), get_balance("sysio.swap"_n, name(EVO.value), "stat"_n, EVO.value,
        "currency_stats")["locked_shares"].as_string() );

    // Removing every share the creator holds succeeds and leaves the locked
    // shares' slice of the pools behind: the pair can never be emptied.
    BOOST_REQUIRE_EQUAL( success(), remliquidity( "alice"_n, lock_of(minted - locked), asset(0, VOICE4), asset(0, EOS4) ) );
    pool = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( locked, pool.at(2) );
    BOOST_REQUIRE_EQUAL( reference::remove_leg(locked, 10'000'000'000, minted), pool.at(1) );   // the EOS the lock still backs
    BOOST_REQUIRE_EQUAL( 0, lp_balance("alice"_n, EVO) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("overdrawn balance"),
        remliquidity( "alice"_n, lock_of(1), asset(0, VOICE4), asset(0, EOS4) ) );

    // The pool keeps working from the locked floor: pricing uses the full supply.
    const auto before = system_balance(EVO.value);
    const int64_t pay1 = reference::add_leg(locked, before[0], before[2]);
    const int64_t pay2 = reference::add_leg(locked, before[1], before[2]);
    BOOST_REQUIRE_EQUAL( success(), addliquidity( "alice"_n, lock_of(locked), asset(pay1, VOICE4), asset(pay2, EOS4) ) );
    BOOST_REQUIRE_EQUAL( 2 * locked, system_balance(EVO.value).at(2) );
    BOOST_REQUIRE_GE( settle_swap("alice"_n, EVO, asset(1000, EOS4), VOICE4, 0), 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Yield pools: the optional yield leg and its uniqueness rule.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( yield_leg_rules, sysio_swap_tester ) try {
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    many_transfer();
    const extended_symbol voice{ VOICE4, "anothertoken"_n };
    const extended_symbol tusd{ TUSD2, "sysio.token"_n };

    // The yield leg must be the pair's first leg: not another token, not the
    // system token, not a look-alike from another contract.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield_leg must be the pair's first leg"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, name{}, 0, tusd ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield_leg must be the pair's first leg"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, name{}, 0, WIRE ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield_leg must be the pair's first leg"), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, name{}, 0,
        extended_symbol{ VOICE4, "sysio.token"_n } ) );

    // EVO is a yield pool on VOICE; ETUSD is a plain pool.
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, name{}, 0, voice ) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset::from_string("1.00 TUSD")), extend(asset::from_string("1.0000 EOS")), 10, name{} ) );
    auto evo = pair_row(EVO);
    BOOST_REQUIRE_EQUAL( "4,VOICE", evo["yield_leg"]["sym"].as_string() );
    BOOST_REQUIRE_EQUAL( "anothertoken", evo["yield_leg"]["contract"].as_string() );
    BOOST_REQUIRE_EQUAL( 0u, evo["conversion_horizon_sec"].as_uint64() );
    BOOST_REQUIRE_EQUAL( 0u, evo["depth_cap_bps"].as_uint64() );
    BOOST_REQUIRE( pair_row(ETUSD)["yield_leg"].is_null() );

    // One yield pool per shadow follows from one pair per first leg: VOICE
    // cannot form a second pair, yielding or plain.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("the pool is already indexed"), inittoken( "alice"_n,
        symbol::from_string("4,BVO"), extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")),
        10, name{}, 0, voice ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("the pool is already indexed"), inittoken( "alice"_n,
        symbol::from_string("4,BVO"), extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")),
        10, name{} ) );

    // setyield: fee authority only, yield pools only, cap within basis points,
    // clip floor within an asset.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair has no yield leg"), setyield( ETUSD, 86400, 3, 1000 ) );
    BOOST_REQUIRE_EQUAL( error("missing authority of sysio"), setyield( EVO, 86400, 3, 1000, "alice"_n ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("depth_cap_bps out of range"), setyield( EVO, 86400, 10001, 1000 ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("clip_floor out of range"), setyield( EVO, 86400, 3, -1 ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), setyield( EOS, 86400, 3, 1000 ) );
    BOOST_REQUIRE_EQUAL( success(), setyield( EVO, 86400, 3, 1000 ) );
    evo = pair_row(EVO);
    BOOST_REQUIRE_EQUAL( 86400u, evo["conversion_horizon_sec"].as_uint64() );
    BOOST_REQUIRE_EQUAL( 3u, evo["depth_cap_bps"].as_uint64() );
    BOOST_REQUIRE_EQUAL( 1000, evo["clip_floor"].as_int64() );
    // The leg is fixed at creation; setyield does not touch it.
    BOOST_REQUIRE_EQUAL( "4,VOICE", evo["yield_leg"]["sym"].as_string() );

    // A plain token in the yield leg has no distribution state: it is owed
    // nothing, and every accrual point is a no-op rather than a failure.
    const auto before = system_balance( EVO.value );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( EVO ) );
    BOOST_REQUIRE( before == system_balance( EVO.value ) );
    BOOST_REQUIRE( pending_payout( "anothertoken"_n ).empty() );
    // Only yield pools accrue; a missing pair is reported as such.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair has no yield leg"), accrueyield( ETUSD ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), accrueyield( EOS ) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Yield accrual: what the pool is owed on the shadow it holds lands in its
// other leg, computed from the token's public state and asserted on receipt.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( yield_accrues_into_the_pool_without_minting, sysio_swap_tester ) try {
    setup_yield_pool();
    const auto pool_wire_at_token = [&]() { return token_balance( "sysio.token"_n, "sysio.swap"_n, EOS.value ); };
    const int64_t supply = system_balance( SHEO.value ).at(2);

    // Nothing distributed yet: accrual is a no-op and leaves no trace.
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( YieldPoolWire, system_balance( SHEO.value ).at(1) );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );

    // bob donates 100 EOS to SHD holders: the index advances by the spec, the
    // truncation remainder is carried, and the pot holds the whole donation.
    const int64_t first_donation = 100'0000;
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( first_donation, EOS4 ), SHD ) );
    const auto first = yield_reference::distribute( first_donation, ShadowIssuance, 0 );
    auto idx = shadow_index( SHD );
    BOOST_REQUIRE_EQUAL( first.index_delta, idx.index );
    BOOST_REQUIRE_EQUAL( first.carry, idx.carry );
    BOOST_REQUIRE_EQUAL( uint64_t(first_donation), idx.pot );
    BOOST_REQUIRE_LT( 0u, idx.carry );   // the chosen supply does not divide evenly

    // The pool is owed its share, floored; accruing credits exactly that to the
    // EOS side, mints nothing, and the token delivers the same amount in the
    // same transaction: the receipt is retired and the contract's EOS grew by it.
    const int64_t owed1 = yield_reference::owed( YieldPoolShadow, idx.index, 0 );
    BOOST_REQUIRE_EQUAL( 333333, owed1 );   // 1e6 units * 1/3, floored
    const int64_t wire_before = pool_wire_at_token();
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    auto pool = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( YieldPoolShadow,        pool.at(0) );
    BOOST_REQUIRE_EQUAL( YieldPoolWire + owed1,  pool.at(1) );
    BOOST_REQUIRE_EQUAL( supply,                 pool.at(2) );
    BOOST_REQUIRE_EQUAL( wire_before + owed1, pool_wire_at_token() );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );
    auto held = shadow_account( "sysio.swap"_n, SHD );
    BOOST_REQUIRE_EQUAL( idx.index, held.index_checkpoint );
    BOOST_REQUIRE_EQUAL( 0u, held.owed_wire );
    BOOST_REQUIRE_EQUAL( uint64_t(first_donation - owed1), shadow_index( SHD ).pot );

    // Settled means settled: a second accrual changes nothing.
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE( pool == system_balance( SHEO.value ) );
    BOOST_REQUIRE_EQUAL( wire_before + owed1, pool_wire_at_token() );

    // Two more distributions before the next accrual: the carry chains through
    // them, and one accrual collects the pool's share of both.
    const int64_t second_donation = 7'0001, third_donation = 50'0000;
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( second_donation, EOS4 ), SHD ) );
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( third_donation, EOS4 ), SHD ) );
    const auto second = yield_reference::distribute( second_donation, ShadowIssuance, first.carry );
    const auto third  = yield_reference::distribute( third_donation,  ShadowIssuance, second.carry );
    const uint64_t index_after_three = first.index_delta + second.index_delta + third.index_delta;
    idx = shadow_index( SHD );
    BOOST_REQUIRE_EQUAL( index_after_three, idx.index );
    BOOST_REQUIRE_EQUAL( third.carry, idx.carry );
    const int64_t owed2 = yield_reference::owed( YieldPoolShadow, idx.index, held.index_checkpoint );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    pool = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( YieldPoolWire + owed1 + owed2, pool.at(1) );
    BOOST_REQUIRE_EQUAL( supply, pool.at(2) );
    BOOST_REQUIRE_EQUAL( wire_before + owed1 + owed2, pool_wire_at_token() );
    BOOST_REQUIRE_EQUAL( idx.index, shadow_account( "sysio.swap"_n, SHD ).index_checkpoint );

    // alice, holding the other two thirds directly, is owed by the same formula,
    // and the pot ends holding only what flooring left behind.
    const int64_t owed_alice = yield_reference::owed( ShadowIssuance - YieldPoolShadow, idx.index, 0 );
    const int64_t alice_before = token_balance( "sysio.token"_n, "alice"_n, EOS.value );
    BOOST_REQUIRE_EQUAL( success(), shadow_claim( "alice"_n, SHD ) );
    BOOST_REQUIRE_EQUAL( alice_before + owed_alice, token_balance( "sysio.token"_n, "alice"_n, EOS.value ) );
    const int64_t donated = first_donation + second_donation + third_donation;
    BOOST_REQUIRE_EQUAL( uint64_t(donated - owed1 - owed2 - owed_alice), shadow_index( SHD ).pot );
    BOOST_REQUIRE_LT( shadow_index( SHD ).pot, 3u );   // at most one unit of dust per holder
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( yield_is_credited_before_shares_are_priced, sysio_swap_tester ) try {
    setup_yield_pool();
    const int64_t donation = 300'0000;
    // alice deposits more shadow to mint with. The contract now holds shadow that
    // is not in the pool, and the token pays yield on all of it: deposit
    // accounts are not yield-bearing, the pool takes what its custody earns.
    const int64_t alice_extra_shadow = 500'000'0000;
    BOOST_REQUIRE_EQUAL( success(), shadow_transfer( "alice"_n, "sysio.swap"_n, asset( alice_extra_shadow, SHD4 ), "" ) );
    const int64_t contract_shadow = YieldPoolShadow + alice_extra_shadow;
    BOOST_REQUIRE_EQUAL( contract_shadow, shadow_account( "sysio.swap"_n, SHD ).balance.get_amount() );
    // What the contract is owed on its whole holding at the current index.
    const auto owed_now = [&]() {
        const auto held = shadow_account( "sysio.swap"_n, SHD );
        return yield_reference::owed( held.balance.get_amount(), shadow_index( SHD ).index,
                                      held.index_checkpoint, held.owed_wire );
    };

    // A mint after a distribution prices against the accrued pool: the yield
    // belongs to the shares that existed, so the new shares pay for their cut.
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( donation, EOS4 ), SHD ) );
    auto before = system_balance( SHEO.value );
    int64_t owed = owed_now();
    BOOST_REQUIRE_LT( yield_reference::owed( before.at(0), shadow_index( SHD ).index, 0 ), owed );
    const int64_t shares = 1000'0000;
    const int64_t pay_shadow = reference::add_leg( shares, before.at(0), before.at(2) );
    const int64_t pay_wire   = reference::add_leg( shares, before.at(1) + owed, before.at(2) );
    const int64_t alice_shadow = deposit_of( "alice"_n, SHADOW );
    const int64_t alice_wire   = deposit_of( "alice"_n, WIRE );
    BOOST_REQUIRE_EQUAL( success(), addliquidity( "alice"_n, asset( shares, SHEO4 ),
                                                  asset( pay_shadow, SHD4 ), asset( pay_wire, EOS4 ) ) );
    auto after = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( before.at(0) + pay_shadow,        after.at(0) );
    BOOST_REQUIRE_EQUAL( before.at(1) + owed + pay_wire,   after.at(1) );
    BOOST_REQUIRE_EQUAL( before.at(2) + shares,            after.at(2) );
    BOOST_REQUIRE_EQUAL( alice_shadow - pay_shadow, deposit_of( "alice"_n, SHADOW ) );
    BOOST_REQUIRE_EQUAL( alice_wire - pay_wire,     deposit_of( "alice"_n, WIRE ) );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );
    // Paying one unit less than the accrued price is refused: the quote is the
    // accrued one, not the stale one a caller might compute from the row.
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( donation, EOS4 ), SHD ) );
    before = after;
    owed = owed_now();
    BOOST_REQUIRE_LT( 0, owed );
    const int64_t stale_wire = reference::add_leg( shares, before.at(1), before.at(2) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
        addliquidity( "alice"_n, asset( shares, SHEO4 ), asset( pay_shadow, SHD4 ), asset( stale_wire, EOS4 ) ) );
    // The refused action left nothing behind: no credit, no receipt.
    BOOST_REQUIRE( before == system_balance( SHEO.value ) );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );

    // A burn after a distribution pays out of the accrued pool too.
    const int64_t get_shadow = reference::remove_leg( shares, before.at(0), before.at(2) );
    const int64_t get_wire   = reference::remove_leg( shares, before.at(1) + owed, before.at(2) );
    const int64_t alice_wire_before = deposit_of( "alice"_n, WIRE );
    BOOST_REQUIRE_EQUAL( success(), remliquidity( "alice"_n, asset( shares, SHEO4 ),
                                                  asset( get_shadow, SHD4 ), asset( get_wire, EOS4 ) ) );
    after = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( before.at(0) - get_shadow,        after.at(0) );
    BOOST_REQUIRE_EQUAL( before.at(1) + owed - get_wire,   after.at(1) );
    BOOST_REQUIRE_EQUAL( before.at(2) - shares,            after.at(2) );
    BOOST_REQUIRE_EQUAL( alice_wire_before + get_wire, deposit_of( "alice"_n, WIRE ) );
    // Mint and burn move shadow between deposits and the pool, never out of the
    // contract, and its row is settled at the current index: nothing is owed
    // until the next distribution.
    const auto held = shadow_account( "sysio.swap"_n, SHD );
    BOOST_REQUIRE_EQUAL( contract_shadow, held.balance.get_amount() );
    BOOST_REQUIRE_EQUAL( shadow_index( SHD ).index, held.index_checkpoint );
    BOOST_REQUIRE_EQUAL( 0u, held.owed_wire );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE( after == system_balance( SHEO.value ) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( yield_accrues_before_a_swap_is_priced, sysio_swap_tester ) try {
    setup_yield_pool();
    const auto shadow_pool_of = [&]( const vector<int64_t>& pool ) { return pool.at(0); };
    const auto wire_pool_of   = [&]( const vector<int64_t>& pool ) { return pool.at(1); };
    const int     fee = pool_fee( SHEO );
    const int64_t pay = 1000'0000;

    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( 100'0000, EOS4 ), SHD ) );
    auto before = system_balance( SHEO.value );
    const auto held = shadow_account( "sysio.swap"_n, SHD );
    const int64_t owed = yield_reference::owed( held.balance.get_amount(), shadow_index( SHD ).index,
                                                held.index_checkpoint, held.owed_wire );
    BOOST_REQUIRE_LT( 0, owed );

    // Buying shadow prices against the pool WITH the owed yield in it, which is
    // strictly worse for the buyer than the stale quote: the WIRE side is larger,
    // so a given payment buys less.
    const int64_t accrued_out = reference::receive( pay, wire_pool_of(before) + owed, shadow_pool_of(before), fee );
    const int64_t stale_out   = reference::receive( pay, wire_pool_of(before),        shadow_pool_of(before), fee );
    BOOST_REQUIRE_LT( accrued_out, stale_out );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
        exchange( "alice"_n, SHEO, extend(asset(pay, EOS4)), asset(stale_out, SHD4) ) );
    BOOST_REQUIRE_EQUAL( success(),
        exchange( "alice"_n, SHEO, extend(asset(pay, EOS4)), asset(accrued_out, SHD4) ) );
    auto after = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( wire_pool_of(before) + owed + pay,   wire_pool_of(after) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) - accrued_out, shadow_pool_of(after) );
    BOOST_REQUIRE_EQUAL( before.at(2), after.at(2) );          // no shares minted
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );

    // Buy, settle, sell back. With the yield already in the pool before the buy
    // is priced there is nothing left to get in front of, so the round trip only
    // pays the fee twice and alice ends with less WIRE than she started.
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( 100'0000, EOS4 ), SHD ) );
    const int64_t wire_start   = deposit_of( "alice"_n, WIRE );
    const int64_t shadow_start = deposit_of( "alice"_n, SHADOW );
    BOOST_REQUIRE_EQUAL( success(), exchange( "alice"_n, SHEO, extend(asset(pay, EOS4)), asset(0, SHD4) ) );
    const int64_t bought = deposit_of( "alice"_n, SHADOW ) - shadow_start;
    BOOST_REQUIRE_LT( 0, bought );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );     // nothing left pending
    BOOST_REQUIRE_EQUAL( success(), exchange( "alice"_n, SHEO, shd(bought), asset(0, EOS4) ) );
    BOOST_REQUIRE_EQUAL( shadow_start, deposit_of( "alice"_n, SHADOW ) );
    BOOST_REQUIRE_LT( deposit_of( "alice"_n, WIRE ), wire_start );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( yield_payout_route_is_exact, sysio_swap_tester ) try {
    setup_yield_pool();
    // A transfer from a shadow contract with no claim outstanding is an ordinary
    // deposit, and needs the ordinary deposit row: the payout route only exists
    // while a receipt does.
    BOOST_REQUIRE_EQUAL( success(), shadow_issue( "alice"_n, "shadowtoken"_n, asset( 1'0000, SHD4 ) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("extended_symbol not registered for this user,"
                                         " please run openext action or write exchange details in the memo of your transfer"),
        shadow_transfer( "shadowtoken"_n, "sysio.swap"_n, asset( 1'0000, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( success(), openext( "shadowtoken"_n, "alice"_n, SHADOW ) );
    BOOST_REQUIRE_EQUAL( success(), shadow_transfer( "shadowtoken"_n, "sysio.swap"_n, asset( 1'0000, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( 1'0000, deposit_of( "shadowtoken"_n, SHADOW ) );
    BOOST_REQUIRE_EQUAL( YieldPoolWire, system_balance( SHEO.value ).at(1) );

    // The shadow's own holding (issued above) makes it a holder too; a
    // distribution and an accrual still settle the contract's share exactly,
    // on everything it holds (the pool plus that one deposited unit).
    const int64_t donation = 10'0000;
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( donation, EOS4 ), SHD ) );
    const int64_t owed = yield_reference::owed( YieldPoolShadow + 1'0000, shadow_index( SHD ).index, 0 );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( YieldPoolWire + owed, system_balance( SHEO.value ).at(1) );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Yield funding: shadow announced with fundyield lands in the pair's
// reservoir, not in the funder's deposit.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( yield_funding_fills_the_reservoir, sysio_swap_tester ) try {
    setup_yield_pool();
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset::from_string("1.00 TUSD")), extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    BOOST_REQUIRE( !has_reservoir( ETUSD ) );   // plain pools queue nothing

    // Only a yield pool, in its shadow symbol, a positive amount, by the funder.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair has no yield leg"), fundyield( "alice"_n, ETUSD, asset( 1'0000, EOS4 ) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), fundyield( "alice"_n, EOS, asset( 1'0000, SHD4 ) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("quantity must be in the pair's shadow symbol"),
                         fundyield( "alice"_n, SHEO, asset( 1'0000, EOS4 ) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("quantity must be positive"), fundyield( "alice"_n, SHEO, asset( 0, SHD4 ) ) );
    BOOST_REQUIRE_EQUAL( error("missing authority of bob"), push_action( "sysio.swap"_n, "alice"_n, "fundyield"_n, mvo()
        ( "from", "bob"_n )( "pair_token", SHEO )( "quantity", asset( 1'0000, SHD4 ) ) ) );
    BOOST_REQUIRE( pending_funding( "alice"_n ).is_null() );

    // Announce, then deliver in a later transaction: until the announced amount
    // arrives every other transfer from the funder is refused, the deposit is
    // untouched, and the delivery goes to the reservoir.
    const int64_t first = 100'0000;
    const int64_t alice_deposit = deposit_of( "alice"_n, SHADOW );
    BOOST_REQUIRE_EQUAL( success(), fundyield( "alice"_n, SHEO, asset( first, SHD4 ) ) );
    auto pending = pending_funding( "alice"_n );
    BOOST_REQUIRE_EQUAL( SHEO4.name(), pending["pair"].as_string() );
    BOOST_REQUIRE_EQUAL( asset( first, SHD4 ).to_string(), pending["quantity"]["quantity"].as_string() );
    BOOST_REQUIRE_EQUAL( "shadowtoken", pending["quantity"]["contract"].as_string() );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield funding does not match the pending fundyield"),
                         shadow_transfer( "alice"_n, "sysio.swap"_n, asset( first / 2, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield funding does not match the pending fundyield"),
                         transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset( 1'0000, EOS4 ), "" ) );
    BOOST_REQUIRE_EQUAL( success(), shadow_transfer( "alice"_n, "sysio.swap"_n, asset( first, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( first, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( alice_deposit, deposit_of( "alice"_n, SHADOW ) );
    BOOST_REQUIRE( pending_funding( "alice"_n ).is_null() );
    BOOST_REQUIRE_EQUAL( YieldPoolShadow, system_balance( SHEO.value ).at(0) );   // not in the pool
    // With nothing pending, a transfer is an ordinary deposit again.
    BOOST_REQUIRE_EQUAL( success(), shadow_transfer( "alice"_n, "sysio.swap"_n, asset( 1'0000, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( alice_deposit + 1'0000, deposit_of( "alice"_n, SHADOW ) );
    BOOST_REQUIRE_EQUAL( first, reservoir_of( SHEO ) );

    // A new announcement replaces a pending one.
    const int64_t second = 20'0000;
    BOOST_REQUIRE_EQUAL( success(), fundyield( "alice"_n, SHEO, asset( 30'0000, SHD4 ) ) );
    BOOST_REQUIRE_EQUAL( success(), fundyield( "alice"_n, SHEO, asset( second, SHD4 ) ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield funding does not match the pending fundyield"),
                         shadow_transfer( "alice"_n, "sysio.swap"_n, asset( 30'0000, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( success(), shadow_transfer( "alice"_n, "sysio.swap"_n, asset( second, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( first + second, reservoir_of( SHEO ) );

    // The intended shape: both steps in one transaction.
    const int64_t third = 7'0000;
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( third, SHD4 ) );
    BOOST_REQUIRE_EQUAL( first + second + third, reservoir_of( SHEO ) );
    BOOST_REQUIRE( pending_funding( "alice"_n ).is_null() );

    // The announcement's row is the funder's, not the contract's, and cancelling
    // gives it back. Otherwise one abandoned announcement per account that ever
    // called fundyield would sit on the contract's RAM with no way to reclaim it.
    {
        const int64_t swap_ram = ram_usage( "sysio.swap"_n );
        const int64_t bob_ram  = ram_usage( "bob"_n );
        BOOST_REQUIRE_EQUAL( success(), fundyield( "bob"_n, SHEO, asset( 5'0000, SHD4 ) ) );
        BOOST_REQUIRE_LT( bob_ram, ram_usage( "bob"_n ) );
        BOOST_REQUIRE_EQUAL( swap_ram, ram_usage( "sysio.swap"_n ) );
        // While it is pending bob cannot deposit anything else...
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield funding does not match the pending fundyield"),
                             transfer( "sysio.token"_n, "bob"_n, "sysio.swap"_n, asset( 1'0000, EOS4 ), "" ) );
        // ...and cancelling is his own call, not anyone else's.
        BOOST_REQUIRE_EQUAL( error("missing authority of bob"), cancelyield( "bob"_n, "alice"_n ) );
        BOOST_REQUIRE_EQUAL( success(), cancelyield( "bob"_n ) );
        BOOST_REQUIRE( pending_funding( "bob"_n ).is_null() );
        BOOST_REQUIRE_EQUAL( bob_ram, ram_usage( "bob"_n ) );
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("no pending fundyield"), cancelyield( "bob"_n ) );
        // Ordinary deposits work again.
        BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "bob"_n, "sysio.swap"_n, asset( 1'0000, EOS4 ), "" ) );
        BOOST_REQUIRE_EQUAL( first + second + third, reservoir_of( SHEO ) );
    }

    // The reservoir is the contract's shadow too: it earns for the pool.
    BOOST_REQUIRE_EQUAL( YieldPoolShadow + 1'0000 + first + second + third,
                         shadow_account( "sysio.swap"_n, SHD ).balance.get_amount() );
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( 100'0000, EOS4 ), SHD ) );
    const int64_t owed = yield_reference::owed( YieldPoolShadow + 1'0000 + first + second + third,
                                                shadow_index( SHD ).index, 0 );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( YieldPoolWire + owed, system_balance( SHEO.value ).at(1) );
    BOOST_REQUIRE_EQUAL( first + second + third, reservoir_of( SHEO ) );   // accrual leaves the queue alone
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The yield tick: the reservoir sells through the pool in clips paced by the
// horizon, and the proceeds go back to the shadow's holders.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( yield_tick_sells_the_reservoir_over_the_horizon, sysio_swap_tester ) try {
    setup_yield_pool();
    // The token moves the proceeds out of the contract under the contract's own
    // authority: the deployment grants it the shadow token's sysio.code seat.
    grant_shadow_code( "sysio.swap"_n, true );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset::from_string("1.00 TUSD")), extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    const uint32_t horizon_sec = 3600;
    const uint32_t cap_bps     = 1;
    // Sized the way setyield's docs prescribe: at a 0.1% pair fee the proportional
    // fee reaches a whole unit at an output of 1000, which is where MIN_SWAP_FEE
    // stops being the binding fee.
    const int64_t  clip_floor  = 1000;
    const int      fee         = pool_fee( SHEO );
    const auto shadow_pool_of  = [&]( const vector<int64_t>& pool ) { return pool.at(0); };   // SHD is pool1
    const auto wire_pool_of    = [&]( const vector<int64_t>& pool ) { return pool.at(1); };

    // Only a yield pool with its parameters set can tick.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair has no yield leg"), tickyield( ETUSD ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("pair token does not exist"), tickyield( EOS ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield tick parameters not set"), tickyield( SHEO ) );
    // A floor of zero is as unset as a horizon of zero: the tick refuses it.
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, 0 ) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("yield tick parameters not set"), tickyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, clip_floor ) );
    const int64_t set_at = last_tick_us( SHEO );
    BOOST_REQUIRE_EQUAL( control->head_block_time().time_since_epoch().count(), set_at );   // setyield starts the clock

    // An empty reservoir: the tick is a no-op and the clock is untouched.
    auto before = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE( before == system_balance( SHEO.value ) );
    BOOST_REQUIRE_EQUAL( set_at, last_tick_us( SHEO ) );

    // Funding an empty reservoir restarts the clock: the queue sells over a
    // fresh horizon from the moment it is funded.
    const int64_t queued = 1000'0000;
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( queued, SHD4 ) );
    const int64_t funded_at = last_tick_us( SHEO );
    BOOST_REQUIRE_LT( set_at, funded_at );
    BOOST_REQUIRE_EQUAL( queued, reservoir_of( SHEO ) );

    // One tick, one block later: the clip is the queue's share of the elapsed
    // horizon (rounded up), sold at the pool's curve and fee; the proceeds leave
    // the contract for the token's pot, which advances the index by the spec.
    before = system_balance( SHEO.value );
    auto idx_before = shadow_index( SHD );
    const int64_t contract_wire_before = token_balance( "sysio.token"_n, "sysio.swap"_n, EOS.value );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    const int64_t ticked_at = last_tick_us( SHEO );
    int64_t clip = yield_reference::clip_size( queued, ticked_at - funded_at, horizon_sec,
                                               shadow_pool_of(before), cap_bps, clip_floor );
    BOOST_REQUIRE_LT( clip_floor, clip );      // over the floor, so it sells
    BOOST_REQUIRE_LT( clip, queued / 1000 );   // a block is a sliver of the horizon
    int64_t proceeds = reference::receive( clip, shadow_pool_of(before), wire_pool_of(before), fee );
    auto after = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + clip,   shadow_pool_of(after) );
    BOOST_REQUIRE_EQUAL( wire_pool_of(before) - proceeds, wire_pool_of(after) );
    BOOST_REQUIRE_EQUAL( before.at(2), after.at(2) );
    BOOST_REQUIRE_EQUAL( queued - clip, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( contract_wire_before - proceeds, token_balance( "sysio.token"_n, "sysio.swap"_n, EOS.value ) );
    auto distributed = yield_reference::distribute( proceeds, ShadowIssuance, idx_before.carry );
    auto idx_after = shadow_index( SHD );
    BOOST_REQUIRE_EQUAL( idx_before.index + distributed.index_delta, idx_after.index );
    BOOST_REQUIRE_EQUAL( idx_before.pot + uint64_t(proceeds), idx_after.pot );
    // The pool kept the fee: the product grew.
    BOOST_REQUIRE( is_increasing( before, after ) );
    BOOST_REQUIRE_LT( wide_product( before ), wide_product( after ) );

    // The proceeds come back: the contract is a holder, so the next accrual
    // (explicit here; every later tick performs it too) credits the pool its
    // share of what the tick distributed.
    const auto held = shadow_account( "sysio.swap"_n, SHD );
    const int64_t returned = yield_reference::owed( held.balance.get_amount(), idx_after.index,
                                                    held.index_checkpoint, held.owed_wire );
    BOOST_REQUIRE_LT( 0, returned );
    BOOST_REQUIRE_LT( returned, proceeds );   // alice holds the rest of the shadow
    before = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( wire_pool_of(before) + returned, wire_pool_of( system_balance( SHEO.value ) ) );

    // Pounding the tick does not sell faster: a second tick in the same block
    // sees no elapsed time and does nothing, and consecutive blocks sell only
    // the slivers of horizon they span.
    const int64_t queued_before_pair = reservoir_of( SHEO );
    before = system_balance( SHEO.value );
    tick_twice_in_one_transaction( SHEO );
    const int64_t paired_at = last_tick_us( SHEO );
    clip = yield_reference::clip_size( queued_before_pair, paired_at - ticked_at, horizon_sec,
                                       shadow_pool_of(before), cap_bps, clip_floor );
    BOOST_REQUIRE_EQUAL( queued_before_pair - clip, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + clip, shadow_pool_of( system_balance( SHEO.value ) ) );

    // After a gap longer than the horizon the time share is the whole queue,
    // and the depth cap is what bounds the clip.
    produce_block();
    produce_block( fc::hours(2) );
    before = system_balance( SHEO.value );
    const int64_t queued_before_gap = reservoir_of( SHEO );
    const int64_t clock_before_gap  = last_tick_us( SHEO );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    clip = yield_reference::clip_size( queued_before_gap, last_tick_us( SHEO ) - clock_before_gap, horizon_sec,
                                       shadow_pool_of(before), cap_bps, clip_floor );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) * cap_bps / yield_reference::BpsTotal, clip );
    BOOST_REQUIRE_LT( clip, queued_before_gap );
    BOOST_REQUIRE_EQUAL( queued_before_gap - clip, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + clip, shadow_pool_of( system_balance( SHEO.value ) ) );

    // With the cap lifted, the same gap drains the queue in one clip, and the
    // tick after that is a no-op again.
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, uint32_t(yield_reference::BpsTotal), clip_floor ) );
    produce_block();
    produce_block( fc::hours(2) );
    before = system_balance( SHEO.value );
    const int64_t remaining = reservoir_of( SHEO );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( 0, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + remaining, shadow_pool_of( system_balance( SHEO.value ) ) );
    const int64_t drained_at = last_tick_us( SHEO );
    // (Settle the drain's own proceeds first, so only the tick's trade is in question.)
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    before = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE( before == system_balance( SHEO.value ) );
    BOOST_REQUIRE_EQUAL( drained_at, last_tick_us( SHEO ) );

    // Accrual comes before the trade: a distribution the pool has not yet
    // collected is credited first, and the clip is priced against that pool.
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( queued, SHD4 ) );
    const int64_t refunded_at = last_tick_us( SHEO );
    BOOST_REQUIRE_EQUAL( success(), shadow_addyield( "bob"_n, asset( 100'0000, EOS4 ), SHD ) );
    const auto held_now = shadow_account( "sysio.swap"_n, SHD );
    const int64_t owed = yield_reference::owed( held_now.balance.get_amount(), shadow_index( SHD ).index,
                                                held_now.index_checkpoint, held_now.owed_wire );
    BOOST_REQUIRE_LT( 0, owed );
    before = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    clip = yield_reference::clip_size( queued, last_tick_us( SHEO ) - refunded_at, horizon_sec,
                                       shadow_pool_of(before), uint32_t(yield_reference::BpsTotal), clip_floor );
    proceeds = reference::receive( clip, shadow_pool_of(before), wire_pool_of(before) + owed, fee );
    after = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + clip,          shadow_pool_of(after) );
    BOOST_REQUIRE_EQUAL( wire_pool_of(before) + owed - proceeds, wire_pool_of(after) );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( yield_tick_cap_ignores_an_inflated_shadow_side, sysio_swap_tester ) try {
    setup_yield_pool();
    grant_shadow_code( "sysio.swap"_n, true );
    const uint32_t horizon_sec = 3600;
    const uint32_t cap_bps     = 1;
    const int64_t  clip_floor  = 1000;
    const auto shadow_pool_of  = [&]( const vector<int64_t>& pool ) { return pool.at(0); };

    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, clip_floor ) );
    const int64_t honest_depth = shadow_pool_of( system_balance( SHEO.value ) );
    BOOST_REQUIRE_EQUAL( honest_depth, last_tick_depth( SHEO ) );

    // Queue enough that the cap, not the time share, is what binds.
    const int64_t queued = 1'0000'0000;
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( queued, SHD4 ) );

    // Double the shadow side by selling into the pool. That is the same move
    // that makes a clip worth sandwiching, so a cap following the current side
    // would be set by the attacker it is meant to bound.
    const int64_t inflate = honest_depth;
    BOOST_REQUIRE_EQUAL( success(), shadow_transfer( "alice"_n, "sysio.swap"_n, asset( inflate, SHD4 ), "" ) );
    BOOST_REQUIRE_EQUAL( success(), exchange( "alice"_n, SHEO, shd(inflate), asset(0, EOS4) ) );
    const int64_t inflated_depth = shadow_pool_of( system_balance( SHEO.value ) );
    BOOST_REQUIRE_EQUAL( honest_depth + inflate, inflated_depth );
    BOOST_REQUIRE_EQUAL( honest_depth, last_tick_depth( SHEO ) );   // the record did not follow

    produce_block();
    produce_block( fc::hours(2) );
    const auto before = system_balance( SHEO.value );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    const int64_t sold = shadow_pool_of( system_balance( SHEO.value ) ) - shadow_pool_of( before );
    BOOST_REQUIRE_EQUAL( honest_depth * cap_bps / yield_reference::BpsTotal, sold );
    BOOST_REQUIRE_EQUAL( inflated_depth * cap_bps / yield_reference::BpsTotal, 2 * sold );   // what it would have been
    BOOST_REQUIRE_LT( sold, queued );                                                        // the cap bound it, not the queue

    // The tick records the pool it actually left, so the next one is bounded by
    // that rather than by the stale figure.
    BOOST_REQUIRE_EQUAL( shadow_pool_of( system_balance( SHEO.value ) ), last_tick_depth( SHEO ) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( yield_tick_never_sells_below_the_clip_floor, sysio_swap_tester ) try {
    setup_yield_pool();
    grant_shadow_code( "sysio.swap"_n, true );
    const uint32_t horizon_sec = 3600;
    const uint32_t cap_bps     = 1;            // cap = 1e6 against the 1e10 shadow side
    const int      fee         = pool_fee( SHEO );
    const auto shadow_pool_of  = [&]( const vector<int64_t>& pool ) { return pool.at(0); };
    const auto wire_pool_of    = [&]( const vector<int64_t>& pool ) { return pool.at(1); };
    const int64_t  queued      = 1000'0000;

    // A floor far above one block's share: the clip is short, so the tick sells
    // nothing AND leaves its clock alone. That is the whole point -- the unsold
    // time is not lost, it accumulates into the next clip.
    const int64_t high_floor = 100'000;
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, high_floor ) );
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( queued, SHD4 ) );
    const int64_t funded_at = last_tick_us( SHEO );
    auto before = system_balance( SHEO.value );
    const int64_t block_share = yield_reference::clip_size( queued, 500'000, horizon_sec,
                                                           shadow_pool_of(before), cap_bps, 0 );
    BOOST_REQUIRE_LT( 0, block_share );            // a block's share is real...
    BOOST_REQUIRE_LT( block_share, high_floor );   // ...but under the floor

    // Ten blocks of cranking: every one a no-op, nothing sold, clock untouched.
    for (int i = 0; i < 10; ++i) {
        BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
        BOOST_REQUIRE_EQUAL( queued, reservoir_of( SHEO ) );
        BOOST_REQUIRE( before == system_balance( SHEO.value ) );
        BOOST_REQUIRE_EQUAL( funded_at, last_tick_us( SHEO ) );
    }

    // Once enough time has accrued the clip clears the floor and sells in one
    // piece, measured from the ORIGINAL clock: the skipped blocks were banked,
    // so throughput is unchanged and only the granularity is coarser.
    produce_block();
    produce_block( fc::seconds(40) );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    const int64_t sold_at = last_tick_us( SHEO );
    const int64_t clip = yield_reference::clip_size( queued, sold_at - funded_at, horizon_sec,
                                                     shadow_pool_of(before), cap_bps, high_floor );
    BOOST_REQUIRE_LE( high_floor, clip );
    const int64_t proceeds = reference::receive( clip, shadow_pool_of(before), wire_pool_of(before), fee );
    BOOST_REQUIRE_LT( 0, proceeds );               // and it actually pays, unlike a 1-unit clip
    BOOST_REQUIRE_EQUAL( queued - clip, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + clip, shadow_pool_of( system_balance( SHEO.value ) ) );

    // A remainder smaller than the floor is not stranded: the floor gives way to
    // what is queued, so it leaves as one sale once the time share reaches the
    // whole queue, which takes exactly one horizon.
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, uint32_t(yield_reference::BpsTotal), high_floor ) );
    produce_block();
    produce_block( fc::hours(2) );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );   // drain whatever is left
    BOOST_REQUIRE_EQUAL( 0, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );

    const int64_t dust = 500;                                  // well under high_floor
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( dust, SHD4 ) );
    const int64_t dust_at = last_tick_us( SHEO );
    before = system_balance( SHEO.value );
    produce_block();
    produce_block( fc::minutes(30) );                          // half a horizon: not yet
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( dust, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( dust_at, last_tick_us( SHEO ) );
    produce_block();
    produce_block( fc::minutes(31) );                          // past one horizon: clears
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( 0, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + dust, shadow_pool_of( system_balance( SHEO.value ) ) );

    // A depth cap below the floor is the one combination with no way out: every
    // clip is capped under the floor, so the pair stops selling however long it
    // waits, until setyield widens one of them.
    BOOST_REQUIRE_EQUAL( success(), accrueyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, 2'000'000 ) );   // cap is 1e6
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( queued, SHD4 ) );
    before = system_balance( SHEO.value );
    produce_block();
    produce_block( fc::hours(6) );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE_EQUAL( queued, reservoir_of( SHEO ) );
    BOOST_REQUIRE( before == system_balance( SHEO.value ) );
    // Lowering the floor under the cap starts it again.
    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, 1000 ) );
    produce_block();
    produce_block( fc::hours(6) );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    BOOST_REQUIRE_LT( reservoir_of( SHEO ), queued );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( yield_tick_never_sells_below_a_fee_bearing_output, sysio_swap_tester ) try {
    setup_yield_pool();
    grant_shadow_code( "sysio.swap"_n, true );
    const uint32_t horizon_sec = 3600;
    const uint32_t cap_bps     = uint32_t(yield_reference::BpsTotal);   // the cap is not what binds here
    const int64_t  clip_floor  = 1000;
    const auto shadow_pool_of  = [&]( const vector<int64_t>& pool ) { return pool.at(0); };
    const auto wire_pool_of    = [&]( const vector<int64_t>& pool ) { return pool.at(1); };

    // A 0.01% fee puts the fee-bearing output at 10000, an order of magnitude
    // above the clip floor, which is what separates the two gates: the floor is
    // a granularity in SHADOW units, and what the pair's rate actually depends
    // on is the OUTPUT. A floor sized for one fee is wrong for another, and
    // changefee can move the fee under a floor that setyield already set.
    const int fee = 1;
    BOOST_REQUIRE_EQUAL( success(), changefee( SHEO, fee ) );
    BOOST_REQUIRE_EQUAL( fee, pool_fee( SHEO ) );
    const int64_t fee_bearing = reference::min_fee_bearing_output( fee );
    BOOST_REQUIRE_EQUAL( 10000, fee_bearing );
    BOOST_REQUIRE_LT( clip_floor, fee_bearing );

    BOOST_REQUIRE_EQUAL( success(), setyield( SHEO, horizon_sec, cap_bps, clip_floor ) );
    const int64_t queued = 1000'0000;
    fund_yield_in_one_transaction( "alice"_n, SHEO, asset( queued, SHD4 ) );
    const int64_t funded_at = last_tick_us( SHEO );
    const auto before = system_balance( SHEO.value );

    // One block on, the clip clears the CLIP floor, so that gate alone would
    // have sold it -- but its output does not reach a whole unit of fee, so the
    // clip would pay the one-unit minimum instead of the pair's rate, out of the
    // holders' distribution. The tick declines, and leaves the clock alone.
    const int64_t short_clip = yield_reference::clip_size( queued, 500'000, horizon_sec,
                                                           shadow_pool_of(before), cap_bps, clip_floor );
    BOOST_REQUIRE_LT( 0, short_clip );                              // clears the clip floor
    const int64_t short_out = reference::receive( short_clip, shadow_pool_of(before), wire_pool_of(before), fee );
    BOOST_REQUIRE_LT( short_out, fee_bearing );                     // but not the output floor
    BOOST_REQUIRE_EQUAL( 0, reference::proportional_fee_on( short_out, fee ) );   // MinSwapFee is what it would pay
    for (int i = 0; i < 5; ++i) {
        BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
        BOOST_REQUIRE_EQUAL( queued, reservoir_of( SHEO ) );
        BOOST_REQUIRE( before == system_balance( SHEO.value ) );
        BOOST_REQUIRE_EQUAL( funded_at, last_tick_us( SHEO ) );
    }

    // The banked time grows the clip until its output does clear, and then it
    // sells in one piece measured from the original clock: declining costs
    // throughput nothing, exactly as the clip floor's own skips do.
    produce_block();
    produce_block( fc::seconds(10) );
    BOOST_REQUIRE_EQUAL( success(), tickyield( SHEO ) );
    const int64_t clip = yield_reference::clip_size( queued, last_tick_us( SHEO ) - funded_at, horizon_sec,
                                                     shadow_pool_of(before), cap_bps, clip_floor );
    const int64_t proceeds = reference::receive( clip, shadow_pool_of(before), wire_pool_of(before), fee );
    BOOST_REQUIRE_LE( fee_bearing, proceeds );
    BOOST_REQUIRE_LE( 1, reference::proportional_fee_on( proceeds, fee ) );   // the pair's own rate, not the minimum
    BOOST_REQUIRE_EQUAL( queued - clip, reservoir_of( SHEO ) );
    BOOST_REQUIRE_EQUAL( shadow_pool_of(before) + clip,      shadow_pool_of( system_balance( SHEO.value ) ) );
    BOOST_REQUIRE_EQUAL( wire_pool_of(before) - proceeds,    wire_pool_of( system_balance( SHEO.value ) ) );
    BOOST_REQUIRE( pending_payout( "shadowtoken"_n ).empty() );

    // (The remainder drain is exempt from this gate, so dust is not stranded by
    // it either. yield_tick_never_sells_below_the_clip_floor pins that: at its
    // 0.1% fee the 500-unit remainder it drains is itself under the 1000-unit
    // fee-bearing output, and it still leaves.)
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( fee_authority_configuration, sysio_swap_tester ) try {
    create_tokens_and_issue();
    // A third token, for a third pair against the system token.
    const symbol CVO4 = symbol::from_string("4,CVO");
    BOOST_REQUIRE_EQUAL( success(), create( "sysio.token"_n, "alice"_n, asset::from_string("1000.0000 CVO") ) );
    BOOST_REQUIRE_EQUAL( success(), issue( "sysio.token"_n, "alice"_n, "alice"_n, asset::from_string("1000.0000 CVO"), "" ) );
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    many_transfer();
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, extended_symbol{ CVO4, "sysio.token"_n } ) );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "sysio.token"_n, "alice"_n, asset::from_string("1000.0000 CVO"), "" ) );

    // EVO adopts the configured authority (sysio); ETUSD names alice as its own.
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1.0000 VOICE")), extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset::from_string("1.00 TUSD")), extend(asset::from_string("1.0000 EOS")), 10, "alice"_n) );

    // Each pair answers only to its own authority.
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 30) );
    BOOST_REQUIRE_EQUAL( error("missing authority of sysio"), changefee(EVO, 40, "alice"_n) );
    BOOST_REQUIRE_EQUAL( 30, pool_fee(EVO) );
    BOOST_REQUIRE_EQUAL( success(), changefee(ETUSD, 30, "alice"_n) );
    BOOST_REQUIRE_EQUAL( error("missing authority of alice"), changefee(ETUSD, 40) );
    BOOST_REQUIRE_EQUAL( 30, pool_fee(ETUSD) );

    // Reconfiguring binds pairs created afterwards; existing pairs keep theirs.
    BOOST_REQUIRE_EQUAL( success(), setconfig( "bob"_n ) );
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 35) );
    BOOST_REQUIRE_EQUAL( error("missing authority of sysio"), changefee(EVO, 45, "bob"_n) );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, symbol::from_string("4,BVO"),
        extend(asset::from_string("1.0000 CVO")), extend(asset::from_string("1.0000 EOS")), 0, name{}) );
    const auto BVO = symbol::from_string("4,BVO").to_symbol_code();
    BOOST_REQUIRE_EQUAL( 0, pool_fee(BVO) );
    BOOST_REQUIRE_EQUAL( success(), changefee(BVO, 9999, "bob"_n) );
    BOOST_REQUIRE_EQUAL( error("missing authority of bob"), changefee(BVO, 1) );
    BOOST_REQUIRE_EQUAL( 9999, pool_fee(BVO) );
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
    seed_transfer( "anothertoken"_n, "alice"_n, asset::from_string("200000000.0000 VOICE"), "");

    BOOST_REQUIRE_EQUAL(success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("100000000.0000 VOICE")),
        extend(asset::from_string("1000000.0000 EOS")), 10, name{}) );

    // One pool per token: a second VOICE pair is refused, and the other way
    // round is not a pair at all.
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the pool is already indexed"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.0000 VOICE")),
        extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the second leg must be the system token"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.0000 EOS")),
        extend(asset::from_string("1.0000 VOICE")), 10, name{}) );

    // The pair's uniqueness row is keyed by its legs in canonical order, the lower
    // (contract, symbol) first: anothertoken/VOICE ahead of sysio.token/EOS. The
    // same two legs in the other order are the same key, so no second row exists.
    BOOST_REQUIRE( "anothertoken"_n < "sysio.token"_n );
    const auto data = get_kv_row( "sysio.swap"_n, "evoindex"_n,
        { "anothertoken"_n.to_uint64_t(), VOICE4.value(), "sysio.token"_n.to_uint64_t(), EOS4.value() } );
    BOOST_REQUIRE( !data.empty() );
    const auto table = abi_ser.binary_to_variant( "pair_index", data,
                                                  abi_serializer::create_yield_function(abi_serializer_max_time) );
    BOOST_REQUIRE_EQUAL(table["evo_symbol"], "4,EVO");
    BOOST_REQUIRE( get_kv_row( "sysio.swap"_n, "evoindex"_n,
        { "sysio.token"_n.to_uint64_t(), EOS4.value(), "anothertoken"_n.to_uint64_t(), VOICE4.value() } ).empty() );

    // A first leg with a different precision is a different token: it has no
    // deposit row. A second leg that is not exactly the system token, by
    // precision or by contract, is not a pair.
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.00000 VOICE")),
        extend(asset::from_string("1.0000 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the second leg must be the system token"),
        inittoken( "alice"_n, EOS4,
        extend(asset::from_string("1.0000 VOICE")),
        extend(asset::from_string("1.00000 EOS")), 10, name{}) );
    BOOST_REQUIRE_EQUAL(wasm_assert_msg("the second leg must be the system token"),
        inittoken( "alice"_n, EOS4, extend(asset::from_string("1.0000 VOICE")),
        extended_asset{asset::from_string("1.0000 EOS"), "anothertoken"_n},
        10, name{}) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Coverage added with the WIRE port: fee bounds, an exact rounding table, and
// the pool invariants under a long randomized operation sequence.
// ---------------------------------------------------------------------------

// A spread of fee rates across the accepted range; the contract itself must
// accept the whole of [0, MAX_FEE] and reject anything outside it.
static const std::vector<int> FeeVector{1, 2, 3, 5, 7, 10, 15, 20, 30, 50, 75, 100, 150, 200, 300};

BOOST_FIXTURE_TEST_CASE( changefee_bounds, sysio_swap_tester ) try {
    setup_pools();

    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee out of range"), changefee(EVO, -1) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee out of range"), changefee(EVO, 10000) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee out of range"), changefee(EVO, std::numeric_limits<int>::max()) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("fee out of range"), changefee(EVO, std::numeric_limits<int>::min()) );
    BOOST_REQUIRE_EQUAL( 10, pool_fee(EVO) );

    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 0) );
    BOOST_REQUIRE_EQUAL( 0, pool_fee(EVO) );
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 9999) );
    BOOST_REQUIRE_EQUAL( 9999, pool_fee(EVO) );
    for (int fee : FeeVector) {
        BOOST_REQUIRE_EQUAL( success(), changefee(EVO, fee) );
        BOOST_REQUIRE_EQUAL( fee, pool_fee(EVO) );
    }

    // At the maximum fee a swap still settles, and its output is exactly the
    // spec quote -- the bound exists so this never overflows int64.
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 9999) );
    auto before = system_balance(EVO.value);
    const int64_t amount_in = 1'000'000'000;
    const int64_t expected  = reference::receive(amount_in, before[1], before[0], 9999);   // EOS in, VOICE out
    BOOST_REQUIRE_EQUAL( success(),
        exchange( "alice"_n, EVO, extend(asset(amount_in, EOS4)), asset(expected, VOICE4) ) );
    auto after = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( before[0] - expected, after[0] );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( compute_rounding_table, sysio_swap_tester ) try {
    setup_pools();
    static const std::vector<int64_t> amounts{1, 2, 3, 10, 100, 12345, 1'000'000};

    // Swaps in both directions, at every fee. pool1 is VOICE, pool2 is EOS.
    for (int fee : FeeVector) {
        BOOST_REQUIRE_EQUAL( success(), changefee(EVO, fee) );
        for (int64_t amount : amounts) {
            // EOS -> VOICE: one unit above the spec quote is refused, the quote itself lands
            auto before = system_balance(EVO.value);
            int64_t out = reference::receive(amount, before[1], before[0], fee);
            int64_t alice_eos = balance("alice"_n, EOS4), alice_voice = balance("alice"_n, VOICE4);
            BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
                exchange( "alice"_n, EVO, extend(asset(amount, EOS4)), asset(out + 1, VOICE4) ) );
            BOOST_REQUIRE_EQUAL( success(),
                exchange( "alice"_n, EVO, extend(asset(amount, EOS4)), asset(out, VOICE4) ) );
            auto after = system_balance(EVO.value);
            BOOST_REQUIRE_EQUAL( before[1] + amount, after[1] );
            BOOST_REQUIRE_EQUAL( before[0] - out,    after[0] );
            BOOST_REQUIRE_EQUAL( alice_eos - amount, balance("alice"_n, EOS4) );
            BOOST_REQUIRE_EQUAL( alice_voice + out,  balance("alice"_n, VOICE4) );

            // VOICE -> EOS
            before = after;
            out = reference::receive(amount, before[0], before[1], fee);
            BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
                exchange( "alice"_n, EVO, extend(asset(amount, VOICE4)), asset(out + 1, EOS4) ) );
            BOOST_REQUIRE_EQUAL( success(),
                exchange( "alice"_n, EVO, extend(asset(amount, VOICE4)), asset(out, EOS4) ) );
            after = system_balance(EVO.value);
            BOOST_REQUIRE_EQUAL( before[0] + amount, after[0] );
            BOOST_REQUIRE_EQUAL( before[1] - out,    after[1] );
        }
    }

    // Liquidity: adding charges ceil + the fixed 0.01% fee per leg, removing returns floor.
    for (int64_t shares : {int64_t(1), int64_t(2), int64_t(3), int64_t(10), int64_t(12345)}) {
        auto before = system_balance(EVO.value);
        const int64_t pay1 = reference::add_leg(shares, before[0], before[2]);
        const int64_t pay2 = reference::add_leg(shares, before[1], before[2]);
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
            addliquidity( "alice"_n, asset(shares, EVO4), asset(pay1 - 1, VOICE4), asset(pay2, EOS4) ) );
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
            addliquidity( "alice"_n, asset(shares, EVO4), asset(pay1, VOICE4), asset(pay2 - 1, EOS4) ) );
        BOOST_REQUIRE_EQUAL( success(),
            addliquidity( "alice"_n, asset(shares, EVO4), asset(pay1, VOICE4), asset(pay2, EOS4) ) );
        auto after = system_balance(EVO.value);
        BOOST_REQUIRE_EQUAL( before[0] + pay1,   after[0] );
        BOOST_REQUIRE_EQUAL( before[1] + pay2,   after[1] );
        BOOST_REQUIRE_EQUAL( before[2] + shares, after[2] );

        before = after;
        const int64_t get1 = reference::remove_leg(shares, before[0], before[2]);
        const int64_t get2 = reference::remove_leg(shares, before[1], before[2]);
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
            remliquidity( "alice"_n, asset(shares, EVO4), asset(get1 + 1, VOICE4), asset(get2, EOS4) ) );
        BOOST_REQUIRE_EQUAL( success(),
            remliquidity( "alice"_n, asset(shares, EVO4), asset(get1, VOICE4), asset(get2, EOS4) ) );
        after = system_balance(EVO.value);
        BOOST_REQUIRE_EQUAL( before[0] - get1,   after[0] );
        BOOST_REQUIRE_EQUAL( before[1] - get2,   after[1] );
        BOOST_REQUIRE_EQUAL( before[2] - shares, after[2] );
    }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( invariants_under_random_sequences, sysio_swap_tester ) try {
    setup_pools();

    struct pool_spec {
        symbol_code code;   // LP token
        symbol      lp;
        symbol      leg1;   // pool1
        symbol      leg2;   // pool2
    };
    const std::vector<pool_spec> pools{
        { EVO,   EVO4,   VOICE4, EOS4 },
        { ETUSD, ETUSD3, TUSD2,  EOS4 },
    };
    const std::vector<name> users{ "alice"_n, "bob"_n };
    // Failures the sequence is allowed to produce: every one is a guard the
    // contract is SUPPOSED to raise, and each leaves state untouched.
    const std::vector<action_result> allowed{
        wasm_assert_msg("insufficient funds"),
        wasm_assert_msg("available is less than expected"),
        wasm_assert_msg("invalid parameters"),
        wasm_assert_msg("computation overflow"),
        wasm_assert_msg("computation underflow"),
        wasm_assert_msg("the pool cannot be left empty"),
        wasm_assert_msg("overdrawn balance"),
        wasm_assert_msg("no balance object found"),
    };

    std::mt19937_64 rng(0x57495245'53574150ULL);   // fixed seed: the sequence is reproducible
    // Log-uniform draw in [1, cap] so dust and large trades are both frequent.
    auto draw = [&](int64_t cap) -> int64_t {
        if (cap < 1) return 1;
        int digits = 0;
        for (int64_t c = cap; c > 0; c /= 10) ++digits;
        int64_t magnitude = 1;
        for (int e = rng() % digits; e > 0; --e) magnitude *= 10;
        return std::min<int64_t>(cap, magnitude * (1 + rng() % 9));
    };

    enum op_kind { op_swap_forward, op_swap_backward, op_negative_in, op_add, op_remove, op_fee_change, op_count };
    std::vector<int> successes(op_count, 0);
    const int steps = 400;

    for (int step = 0; step < steps; ++step) {
        const auto& pool = pools[rng() % pools.size()];
        const name  user = users[rng() % users.size()];
        const auto  old_total = total();
        const auto  old_vec   = system_balance(pool.code.value);
        const int64_t user_leg1 = balance(user, pool.leg1);
        const int64_t user_leg2 = balance(user, pool.leg2);
        const int   op = rng() % op_count;

        action_result r;
        if (op == op_swap_forward) {
            r = exchange( user, pool.code, extend(asset(draw(user_leg1), pool.leg1)), asset(0, pool.leg2) );
        } else if (op == op_swap_backward) {
            r = exchange( user, pool.code, extend(asset(draw(user_leg2), pool.leg2)), asset(0, pool.leg1) );
        } else if (op == op_negative_in) {
            // a negative input (the retired exact-output mode) is refused outright,
            // whatever the sign of min_expected, and must leave state untouched
            const int64_t w = draw(old_vec[0] / 2);
            const int64_t limit = (rng() % 2) ? -user_leg2 : user_leg2;
            r = exchange( user, pool.code, extend(asset(-w, pool.leg1)), asset(limit, pool.leg2) );
            BOOST_REQUIRE_EQUAL( wasm_assert_msg("ext_asset_in must be positive"), r );
        } else if (op == op_add) {
            r = addliquidity( user, asset(draw(old_vec[2] / 10), pool.lp),
                              asset(user_leg1, pool.leg1), asset(user_leg2, pool.leg2) );
        } else if (op == op_remove) {
            const int64_t lp = lp_balance(user, pool.code);
            r = remliquidity( user, asset(draw(lp), pool.lp), asset(0, pool.leg1), asset(0, pool.leg2) );
        } else {
            r = changefee( pool.code, FeeVector[rng() % FeeVector.size()] );
        }

        if (r == success()) {
            ++successes[op];
        } else if (op == op_negative_in) {
            ++successes[op];   // the rejection IS the expected outcome, asserted above
        } else {
            BOOST_REQUIRE_MESSAGE( std::find(allowed.begin(), allowed.end(), r) != allowed.end(),
                                   "step " << step << " op " << op << ": unexpected failure: " << r );
        }
        // Conservation: nothing enters or leaves the contract in any of these ops.
        BOOST_REQUIRE_MESSAGE( old_total == total(), "step " << step << " op " << op << ": totals changed" );
        // Share value: P1*P2/S^2 never decreases through a swap, add, or remove.
        BOOST_REQUIRE_MESSAGE( is_increasing(old_vec, system_balance(pool.code.value)),
                               "step " << step << " op " << op << ": pool value per share decreased" );
    }

    // The run must actually have exercised every path, not merely survived it.
    BOOST_REQUIRE_GE( successes[op_swap_forward] + successes[op_swap_backward], 60 );
    BOOST_REQUIRE_GE( successes[op_negative_in], 40 );
    BOOST_REQUIRE_GE( successes[op_add], 15 );
    BOOST_REQUIRE_GE( successes[op_remove], 10 );
    BOOST_REQUIRE_GE( successes[op_fee_change], 20 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Characterization for the amm_math substitution. Every expectation below is
// written from the spec (`reference`) or from the host-side amm_math model
// (`model`), never from the contract's own arithmetic, so the cases survive
// a change of implementation and fail only on a change of behaviour.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( swap_matches_amm_math_model, sysio_swap_tester ) try {
    setup_pools();
    struct leg { symbol_code pair; symbol in; symbol out; int in_leg; int out_leg; };
    const std::vector<leg> legs{   // EOS, the system token, is pool2 of both pairs
        { EVO,   EOS4,   VOICE4, 1, 0 }, { EVO,   VOICE4, EOS4, 0, 1 },
        { ETUSD, EOS4,   TUSD2,  1, 0 }, { ETUSD, TUSD2,  EOS4, 0, 1 },
    };
    static const std::vector<int64_t> amounts{1, 7, 999, 1'000'000, 1'000'000'000, 10'000'000'000'000};
    for (int fee : {0, 1, 10, 100, 9999}) {
        BOOST_REQUIRE_EQUAL( success(), changefee(EVO, fee) );
        BOOST_REQUIRE_EQUAL( success(), changefee(ETUSD, fee) );
        for (const auto& l : legs) {
            for (int64_t amount : amounts) {
                const auto before = system_balance(l.pair.value);
                const int64_t expected = model::receive(amount, before[l.in_leg], before[l.out_leg], fee);
                BOOST_REQUIRE_EQUAL( expected, reference::receive(amount, before[l.in_leg], before[l.out_leg], fee) );
                BOOST_REQUIRE_EQUAL( expected, settle_swap("alice"_n, l.pair, asset(amount, l.in), l.out, l.out_leg) );
            }
        }
    }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( fee_zero_is_the_bare_constant_product_curve, sysio_swap_tester ) try {
    setup_pools();
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 0) );
    using wide = boost::multiprecision::int256_t;
    for (int64_t amount : {int64_t(1), int64_t(3), int64_t(12345), int64_t(1'000'000'000), int64_t(123'456'789'012'345)}) {
        for (int in_leg = 0; in_leg < 2; ++in_leg) {
            const int out_leg = 1 - in_leg;
            const symbol in_symbol = in_leg == 0 ? VOICE4 : EOS4;
            const symbol out_symbol = in_leg == 0 ? EOS4 : VOICE4;
            const auto before = system_balance(EVO.value);
            const wide k = wide(before[0]) * wide(before[1]);
            const int64_t out = settle_swap("alice"_n, EVO, asset(amount, in_symbol), out_symbol, out_leg);
            const auto after = system_balance(EVO.value);
            // With no fee the output IS the bare kernel...
            BOOST_REQUIRE_EQUAL( out, model::gross(amount, before[in_leg], before[out_leg]) );
            // ...which is the largest integer output that keeps x*y from falling:
            // one unit more would have broken the invariant.
            BOOST_REQUIRE( wide(after[0]) * wide(after[1]) >= k );
            BOOST_REQUIRE( wide(after[in_leg]) * wide(after[out_leg] - 1) < k );
        }
    }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( round_trip_never_profits, sysio_swap_tester ) try {
    setup_pools();
    using wide = boost::multiprecision::int256_t;
    for (int fee : {0, 10, 100, 9999}) {
        BOOST_REQUIRE_EQUAL( success(), changefee(EVO, fee) );
        for (int64_t amount : {int64_t(1), int64_t(10), int64_t(12345), int64_t(1'000'000), int64_t(50'000'000'000'000)}) {
            const auto start  = system_balance(EVO.value);
            const auto totals = total();
            const int64_t voice = settle_swap("alice"_n, EVO, asset(amount, EOS4), VOICE4, 0);
            const int64_t eos = voice > 0 ? settle_swap("alice"_n, EVO, asset(voice, VOICE4), EOS4, 1) : 0;
            const auto end = system_balance(EVO.value);
            // Out and back never returns more than went in; with a fee and a
            // trade big enough for the fee to bite, strictly less.
            BOOST_REQUIRE_LE( eos, amount );
            if (fee > 0 && amount >= 1'000'000) BOOST_REQUIRE_LT( eos, amount );
            BOOST_REQUIRE( wide(end[0]) * wide(end[1]) >= wide(start[0]) * wide(start[1]) );
            BOOST_REQUIRE_EQUAL( end[2], start[2] );
            BOOST_REQUIRE( totals == total() );
        }
    }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( precision_extremes, sysio_swap_tester ) try {
    // Every unit of every token sits in the contract under alice, so pools and
    // trades can be pushed to the int64 asset ceiling with no external limit.
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    const asset all_eos   = asset::from_string("461168601842738.7903 EOS");
    const asset all_voice = asset::from_string("461168601842738.7903 VOICE");
    const asset all_tusd  = asset::from_string("46116860184273879.03 TUSD");
    BOOST_REQUIRE_EQUAL( asset::max_amount, all_eos.get_amount() );
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, all_eos, "") );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "anothertoken"_n, "bob"_n, all_voice, "deposit to: alice") );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "sysio.token"_n, "alice"_n, all_tusd, "") );

    // A dust pool: the largest side inittoken accepts against one unit of EOS.
    // (pool1 is the token, pool2 is EOS, the system token.)
    const int64_t init_max = 1'000'000'000'000'000 - 1;
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset(init_max, VOICE4)), extend(asset(1, EOS4)), 10, name{}) );
    // A whale pool: both sides at the inittoken ceiling, then grown 2500x by
    // adding liquidity, which has no ceiling of its own.
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, ETUSD3,
        extend(asset(init_max, TUSD2)), extend(asset(init_max, EOS4)), 10, name{}) );
    {
        const auto before = system_balance(ETUSD.value);
        const int64_t shares = 2500 * before[2];
        const int64_t pay1 = reference::add_leg(shares, before[0], before[2]);
        const int64_t pay2 = reference::add_leg(shares, before[1], before[2]);
        BOOST_REQUIRE_EQUAL( success(),
            addliquidity( "alice"_n, asset(shares, ETUSD3), asset(pay1, TUSD2), asset(pay2, EOS4) ) );
        const auto after = system_balance(ETUSD.value);
        BOOST_REQUIRE_EQUAL( before[0] + pay1,   after[0] );
        BOOST_REQUIRE_EQUAL( before[1] + pay2,   after[1] );
        BOOST_REQUIRE_EQUAL( before[2] + shares, after[2] );
    }

    // Dust pool: one unit in each direction.
    {
        auto before = system_balance(EVO.value);
        const int64_t out = reference::receive(1, before[1], before[0], 10);   // EOS in, VOICE out
        BOOST_REQUIRE_EQUAL( out, model::receive(1, before[1], before[0], 10) );
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
            exchange( "alice"_n, EVO, extend(asset(1, EOS4)), asset(out + 1, VOICE4) ) );
        BOOST_REQUIRE_EQUAL( out, settle_swap("alice"_n, EVO, asset(1, EOS4), VOICE4, 0) );
        // One VOICE unit back into the now lopsided pool buys nothing; the pool keeps it.
        before = system_balance(EVO.value);
        BOOST_REQUIRE_EQUAL( 0, reference::receive(1, before[0], before[1], 10) );
        BOOST_REQUIRE_EQUAL( 0, settle_swap("alice"_n, EVO, asset(1, VOICE4), EOS4, 1) );
        const auto after = system_balance(EVO.value);
        BOOST_REQUIRE_EQUAL( before[1],     after[1] );
        BOOST_REQUIRE_EQUAL( before[0] + 1, after[0] );
    }
    // Dust pool: alice's entire VOICE balance in one trade. The pool side lands
    // exactly on the int64 ceiling and the gross quote is a single unit, which
    // the one-unit minimum fee keeps in the pool: no fee-bearing trade is free.
    {
        const auto before = system_balance(EVO.value);
        const int64_t amount = balance("alice"_n, VOICE4);
        BOOST_REQUIRE_EQUAL( asset::max_amount, before[0] + amount );
        BOOST_REQUIRE_EQUAL( 1, model::gross(amount, before[0], before[1]) );
        const int64_t out = reference::receive(amount, before[0], before[1], 10);
        BOOST_REQUIRE_EQUAL( 0, out );
        BOOST_REQUIRE_EQUAL( out, settle_swap("alice"_n, EVO, asset(amount, VOICE4), EOS4, 1) );
        const auto after = system_balance(EVO.value);
        BOOST_REQUIRE_EQUAL( asset::max_amount, after[0] );
        BOOST_REQUIRE_EQUAL( 0, balance("alice"_n, VOICE4) );
    }

    // Whale pool: the smallest trade, then the largest alice can make.
    {
        auto before = system_balance(ETUSD.value);
        BOOST_REQUIRE_EQUAL( 0, reference::receive(1, before[1], before[0], 10) );   // EOS in, TUSD out
        BOOST_REQUIRE_EQUAL( 0, settle_swap("alice"_n, ETUSD, asset(1, EOS4), TUSD2, 0) );
        auto after = system_balance(ETUSD.value);
        BOOST_REQUIRE_EQUAL( before[1] + 1, after[1] );
        BOOST_REQUIRE_EQUAL( before[0],     after[0] );

        // Every unit of EOS alice still holds: pool_in + amount is the whole
        // supply less the two units parked in the dust pool.
        before = after;
        const int64_t amount = balance("alice"_n, EOS4);
        BOOST_REQUIRE_EQUAL( 2, system_balance(EVO.value)[1] );
        BOOST_REQUIRE_EQUAL( asset::max_amount - 2, before[1] + amount );
        const int64_t out = reference::receive(amount, before[1], before[0], 10);
        BOOST_REQUIRE_EQUAL( out, model::receive(amount, before[1], before[0], 10) );
        BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
            exchange( "alice"_n, ETUSD, extend(asset(amount, EOS4)), asset(out + 1, TUSD2) ) );
        BOOST_REQUIRE_EQUAL( out, settle_swap("alice"_n, ETUSD, asset(amount, EOS4), TUSD2, 0) );
        after = system_balance(ETUSD.value);
        BOOST_REQUIRE_EQUAL( asset::max_amount - 2, after[1] );
        BOOST_REQUIRE_EQUAL( 0, balance("alice"_n, EOS4) );

        // And every unit of TUSD the other way.
        before = after;
        const int64_t tusd = balance("alice"_n, TUSD2);
        const int64_t out2 = reference::receive(tusd, before[0], before[1], 10);
        BOOST_REQUIRE_EQUAL( out2, model::receive(tusd, before[0], before[1], 10) );
        BOOST_REQUIRE_EQUAL( out2, settle_swap("alice"_n, ETUSD, asset(tusd, TUSD2), EOS4, 1) );
        BOOST_REQUIRE_EQUAL( 0, balance("alice"_n, TUSD2) );
    }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( minimum_fee_closes_the_free_window, sysio_swap_tester ) try {
    // A small, balanced pool so single-unit inputs produce single-unit quotes:
    // 1000.0000 EOS against 1000.0000 VOICE, 10 bps.
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("2000.0000 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "anothertoken"_n, "bob"_n, asset::from_string("2000.0000 VOICE"), "deposit to: alice") );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset::from_string("1000.0000 VOICE")), extend(asset::from_string("1000.0000 EOS")), 10, name{}) );
    // Every trade below sells EOS (pool2) for VOICE (pool1).

    // Below one unit of quote there is nothing to charge: the input is kept, nothing is paid.
    BOOST_REQUIRE_EQUAL( 0, settle_swap("alice"_n, EVO, asset(1, EOS4), VOICE4, 0) );
    // A 999-unit quote would round to a zero fee; the minimum makes it one unit.
    {
        const auto before = system_balance(EVO.value);
        BOOST_REQUIRE_EQUAL( 999, model::gross(1000, before[1], before[0]) );
        BOOST_REQUIRE_EQUAL( 998, reference::receive(1000, before[1], before[0], 10) );
        BOOST_REQUIRE_EQUAL( 998, settle_swap("alice"_n, EVO, asset(1000, EOS4), VOICE4, 0) );
    }
    // Once the floored fee reaches a unit on its own the minimum is inert.
    {
        const auto before = system_balance(EVO.value);
        const int64_t g = model::gross(20000, before[1], before[0]);
        BOOST_REQUIRE( g * 10 / 10000 >= 1 );
        BOOST_REQUIRE_EQUAL( g - g * 10 / 10000, settle_swap("alice"_n, EVO, asset(20000, EOS4), VOICE4, 0) );
    }
    // At a zero fee rate there is no minimum: the quote is the bare curve.
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 0) );
    {
        const auto before = system_balance(EVO.value);
        const int64_t g = model::gross(1000, before[1], before[0]);
        BOOST_REQUIRE_EQUAL( g, settle_swap("alice"_n, EVO, asset(1000, EOS4), VOICE4, 0) );
    }
    // And back at a nonzero rate, x*y grows on EVERY fee-bearing trade, including
    // the smallest quotes -- there is no fee-free window to hunt for.
    BOOST_REQUIRE_EQUAL( success(), changefee(EVO, 10) );
    using wide = boost::multiprecision::int256_t;
    for (int64_t amount : {int64_t(1000), int64_t(1001), int64_t(1500), int64_t(2), int64_t(3)}) {
        const auto before = system_balance(EVO.value);
        const int64_t g = model::gross(amount, before[1], before[0]);
        const int64_t out = settle_swap("alice"_n, EVO, asset(amount, EOS4), VOICE4, 0);
        const auto after = system_balance(EVO.value);
        if (g > 0) {
            BOOST_REQUIRE_LT( out, g );
            BOOST_REQUIRE( wide(after[0]) * wide(after[1]) > wide(before[0]) * wide(before[1]) );
        }
    }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( guard_semantics_on_the_memo_path, sysio_swap_tester ) try {
    // badtoken forwards any transfer notification untouched, including a zero
    // amount a real token contract refuses, so with it hosting one leg of a
    // pair the swap path can be driven with inputs sysio.token cannot produce.
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    // badtoken's EOS is a different token from the system token's EOS: it is the
    // pair's first leg, seeded with the contract's authority.
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, extended_symbol{EOS4, "badtoken"_n}) );
    BOOST_REQUIRE_EQUAL( success(), openext( "alice"_n, "alice"_n, WIRE) );
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("token is not a leg of any pair"),
        transfer( "badtoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("2000.0000 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "badtoken"_n, "alice"_n, asset::from_string("2000.0000 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("2000.0000 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extended_asset{asset::from_string("1000.0000 EOS"), "badtoken"_n},
        extend(asset::from_string("1000.0000 EOS")), 10, name{}) );

    const auto before = system_balance(EVO.value);
    // A zero input through the memo path is refused before the pools move...
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("invalid parameters"),
        transfer( "badtoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("0.0000 EOS"), "exchange: EVO, 0.0000 EOS") );
    // ...and a negative one is refused one layer earlier.
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("quantity must be positive"),
        transfer( "badtoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("-1.0000 EOS"), "exchange: EVO, 0.0000 EOS") );
    BOOST_REQUIRE( before == system_balance(EVO.value) );

    // The slippage floor is inclusive: the quote itself settles, one unit more is refused.
    const int64_t out = reference::receive(10000, before[0], before[1], 10);
    BOOST_REQUIRE_EQUAL( wasm_assert_msg("available is less than expected"),
        transfer( "badtoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("1.0000 EOS"),
                  "exchange: EVO, " + asset(out + 1, EOS4).to_string() ) );
    BOOST_REQUIRE_EQUAL( success(),
        transfer( "badtoken"_n, "alice"_n, "sysio.swap"_n, asset::from_string("1.0000 EOS"),
                  "exchange: EVO, " + asset(out, EOS4).to_string() ) );
    const auto after = system_balance(EVO.value);
    BOOST_REQUIRE_EQUAL( before[0] + 10000, after[0] );
    BOOST_REQUIRE_EQUAL( before[1] - out,   after[1] );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Time-weighted average price: the cumulative-price accumulators.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( price_accumulators_follow_the_pools, sysio_swap_tester ) try {
    setup_pools();
    // The tester replays a still-pending block's transactions at the skipped
    // time when asked to skip ahead, so the pool creation is sealed at its own
    // time before the clock moves. (The fixture's push_action seals a block per
    // action, so the ops below need no sealing of their own.)
    produce_block();
    using boost::multiprecision::uint256_t;
    namespace twap = sysio::opp::twap;

    auto row = price_accumulator(EVO);
    BOOST_REQUIRE( row.price1 == 0 && row.price2 == 0 );
    const auto snapshot = row;   // the reader's t0
    uint256_t expect1 = 0, expect2 = 0;

    // Advance chain time by `skip`, apply `op`, and require the accumulators to
    // have grown by the spot prices that held BEFORE the op, times the interval
    // the row reports -- whatever the op then did to the pools.
    auto step = [&](const fc::microseconds& skip, auto&& op) {
        const auto pools = system_balance(EVO.value);
        produce_block(skip);   // an empty block `skip` after the sealed head
        op();
        const auto next = price_accumulator(EVO);
        const int64_t elapsed = next.last_update_us - row.last_update_us;
        BOOST_REQUIRE_GE( elapsed, skip.count() );
        expect1 += twap_reference::price_fp(pools[1], pools[0]) * elapsed;
        expect2 += twap_reference::price_fp(pools[0], pools[1]) * elapsed;
        BOOST_REQUIRE_MESSAGE( next.price1 == expect1, "price1 " << next.price1 << " expected " << expect1
            << " elapsed " << elapsed << " pools " << pools[0] << "," << pools[1]
            << " last_update " << row.last_update_us << " -> " << next.last_update_us );
        BOOST_REQUIRE_MESSAGE( next.price2 == expect2, "price2 " << next.price2 << " expected " << expect2 );
        row = next;
    };
    step( fc::seconds(10), [&]{ BOOST_REQUIRE_EQUAL( success(),
        exchange("alice"_n, EVO, extend(asset::from_string("4.0000 EOS")), asset(0, VOICE4)) ); } );
    step( fc::seconds(30), [&]{ BOOST_REQUIRE_EQUAL( success(),
        addliquidity("alice"_n, asset::from_string("50.0000 EVO"),
                     asset::from_string("100000.0000 VOICE"), asset::from_string("100000.0000 EOS")) ); } );
    step( fc::minutes(5), [&]{ BOOST_REQUIRE_EQUAL( success(),
        remliquidity("alice"_n, asset::from_string("25.0000 EVO"), asset(0, VOICE4), asset(0, EOS4)) ); } );
    step( fc::hours(1), [&]{ BOOST_REQUIRE_EQUAL( success(),
        exchange("alice"_n, EVO, extend(asset::from_string("7.0000 VOICE")), asset(0, EOS4)) ); } );
    step( fc::hours(1), [&]{ BOOST_REQUIRE_EQUAL( success(), sync(EVO) ); } );

    // The reader's window: (r_now - r_snapshot) / (t - t0), computed by the
    // shared kernel and by exact 256-bit division, must agree...
    const int64_t window = row.last_update_us - snapshot.last_update_us;
    const uint256_t delta1 = row.price1 - snapshot.price1;
    const twap::u128 average1 = twap::average_price(
        twap::difference(twap_testing::to_cumulative(row.price1), twap_testing::to_cumulative(snapshot.price1)),
        uint64_t(window) );
    BOOST_REQUIRE( twap_testing::wide(average1) == delta1 / window );
    // ...and land where the pools were all along: price1 is EOS per VOICE (pool2
    // over pool1), a little under a quarter; price2 a little over four.
    BOOST_REQUIRE( average1 > twap::PRICE_ONE / 5 && average1 < twap::PRICE_ONE / 4 );
    const twap::u128 average2 = twap::average_price(
        twap::difference(twap_testing::to_cumulative(row.price2), twap_testing::to_cumulative(snapshot.price2)),
        uint64_t(window) );
    BOOST_REQUIRE( twap_testing::wide(average2) == (row.price2 - snapshot.price2) / window );
    BOOST_REQUIRE( average2 >= 4 * twap::PRICE_ONE && average2 < 5 * twap::PRICE_ONE );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( price_accumulators_ignore_same_block_moves, sysio_swap_tester ) try {
    setup_pools();
    produce_block();   // seal the pool creation before the clock moves
    using boost::multiprecision::uint256_t;

    auto row = price_accumulator(EVO);
    const auto pools = system_balance(EVO.value);
    produce_block(fc::seconds(20));

    // Two trades in one transaction: the first settles the interval at the
    // pre-trade price; the second -- more than twice the EOS side, collapsing
    // the VOICE-per-EOS spot price (pool1 over pool2) -- happens with no time
    // elapsed, so it contributes nothing however far it moves the spot.
    exchange_twice_in_one_transaction( "alice"_n, EVO,
        extend(asset::from_string("1.0000 EOS")), extend(asset::from_string("50000000000.0000 EOS")), VOICE4 );
    const auto moved = system_balance(EVO.value);
    BOOST_REQUIRE( twap_reference::price_fp(moved[0], moved[1]) * 2 < twap_reference::price_fp(pools[0], pools[1]) );
    const auto after = price_accumulator(EVO);
    const int64_t elapsed = after.last_update_us - row.last_update_us;
    BOOST_REQUIRE_GE( elapsed, fc::seconds(20).count() );
    BOOST_REQUIRE( after.price1 == twap_reference::price_fp(pools[1], pools[0]) * elapsed );
    BOOST_REQUIRE( after.price2 == twap_reference::price_fp(pools[0], pools[1]) * elapsed );
    produce_block();   // seal the transaction in its block

    // Only once time passes does the moved price count, for exactly that time.
    produce_block(fc::seconds(20));
    BOOST_REQUIRE_EQUAL( success(), sync(EVO) );
    const auto synced = price_accumulator(EVO);
    const int64_t held = synced.last_update_us - after.last_update_us;
    BOOST_REQUIRE_GE( held, fc::seconds(20).count() );
    BOOST_REQUIRE( synced.price1 == after.price1 + twap_reference::price_fp(moved[1], moved[0]) * held );
    BOOST_REQUIRE( synced.price2 == after.price2 + twap_reference::price_fp(moved[0], moved[1]) * held );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( price_accumulators_span_extreme_prices, sysio_swap_tester ) try {
    // The steepest price int64 balances allow, held for a decade: the sum
    // needs the high limb and the average still recovers the price exactly.
    create_tokens_and_issue();
    abi_ser.set_abi( swap_abi_def(), abi_serializer::create_yield_function(abi_serializer_max_time) );
    many_openext();
    BOOST_REQUIRE_EQUAL( success(), transfer( "sysio.token"_n, "alice"_n, "sysio.swap"_n, asset::from_string("461168601842738.7903 EOS"), "") );
    BOOST_REQUIRE_EQUAL( success(), seed_transfer( "anothertoken"_n, "bob"_n, asset::from_string("461168601842738.7903 VOICE"), "deposit to: alice") );
    const int64_t init_max = 1'000'000'000'000'000 - 1;
    // The largest VOICE side against one unit of EOS: price2 (VOICE per EOS) is
    // the steep one.
    BOOST_REQUIRE_EQUAL( success(), inittoken( "alice"_n, EVO4,
        extend(asset(init_max, VOICE4)), extend(asset(1, EOS4)), 10, name{}) );
    namespace twap = sysio::opp::twap;

    // At this price a single block already carries the sum past 128 bits; a
    // week makes the point without stretching the chain clock.
    produce_block();   // seal the pool creation before the clock moves
    const auto row = price_accumulator(EVO);
    produce_block(fc::days(7));
    BOOST_REQUIRE_EQUAL( success(), sync(EVO) );
    const auto next = price_accumulator(EVO);
    const int64_t elapsed = next.last_update_us - row.last_update_us;
    BOOST_REQUIRE_GE( elapsed, fc::days(7).count() );

    BOOST_REQUIRE( next.price1 == twap_reference::price_fp(1, init_max) * elapsed );
    BOOST_REQUIRE( next.price2 == twap_reference::price_fp(init_max, 1) * elapsed );
    BOOST_REQUIRE( (next.price2 >> 128) != 0 );   // beyond 128 bits, as designed for
    const twap::u128 average2 = twap::average_price(
        twap::difference(twap_testing::to_cumulative(next.price2), twap_testing::to_cumulative(row.price2)),
        uint64_t(elapsed) );
    BOOST_REQUIRE( average2 == twap::price_fp(uint64_t(init_max), 1) );

    // And the pool still trades.
    BOOST_REQUIRE_EQUAL( reference::receive(1, 1, init_max, 10), settle_swap("alice"_n, EVO, asset(1, EOS4), VOICE4, 0) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( abi_surface_is_pinned, sysio_swap_tester ) try {
    // The substitution is internal: the action set, the swap and fee
    // signatures, the pair row, and the table set must not move.
    const abi_def abi = swap_abi_def();

    std::set<std::string> actions;
    for (const auto& a : abi.actions) actions.insert(a.name.to_string());
    const std::set<std::string> expected_actions{
        "accrueyield", "addliquidity", "cancelyield", "changefee", "close", "closeext", "exchange", "fundyield",
        "inittoken", "open", "openext", "remliquidity", "setconfig", "setyield", "sync", "tickyield", "transfer",
        "withdraw" };
    BOOST_REQUIRE( actions == expected_actions );

    using field_list = std::vector<std::pair<std::string, std::string>>;
    auto fields = [&](const std::string& struct_name) {
        field_list out;
        for (const auto& s : abi.structs) {
            if (s.name != struct_name) continue;
            for (const auto& f : s.fields) out.emplace_back(f.name, f.type);
        }
        return out;
    };
    const field_list exchange_fields{
        {"user", "name"}, {"pair_token", "symbol_code"}, {"ext_asset_in", "extended_asset"}, {"min_expected", "asset"} };
    const field_list changefee_fields{ {"pair_token", "symbol_code"}, {"newfee", "int32"} };
    const field_list inittoken_fields{
        {"user", "name"}, {"new_symbol", "symbol"}, {"initial_pool1", "extended_asset"},
        {"initial_pool2", "extended_asset"}, {"initial_fee", "int32"}, {"fee_authority", "name"},
        {"locked_shares", "asset"}, {"yield_leg", "extended_symbol?"} };
    const field_list currency_stats_fields{
        {"supply", "asset"}, {"max_supply", "asset"}, {"issuer", "name"}, {"pool1", "extended_asset"},
        {"pool2", "extended_asset"}, {"fee", "int32"}, {"fee_authority", "name"}, {"locked_shares", "asset"},
        {"yield_leg", "extended_symbol?"}, {"conversion_horizon_sec", "uint32"}, {"depth_cap_bps", "uint32"},
        {"clip_floor", "int64"}, {"last_tick_depth", "int64"}, {"last_tick", "time_point"} };
    const field_list setyield_fields{
        {"pair_token", "symbol_code"}, {"conversion_horizon_sec", "uint32"}, {"depth_cap_bps", "uint32"},
        {"clip_floor", "int64"} };
    const field_list accrueyield_fields{ {"pair_token", "symbol_code"} };
    const field_list payout_receipt_fields{ {"pair", "symbol_code"}, {"quantity", "extended_asset"} };
    const field_list fundyield_fields{ {"from", "name"}, {"pair_token", "symbol_code"}, {"quantity", "asset"} };
    const field_list cancelyield_fields{ {"from", "name"} };
    const field_list fund_receipt_fields{ {"pair", "symbol_code"}, {"quantity", "extended_asset"} };
    const field_list reservoir_fields{ {"balance", "extended_asset"} };
    const field_list tickyield_fields{ {"pair_token", "symbol_code"} };
    const field_list setconfig_fields{ {"fee_authority", "name"}, {"system_token", "extended_symbol"} };
    const field_list swap_config_fields{ {"fee_authority", "name"}, {"system_token", "extended_symbol"} };
    const field_list sync_fields{ {"pair_token", "symbol_code"} };
    const field_list cumulative_price_fields{ {"lo", "uint128"}, {"hi", "uint128"} };
    const field_list price_accumulator_fields{
        {"price1", "cumulative_price"}, {"price2", "cumulative_price"}, {"last_update", "time_point"} };
    const field_list account_fields{ {"balance", "asset"} };
    const field_list evodex_account_fields{ {"balance", "extended_asset"} };
    const field_list pair_index_fields{ {"evo_symbol", "symbol"} };
    BOOST_REQUIRE( fields("exchange") == exchange_fields );
    BOOST_REQUIRE( fields("changefee") == changefee_fields );
    BOOST_REQUIRE( fields("inittoken") == inittoken_fields );
    BOOST_REQUIRE( fields("currency_stats") == currency_stats_fields );
    BOOST_REQUIRE( fields("sync") == sync_fields );
    BOOST_REQUIRE( fields("cumulative_price") == cumulative_price_fields );
    BOOST_REQUIRE( fields("price_accumulator") == price_accumulator_fields );
    BOOST_REQUIRE( fields("account") == account_fields );
    BOOST_REQUIRE( fields("evodex_account") == evodex_account_fields );
    BOOST_REQUIRE( fields("pair_index") == pair_index_fields );
    BOOST_REQUIRE( fields("setconfig") == setconfig_fields );
    BOOST_REQUIRE( fields("swap_config") == swap_config_fields );
    BOOST_REQUIRE( fields("setyield") == setyield_fields );
    BOOST_REQUIRE( fields("accrueyield") == accrueyield_fields );
    BOOST_REQUIRE( fields("payout_receipt") == payout_receipt_fields );
    BOOST_REQUIRE( fields("fundyield") == fundyield_fields );
    BOOST_REQUIRE( fields("cancelyield") == cancelyield_fields );
    BOOST_REQUIRE( fields("fund_receipt") == fund_receipt_fields );
    BOOST_REQUIRE( fields("reservoir") == reservoir_fields );
    BOOST_REQUIRE( fields("tickyield") == tickyield_fields );

    // KV tables: the row type and the key layout an explorer needs to decode
    // the raw key bytes. A scoped table's first key word is the scope.
    struct table_shape { std::string type; std::vector<std::string> key_names; std::vector<std::string> key_types; };
    auto shape = [&](const std::string& table_name) {
        for (const auto& t : abi.tables)
            if (t.name == table_name)
                return table_shape{ t.type, std::vector<std::string>(t.key_names.begin(), t.key_names.end()),
                                            std::vector<std::string>(t.key_types.begin(), t.key_types.end()) };
        return table_shape{};
    };
    auto same = [](const table_shape& a, const table_shape& b) {
        return a.type == b.type && a.key_names == b.key_names && a.key_types == b.key_types;
    };
    BOOST_REQUIRE( same( shape("accounts"),    { "account",           {"scope", "symbol_code"},                        {"name", "uint64"} } ) );
    BOOST_REQUIRE( same( shape("evodexacnts"), { "evodex_account",    {"scope", "contract", "symbol"},                 {"name", "name", "uint64"} } ) );
    BOOST_REQUIRE( same( shape("stat"),        { "currency_stats",    {"symbol_code"},                                 {"uint64"} } ) );
    BOOST_REQUIRE( same( shape("evoindex"),    { "pair_index",        {"contract1", "symbol1", "contract2", "symbol2"}, {"name", "uint64", "name", "uint64"} } ) );
    BOOST_REQUIRE( same( shape("priceaccum"),  { "price_accumulator", {"symbol_code"},                                 {"uint64"} } ) );
    BOOST_REQUIRE( same( shape("swapconfig"),  { "swap_config",       {"name"},                                        {"name"} } ) );
    BOOST_REQUIRE( same( shape("yieldpayouts"), { "payout_receipt",   {"contract"},                                    {"name"} } ) );
    BOOST_REQUIRE( same( shape("yieldfunds"),  { "fund_receipt",      {"funder"},                                      {"name"} } ) );
    BOOST_REQUIRE( same( shape("reservoirs"),  { "reservoir",         {"symbol_code"},                                 {"uint64"} } ) );

    // The shadow token's tables are read by this contract but are not its own.
    std::set<std::string> tables;
    for (const auto& t : abi.tables) tables.insert(t.name);
    const std::set<std::string> expected_tables{
        "accounts", "evodexacnts", "evoindex", "priceaccum", "reservoirs", "stat", "swapconfig",
        "yieldfunds", "yieldpayouts" };
    BOOST_REQUIRE( tables == expected_tables );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()