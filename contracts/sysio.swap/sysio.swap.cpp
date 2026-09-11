#include <sysio.swap/sysio.swap.hpp>
#include <sysio.swap/utils.hpp>
#include <tuple>

namespace sysio {

void swap::openext( const name& user, const name& payer, const extended_symbol& ext_symbol) {
    check( is_account( user ), "user account does not exist" );
    require_auth( payer );
    evodexacnts acnts( get_self(), user.value );
    const auto key = key_of(ext_symbol);
    if( !acnts.contains(key) ) {
        acnts.emplace( payer, key, evodex_account{ extended_asset{0, ext_symbol} } );
    }
}

void swap::closeext( const name& user, const name& to, const extended_symbol& ext_symbol, string memo) {
    require_auth( user );
    evodexacnts acnts( get_self(), user.value );
    const auto key = key_of(ext_symbol);
    const auto row = acnts.try_get(key);
    check( row.has_value(), "User does not have such token" );
    const extended_asset ext_balance = row->balance;
    if (ext_balance.quantity.amount > 0) {
        action(permission_level{ get_self(), "active"_n }, ext_balance.contract, "transfer"_n,
          std::make_tuple( get_self(), to, ext_balance.quantity, memo) ).send();
    }
    acnts.erase( key );
}

void swap::ontransfer(name from, name to, asset quantity, string memo) {
    constexpr string_view DEPOSIT_TO = "deposit to:";
    constexpr string_view EXCHANGE   = "exchange:";

    if (from == get_self()) return;
    check(to == get_self(), "This transfer is not for sysio.swap");
    check(quantity.amount >= 0, "quantity must be positive");

    auto incoming = extended_asset{quantity, get_first_receiver()};
    string_view memosv(memo);
    if ( starts_with(memosv, EXCHANGE) ) {
      memoexchange(from, incoming, memosv.substr(EXCHANGE.size()) );
    } else {
      if ( starts_with(memosv, DEPOSIT_TO) ) {
          from = name(trim(memosv.substr(DEPOSIT_TO.size())));
          check(from != get_self(), "Donation not accepted");
      }
      add_signed_ext_balance(from, incoming);
    }
}

void swap::withdraw(name user, name to, extended_asset to_withdraw, string memo){
    require_auth( user );
    check(to_withdraw.quantity.amount > 0, "quantity must be positive");
    add_signed_ext_balance(user, -to_withdraw);
    action(permission_level{ get_self(), "active"_n }, to_withdraw.contract, "transfer"_n,
      std::make_tuple( get_self(), to, to_withdraw.quantity, memo) ).send();
}

void swap::addliquidity(name user, asset to_buy, 
  asset max_asset1, asset max_asset2) {
    require_auth(user);
    check( (to_buy.amount > 0), "to_buy amount must be positive");
    check( (max_asset1.amount >= 0) && (max_asset2.amount >= 0), "assets must be nonnegative");
    add_signed_liq(user, to_buy, true, max_asset1, max_asset2);
}

void swap::remliquidity(name user, asset to_sell,
  asset min_asset1, asset min_asset2) {
    require_auth(user);
    check(to_sell.amount > 0, "to_sell amount must be positive");
    check( (min_asset1.amount >= 0) && (min_asset2.amount >= 0), "assets must be nonnegative");
    add_signed_liq(user, -to_sell, false, -min_asset1, -min_asset2);
}

int128_t swap::ceil_fee(int128_t amount, int fee) {
    return (amount * fee + (FEE_DENOMINATOR - 1)) / FEE_DENOMINATOR;
}

int64_t swap::compute(int64_t x, int64_t y, int64_t z, int fee) {
    check( (x != 0) && (y > 0) && (z > 0), "invalid parameters");
    int128_t prod = int128_t(x) * int128_t(y);
    int128_t tmp = 0;
    if (x > 0) {
        tmp = 1 + (prod - 1) / int128_t(z);
        check( (tmp <= MAX), "computation overflow" );
        tmp += ceil_fee(tmp, fee);
    } else {
        tmp = prod / int128_t(z);
        check( (tmp >= -MAX), "computation underflow" );
        tmp += ceil_fee(-tmp, fee);
    }
    return int64_t(tmp);
}

void swap::add_signed_liq(name user, asset to_add, bool is_buying,
  asset max_asset1, asset max_asset2){
    check( to_add.is_valid(), "invalid asset");
    stats statstable( get_self() );
    const pair_key key{ to_add.symbol.code().raw() };
    const auto token = statstable.try_get( key );
    check ( token.has_value(), "pair token does not exist" );
    auto A = token-> supply.amount;
    auto P1 = token-> pool1.quantity.amount;
    auto P2 = token-> pool2.quantity.amount;

    int fee = is_buying? ADD_LIQUIDITY_FEE : 0;
    auto to_pay1 = extended_asset{ asset{compute(to_add.amount, P1, A, fee),
      token->pool1.quantity.symbol}, token->pool1.contract};
    auto to_pay2 = extended_asset{ asset{compute(to_add.amount, P2, A, fee),
      token->pool2.quantity.symbol}, token->pool2.contract};
    check( (to_pay1.quantity.symbol == max_asset1.symbol) && 
           (to_pay2.quantity.symbol == max_asset2.symbol), "incorrect symbol");
    check( (to_pay1.quantity.amount <= max_asset1.amount) && 
           (to_pay2.quantity.amount <= max_asset2.amount), "available is less than expected");

    add_signed_ext_balance(user, -to_pay1);
    add_signed_ext_balance(user, -to_pay2);
    (to_add.amount > 0)? add_balance(user, to_add, user) : sub_balance(user, -to_add);
    update_price_accumulators(*token);
    statstable.modify( name{}, key, [&]( auto& a ) {
      a.supply += to_add;
      a.pool1 += to_pay1;
      a.pool2 += to_pay2;
    });
    // Ownership already bounds a removal by the caller's own shares, so supply can
    // only reach the locked floor when nothing is locked and the last share goes.
    const int64_t remaining = token->supply.amount + to_add.amount;
    check(remaining != 0, "the pool cannot be left empty");
    check(remaining >= token->locked_shares.amount, "locked shares cannot be removed");
}

void swap::exchange( name user, symbol_code pair_token, 
  extended_asset ext_asset_in, asset min_expected) {
    require_auth(user);
    check( ext_asset_in.quantity.amount > 0, "ext_asset_in must be positive" );
    check( min_expected.amount >= 0, "min_expected must be nonnegative" );
    auto ext_asset_out = process_exch(pair_token, ext_asset_in, min_expected);
    add_signed_ext_balance(user, -ext_asset_in);
    add_signed_ext_balance(user, ext_asset_out);
}

extended_asset swap::process_exch(symbol_code pair_token,
  extended_asset ext_asset_in, asset min_expected){
    stats statstable( get_self() );
    const pair_key key{ pair_token.raw() };
    const auto token = statstable.try_get( key );
    check ( token.has_value(), "pair token does not exist" );
    bool in_first;
    if ((token->pool1.get_extended_symbol() == ext_asset_in.get_extended_symbol()) && 
        (token->pool2.quantity.symbol == min_expected.symbol)) {
        in_first = true;
    } else if ((token->pool1.quantity.symbol == min_expected.symbol) &&
               (token->pool2.get_extended_symbol() == ext_asset_in.get_extended_symbol())) {
        in_first = false;
    }
    else check(false, "extended_symbol mismatch");
    int64_t P_in, P_out;
    if (in_first) { 
      P_in = token-> pool1.quantity.amount;
      P_out = token-> pool2.quantity.amount;
    } else {
      P_in = token-> pool2.quantity.amount;
      P_out = token-> pool1.quantity.amount;
    }
    const int64_t A_in = ext_asset_in.quantity.amount;
    check( (A_in > 0) && (P_in > 0) && (P_out > 0), "invalid parameters");
    // Constant-product quote, floored, then the pair's fee taken off it with the
    // depot-wide decomposition. The fee has no recipient here -- it stays in the
    // pool for the liquidity providers. The decomposition rounds the fee down,
    // which would let a quote below FEE_DENOMINATOR/fee units trade fee-free;
    // "units" is precision-relative, so a nonzero fee rate collects at least
    // MIN_SWAP_FEE on any nonzero quote and every fee-bearing trade grows x*y.
    const uint64_t gross = opp::amm::out_given_in(uint64_t(P_in), CP_WEIGHT_BPS,
                                                  uint64_t(P_out), CP_WEIGHT_BPS,
                                                  uint64_t(A_in));
    uint64_t fee = opp::amm::split_wire_fee(gross, uint32_t(token->fee), NO_UNDERWRITER_SHARE_BPS).fee;
    if (token->fee > 0 && gross > 0) fee = std::max(fee, MIN_SWAP_FEE);
    const int64_t A_out = int64_t(gross - fee);
    check(min_expected.amount <= A_out, "available is less than expected");
    extended_asset ext_asset1, ext_asset2, ext_asset_out;
    if (in_first) {
      ext_asset1 = ext_asset_in;
      ext_asset2 = extended_asset{-A_out, token-> pool2.get_extended_symbol()};
      ext_asset_out = -ext_asset2;
    } else {
      ext_asset1 = extended_asset{-A_out, token-> pool1.get_extended_symbol()};
      ext_asset2 = ext_asset_in;
      ext_asset_out = -ext_asset1;
    }
    update_price_accumulators(*token);
    statstable.modify( name{}, key, [&]( auto& a ) {
      a.pool1 += ext_asset1;
      a.pool2 += ext_asset2;
    });
    return ext_asset_out;
}

void swap::memoexchange(name user, extended_asset ext_asset_in, string_view details){
    auto parts = split(details, ",");
    check(parts.size() >= 2, "Expected format 'EVOTOKEN,min_expected_asset,optional memo'");

    auto pair_token   = symbol_code(parts[0]);
    auto min_expected = asset_from_string(parts[1]);
    auto second_comma_pos = details.find(",", 1 + details.find(","));
    auto memo = (second_comma_pos == string::npos)? "" : details.substr(1 + second_comma_pos);

    check(min_expected.amount >= 0, "min_expected must be expressed with a positive amount");
    auto ext_asset_out = process_exch(pair_token, ext_asset_in, min_expected);
    action(permission_level{ get_self(), "active"_n }, ext_asset_out.contract, "transfer"_n,
      std::make_tuple( get_self(), user, ext_asset_out.quantity, std::string(memo)) ).send();
}

void swap::setconfig(name fee_authority) {
    require_auth( get_self() );
    check( is_account( fee_authority ), "fee authority account does not exist" );
    swapconfig_t config( get_self() );
    config.set( swap_config{ fee_authority }, get_self() );
}

void swap::inittoken(name user, symbol new_symbol, extended_asset initial_pool1,
extended_asset initial_pool2, int initial_fee, name fee_authority, asset locked_shares)
{
    require_auth( user );
    require_auth( get_self() );
    check((initial_pool1.quantity.amount > 0) && (initial_pool2.quantity.amount > 0), "Both assets must be positive");
    check((initial_pool1.quantity.amount < INIT_MAX) && (initial_pool2.quantity.amount < INIT_MAX), "Initial amounts must be less than 10^15");
    uint8_t new_precision = ( initial_pool1.quantity.symbol.precision() + initial_pool2.quantity.symbol.precision() ) / 2;
    check( new_symbol.precision() == new_precision, "new_symbol precision must be (precision1 + precision2) / 2" );
    const auto new_token = asset{ int64_t(opp::amm::geometric_mean(uint64_t(initial_pool1.quantity.amount),
                                                                   uint64_t(initial_pool2.quantity.amount))),
                                  new_symbol };
    check( locked_shares.symbol == new_symbol, "locked_shares must be in new_symbol" );
    check( locked_shares.amount >= 0, "locked_shares must be nonnegative" );
    check( locked_shares.amount < new_token.amount, "locked_shares must leave the creator at least one share" );
    check( initial_pool1.get_extended_symbol() != initial_pool2.get_extended_symbol(), "extended symbols must be different");

    stats statstable( get_self() );
    const pair_key key{ new_symbol.code().raw() };
    check ( !statstable.contains( key ), "token symbol already exists" );
    check( 0 <= initial_fee && initial_fee <= MAX_FEE, "fee out of range" );
    if (fee_authority == name{}) {
        swapconfig_t config( get_self() );
        fee_authority = config.get( "fee authority not configured" ).fee_authority;
    } else {
        check( is_account( fee_authority ), "fee authority account does not exist" );
    }

    statstable.emplace( user, key, currency_stats{
        .supply        = new_token,
        .max_supply    = asset{MAX, new_symbol},
        .issuer        = get_self(),
        .pool1         = initial_pool1,
        .pool2         = initial_pool2,
        .fee           = initial_fee,
        .fee_authority = fee_authority,
        .locked_shares = locked_shares,
    } );

    priceaccums accums( get_self() );
    accums.emplace( user, key, price_accumulator{ .last_update = current_time_point() } );

    placeindex(user, new_symbol, initial_pool1, initial_pool2 );
    // The locked shares count toward supply but are credited to nobody.
    add_balance(user, new_token - locked_shares, user);
    add_signed_ext_balance(user, -initial_pool1);
    add_signed_ext_balance(user, -initial_pool2);
}

void swap::indexpair(name user, symbol evo_symbol) {
    stats statstable( get_self() );
    const auto token = statstable.get( pair_key{ evo_symbol.code().raw() }, "token symbol does not exist" );
    placeindex(user, evo_symbol, token.pool1, token.pool2);
}

void swap::placeindex(name user, symbol evo_symbol,
  extended_asset pool1, extended_asset pool2 ) {
    evoindexes indextable( get_self() );
    indextable.emplace( user, identity_of(pool1.get_extended_symbol(), pool2.get_extended_symbol()),
                        pair_index{ evo_symbol }, "the pool is already indexed" );
}

void swap::update_price_accumulators(const currency_stats& token) {
    priceaccums accums( get_self() );
    const pair_key key{ token.supply.symbol.code().raw() };
    const auto accum = accums.try_get( key );
    check( accum.has_value(), "price accumulator does not exist" );
    const time_point now = current_time_point();
    if (now <= accum->last_update) return;
    const uint64_t elapsed = uint64_t((now - accum->last_update).count());
    const uint64_t P1 = uint64_t(token.pool1.quantity.amount);
    const uint64_t P2 = uint64_t(token.pool2.quantity.amount);
    accums.modify( name{}, key, [&]( auto& a ) {
        opp::twap::accumulate( a.price1, opp::twap::price_fp(P2, P1), elapsed );
        opp::twap::accumulate( a.price2, opp::twap::price_fp(P1, P2), elapsed );
        a.last_update = now;
    } );
}

void swap::sync(symbol_code pair_token) {
    stats statstable( get_self() );
    const auto token = statstable.try_get( pair_key{ pair_token.raw() } );
    check ( token.has_value(), "pair token does not exist" );
    update_price_accumulators(*token);
}

void swap::changefee(symbol_code pair_token, int newfee) {
    stats statstable( get_self() );
    const pair_key key{ pair_token.raw() };
    const auto token = statstable.try_get( key );
    check ( token.has_value(), "pair token does not exist" );
    require_auth(token->fee_authority);
    check( 0 <= newfee && newfee <= MAX_FEE, "fee out of range" );
    statstable.modify( name{}, key, [&]( auto& a ) {
      a.fee = newfee;
    } );
}

swap::extended_symbol_key swap::key_of(const extended_symbol& ext_symbol) {
    return extended_symbol_key{ ext_symbol.get_contract(), ext_symbol.get_symbol().raw() };
}

swap::pair_identity_key swap::identity_of(const extended_symbol& a, const extended_symbol& b) {
    const extended_symbol_key ka = key_of(a);
    const extended_symbol_key kb = key_of(b);
    const bool a_first = std::tie(ka.contract.value, ka.symbol) < std::tie(kb.contract.value, kb.symbol);
    const extended_symbol_key& first  = a_first ? ka : kb;
    const extended_symbol_key& second = a_first ? kb : ka;
    return pair_identity_key{ first.contract, first.symbol, second.contract, second.symbol };
}

void swap::add_signed_ext_balance( const name& user, const extended_asset& to_add )
{
    check( to_add.quantity.is_valid(), "invalid asset" );
    evodexacnts acnts( get_self(), user.value );
    const auto key = key_of(to_add.get_extended_symbol());
    check( acnts.contains(key), "extended_symbol not registered for this user,\
 please run openext action or write exchange details in the memo of your transfer");
    acnts.modify( name{}, key, [&]( auto& a ) {
        a.balance += to_add;
        check( a.balance.quantity.amount >= 0, "insufficient funds");
    });
}
} // namespace sysio
