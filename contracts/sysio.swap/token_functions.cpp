#include <sysio.swap/sysio.swap.hpp>

namespace sysio {

void swap::transfer( const name& from, const name& to, const asset& quantity,
  const string& memo) {
    check( from != to, "cannot transfer to self" );
    require_auth( from );
    require_clear();
    check( is_account( to ), "to account does not exist");
    auto sym = quantity.symbol.code();
    stats statstable( get_self() );
    const auto st = statstable.get( pair_key{ sym.raw() }, "pair token does not exist" );

    require_recipient( from );
    require_recipient( to );

    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must transfer positive quantity" );
    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    sub_balance( from, quantity );
    add_balance( to, quantity );
    if (to == get_self()) ontransfer(from, to, quantity, memo); // line added to code from eosio.token
}

void swap::sub_balance( const name& owner, const asset& value ) {
    accounts from_acnts( get_self(), owner.value );
    const account_key key{ value.symbol.code().raw() };

    const auto from = from_acnts.try_get( key );
    check( from.has_value(), "no balance object found" );
    check( from->balance.amount >= value.amount, "overdrawn balance" );

    from_acnts.modify( RAM_PAYER, key, [&]( auto& a ) {
            a.balance -= value;
        });
}

void swap::add_balance( const name& owner, const asset& value )
{
    accounts to_acnts( get_self(), owner.value );
    to_acnts.upsert( RAM_PAYER, account_key{ value.symbol.code().raw() },
       account{ value },                            // default if new
       [&]( auto& a ) { a.balance += value; } );   // updater if exists
}

void swap::close( const name& owner, const symbol& symbol )
{
   require_auth( owner );
   accounts acnts( get_self(), owner.value );
   const account_key key{ symbol.code().raw() };
   const auto row = acnts.try_get( key );
   check( row.has_value(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( row->balance.amount == 0, "Cannot close because the balance is not zero." );
   acnts.erase( key );
}

} // namespace sysio
