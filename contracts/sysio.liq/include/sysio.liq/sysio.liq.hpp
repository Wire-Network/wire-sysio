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
 * Inbound, dispatched by sysio.msgch and therefore never throwing:
 *   SYNDICATE_LIQ -> `mintsynd` for an AuthX-linked user, `park` for one without a link;
 *   LIQ_YIELD     -> `mintyield`, into a pending balance outside supply that the
 *                    permissionless `queueyield` hands to sysio.swap's reservoir.
 * Outbound: `desyndicate` burns and queues DESYNDICATE_LIQ; the burn is final.
 * Yield intake: sysio.swap's tick sells reservoir shadow and pays the proceeds in
 * through `addyield`, which also draws the kicker from T5 (sysio.system::fundclaim).
 * Launch: `regliqpool` seeds the swap's yield pool and `importsynd` / `importdone`
 * replay the pre-launch positions, all inside the epoch-0 bootstrap window.
 *
 * Privileged (roa::setsyscode): holder rows bill the `sysio` RAM pool, and every
 * inline action carries the authority it needs, so no `sysio.code` grant exists.
 */

#include <sysio/sysio.hpp>
#include <sysio/asset.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/shadow_yield.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/wire_asset.hpp>

#include <magic_enum/magic_enum.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sysio {

   using std::string;

   class [[sysio::contract("sysio.liq")]] liq : public contract {
   public:
      using contract::contract;

      // Well-known accounts.
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name TOKENS_ACCOUNT = "sysio.tokens"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name SWAP_ACCOUNT   = "sysio.swap"_n;
      /// Governance executes approved proposals as `sysio`; it is also the T5
      /// treasury the bootstrap drains and the holder of the protocol's pool shares.
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      /// The yield asset. The kicker arrives in it from sysio.system, so every
      /// pot is in WIRE and nothing else.
      static constexpr symbol WIRE_SYM = opp::wire::asset_symbol;

      /// The kicker at launch: 2% of every yield intake, drawn from T5.
      static constexpr uint32_t DEFAULT_KICKER_BPS = 200;

      // -----------------------------------------------------------------------
      //  Deployment and governance
      // -----------------------------------------------------------------------

      /// Register the shadow symbol `sym` for the liq token `token_code` of the
      /// outpost `chain_code`. Both must be active registry rows, the token a
      /// TOKEN_KIND_LIQ whose depot precision is `sym`'s; one shadow per token.
      /// Requires this contract's authority.
      [[sysio::action]] void create(symbol sym, sysio::slug_name chain_code, sysio::slug_name token_code);

      /// Set the kicker, in basis points of each yield intake, for the intakes
      /// from now on. Auth=sysio: council proposals execute as it.
      [[sysio::action]] void setkicker(uint32_t bps);

      /// Governance: mint `quantity` back to `holder` after its outpost refused
      /// a de-syndication (reconciled from the outpost log). Requires this
      /// contract's authority.
      [[sysio::action]] void recredit(name holder, asset quantity);

      // -----------------------------------------------------------------------
      //  Inbound OPP effects (sysio.msgch dispatch; never throw)
      // -----------------------------------------------------------------------

      /// SYNDICATE_LIQ for an AuthX-linked user: mint `amount` of `token_code`'s
      /// shadow to `account`. Auth=sysio.msgch. A replayed `sequence`, an
      /// unknown token, a token of another chain or an out-of-range amount is
      /// dropped with a diagnostic, never an abort.
      [[sysio::action]] void mintsynd(sysio::slug_name chain_code, uint64_t sequence, name account,
                                      sysio::slug_name token_code, uint64_t amount);

      /// SYNDICATE_LIQ for a user with no AuthX link yet: mint to a parked row
      /// keyed by the user's native pubkey, which accrues like any holder until
      /// `linkswept` or `sweep` delivers it. Same contract as `mintsynd`.
      [[sysio::action]] void park(sysio::slug_name chain_code, uint64_t sequence,
                                  opp::types::ChainKind chain_kind, std::vector<char> pubkey,
                                  sysio::slug_name token_code, uint64_t amount);

      /// LIQ_YIELD: the outpost claimed `amount` of yield for its syndicated
      /// pool. Held in the symbol's pending balance, outside supply, until
      /// `queueyield` moves it; `epoch` is kept on the cursor for forensics.
      /// Same contract as `mintsynd`.
      [[sysio::action]] void mintyield(sysio::slug_name chain_code, uint64_t sequence, uint64_t epoch,
                                       sysio::slug_name token_code, uint64_t amount);

      // -----------------------------------------------------------------------
      //  Cranks
      // -----------------------------------------------------------------------

      /// Hand `sym`'s pending yield to sysio.swap's reservoir for its pool: mint
      /// it to this contract, announce it with `fundyield` and transfer it, all
      /// in one transaction. Permissionless; a no-op with nothing pending.
      [[sysio::action]] void queueyield(symbol_code sym);

      /// Deliver every parked row of the pubkey `account` has linked for
      /// `chain_kind`, for a link that already exists: late arrivals, and the
      /// node-owner path that records links without `createlink`. Permissionless.
      [[sysio::action]] void sweep(name account, opp::types::ChainKind chain_kind);

      // -----------------------------------------------------------------------
      //  The token
      // -----------------------------------------------------------------------

      [[sysio::action]] void transfer(name from, name to, asset quantity, string memo);
      [[sysio::action]] void open(name owner, symbol symbol, name ram_payer);
      /// Erase `owner`'s empty row for `symbol`. Refused while the row is still
      /// owed yield, so closing never discards WIRE.
      [[sysio::action]] void close(name owner, symbol symbol);
      /// Pay `holder` the WIRE its row for `sym` is owed and settle the row.
      /// Holder's authority; sends nothing when nothing is owed.
      [[sysio::action]] void claim(name holder, symbol_code sym);
      /// Distribute `quantity` WIRE to `target`'s holders: the index advances by
      /// quantity / supply with the remainder carried, the WIRE is pulled from
      /// `from` by inline transfer, and the kicker is requested from T5.
      [[sysio::action]] void addyield(name from, asset quantity, symbol_code target);
      /// Fold the kicker `fundclaim` delivered into `sym`'s index: what this
      /// contract's WIRE balance now exceeds `base_balance` by, at most
      /// `requested`. Inline from `addyield`; this contract's authority.
      [[sysio::action]] void addkicker(symbol_code sym, int64_t base_balance, uint64_t requested);

      /// AuthX link completed for `account` on `chain_kind`: deliver every parked
      /// row of that pubkey to `account`, accrued WIRE included. Auth=sysio.authex.
      [[sysio::action]] void linkswept(name account, opp::types::ChainKind chain_kind, std::vector<char> pubkey);

      /// Burn `quantity` of `holder`'s shadow and queue DESYNDICATE_LIQ to the
      /// symbol's outpost, paying the pubkey `holder` has linked for that chain.
      /// The burn is final: an outpost refusal is reconciled by governance through
      /// `recredit`. Holder's authority.
      [[sysio::action]] void desyndicate(name holder, asset quantity);

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

      /// One pre-launch position: the holder's native pubkey (32-byte Ed25519 on
      /// SVM, 33-byte compressed secp256k1 on EVM) and its shadow amount in
      /// subunits, the LCO yield already folded in.
      struct import_credit {
         std::vector<char> pubkey;
         uint64_t          amount = 0;
         SYSLIB_SERIALIZE(import_credit, (pubkey)(amount))
      };

      /// Replay pre-launch positions of `token_code` on `chain_code`: each credit
      /// mints to the account its pubkey has linked, or to a parked row. Batched;
      /// the same pubkey across batches sums. Refused once `importdone` ran.
      [[sysio::action]] void importsynd(sysio::slug_name chain_code, sysio::slug_name token_code,
                                        std::vector<import_credit> credits);

      /// Close the import: every later `importsynd` is refused.
      [[sysio::action]] void importdone();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      using symbol_key = opp::shadow::symbol_key;

      /// One shadow symbol: its supply and the registry rows it mirrors.
      struct [[sysio::table("stat")]] currency_stats {
         asset            supply;
         sysio::slug_name chain_code;    ///< the outpost whose liq this shadow mirrors
         sysio::slug_name token_code;    ///< that outpost's liq token in sysio.tokens
         symbol_code      pair_symbol;   ///< the swap's pair token of this shadow's yield pool; empty until regliqpool

         uint64_t by_token_code() const { return token_code.value; }

         SYSLIB_SERIALIZE(currency_stats, (supply)(chain_code)(token_code)(pair_symbol))
      };

      using stats = kv::table<"stat"_n, symbol_key, currency_stats,
         kv::index<"bytoken"_n, const_mem_fun<currency_stats, uint64_t, &currency_stats::by_token_code>>>;

      /// Holder rows (scope = holder, key = symbol code) and the per-symbol
      /// index, laid out by sysio.opp.common/shadow_yield.hpp.
      using accounts  = kv::scoped_table<"accounts"_n, symbol_key, opp::shadow::account>;
      using yieldidxs = kv::table<"yieldidx"_n, symbol_key, opp::shadow::yield_index>;

      /// A parked row: the symbol, the chain family and the holder's native pubkey.
      struct parked_key {
         uint64_t          symbol_code;
         uint64_t          chain_kind;   ///< magic_enum::enum_integer of the ChainKind; the row keeps the enum
         std::vector<char> pubkey;
         SYSLIB_SERIALIZE(parked_key, (symbol_code)(chain_kind)(pubkey))
      };

      /// Shadow minted for a pubkey with no AuthX link yet. `holding` accrues
      /// exactly as a holder row does, so linking late costs no yield.
      struct [[sysio::table("parked")]] parked_row {
         opp::types::ChainKind chain_kind;
         std::vector<char>     pubkey;
         opp::shadow::account  holding;
         SYSLIB_SERIALIZE(parked_row, (chain_kind)(pubkey)(holding))
      };

      using parkeds = kv::table<"parked"_n, parked_key, parked_row>;

      /// Yield minted by LIQ_YIELD and not yet queued. Outside supply, so it
      /// earns nothing while it waits and nothing is stranded when it leaves.
      struct [[sysio::table("liqpending")]] pending_yield {
         asset quantity;
         SYSLIB_SERIALIZE(pending_yield, (quantity))
      };

      using liqpendings = kv::table<"liqpending"_n, symbol_key, pending_yield>;

      struct cursor_key {
         uint64_t chain_code;
         SYSLIB_SERIALIZE(cursor_key, (chain_code))
      };

      /// Per-outpost replay guard over the sequence SYNDICATE_LIQ and LIQ_YIELD share.
      struct [[sysio::table("liqcursors")]] liq_cursor {
         sysio::slug_name chain_code;
         uint64_t         last_sequence = 0;   ///< highest sequence admitted; anything at or below it is a replay
         uint64_t         last_epoch    = 0;   ///< outpost epoch of the last LIQ_YIELD, for forensics
         SYSLIB_SERIALIZE(liq_cursor, (chain_code)(last_sequence)(last_epoch))
      };

      using liqcursors = kv::table<"liqcursors"_n, cursor_key, liq_cursor>;

      struct [[sysio::table("liqconfig")]] liq_config {
         uint32_t kicker_bps      = DEFAULT_KICKER_BPS;
         bool     import_complete = false;
         SYSLIB_SERIALIZE(liq_config, (kicker_bps)(import_complete))
      };

      using liqconfig_t = kv::global<"liqconfig"_n, liq_config>;

      struct [[sysio::table("liqcounters")]] liq_counters {
         uint64_t next_request_id = 1;   ///< DESYNDICATE_LIQ ids; the outpost reads 0 as "no id"
         SYSLIB_SERIALIZE(liq_counters, (next_request_id))
      };

      using liqcounters_t = kv::global<"liqcounters"_n, liq_counters>;

   private:
      using ChainKind = opp::types::ChainKind;
      using u128      = opp::shadow::u128;

      /// The stat row of `sym`, or a check failure.
      currency_stats stat_of(symbol_code sym) const;
      /// The stat row bound to `token_code`, if any.
      std::optional<currency_stats> stat_by_token(sysio::slug_name token_code) const;
      /// The chain family of the registered outpost `chain_code`, or a check failure.
      ChainKind kind_of_chain(sysio::slug_name chain_code) const;
      /// `sym`'s index now; zero before the first distribution.
      u128 current_index(symbol_code sym) const;
      /// This contract's WIRE balance on sysio.token; zero without a row.
      int64_t wire_balance() const;

      /// The one funnel every balance move goes through: settle `row` at `index`,
      /// stamp it, then apply `delta`. Nothing else writes `balance`.
      static void settle_and_adjust(opp::shadow::account& row, u128 index, const asset& delta);
      /// Apply `delta` to `owner`'s row for its symbol through the funnel. A row
      /// created here is stamped at the current index, so no history is credited.
      void adjust_account(name owner, const asset& delta, name payer);
      /// The same for a parked row.
      void adjust_parked(const parked_key& key, ChainKind chain_kind, const std::vector<char>& pubkey,
                         const asset& delta);
      /// Grow `sym`'s supply by `quantity`; false (and no change) past the asset range.
      bool mint(symbol_code sym, uint64_t quantity);
      /// Advance `sym`'s index by `quantity` WIRE over its supply, carrying the
      /// remainder, and grow its pot by the same.
      void distribute(symbol_code sym, uint64_t quantity);
      /// Admit `sequence` for `chain_code` and advance its cursor; false on a replay.
      bool admit_sequence(sysio::slug_name chain_code, uint64_t sequence, uint64_t epoch);
      /// Move every parked row of `(chain_kind, pubkey)` into `account`'s rows.
      void deliver_parked(name account, ChainKind chain_kind, const std::vector<char>& pubkey);
      /// Credit `amount` of `sym` to the account `pubkey` has linked, else to its parked row.
      void credit_by_pubkey(symbol_code sym, ChainKind chain_kind, const std::vector<char>& pubkey, uint64_t amount);
      /// The `mintsynd` / `park` / `mintyield` preamble: the symbol for `token_code`
      /// on `chain_code`, with `amount` in range and room in the supply, or nullopt
      /// after a diagnostic. Touches no cursor: the caller admits the sequence only
      /// once every check has passed, so a dropped attestation consumes nothing.
      std::optional<currency_stats> resolve_inbound(const char* path, sysio::slug_name chain_code,
                                                    sysio::slug_name token_code, uint64_t amount);
   };

} // namespace sysio
