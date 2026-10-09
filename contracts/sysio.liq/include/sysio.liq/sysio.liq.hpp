#pragma once
/**
 * @file sysio.liq.hpp
 * @brief sysio.liq: the depot's shadow token for syndicated liq (LIQETH, LIQSOL).
 *
 * One shadow symbol per outpost liq token, minted 1:1 against liq the outpost
 * holds in its syndicated pool and burned when a holder de-syndicates. Holders
 * earn WIRE yield through the cumulative index of
 * sysio.opp.common/shadow_yield.hpp: every balance move settles the row first,
 * and `claim` pays what `shadow::owed` says.
 *
 * Supply is sysio.synd's to grow and shrink: every inbound SYNDICATE_LIQ is held by
 * sysio.synd, which mints it into its own holder row with `mint`, and every
 * de-syndication and pre-launch import goes through sysio.synd too, which burns with
 * `burn` and mints the replayed positions; a LIQ_YIELD report it releases lands through
 * `mintyield` in a pending balance outside supply that the permissionless `queueyield`
 * hands to sysio.swap's reservoir. Return recovery is request-keyed in sysio.synd;
 * `recredit` remains exceptional privileged supply repair.
 * Yield intake: sysio.swap's tick sells reservoir shadow and pays the proceeds in
 * through `addyield`, which distributes only the supplied WIRE proceeds.
 * Launch: `regliqpool` seeds the swap's yield pool inside the epoch-0 bootstrap window.
 *
 * Emergency stop: while the `sysio.andon` cord is pulled, `transfer` to anything but a
 * custody contract (sysio.bond, sysio.synd, sysio.opreg), `claim` by anyone but a custody
 * contract, and `queueyield` are refused; `mint`, `burn` and transfers into custody run.
 *
 * Privileged (roa::setsyscode): holder rows bill the `sysio` RAM pool, and every
 * inline action carries the authority it needs, so no `sysio.code` grant exists.
 */

