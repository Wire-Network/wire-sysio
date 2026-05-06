#include "sysio.token.hpp"

namespace sysio {

// The sysio.token WIRE token RAM is paid for by sysio
constexpr name ram_payer = "sysio"_n;

void token::create( const name&   issuer,
                    const asset&  maximum_supply )
{
    require_auth( get_self() );

    auto sym = maximum_supply.symbol;
    check( maximum_supply.is_valid(), "invalid supply");
    check( maximum_supply.amount > 0, "max-supply must be positive");

    stats statstable( get_self(), sym.code().raw() );
    statstable.emplace( ram_payer, stat_key{sym.code().raw()}, currency_stats{
       .supply     = asset{0, maximum_supply.symbol},
       .max_supply = maximum_supply,
       .issuer     = issuer,
    }, "token with symbol already exists");
}


void token::issue( const name& to, const asset& quantity, const string& memo )
{
    auto sym = quantity.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( get_self(), sym.code().raw() );
    const auto st = statstable.get( stat_key{sym.code().raw()}, "token with symbol does not exist, create token before issue" );
    check( to == st.issuer, "tokens can only be issued to issuer account" );

    require_auth( st.issuer );
    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must issue positive quantity" );
    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify( ram_payer, stat_key{sym.code().raw()}, [&]( auto& s ) {
       s.supply += quantity;
    });

    add_balance( st.issuer, quantity, ram_payer );
}

void token::retire( const asset& quantity, const string& memo )
{
    auto sym = quantity.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( get_self(), sym.code().raw() );
    const auto st = statstable.get( stat_key{sym.code().raw()}, "token with symbol does not exist" );

    require_auth( st.issuer );
    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must retire positive quantity" );

    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );

    statstable.modify( ram_payer, stat_key{sym.code().raw()}, [&]( auto& s ) {
       s.supply -= quantity;
    });

    sub_balance( st.issuer, quantity );
}

void token::transfer( const name&    from,
                      const name&    to,
                      const asset&   quantity,
                      const string&  memo )
{
    check( from != to, "cannot transfer to self" );
    require_auth( from );
    check( is_account( to ), "to account does not exist");
    auto sym = quantity.symbol.code();
    stats statstable( get_self(), sym.raw() );
    const auto& st = statstable.get( stat_key{sym.raw()}, "token with symbol does not exist" );

    require_recipient( from );
    require_recipient( to );

    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must transfer positive quantity" );
    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    sub_balance( from, quantity );
    add_balance( to, quantity, ram_payer );
}

void token::sub_balance( const name& owner, const asset& value ) {
   accounts from_acnts( get_self(), owner.value );
   from_acnts.modify( ram_payer, acct_key{value.symbol.code().raw()}, [&]( auto& a ) {
      check( a.balance.amount >= value.amount, "overdrawn balance" );
      a.balance -= value;
   });
}

void token::add_balance( const name& owner, const asset& value, const name& ram_payer )
{
   accounts to_acnts( get_self(), owner.value );
   to_acnts.upsert( ram_payer, acct_key{value.symbol.code().raw()},
      account{value},                               // default if new
      [&]( auto& a ) { a.balance += value; } );     // updater if exists
}

void token::open( const name& owner, const symbol& symbol, const name& ram_payer )
{
   require_auth( ram_payer );

   check( is_account( owner ), "owner account does not exist" );

   auto sym_code_raw = symbol.code().raw();
   stats statstable( get_self(), sym_code_raw );
   const auto& st = statstable.get( stat_key{sym_code_raw}, "symbol does not exist" );
   check( st.supply.symbol == symbol, "symbol precision mismatch" );

   accounts acnts( get_self(), owner.value );
   if( !acnts.contains( acct_key{sym_code_raw} ) ) {
      acnts.emplace( ram_payer, acct_key{sym_code_raw}, account{asset{0, symbol}} );
   }
}

void token::close( const name& owner, const symbol& symbol )
{
   require_auth( owner );
   accounts acnts( get_self(), owner.value );
   auto key = acct_key{symbol.code().raw()};
   auto bal = acnts.get( key, "Balance row already deleted or never existed. Action won't have any effect." );
   check( bal.balance.amount == 0, "Cannot close because the balance is not zero." );
   acnts.erase( key );
}

} /// namespace sysio
