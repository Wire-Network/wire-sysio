#include <sysio.swap/sysio.swap.hpp>
#include <sysio.swap/utils.hpp>
#include <tuple>

namespace {
   // The shadow token's tables, read in place (sysio.opp.common/shadow_yield.hpp):
   // constructed with the token contract as code, the holder table scoped by the
   // holder. Declared outside the contract class on purpose: the ABI generator
   // lists every kv table alias it finds inside the class as the contract's own.
   using shadow_accounts = sysio::kv::scoped_table<sysio::opp::shadow::ACCOUNTS_TABLE,
                                                   sysio::opp::shadow::symbol_key, sysio::opp::shadow::account>;
   using shadow_indexes  = sysio::kv::table<sysio::opp::shadow::YIELD_INDEX_TABLE,
                                            sysio::opp::shadow::symbol_key, sysio::opp::shadow::yield_index>;

   /// Whether a delivered extended asset is exactly the expected one: same
   /// contract, same symbol, same amount. (asset's own == asserts on a symbol
   /// mismatch, which here must be an ordinary "does not match" failure.)
   bool delivers(const sysio::extended_asset& delivered, const sysio::extended_asset& expected) {
      return delivered.contract == expected.contract
          && delivered.quantity.symbol == expected.quantity.symbol
          && delivered.quantity.amount == expected.quantity.amount;
   }
}

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
    require_deposit_token( incoming.get_extended_symbol() );
    // A payout this contract claimed from a shadow token and already credited to
    // the pool: match it against the receipt and retire the receipt. Nothing
    // else is accepted from a contract with a claim outstanding.
    yieldpayouts payouts( get_self() );
    const contract_key payer{ from };
    if (const auto receipt = payouts.try_get( payer )) {
        check( delivers( incoming, receipt->quantity ), "yield payout does not match the claim" );
        payouts.erase( payer );
        return;
    }
    // A funding `from` announced with fundyield: the announced amount fills the
    // pair's reservoir, anything else from `from` is refused until it does.
    yieldfunds funds( get_self() );
    const funder_key funder{ from };
    if (const auto receipt = funds.try_get( funder )) {
        check( delivers( incoming, receipt->quantity ), "yield funding does not match the pending fundyield" );
        reservoirs reservoir_table( get_self() );
        const pair_key pair{ receipt->pair.raw() };
        // A reservoir that was empty starts selling over a fresh horizon; one that
        // was not keeps the clock it had.
        const bool was_empty = reservoir_table.get( pair ).balance.quantity.amount == 0;
        reservoir_table.modify( name{}, pair, [&]( auto& r ) { r.balance += incoming; } );
        if (was_empty) restart_tick_clock( pair );
        funds.erase( funder );
        return;
    }
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
    // The shared kernel prices a magnitude and says nothing about direction, so
    // the sign of `x` picks the rounding there and the sign of the result here.
    // Its value can exceed an asset, which is what the bounds below are for;
    // they run before the fee, so the fee is charged on a sane amount.
    const uint64_t shares = uint64_t( x > 0 ? int128_t(x) : -int128_t(x) );
    int128_t tmp = 0;
    if (x > 0) {
        tmp = int128_t( opp::amm::in_given_shares(uint64_t(y), uint64_t(z), shares) );
        check( (tmp <= MAX), "computation overflow" );
        tmp += ceil_fee(tmp, fee);
    } else {
        tmp = -int128_t( opp::amm::out_given_shares(uint64_t(y), uint64_t(z), shares) );
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
    const auto stored = statstable.try_get( key );
    check ( stored.has_value(), "pair token does not exist" );
    // Yield owed to the pool belongs to the shares that exist now: settle it
    // before any share is priced, minted or burned.
    const currency_stats token = accrue( key, *stored );
    auto A = token.supply.amount;
    auto P1 = token.pool1.quantity.amount;
    auto P2 = token.pool2.quantity.amount;

    int fee = is_buying? ADD_LIQUIDITY_FEE : 0;
    auto to_pay1 = extended_asset{ asset{compute(to_add.amount, P1, A, fee),
      token.pool1.quantity.symbol}, token.pool1.contract};
    auto to_pay2 = extended_asset{ asset{compute(to_add.amount, P2, A, fee),
      token.pool2.quantity.symbol}, token.pool2.contract};
    check( (to_pay1.quantity.symbol == max_asset1.symbol) && 
           (to_pay2.quantity.symbol == max_asset2.symbol), "incorrect symbol");
    check( (to_pay1.quantity.amount <= max_asset1.amount) && 
           (to_pay2.quantity.amount <= max_asset2.amount), "available is less than expected");

    add_signed_ext_balance(user, -to_pay1);
    add_signed_ext_balance(user, -to_pay2);
    (to_add.amount > 0)? add_balance(user, to_add, user) : sub_balance(user, -to_add);
    update_price_accumulators(token);
    statstable.modify( name{}, key, [&]( auto& a ) {
      a.supply += to_add;
      a.pool1 += to_pay1;
      a.pool2 += to_pay2;
    });
    // Ownership already bounds a removal by the caller's own shares, so supply can
    // only reach the locked floor when nothing is locked and the last share goes.
    const int64_t remaining = token.supply.amount + to_add.amount;
    check(remaining != 0, "the pool cannot be left empty");
    check(remaining >= token.locked_shares.amount, "locked shares cannot be removed");
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
    const auto stored = statstable.try_get( key );
    check ( stored.has_value(), "pair token does not exist" );
    // Yield owed to the pool is part of the pool, so it is settled before anyone
    // prices a trade against it. Without this an atomic buy, accrueyield, sell
    // takes a share of the pending yield off the liquidity providers.
    const currency_stats token = accrue( key, *stored );
    bool in_first;
    if ((token.pool1.get_extended_symbol() == ext_asset_in.get_extended_symbol()) &&
        (token.pool2.quantity.symbol == min_expected.symbol)) {
        in_first = true;
    } else if ((token.pool1.quantity.symbol == min_expected.symbol) &&
               (token.pool2.get_extended_symbol() == ext_asset_in.get_extended_symbol())) {
        in_first = false;
    }
    else check(false, "extended_symbol mismatch");
    int64_t P_in, P_out;
    if (in_first) {
      P_in = token.pool1.quantity.amount;
      P_out = token.pool2.quantity.amount;
    } else {
      P_in = token.pool2.quantity.amount;
      P_out = token.pool1.quantity.amount;
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
    uint64_t fee = opp::amm::split_wire_fee(gross, uint32_t(token.fee), NO_UNDERWRITER_SHARE_BPS).fee;
    if (token.fee > 0 && gross > 0) fee = std::max(fee, MIN_SWAP_FEE);
    const int64_t A_out = int64_t(gross - fee);
    check(min_expected.amount <= A_out, "available is less than expected");
    extended_asset ext_asset1, ext_asset2, ext_asset_out;
    if (in_first) {
      ext_asset1 = ext_asset_in;
      ext_asset2 = extended_asset{-A_out, token.pool2.get_extended_symbol()};
      ext_asset_out = -ext_asset2;
    } else {
      ext_asset1 = extended_asset{-A_out, token.pool1.get_extended_symbol()};
      ext_asset2 = ext_asset_in;
      ext_asset_out = -ext_asset1;
    }
    update_price_accumulators(token);
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

void swap::setconfig(name fee_authority, extended_symbol system_token) {
    require_auth( get_self() );
    check( is_account( fee_authority ), "fee authority account does not exist" );
    check( is_account( system_token.get_contract() ), "system token contract does not exist" );
    check( system_token.get_symbol().is_valid(), "invalid system token symbol" );
    swapconfig_t config( get_self() );
    config.set( swap_config{ fee_authority, system_token }, get_self() );
}

swap::swap_config swap::configured() const {
    swapconfig_t config( get_self() );
    return config.get( "swap not configured" );
}

void swap::require_deposit_token(const extended_symbol& token) const {
    const swap_config cfg = configured();
    if (token == cfg.system_token) return;
    evoindexes indextable( get_self() );
    if (indextable.contains( identity_of( token, cfg.system_token ) )) return;
    check( has_auth( get_self() ), "token is not a leg of any pair" );
}

void swap::inittoken(name user, symbol new_symbol, extended_asset initial_pool1,
extended_asset initial_pool2, int initial_fee, name fee_authority, asset locked_shares,
std::optional<extended_symbol> yield_leg)
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
    const swap_config cfg = configured();
    check( initial_pool2.get_extended_symbol() == cfg.system_token, "the second leg must be the system token" );
    stats statstable( get_self() );
    const pair_key key{ new_symbol.code().raw() };
    check ( !statstable.contains( key ), "token symbol already exists" );
    if (yield_leg) {
        // Pairs are unique per first leg, so this is also the only yield pool the
        // shadow can have.
        check( *yield_leg == initial_pool1.get_extended_symbol(), "yield_leg must be the pair's first leg" );
        reservoirs reservoir_table( get_self() );
        reservoir_table.emplace( user, key, reservoir{ extended_asset{ 0, *yield_leg } } );
    }
    check( 0 <= initial_fee && initial_fee <= MAX_FEE, "fee out of range" );
    if (fee_authority == name{}) {
        fee_authority = cfg.fee_authority;
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
        .yield_leg     = yield_leg,
    } );

    priceaccums accums( get_self() );
    accums.emplace( user, key, price_accumulator{ .last_update = current_time_point() } );

    placeindex(user, new_symbol, initial_pool1, initial_pool2 );
    // The locked shares count toward supply but are credited to nobody.
    add_balance(user, new_token - locked_shares, user);
    add_signed_ext_balance(user, -initial_pool1);
    add_signed_ext_balance(user, -initial_pool2);
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

const extended_symbol& swap::require_yield_leg(const currency_stats& token) {
    check( token.yield_leg.has_value(), "pair has no yield leg" );
    return *token.yield_leg;
}

const extended_asset& swap::pool_of(const currency_stats& token, const extended_symbol& leg) {
    if (token.pool1.get_extended_symbol() == leg) return token.pool1;
    check( token.pool2.get_extended_symbol() == leg, "not a leg of this pair" );
    return token.pool2;
}

const extended_asset& swap::other_pool(const currency_stats& token, const extended_symbol& leg) {
    if (token.pool1.get_extended_symbol() == leg) return token.pool2;
    check( token.pool2.get_extended_symbol() == leg, "not a leg of this pair" );
    return token.pool1;
}

uint64_t swap::owed_yield(const extended_symbol& shadow) const {
    const opp::shadow::symbol_key key{ shadow.get_symbol().code().raw() };
    shadow_indexes indexes( shadow.get_contract() );
    const auto index = indexes.try_get( key );
    if (!index || index->index == 0) return 0;
    shadow_accounts holdings( shadow.get_contract(), get_self().value );
    const auto row = holdings.try_get( key );
    return row ? opp::shadow::owed( *row, index->index ) : 0;
}

swap::currency_stats swap::accrue(const pair_key& key, const currency_stats& token) {
    if (!token.yield_leg) return token;
    const extended_symbol& shadow = *token.yield_leg;
    const uint64_t owed = owed_yield( shadow );
    if (owed == 0) return token;
    check( owed <= uint64_t(MAX), "yield payout overflows" );
    const extended_symbol payout_symbol = other_pool( token, shadow ).get_extended_symbol();
    const extended_asset payout{ asset{ int64_t(owed), payout_symbol.get_symbol() }, payout_symbol.get_contract() };

    yieldpayouts payouts( get_self() );
    payouts.emplace( get_self(), contract_key{ shadow.get_contract() },
                     payout_receipt{ token.supply.symbol.code(), payout },
                     "a yield payout from this contract is still pending" );
    // The pool changes: close the accumulators' interval at the old price first.
    update_price_accumulators( token );
    stats statstable( get_self() );
    statstable.modify( name{}, key, [&]( auto& a ) {
        if (a.pool1.get_extended_symbol() == shadow) a.pool2 += payout;
        else                                         a.pool1 += payout;
    } );
    action( permission_level{ get_self(), "active"_n }, shadow.get_contract(), opp::shadow::CLAIM_ACTION,
            std::make_tuple( get_self(), shadow.get_symbol().code() ) ).send();
    return statstable.get( key );
}

void swap::fundyield(name from, symbol_code pair_token, asset quantity) {
    require_auth( from );
    stats statstable( get_self() );
    const auto token = statstable.try_get( pair_key{ pair_token.raw() } );
    check ( token.has_value(), "pair token does not exist" );
    const extended_symbol& shadow = require_yield_leg(*token);
    check( quantity.symbol == shadow.get_symbol(), "quantity must be in the pair's shadow symbol" );
    check( quantity.amount > 0, "quantity must be positive" );
    // Billed to `from`: only the matching transfer erases the row, so an
    // announcement nobody delivers would otherwise sit on the contract's RAM,
    // one per account that ever called this. cancelyield refunds it.
    yieldfunds funds( get_self() );
    funds.upsert( from, funder_key{ from },
                  fund_receipt{ pair_token, extended_asset{ quantity, shadow.get_contract() } } );
}

void swap::cancelyield(name from) {
    require_auth( from );
    yieldfunds funds( get_self() );
    const funder_key funder{ from };
    check( funds.contains( funder ), "no pending fundyield" );
    funds.erase( funder );
}

void swap::accrueyield(symbol_code pair_token) {
    stats statstable( get_self() );
    const pair_key key{ pair_token.raw() };
    const auto token = statstable.try_get( key );
    check ( token.has_value(), "pair token does not exist" );
    require_yield_leg(*token);
    accrue( key, *token );
}

void swap::setyield(symbol_code pair_token, uint32_t conversion_horizon_sec, uint32_t depth_cap_bps,
  int64_t clip_floor) {
    stats statstable( get_self() );
    const pair_key key{ pair_token.raw() };
    const auto token = statstable.try_get( key );
    check ( token.has_value(), "pair token does not exist" );
    require_auth(token->fee_authority);
    const extended_symbol& shadow = require_yield_leg(*token);
    check( depth_cap_bps <= opp::amm::BPS_TOTAL, "depth_cap_bps out of range" );
    check( clip_floor >= 0 && clip_floor <= MAX, "clip_floor out of range" );
    const int64_t shadow_depth = pool_of( *token, shadow ).quantity.amount;
    statstable.modify( name{}, key, [&]( auto& a ) {
      a.conversion_horizon_sec = conversion_horizon_sec;
      a.depth_cap_bps          = depth_cap_bps;
      a.clip_floor             = clip_floor;
      a.last_tick_depth        = shadow_depth;
      a.last_tick              = current_time_point();   // new parameters, fresh horizon
    } );
}

void swap::restart_tick_clock(const pair_key& key) {
    stats statstable( get_self() );
    statstable.modify( name{}, key, [&]( auto& a ) { a.last_tick = current_time_point(); } );
}

void swap::tickyield(symbol_code pair_token) {
    stats statstable( get_self() );
    const pair_key key{ pair_token.raw() };
    const auto stored = statstable.try_get( key );
    check ( stored.has_value(), "pair token does not exist" );
    const extended_symbol shadow = require_yield_leg(*stored);
    check( stored->conversion_horizon_sec > 0 && stored->depth_cap_bps > 0 && stored->clip_floor > 0,
           "yield tick parameters not set" );
    // process_exch settles what the pool is owed before it prices the clip, so
    // this does not accrue itself. Nothing read below moves when it does: accrual
    // credits the other leg, and the clip is measured against the shadow side.
    const currency_stats& token = *stored;

    reservoirs reservoir_table( get_self() );
    const int64_t queued = reservoir_table.get( key ).balance.quantity.amount;
    const time_point now = current_time_point();
    if (queued <= 0 || now <= token.last_tick) return;

    // The clip: the reservoir's share of the horizon that has elapsed, FLOORED,
    // capped by depth_cap_bps of the pool's shadow side and by what is queued.
    const uint128_t elapsed_us = uint128_t( (now - token.last_tick).count() );
    const uint128_t horizon_us = uint128_t( sysio::seconds( token.conversion_horizon_sec ).count() );
    // The cap is taken against the SMALLER of the shadow side now and as of the
    // last setyield or selling tick. Selling shadow into the pool is what widens
    // the current side, and it is the same move that makes a clip worth
    // sandwiching, so a cap that followed it would be set by the attacker it is
    // meant to bound. Taking the smaller also tightens immediately when the pool
    // genuinely shrinks, and lets genuine growth through one tick later.
    const uint128_t cap_depth = std::min( uint128_t( pool_of( token, shadow ).quantity.amount ),
                                          uint128_t( token.last_tick_depth ) );
    const uint128_t cap = cap_depth * token.depth_cap_bps / opp::amm::BPS_TOTAL;
    uint128_t clip = ( uint128_t(queued) * elapsed_us ) / horizon_us;
    clip = std::min( { clip, cap, uint128_t(queued) } );
    // Below the floor there is nothing worth selling yet, so return WITHOUT
    // touching last_tick: the clock keeps running and the next tick measures a
    // longer window, offering a proportionally larger clip. That is what makes
    // cranking every block harmless, and it costs no throughput -- waiting N
    // times as long sells N times as much, so the average rate is unchanged.
    //
    // The floor gives way to `queued` so a remainder smaller than it is not
    // stranded: that leaves as one sale once the time share reaches the whole
    // queue, which takes exactly one horizon. A depth cap below the floor is
    // the one combination with no way out -- every clip is capped under the
    // floor and the pair stops selling until setyield widens one of them.
    if (clip < std::min( uint128_t(token.clip_floor), uint128_t(queued) )) return;

    const extended_asset selling{ asset{ int64_t(clip), shadow.get_symbol() }, shadow.get_contract() };
    const symbol proceeds_symbol = other_pool( token, shadow ).quantity.symbol;
    const extended_asset proceeds = process_exch( pair_token, selling, asset{ 0, proceeds_symbol } );
    reservoir_table.modify( name{}, key, [&]( auto& r ) { r.balance -= selling; } );
    statstable.modify( name{}, key, [&]( auto& a ) {
      a.last_tick       = now;
      a.last_tick_depth = pool_of( a, shadow ).quantity.amount;   // a is post-trade
    } );
    // The proceeds reach every holder of the shadow through the token's own
    // distribution; the pool, a holder, takes its share back on the next accrual.
    if (proceeds.quantity.amount > 0) {
        action( permission_level{ get_self(), "active"_n }, shadow.get_contract(), opp::shadow::ADDYIELD_ACTION,
                std::make_tuple( get_self(), proceeds.quantity, shadow.get_symbol().code() ) ).send();
    }
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
