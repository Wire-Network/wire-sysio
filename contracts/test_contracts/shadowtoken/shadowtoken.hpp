#pragma once
/**
 * @file shadowtoken.hpp
 * @brief Test stand-in for the shadow token: sysio.token's shape plus the yield
 *        distribution of sysio.opp.common/shadow_yield.hpp, settled in every
 *        balance move.
 *
 * What sysio.swap depends on is the read layout in that shared header and the
 * two typed calls below; this contract implements the minimum that exercises
 * them end to end, including the rules the design plan marks as mandatory:
 * settle before mutate, a new holder stamped at the current index, and the
 * truncation remainder carried rather than dropped.
 */

#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio.opp.common/shadow_yield.hpp>
#include <string>

using namespace sysio;
using std::string;

class [[sysio::contract("shadowtoken")]] shadowtoken : public contract {
public:
   using contract::contract;

   /// Register a shadow symbol and the WIRE token its yield is paid in.
   [[sysio::action]] void create(name issuer, asset maximum_supply, name wire_contract, symbol wire_symbol);
   [[sysio::action]] void issue(name to, asset quantity, string memo);
   [[sysio::action]] void transfer(name from, name to, asset quantity, string memo);
   /// Move `quantity` WIRE from `from` (its own authority, by inline transfer)
   /// into `target`'s pot, advancing the index by quantity / supply and
   /// carrying the remainder.
   [[sysio::action]] void addyield(name from, asset quantity, symbol_code target);
   /// Settle `holder`'s row for `sym` and pay what it is owed from the
   /// contract's own WIRE, then zero it. Holder's authority.
   [[sysio::action]] void claim(name holder, symbol_code sym);

   static_assert( "addyield"_n == opp::shadow::ADDYIELD_ACTION && "claim"_n == opp::shadow::CLAIM_ACTION,
                  "the action names above are the ones holders call through shadow_yield.hpp" );

private:
   struct [[sysio::table("stat")]] currency_stats {
      asset  supply;
      asset  max_supply;
      name   issuer;
      name   wire_contract;
      symbol wire_symbol;
      SYSLIB_SERIALIZE(currency_stats, (supply)(max_supply)(issuer)(wire_contract)(wire_symbol))
   };

   using stats     = kv::table<"stat"_n, opp::shadow::symbol_key, currency_stats>;
   using accounts  = kv::scoped_table<"accounts"_n, opp::shadow::symbol_key, opp::shadow::account>;
   using yieldidxs = kv::table<"yieldidx"_n, opp::shadow::symbol_key, opp::shadow::yield_index>;

   opp::shadow::u128 current_index(symbol_code sym) const;
   /// Settle `owner`'s row at the current index, then apply `delta` to its
   /// balance. A holder without a row is stamped at the current index, so no
   /// history is credited to it.
   void settle_and_adjust(name owner, const asset& delta);
};