#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/shadow_yield.hpp>
#include <sysio/slug_name.hpp>
#include <sysio.opp.common/wire_asset.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace sysio {

   using std::string;

   class [[sysio::contract("sysio.liq")]] liq : public contract {
   public:
      using contract::contract;

      // Well-known accounts.
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name TOKENS_ACCOUNT = "sysio.tokens"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name SWAP_ACCOUNT   = "sysio.swap"_n;
      /// The syndication contract: the only account that mints, burns and reports yield.
      static constexpr name SYND_ACCOUNT   = "sysio.synd"_n;
      /// Governance executes approved proposals as `sysio`; it is also the T5
      /// treasury the bootstrap drains and the holder of the protocol's pool shares.
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      /// The yield asset: every pot holds supplied WIRE proceeds.
      static constexpr symbol WIRE_SYM = opp::wire::asset_symbol;

      // -----------------------------------------------------------------------
      //  Deployment and governance
      // -----------------------------------------------------------------------

      /// Register the shadow symbol `sym` for the liq token `token_code` of the
      /// outpost `chain_code`. Both must be active registry rows, the token a
      /// TOKEN_KIND_LIQ whose depot precision is `sym`'s; one shadow per token.
      /// A precision below two decimals is accepted, but sysio.bond cannot bond
      /// such a token, so no syndication of it can ever be underwritten and
      /// every envelope of it waits in sysio.synd until `sysio.synd::dropenv`.
      /// Requires this contract's authority.
      [[sysio::action]] void create(symbol sym, sysio::slug_name chain_code, sysio::slug_name token_code);

      /// Privileged supply repair requiring this contract's authority; never recover a return
      /// tracked in sysio.synd::returns, which must use request-keyed refundreturn instead.
      [[sysio::action]] void recredit(name holder, asset quantity);

      // -----------------------------------------------------------------------
      //  Supply (sysio.synd only)
      // -----------------------------------------------------------------------

      /// Mint `amount` base units of `token_code`'s shadow to `to`, growing the
      /// supply. Refused for a token with no shadow, a zero amount or an amount
      /// past the symbol's headroom (the asset range net of the supply and of the
      /// pending yield). Auth=sysio.synd.
      [[sysio::action]] void mint(name to, sysio::slug_name token_code, uint64_t amount);

      /// Burn `amount` base units of `token_code`'s shadow out of sysio.synd's own
      /// holder row, settled first, shrinking the supply. Refused for a token with
      /// no shadow, a zero amount or more than the row holds. Auth=sysio.synd.
      [[sysio::action]] void burn(sysio::slug_name token_code, uint64_t amount);

      /// LIQ_YIELD released by sysio.synd: `amount` of yield the outpost
      /// `chain_code` claimed for its syndicated pool. Held in the symbol's
      /// pending balance, outside supply, until `queueyield` moves it. Replay is
      /// sysio.synd's to refuse. Never throws: an unknown token, a token of
      /// another chain or an out-of-range amount is dropped with a diagnostic.
      /// Auth=sysio.synd.
      [[sysio::action]] void mintyield(sysio::slug_name chain_code, sysio::slug_name token_code, uint64_t amount);

      /// Credit `holder` the `wire` WIRE sysio.synd holds for it -- the yield a held or parked position
      /// banked -- as owed yield of its `sym` row, claimable with `claim` like any yield: the row is
      /// settled first (created at balance 0 when there is none), `wire` joins its banked `owed_wire`
      /// and the symbol's pot, and the WIRE moves from sysio.synd into this contract by an inline
      /// `sysio.token::transfer` in the same action. Nothing is pushed to `holder` and it is not
      /// notified, so a holder whose transfer handler would refuse cannot fail the caller. Refused for
      /// a missing account, a zero amount, an unknown symbol, or a banked total past the asset range.
      /// Auth=sysio.synd.
      [[sysio::action]] void creditowed(name holder, symbol_code sym, uint64_t wire);

      // -----------------------------------------------------------------------
      //  Cranks
      // -----------------------------------------------------------------------

      /// Hand `sym`'s pending yield to sysio.swap's reservoir for its pool: mint
      /// it to this contract, announce it with `fundyield` and transfer it, all
      /// in one transaction. Permissionless; a no-op with nothing pending. Refused
      /// while the `sysio.andon` cord is pulled.
      [[sysio::action]] void queueyield(symbol_code sym);

      // -----------------------------------------------------------------------
      //  The token
      // -----------------------------------------------------------------------

      /// Move `quantity` of a shadow from `from` to `to`, settling both rows first. While the
      /// `sysio.andon` cord is pulled, refused unless `to` is a custody contract. Auth=from.
      [[sysio::action]] void transfer(name from, name to, asset quantity, string memo);

      /// Credit an existing custody obligation without notifying either account. Only sysio.synd
      /// and sysio.bond may debit their own balance. Settles both yield positions, preserves supply,
      /// and requires the Andon cord clear, including self-settlement.
      [[sysio::action]] void settle(name custodian, name beneficiary, asset quantity);
      [[sysio::action]] void open(name owner, symbol symbol, name ram_payer);
      /// Erase `owner`'s empty row for `symbol`. Refused while the row is still
      /// owed yield, so closing never discards WIRE.
      [[sysio::action]] void close(name owner, symbol symbol);
      /// Pay `holder` the WIRE its row for `sym` is owed and settle the row.
      /// Holder's authority; sends nothing when nothing is owed. While the
      /// `sysio.andon` cord is pulled, refused unless `holder` is a custody contract.
      [[sysio::action]] void claim(name holder, symbol_code sym);
      /// Distribute `quantity` WIRE to `target`'s holders: the index advances by
      /// quantity / supply with the remainder carried and the WIRE is pulled from
      /// `from` by inline transfer. No treasury bonus is added.
      [[sysio::action]] void addyield(name from, asset quantity, symbol_code target);

      // -----------------------------------------------------------------------
      //  Launch ingestion (privileged caller, epoch-0 bootstrap window)
      // -----------------------------------------------------------------------

      /// Seed the swap's yield pool for `token_code`'s shadow: mint the LCO liq
      /// (`initial_chain_amount`, already in outpost custody) to `sysio`, deposit
      /// it with `initial_wire_amount` WIRE from the T5 dex earmark into
      /// sysio.swap, create the pair with `sysio` as its fee authority and the
      /// shadow as its yield leg, and set the tick parameters. `fee` is in
      /// 1/10000 of the traded amount, `locked_shares` in `pair_symbol`.
      [[sysio::action]] void regliqpool(sysio::slug_name chain_code, sysio::slug_name token_code, symbol pair_symbol,
                                        uint64_t initial_chain_amount, uint64_t initial_wire_amount, int32_t fee,
                                        int64_t locked_shares, uint32_t conversion_horizon_sec,
                                        uint32_t depth_cap_bps, int64_t clip_floor);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      using symbol_key = opp::shadow::symbol_key;

      /// One shadow symbol: its supply and the registry rows it mirrors.
      ///
      /// LAYOUT IS SHARED STATE. Other contracts deserialize this row on consensus paths:
      /// `sysio.opreg` resolves a depot-native collateral token through the `bytoken` index on its
      /// never-throw remit paths (withdraw flush, lock release, termination) as well as its signed
      /// actions, and `sysio.chalg` prices shadow collateral from it. Changing the layout means
      /// `sysio.liq` and every one of those readers redeploy together.
      struct [[sysio::table("stat")]] currency_stats {
         asset            supply;
         sysio::slug_name chain_code;    ///< the outpost whose liq this shadow mirrors
         sysio::slug_name token_code;    ///< that outpost's liq token in sysio.tokens
         symbol_code      pair_symbol;   ///< the swap's pair token of this shadow's yield pool; empty until regliqpool

         uint64_t by_token_code() const { return token_code.value; }

         SYSLIB_SERIALIZE(currency_stats, (supply)(chain_code)(token_code)(pair_symbol))
      };

      /// Secondary index of `stat` keyed on each shadow symbol's registry `token_code` -- the one
      /// declaration of its name; `find_stat_by_token` is the one lookup through it.
      static constexpr name STAT_BY_TOKEN_INDEX = "bytoken"_n;

      using stats = kv::table<"stat"_n, symbol_key, currency_stats,
         kv::index<STAT_BY_TOKEN_INDEX, const_mem_fun<currency_stats, uint64_t, &currency_stats::by_token_code>>>;

      /// The `stat` row of the shadow symbol registered under registry `token_code` in the
      /// `sysio.liq` deployed at `liq_account`, or `std::nullopt` when none is. Never throws. The ONE
      /// stat-by-token lookup: `sysio.liq` itself, `sysio.opreg`'s depot-native token resolver
      /// (including its never-throw remit paths) and `sysio.chalg`'s collateral pricing all use it.
      static std::optional<currency_stats> find_stat_by_token(name liq_account, sysio::slug_name token_code) {
         stats statstable(liq_account);
         auto  by_token = statstable.get_index<STAT_BY_TOKEN_INDEX>();
         auto  it       = by_token.find(token_code.value);
         if (it == by_token.end()) return std::nullopt;
         return *it;
      }

      /// Holder rows (scope = holder, key = symbol code) and the per-symbol
      /// index, laid out by sysio.opp.common/shadow_yield.hpp.
      using accounts  = opp::shadow::accounts_table;
      using yieldidxs = opp::shadow::yield_index_table;

      /// Yield minted by LIQ_YIELD and not yet queued. Outside supply, so it
      /// earns nothing while it waits and nothing is stranded when it leaves;
      /// reserved against the asset range beside supply, so queueing always fits.
      struct [[sysio::table("liqpending")]] pending_yield {
         asset quantity;
         SYSLIB_SERIALIZE(pending_yield, (quantity))
      };

      using liqpendings = kv::table<"liqpending"_n, symbol_key, pending_yield>;

      /// Base units of shadow the depot has committed for `st`, a `stat` row of the
      /// `sysio.liq` deployed at `liq_account`: its supply plus the yield parked in
      /// `liqpending`, which is committed once parked because the permissionless
      /// `queueyield` mints it into supply. This is the depot's outstanding shadow that an
      /// outpost's custody of the token must cover. Never throws: supply and pending are
      /// each non-negative and their sum stays within the asset range (`headroom_of`).
      static uint64_t outstanding_of(name liq_account, const currency_stats& st) {
         liqpendings    pendings(liq_account);
         const auto     pending = pendings.try_get(symbol_key{ st.supply.symbol.code().raw() });
         const uint64_t parked  = pending ? static_cast<uint64_t>(pending->quantity.amount) : 0;
         return static_cast<uint64_t>(st.supply.amount) + parked;
      }

      /// Base units the supply of `st`, a `stat` row of the `sysio.liq` deployed at
      /// `liq_account`, can still grow by: the asset range net of `outstanding_of`, the
      /// supply and the yield parked in `liqpending`, which mints when queued. Never throws.
      /// The ONE headroom computation: `sysio.liq`'s own mints and `sysio.synd`'s intake,
      /// which must know a `mint` will fit before it sends one, both use it.
      static uint64_t headroom_of(name liq_account, const currency_stats& st) {
         const uint64_t range       = static_cast<uint64_t>(asset::max_amount);
         const uint64_t outstanding = outstanding_of(liq_account, st);
         return outstanding < range ? range - outstanding : 0;
      }

   private:
      using ChainKind = opp::types::ChainKind;
      using u128      = opp::shadow::u128;

      /// The stat row of `sym`, or a check failure.
      currency_stats stat_of(symbol_code sym) const;
      /// The stat row of the shadow registered under `token_code`, or a check failure.
      currency_stats stat_by_token(sysio::slug_name token_code) const;
      /// The chain family of the registered outpost `chain_code`, or a check failure.
      ChainKind kind_of_chain(sysio::slug_name chain_code) const;
      /// `sym`'s index now; zero before the first distribution.
      u128 current_index(symbol_code sym) const;

      /// The one funnel every balance move goes through: settle `row` at `index`,
      /// stamp it, then apply `delta`. Nothing else writes `balance`.
      static void settle_and_adjust(opp::shadow::account& row, u128 index, const asset& delta);
      /// Apply `delta` to `owner`'s row for its symbol through the funnel. A row
      /// created here is stamped at the current index, so no history is credited.
      void adjust_account(name owner, const asset& delta, name payer);
      /// Grow `sym`'s supply by `quantity`; false (and no change) past its headroom.
      bool grow_supply(symbol_code sym, uint64_t quantity);
      /// Advance `sym`'s index by `quantity` WIRE over its supply, carrying the
      /// remainder, and grow its pot by the same.
      void distribute(symbol_code sym, uint64_t quantity);
      /// The `mintyield` preamble: the symbol for `token_code` on `chain_code`, with
      /// `amount` in range and room in the supply, or nullopt after a diagnostic.
      std::optional<currency_stats> resolve_inbound(const char* path, sysio::slug_name chain_code,
                                                    sysio::slug_name token_code, uint64_t amount);
   };

} // namespace sysio
