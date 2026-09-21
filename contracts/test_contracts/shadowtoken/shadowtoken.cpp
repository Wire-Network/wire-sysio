#include "shadowtoken.hpp"

namespace {
   using u128 = sysio::opp::shadow::u128;
}

void shadowtoken::create(name issuer, asset maximum_supply, name wire_contract, symbol wire_symbol) {
   require_auth( get_self() );
   check( maximum_supply.is_valid() && maximum_supply.amount > 0, "invalid supply" );
   stats statstable( get_self() );
   const opp::shadow::symbol_key key{ maximum_supply.symbol.code().raw() };
   statstable.emplace( get_self(), key, currency_stats{
      .supply        = asset{ 0, maximum_supply.symbol },
      .max_supply    = maximum_supply,
      .issuer        = issuer,
      .wire_contract = wire_contract,
      .wire_symbol   = wire_symbol,
   }, "symbol already exists" );
}

void shadowtoken::issue(name to, asset quantity, string memo) {
   stats statstable( get_self() );
   const opp::shadow::symbol_key key{ quantity.symbol.code().raw() };
   const auto st = statstable.get( key, "symbol does not exist" );
   require_auth( st.issuer );
   check( quantity.is_valid() && quantity.amount > 0, "invalid quantity" );
   check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
   check( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply" );
   statstable.modify( name{}, key, [&]( auto& s ) { s.supply += quantity; } );
   settle_and_adjust( to, quantity );
}

void shadowtoken::transfer(name from, name to, asset quantity, string memo) {
   check( from != to, "cannot transfer to self" );
   require_auth( from );
   check( is_account( to ), "to account does not exist" );
   check( quantity.is_valid() && quantity.amount > 0, "invalid quantity" );
   require_recipient( from );
   require_recipient( to );
   settle_and_adjust( from, -quantity );
   settle_and_adjust( to, quantity );
}

void shadowtoken::addyield(name from, asset quantity, symbol_code target) {
   require_auth( from );
   check( quantity.amount > 0, "yield must be positive" );
   stats statstable( get_self() );
   const opp::shadow::symbol_key key{ target.raw() };
   const auto st = statstable.get( key, "shadow symbol does not exist" );
   check( quantity.symbol == st.wire_symbol, "yield must be in the wire symbol" );
   check( st.supply.amount > 0, "no holders to distribute to" );

   yieldidxs indexes( get_self() );
   opp::shadow::yield_index idx = indexes.try_get( key ).value_or( opp::shadow::yield_index{} );
   const u128 total = static_cast<u128>(quantity.amount) * opp::shadow::YIELD_INDEX_SCALE + idx.carry;
   idx.index += total / static_cast<u128>(st.supply.amount);
   idx.carry  = static_cast<uint64_t>( total % static_cast<u128>(st.supply.amount) );
   idx.pot   += quantity.amount;
   indexes.upsert( get_self(), key, idx );

   action( permission_level{ from, "active"_n }, st.wire_contract, "transfer"_n,
           std::make_tuple( from, get_self(), quantity, string("yield") ) ).send();
}

void shadowtoken::claim(name holder, symbol_code sym) {
   require_auth( holder );
   accounts holdings( get_self(), holder.value );
   const opp::shadow::symbol_key key{ sym.raw() };
   const auto row = holdings.try_get( key );
   check( row.has_value(), "no balance object found" );
   const u128     index = current_index( sym );
   const uint64_t owed  = opp::shadow::owed( *row, index );
   holdings.modify( name{}, key, [&]( auto& a ) {
      a.index_checkpoint = index;
      a.owed_wire        = 0;
   } );
   if (owed == 0) return;

   stats statstable( get_self() );
   const auto st = statstable.get( key, "shadow symbol does not exist" );
   yieldidxs indexes( get_self() );
   indexes.modify( name{}, key, [&]( auto& y ) {
      check( y.pot >= owed, "pot underfunded" );
      y.pot -= owed;
   } );
   action( permission_level{ get_self(), "active"_n }, st.wire_contract, "transfer"_n,
           std::make_tuple( get_self(), holder, asset{ int64_t(owed), st.wire_symbol }, string("") ) ).send();
}

u128 shadowtoken::current_index(symbol_code sym) const {
   yieldidxs indexes( get_self() );
   const auto idx = indexes.try_get( opp::shadow::symbol_key{ sym.raw() } );
   return idx ? idx->index : 0;
}

void shadowtoken::settle_and_adjust(name owner, const asset& delta) {
   accounts holdings( get_self(), owner.value );
   const opp::shadow::symbol_key key{ delta.symbol.code().raw() };
   const u128 index = current_index( delta.symbol.code() );
   const auto row = holdings.try_get( key );
   if (!row) {
      check( delta.amount >= 0, "no balance object found" );
      holdings.emplace( get_self(), key, opp::shadow::account{ delta, index, 0 } );
      return;
   }
   opp::shadow::account updated = *row;
   updated.owed_wire        = opp::shadow::owed( updated, index );   // settle before mutate
   updated.index_checkpoint = index;
   updated.balance         += delta;
   check( updated.balance.amount >= 0, "overdrawn balance" );
   holdings.modify( name{}, key, [&]( auto& a ) { a = updated; } );
}
