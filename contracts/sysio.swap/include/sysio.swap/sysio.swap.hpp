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
#include <sysio.opp.common/shadow_yield.hpp>
#include <algorithm>
#include <cmath>
#include <optional>

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
         /// Deployment configuration, required before anything else works. The fee
         /// authority is the account whose signature `changefee` requires for every
         /// pair that did not name its own at creation; governance executes approved
         /// proposals as `sysio`, so deployment sets it to `sysio`. The system token
         /// (WIRE) is the second leg of every pair, which is what lets a transfer be
         /// recognised without an index: a token is accepted if it is the system token
         /// or the first leg of the one pair it can form with it. Requires the
         /// contract's own authority.
         [[sysio::action]] void setconfig(name fee_authority, extended_symbol system_token);
         /// Create a pair, minting sqrt(pool1 * pool2) LP shares. `initial_pool2` must
         /// be in the system token; `initial_pool1` is the pair's own token, and since
         /// pairs are unique there is exactly one pair per such token. `initial_fee`
         /// may be anything in [0, MAX_FEE]. An empty `fee_authority` adopts the
         /// configured one; a name overrides it for this pair. `locked_shares` (in the
         /// new symbol, below the minted amount) are held by no account and can never
         /// be redeemed: they keep the pool from ever being emptied and bound how far
         /// the value of one share can be pushed. Seed-time attacks victimise the
         /// creator, so the size of the lock is the creator's call; zero is allowed.
         /// `yield_leg`, when set, must be the first leg and names it as a shadow
         /// token, making the pair a yield pool: the pool absorbs the WIRE yield
         /// distributed on the shadow it holds, and sells queued yield shadow through
         /// itself. Empty makes a plain pool. Both seeds must already be on deposit.
         /// The system token's always can be; the first leg has no pair yet, so its
         /// seed transfer must also carry this contract's authority.
         [[sysio::action]] void inittoken(name user, symbol new_symbol,
           extended_asset initial_pool1, extended_asset initial_pool2,
           int initial_fee, name fee_authority, asset locked_shares,
           std::optional<extended_symbol> yield_leg);
         /// Set a yield pool's tick parameters (fee authority): the horizon over which
         /// its queued yield is meant to sell, and the hard ceiling on one clip as basis
         /// points of the pool's shadow side. Both must be nonzero before tickyield runs.
         [[sysio::action]] void setyield(symbol_code pair_token,
           uint32_t conversion_horizon_sec, uint32_t depth_cap_bps);
         /// Settle the WIRE yield a yield pool is owed on the shadow it holds into the
         /// pool's other leg, minting nothing: the pool is credited now and the token's
         /// `claim` delivers the same amount in the same transaction (a mismatch fails
         /// it). Runs implicitly before every mint and burn; this call lets anyone
         /// settle between them. No authorization is required.
         [[sysio::action]] void accrueyield(symbol_code pair_token);
         /// Announce that `from` is about to transfer exactly `quantity` of the pair's
         /// shadow to this contract as queued yield for the pool: the transfer, when it
         /// lands, fills the pair's reservoir instead of `from`'s deposit. One
         /// announcement per account is pending at a time; a new one replaces it, and
         /// while one is pending any other transfer from `from` is refused. Typed and
         /// memo-free, so a contract can do both steps inline in one transaction.
         /// Requires `from`'s authority.
         [[sysio::action]] void fundyield(name from, symbol_code pair_token, asset quantity);
         /// Sell one clip of a yield pool's reservoir through the pool and hand the
         /// proceeds to the shadow token's holders. The clip is the reservoir's share
         /// of the horizon elapsed since the last tick, rounded up, capped by
         /// `depth_cap_bps` of the pool's shadow side and by what is queued; it is sold
         /// at the pool's own curve and fee, after the pool's owed yield has been
         /// settled, and the proceeds go out through the token's `addyield`, so the
         /// pool takes its own share back on the next accrual. Nothing queued, or no
         /// time elapsed, makes it a no-op. Requires `setyield` to have run. No
         /// authorization is required; a crank runs it every block.
         [[sysio::action]] void tickyield(symbol_code pair_token);
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

         /// A pending yield payout: keyed by the shadow contract that owes it, which is
         /// the `from` of the transfer that will settle it.
         struct contract_key {
            name contract;
            SYSLIB_SERIALIZE(contract_key, (contract))
         };

         /// A pending yield funding: keyed by the account that announced it, which is
         /// the `from` of the transfer that will fill it.
         struct funder_key {
            name funder;
            SYSLIB_SERIALIZE(funder_key, (funder))
         };

         // --- Rows ---

         /// Contract-wide configuration, set on deployment by `setconfig`.
         struct [[sysio::table("swapconfig")]] swap_config {
            name            fee_authority;
            extended_symbol system_token;   ///< the second leg of every pair
            SYSLIB_SERIALIZE(swap_config, (fee_authority)(system_token))
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
            std::optional<extended_symbol> yield_leg;   ///< the shadow leg of a yield pool; empty for a plain pool
            uint32_t       conversion_horizon_sec = 0;  ///< H: the reservoir is meant to sell over this long
            uint32_t       depth_cap_bps          = 0;  ///< hard ceiling on one clip, bps of the pool's shadow side
            time_point     last_tick{};                 ///< elapsed-time base of the clip formula: the last tick that
                                                        ///< sold, the last setyield, or when the reservoir last
                                                        ///< went from empty to funded, whichever is latest
            SYSLIB_SERIALIZE(currency_stats, (supply)(max_supply)(issuer)(pool1)(pool2)(fee)(fee_authority)
                                             (locked_shares)(yield_leg)(conversion_horizon_sec)(depth_cap_bps)(last_tick))
         };

         /// A yield payout the contract has claimed and credited to `pair` but not yet
         /// received. `quantity` is what the shadow contract's `claim` must deliver,
         /// computed from the token's public state before the call; the transfer that
         /// delivers it is matched against this row and the row erased. A row that
         /// outlives its transaction means the token paid something else, and blocks
         /// every further accrual through that contract until it is understood.
         struct [[sysio::table("yieldpayouts")]] payout_receipt {
            symbol_code    pair;
            extended_asset quantity;
            SYSLIB_SERIALIZE(payout_receipt, (pair)(quantity))
         };

         /// A yield funding announced by `fundyield` and not yet delivered: the
         /// transfer from the funder that matches `quantity` fills `pair`'s reservoir
         /// and erases the row.
         struct [[sysio::table("yieldfunds")]] fund_receipt {
            symbol_code    pair;
            extended_asset quantity;
            SYSLIB_SERIALIZE(fund_receipt, (pair)(quantity))
         };

         /// A yield pool's reservoir: the shadow queued to be sold through the pool,
         /// held by the contract but in no pool and no deposit. Its own row rather
         /// than a deposit row under a synthetic owner, so no account name can ever
         /// alias it. Exists for yield pools only, from creation.
         struct [[sysio::table("reservoirs")]] reservoir {
            extended_asset balance;
            SYSLIB_SERIALIZE(reservoir, (balance))
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
         using yieldpayouts = kv::table<"yieldpayouts"_n, contract_key, payout_receipt>;
         using yieldfunds   = kv::table<"yieldfunds"_n,   funder_key,   fund_receipt>;
         using reservoirs   = kv::table<"reservoirs"_n,   pair_key,     reservoir>;
         // (The shadow token's own tables are read through aliases local to the
         // implementation file: an alias declared here would make the ABI generator
         // list them as this contract's.)

         /// The deployment configuration, or a check failure before `setconfig` has run.
         swap_config configured() const;
         /// Refuse a transfer of anything this contract does not trade: `token` must be
         /// the system token or the first leg of the pair it forms with the system
         /// token (one lookup in the pair index, no table of its own). A first leg's
         /// seed lands before its pair exists, so a transfer carrying this contract's
         /// authority, the authority that creates pairs, is accepted regardless.
         void require_deposit_token(const extended_symbol& token) const;
         /// The deposit-row key of an extended symbol.
         static extended_symbol_key key_of(const extended_symbol& ext_symbol);
         /// The uniqueness-row key of a pair, in canonical leg order.
         static pair_identity_key identity_of(const extended_symbol& a, const extended_symbol& b);
         /// The pair's shadow leg, or a check failure on a plain pool: every yield path starts here.
         static const extended_symbol& require_yield_leg(const currency_stats& token);
         /// The pool holding `leg`; `leg` must be one of the pair's legs.
         static const extended_asset& pool_of(const currency_stats& token, const extended_symbol& leg);
         /// The pool holding the leg that is NOT `leg`; `leg` must be one of the pair's legs.
         static const extended_asset& other_pool(const currency_stats& token, const extended_symbol& leg);
         /// Restart the horizon clock of the pair at `key`: the next clip is measured
         /// from now.
         void restart_tick_clock(const pair_key& key);
         /// The WIRE this contract is owed right now on the shadow it holds, from the
         /// token's public state: `shadow::owed` over the contract's row and the current
         /// index. Zero when the token has never distributed (no index row), so a
         /// plain token in a yield leg reads as owing nothing.
         uint64_t owed_yield(const extended_symbol& shadow) const;
         /// Settle the owed yield of a yield pool into its other leg without minting:
         /// credit the pool, record the receipt keyed by the shadow contract, and call
         /// the token's `claim` inline; the transfer it sends lands in `ontransfer`
         /// against the receipt. A plain pool, or nothing owed, changes nothing.
         /// Returns the pair row as it now stands, for a caller that goes on to price.
         currency_stats accrue(const pair_key& key, const currency_stats& token);

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
