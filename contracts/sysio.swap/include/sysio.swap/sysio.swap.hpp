#pragma once

#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>
#include <sysio/system.hpp>
#include <sysio/print.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <cmath>

using namespace sysio;
using namespace std;

namespace sysio {

   class [[sysio::contract("sysio.swap")]] swap : public contract {
      public:
         const int64_t MAX = sysio::asset::max_amount;
         const int64_t INIT_MAX = 1000000000000000;  // 10^15 
         const int ADD_LIQUIDITY_FEE = 1;
         const int DEFAULT_FEE = 10;
         /// Fees are expressed in units of 1/FEE_DENOMINATOR of the traded amount.
         static constexpr int FEE_DENOMINATOR = 10000;
         /// Upper bound accepted by changefee. compute() checks its result against the
         /// int64 range BEFORE adding the fee, so a fee at or above 100% could push the
         /// final amount past that range; below 100% the sum stays within int64.
         static constexpr int MAX_FEE = FEE_DENOMINATOR - 1;
         /// Both pool sides carry the same weight: every pair is a plain constant-product
         /// (x*y=k) pool, which is the exact-integer path of amm::out_given_in.
         static constexpr uint64_t CP_WEIGHT_BPS = sysio::opp::amm::WEIGHT_TOTAL_BPS / 2;

         using contract::contract;
         [[sysio::action]] void inittoken(name user, symbol new_symbol, 
           extended_asset initial_pool1, extended_asset initial_pool2, 
           int initial_fee, name fee_contract);
         [[sysio::on_notify("*::transfer")]] void ontransfer(name from, name to, asset quantity, string memo);
         [[sysio::action]] void openext( const name& user, const name& payer, const extended_symbol& ext_symbol);
         [[sysio::action]] void closeext ( const name& user, const name& to, const extended_symbol& ext_symbol, string memo);
         [[sysio::action]] void withdraw(name user, name to, extended_asset to_withdraw, string memo);
         [[sysio::action]] void addliquidity(name user, asset to_buy, asset max_asset1, asset max_asset2);
         [[sysio::action]] void remliquidity(name user, asset to_sell, asset min_asset1, asset min_asset2);
         [[sysio::action]] void exchange( name user, symbol_code pair_token, extended_asset ext_asset_in, asset min_expected );
         [[sysio::action]] void changefee(symbol_code pair_token, int newfee);

         [[sysio::action]] void transfer(const name& from, const name& to, 
           const asset& quantity, const string&  memo );
         [[sysio::action]] void open( const name& owner, const symbol& symbol, const name& ram_payer );
         [[sysio::action]] void close( const name& owner, const symbol& symbol );
         [[sysio::action]] void indexpair(name user, symbol evo_symbol); // This action is only temporarily useful

      private:

         struct [[sysio::table]] account {
            asset    balance;
            uint64_t primary_key()const { return balance.symbol.code().raw(); }
         };

         struct [[sysio::table]] evodexaccount {
            extended_asset   balance;
            uint64_t id;
            uint64_t primary_key()const { return id; }
            uint128_t secondary_key()const { return 
              make128key(balance.contract.value, balance.quantity.symbol.raw() ); }
         };

         struct [[sysio::table]] currency_stats {
            asset    supply;
            asset    max_supply;
            name     issuer;
            extended_asset    pool1;
            extended_asset    pool2;
            int fee;
            name fee_contract;
            uint64_t primary_key()const { return supply.symbol.code().raw(); }
         };

         struct [[sysio::table]] index_struct{
            symbol evo_symbol;
            checksum256 id_256;
            uint64_t primary_key()const { return evo_symbol.code().raw(); }
            checksum256 secondary_key()const { return id_256; }
         };

         typedef sysio::multi_index< "evodexacnts"_n, evodexaccount,
         indexed_by<"extended"_n, const_mem_fun<evodexaccount, uint128_t, 
           &evodexaccount::secondary_key>> > evodexacnts;
         typedef sysio::multi_index< "stat"_n, currency_stats > stats;
         typedef sysio::multi_index< "evoindex"_n, index_struct,
         indexed_by<"extended"_n, const_mem_fun<index_struct, checksum256, 
           &index_struct::secondary_key>> > evoindexes;
         typedef sysio::multi_index< "accounts"_n, account > accounts;

         static uint128_t make128key(uint64_t a, uint64_t b);
         static checksum256 make256key(uint64_t a, uint64_t b, uint64_t c, uint64_t d);

         void add_signed_ext_balance( const name& owner, const extended_asset& value );
         void add_signed_liq(name user, asset to_buy, bool is_buying, asset max_asset1, asset max_asset2);
         void memoexchange(name user, extended_asset ext_asset_in, string_view details);
         /// Settle an exact-input swap of `paying` through the pair `evo_token`: the
         /// output is the constant-product quote (amm::out_given_in at equal weights)
         /// less the pair's fee, and must reach `min_expected`. Moves the pools and
         /// returns the extended asset the user receives.
         extended_asset process_exch(symbol_code evo_token, extended_asset paying, asset min_expected);
         /// Liquidity pricing: `x * y / z` rounded the pool's way -- up when `x > 0`
         /// (a leg the user pays), down when `x < 0` (a leg the user receives) -- plus
         /// `fee` (in 1/FEE_DENOMINATOR units) of that amount, rounded up.
         int64_t compute(int64_t x, int64_t y, int64_t z, int fee);
         /// `fee`/FEE_DENOMINATOR of `amount`, rounded up so a non-zero amount never
         /// pays a zero fee. `amount` must be nonnegative.
         static int128_t ceil_fee(int128_t amount, int fee);
         asset string_to_asset(string input);
         void placeindex(name user, symbol evo_symbol, extended_asset pool1, extended_asset pool2 );
         void add_balance( const name& owner, const asset& value, const name& ram_payer );
         void sub_balance( const name& owner, const asset& value );
   };
}