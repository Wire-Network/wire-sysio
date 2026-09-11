#pragma once

#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>
#include <sysio/system.hpp>
#include <sysio/print.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_global.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <sysio.opp.common/twap.hpp>
#include <algorithm>
#include <cmath>

using namespace sysio;
using namespace std;

namespace sysio {

   class [[sysio::contract("sysio.swap")]] swap : public contract {
      public:
         const int64_t MAX = sysio::asset::max_amount;
         const int64_t INIT_MAX = 1000000000000000;  // 10^15
         const int ADD_LIQUIDITY_FEE = 1;
         /// Fees are expressed in units of 1/FEE_DENOMINATOR of the traded amount.
         static constexpr int FEE_DENOMINATOR = 10000;
         /// Upper bound accepted by changefee: strictly below 100%, so a positive quote
         /// always nets at least one unit (amm::split_wire_fee reports net 0 at 100%).
         static constexpr int MAX_FEE = FEE_DENOMINATOR - 1;
         /// Both pool sides carry the same weight: every pair is a plain constant-product
         /// (x*y=k) pool, which is the exact-integer path of amm::out_given_in.
         static constexpr uint64_t CP_WEIGHT_BPS = sysio::opp::amm::WEIGHT_TOTAL_BPS / 2;
         /// The pair fee stays in the pool; no underwriter takes a share of it.
         static constexpr uint32_t NO_UNDERWRITER_SHARE_BPS = 0;
         /// Least fee, in units of the output token, a nonzero fee rate collects on a
         /// nonzero quote. Without it the floored fee is zero on any quote below
         /// FEE_DENOMINATOR/fee units, a window that is a thousand whole tokens for a
         /// zero-precision symbol.
         static constexpr uint64_t MIN_SWAP_FEE = 1;

         using contract::contract;
         /// Set the contract-wide fee authority: the account whose signature `changefee`
         /// requires for every pair that did not name its own at creation. Governance
         /// executes approved proposals as `sysio`, so deployment sets it to `sysio`.
         /// Requires the contract's own authority.
         [[sysio::action]] void setconfig(name fee_authority);
         /// Create a pair, minting sqrt(pool1 * pool2) LP shares. `initial_fee` may be
         /// anything in [0, MAX_FEE]. An empty `fee_authority` adopts the configured one;
         /// a name overrides it for this pair. `locked_shares` (in the new symbol, below
         /// the minted amount) are held by no account and can never be redeemed: they
         /// keep the pool from ever being emptied and bound how far the value of one
         /// share can be pushed. Seed-time attacks victimise the creator, so the size
         /// of the lock is the creator's call; zero is allowed.
         [[sysio::action]] void inittoken(name user, symbol new_symbol,
           extended_asset initial_pool1, extended_asset initial_pool2,
           int initial_fee, name fee_authority, asset locked_shares);
         [[sysio::on_notify("*::transfer")]] void ontransfer(name from, name to, asset quantity, string memo);
         [[sysio::action]] void openext( const name& user, const name& payer, const extended_symbol& ext_symbol);
         [[sysio::action]] void closeext ( const name& user, const name& to, const extended_symbol& ext_symbol, string memo);
         [[sysio::action]] void withdraw(name user, name to, extended_asset to_withdraw, string memo);
         [[sysio::action]] void addliquidity(name user, asset to_buy, asset max_asset1, asset max_asset2);
         [[sysio::action]] void remliquidity(name user, asset to_sell, asset min_asset1, asset min_asset2);
         [[sysio::action]] void exchange( name user, symbol_code pair_token, extended_asset ext_asset_in, asset min_expected );
         [[sysio::action]] void changefee(symbol_code pair_token, int newfee);
         /// Bring the pair's cumulative-price accumulators up to the current block
         /// time without trading, so a reader can take an up-to-date snapshot. No
         /// authorization is required; the caller pays only the CPU.
         [[sysio::action]] void sync(symbol_code pair_token);

         [[sysio::action]] void transfer(const name& from, const name& to,
           const asset& quantity, const string&  memo );
         [[sysio::action]] void open( const name& owner, const symbol& symbol, const name& ram_payer );
         [[sysio::action]] void close( const name& owner, const symbol& symbol );
         [[sysio::action]] void indexpair(name user, symbol evo_symbol); // This action is only temporarily useful

      private:

         // --- Keys ---

         /// A pair's rows in `stat` and `priceaccum`: keyed by the LP token's symbol code.
         struct pair_key {
            uint64_t symbol_code;
            SYSLIB_SERIALIZE(pair_key, (symbol_code))
         };

         /// An LP-token balance row, scoped by its owner: keyed by the symbol code.
         struct account_key {
            uint64_t symbol_code;
            SYSLIB_SERIALIZE(account_key, (symbol_code))
         };

         /// A deposit row, scoped by its owner: keyed by the deposited token's extended
         /// symbol, so a balance is one primary lookup and needs no surrogate id.
         struct extended_symbol_key {
            name     contract;
            uint64_t symbol;
            SYSLIB_SERIALIZE(extended_symbol_key, (contract)(symbol))
         };

         /// A pair's uniqueness row: keyed by its two legs, the lower (contract, symbol)
         /// first, so the same two tokens in either order resolve to one key.
         struct pair_identity_key {
            name     contract1;
            uint64_t symbol1;
            name     contract2;
            uint64_t symbol2;
            SYSLIB_SERIALIZE(pair_identity_key, (contract1)(symbol1)(contract2)(symbol2))
         };

         // --- Rows ---

         /// Contract-wide configuration, set on deployment by `setconfig`.
         struct [[sysio::table("swapconfig")]] swap_config {
            name fee_authority;
            SYSLIB_SERIALIZE(swap_config, (fee_authority))
         };

         struct [[sysio::table("accounts")]] account {
            asset balance;
            SYSLIB_SERIALIZE(account, (balance))
         };

         struct [[sysio::table("evodexacnts")]] evodex_account {
            extended_asset balance;
            SYSLIB_SERIALIZE(evodex_account, (balance))
         };

         struct [[sysio::table("stat")]] currency_stats {
            asset          supply;
            asset          max_supply;
            name           issuer;
            extended_asset pool1;
            extended_asset pool2;
            int            fee;
            name           fee_authority;   ///< whose signature changefee requires for this pair
            asset          locked_shares;   ///< part of `supply` held by no account, never redeemable
            SYSLIB_SERIALIZE(currency_stats, (supply)(max_supply)(issuer)(pool1)(pool2)(fee)(fee_authority)(locked_shares))
         };

         struct [[sysio::table("evoindex")]] pair_index {
            symbol evo_symbol;
            SYSLIB_SERIALIZE(pair_index, (evo_symbol))
         };

         /// Cumulative-price accumulators for a pair (sysio.opp.common/twap.hpp).
         /// `price1` sums the Q64.64 price of one unit of pool1 in units of pool2,
         /// times elapsed microseconds; `price2` the reverse. Both advance, at the
         /// spot price that held since `last_update`, immediately before the pools
         /// change and on `sync`. A reader snapshots the row at t0 and computes
         /// `twap::average_price(twap::difference(now, snapshot), t - t0)`.
         struct [[sysio::table("priceaccum")]] price_accumulator {
            sysio::opp::twap::cumulative_price price1;
            sysio::opp::twap::cumulative_price price2;
            time_point                         last_update;
            SYSLIB_SERIALIZE(price_accumulator, (price1)(price2)(last_update))
         };

         // --- Tables ---

         using swapconfig_t = kv::global<"swapconfig"_n, swap_config>;
         using accounts    = kv::scoped_table<"accounts"_n,    account_key,         account>;
         using evodexacnts = kv::scoped_table<"evodexacnts"_n, extended_symbol_key, evodex_account>;
         using stats       = kv::table<"stat"_n,       pair_key,          currency_stats>;
         using evoindexes  = kv::table<"evoindex"_n,   pair_identity_key, pair_index>;
         using priceaccums = kv::table<"priceaccum"_n, pair_key,          price_accumulator>;

         /// The deposit-row key of an extended symbol.
         static extended_symbol_key key_of(const extended_symbol& ext_symbol);
         /// The uniqueness-row key of a pair, in canonical leg order.
         static pair_identity_key identity_of(const extended_symbol& a, const extended_symbol& b);

         void add_signed_ext_balance( const name& owner, const extended_asset& value );
         void add_signed_liq(name user, asset to_buy, bool is_buying, asset max_asset1, asset max_asset2);
         void memoexchange(name user, extended_asset ext_asset_in, string_view details);
         /// Settle an exact-input swap of `paying` through the pair `evo_token`: the
         /// output is the constant-product quote (amm::out_given_in at equal weights)
         /// net of the pair's fee (amm::split_wire_fee, at least MIN_SWAP_FEE when the
         /// rate and the quote are both nonzero), and must reach `min_expected`.
         /// Moves the pools and returns the extended asset the user receives.
         extended_asset process_exch(symbol_code evo_token, extended_asset paying, asset min_expected);
         /// Liquidity pricing: `x * y / z` rounded the pool's way -- up when `x > 0`
         /// (a leg the user pays), down when `x < 0` (a leg the user receives) -- plus
         /// `fee` (in 1/FEE_DENOMINATOR units) of that amount, rounded up.
         int64_t compute(int64_t x, int64_t y, int64_t z, int fee);
         /// The liquidity fee: `fee`/FEE_DENOMINATOR of `amount`, rounded up so a
         /// non-zero amount never pays a zero fee. `amount` must be nonnegative.
         static int128_t ceil_fee(int128_t amount, int fee);
         /// Advance the pair's accumulators by `token`'s current spot prices times
         /// the time since the last update. Must run BEFORE the pools change, so the
         /// interval is weighted at the price that actually held during it.
         void update_price_accumulators(const currency_stats& token);
         asset string_to_asset(string input);
         void placeindex(name user, symbol evo_symbol, extended_asset pool1, extended_asset pool2 );
         void add_balance( const name& owner, const asset& value, const name& ram_payer );
         void sub_balance( const name& owner, const asset& value );
   };
}
