#include <sysio.liq/sysio.liq.hpp>
#include <sysio.andon/sysio.andon.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.tokens/sysio.tokens.hpp>
#include <sysio.opp.common/require_privileged.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio/print.hpp>


namespace sysio {

namespace {

constexpr auto unsupported_custodian_msg = "unsupported custodian";
constexpr auto beneficiary_account_does_not_exist_msg = "beneficiary account does not exist";
constexpr auto invalid_quantity_msg = "invalid quantity";
constexpr auto must_settle_positive_quantity_msg = "must settle positive quantity";
constexpr auto symbol_precision_mismatch_msg = "symbol precision mismatch";
constexpr auto overdrawn_balance_msg = "overdrawn balance";
constexpr name bond_account = "sysio.bond"_n;


using opp::types::ChainKind;
using opp::types::TokenKind;
using u128 = opp::shadow::u128;

// System-owned rows bill the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses).
constexpr name ram_payer = "sysio"_n;

constexpr size_t MAX_MEMO_BYTES  = 256;

constexpr std::string_view YIELD_MEMO  = "sysio.liq yield";
constexpr std::string_view CLAIM_MEMO  = "sysio.liq claim";
/// Memo of the transfer that brings the WIRE `creditowed` credits in from sysio.synd.
constexpr std::string_view CREDIT_MEMO = "sysio.liq creditowed";

/// The `sysio.payer` seat: an inline action to an unprivileged contract that bills a
/// row to `actor` must carry it beside `actor`'s active permission.
permission_level payer_of(name actor) { return permission_level{ actor, "sysio.payer"_n }; }
permission_level active_of(name actor) { return permission_level{ actor, "active"_n }; }

void drop(const char* path, const char* reason) {
   sysio::print("sysio.liq::", path, ": DROP -- ", reason, "\n");
}

} // namespace

// ---------------------------------------------------------------------------
//  Deployment and governance
// ---------------------------------------------------------------------------

void liq::create(symbol sym, sysio::slug_name chain_code, sysio::slug_name token_code) {
   require_auth(get_self());
   check(sym.is_valid(), "invalid symbol");

   const ChainKind kind = kind_of_chain(chain_code);
   check(kind == ChainKind::CHAIN_KIND_EVM || kind == ChainKind::CHAIN_KIND_SVM,
         "the chain must be an EVM or SVM outpost");

   sysio::tokens::tokens_t tokens(TOKENS_ACCOUNT);
   const auto token = tokens.try_get(sysio::tokens::token_key{ token_code });
   check(token.has_value() && token->active, "token_code is not an active registry token");
   check(token->kind == TokenKind::TOKEN_KIND_LIQ, "token_code is not a liq token");
   check(token->precision == sym.precision(), "symbol precision must match the token's depot precision");

   sysio::tokens::chaintokens_t chaintokens(TOKENS_ACCOUNT);
   const auto binding = chaintokens.try_get(sysio::tokens::chain_token_key{ chain_code, token_code });
   check(binding.has_value() && binding->active, "token_code is not bound to chain_code");

   stats statstable(get_self());
   check(!find_stat_by_token(get_self(), token_code).has_value(), "token_code already has a shadow symbol");
   statstable.emplace(ram_payer, symbol_key{ sym.code().raw() }, currency_stats{
      .supply      = asset{ 0, sym },
      .chain_code  = chain_code,
      .token_code  = token_code,
      .pair_symbol = symbol_code{},
   }, "symbol already exists");
}

void liq::recredit(name holder, asset quantity) {
   require_auth(get_self());
   check(is_account(holder), "holder account does not exist");
   check(quantity.is_valid() && quantity.amount > 0, "quantity must be positive");
   const currency_stats st = stat_of(quantity.symbol.code());
   check(quantity.symbol == st.supply.symbol, symbol_precision_mismatch_msg);
   check(grow_supply(quantity.symbol.code(), static_cast<uint64_t>(quantity.amount)), "supply exceeds the asset range");
   adjust_account(holder, quantity, ram_payer);
}

// ---------------------------------------------------------------------------
//  Supply
// ---------------------------------------------------------------------------

void liq::mint(name to, sysio::slug_name token_code, uint64_t amount) {
   require_auth(SYND_ACCOUNT);
   check(is_account(to), "to account does not exist");
   check(amount > 0, "amount must be positive");
   const currency_stats st  = stat_by_token(token_code);
   const symbol_code    sym = st.supply.symbol.code();
   check(grow_supply(sym, amount), "supply exceeds the asset range");
   adjust_account(to, asset{ static_cast<int64_t>(amount), st.supply.symbol }, ram_payer);
}

void liq::burn(sysio::slug_name token_code, uint64_t amount) {
   require_auth(SYND_ACCOUNT);
   check(amount > 0 && amount <= static_cast<uint64_t>(asset::max_amount), "amount out of range");
   const currency_stats st = stat_by_token(token_code);
   const asset quantity{ static_cast<int64_t>(amount), st.supply.symbol };
   // Settle, then burn: the row keeps every subunit of yield accrued to now.
   adjust_account(SYND_ACCOUNT, -quantity, ram_payer);
   stats statstable(get_self());
   statstable.modify(ram_payer, symbol_key{ st.supply.symbol.code().raw() }, [&](currency_stats& s) {
      s.supply -= quantity;
   });
}

std::optional<liq::currency_stats> liq::resolve_inbound(const char* path, sysio::slug_name chain_code,
                                                       sysio::slug_name token_code, uint64_t amount) {
   const auto st = find_stat_by_token(get_self(), token_code);
   if (!st) { drop(path, "token_code has no shadow symbol"); return std::nullopt; }
   if (st->chain_code != chain_code) { drop(path, "token_code belongs to another chain"); return std::nullopt; }
   if (amount == 0 || amount > static_cast<uint64_t>(opp::safe::depot_amount_max)) {
      drop(path, "amount out of range");
      return std::nullopt;
   }
   if (amount > headroom_of(get_self(), *st)) {
      drop(path, "supply exceeds the asset range");
      return std::nullopt;
   }
   return st;
}

void liq::mintyield(sysio::slug_name chain_code, sysio::slug_name token_code, uint64_t amount) {
   require_auth(SYND_ACCOUNT);
   const auto st = resolve_inbound("mintyield", chain_code, token_code, amount);
   if (!st) return;
   const symbol_key key{ st->supply.symbol.code().raw() };
   liqpendings pendings(get_self());
   const pending_yield fresh{ asset{ static_cast<int64_t>(amount), st->supply.symbol } };
   // resolve_inbound bounded the amount by the headroom net of what is already pending,
   // so supply plus pending stays within the asset range and queueyield always fits.
   pendings.upsert(ram_payer, key, fresh, [&](pending_yield& p) {
      p.quantity.amount += static_cast<int64_t>(amount);
   });
}

void liq::creditowed(name holder, symbol_code sym, uint64_t wire) {
   require_auth(SYND_ACCOUNT);
   check(is_account(holder), "holder account does not exist");
   check(wire > 0 && wire <= static_cast<uint64_t>(asset::max_amount), "amount out of range");
   const currency_stats st = stat_of(sym);
   const symbol_key     key{ sym.raw() };
   const u128           index = current_index(sym);

   // Settle first, so the row keeps what its balance earned to now, then bank the credit on top.
   accounts   holdings(get_self(), holder.value);
   const auto existing = holdings.try_get(key);
   opp::shadow::account row = existing.value_or(opp::shadow::account{ asset{ 0, st.supply.symbol }, index, 0 });
   settle_and_adjust(row, index, asset{ 0, st.supply.symbol });
   check(wire <= static_cast<uint64_t>(asset::max_amount) - row.owed_wire, "owed yield exceeds the asset range");
   row.owed_wire += wire;
   if (existing) {
      holdings.modify(ram_payer, key, [&](opp::shadow::account& a) { a = row; });
   } else {
      holdings.emplace(ram_payer, key, row);
   }

   // The pot backs every claim; the WIRE that backs this credit arrives by the transfer queued below,
   // ahead of any claim a later action sends.
   yieldidxs indexes(get_self());
   opp::shadow::yield_index idx = indexes.try_get(key).value_or(opp::shadow::yield_index{});
   idx.pot = opp::safe::add_sat_u64(idx.pot, wire);
   indexes.upsert(ram_payer, key, idx);
   action(active_of(SYND_ACCOUNT), TOKEN_ACCOUNT, "transfer"_n,
          std::make_tuple(SYND_ACCOUNT, get_self(), asset{ static_cast<int64_t>(wire), WIRE_SYM },
                          std::string{ CREDIT_MEMO })).send();
}

// ---------------------------------------------------------------------------
//  Cranks
// ---------------------------------------------------------------------------

void liq::queueyield(symbol_code sym) {
   // Queueing hands shadow to sysio.swap, which is no custody contract: refused while the cord is pulled.
   andon::check_clear(andon::ANDON_ACCOUNT);
   liqpendings pendings(get_self());
   const symbol_key key{ sym.raw() };
   const auto pending = pendings.try_get(key);
   if (!pending || pending->quantity.amount <= 0) return;   // nothing queued: a cheap no-op for the crank

   const currency_stats st = stat_of(sym);
   check(st.pair_symbol != symbol_code{}, "no yield pool registered for this shadow");
   const asset quantity = pending->quantity;
   // Erased before the mint: the headroom mint measures is net of what is pending, and
   // this is the pending being minted. Every intake reserved it, so the mint cannot fail.
   pendings.erase(key);

   // Minted to this contract and handed on in the same transaction, so its row is
   // settled at one index and accrues nothing on the way through.
   check(grow_supply(sym, static_cast<uint64_t>(quantity.amount)), "supply exceeds the asset range");
   adjust_account(get_self(), quantity, ram_payer);

   action(std::vector<permission_level>{ payer_of(get_self()), active_of(get_self()) },
          SWAP_ACCOUNT, "fundyield"_n,
          std::make_tuple(get_self(), st.pair_symbol, quantity)).send();
   action(active_of(get_self()), get_self(), "transfer"_n,
          std::make_tuple(get_self(), SWAP_ACCOUNT, quantity, std::string{})).send();
}

// ---------------------------------------------------------------------------
//  The token
// ---------------------------------------------------------------------------

void liq::transfer(name from, name to, asset quantity, string memo) {
   check(from != to, "cannot transfer to self");
   require_auth(from);
   check(is_account(to), "to account does not exist");
   // While the cord is pulled only a transfer INTO a custody contract passes: bonds, challenges, holds and
   // desyndications keep flowing in, and nothing leaves.
   if (!andon::is_custody(to)) andon::check_clear(andon::ANDON_ACCOUNT);
   const currency_stats st = stat_of(quantity.symbol.code());

   require_recipient(from);
   require_recipient(to);

   check(quantity.is_valid(), invalid_quantity_msg);
   check(quantity.amount > 0, "must transfer positive quantity");
   check(quantity.symbol == st.supply.symbol, symbol_precision_mismatch_msg);
   check(memo.size() <= MAX_MEMO_BYTES, "memo has more than 256 bytes");

   adjust_account(from, -quantity, ram_payer);
   adjust_account(to, quantity, ram_payer);
}

void liq::settle(name custodian, name beneficiary, asset quantity) {
   check(custodian == SYND_ACCOUNT || custodian == bond_account, unsupported_custodian_msg);
   require_auth(custodian);
   andon::check_clear(andon::ANDON_ACCOUNT);
   check(is_account(beneficiary), beneficiary_account_does_not_exist_msg);
   check(quantity.is_valid(), invalid_quantity_msg);
   check(quantity.amount > 0, must_settle_positive_quantity_msg);
   const currency_stats st = stat_of(quantity.symbol.code());
   check(quantity.symbol == st.supply.symbol, symbol_precision_mismatch_msg);
   if (custodian == beneficiary) {
      const auto row = accounts(get_self(), custodian.value).try_get(symbol_key{quantity.symbol.code().raw()});
      check(row && row->balance.amount >= quantity.amount, overdrawn_balance_msg);
      adjust_account(custodian, asset{0, quantity.symbol}, ram_payer);
   } else {
      adjust_account(custodian, -quantity, ram_payer);
      adjust_account(beneficiary, quantity, ram_payer);
   }
}

void liq::open(name owner, symbol symbol, name ram_payer_) {
   require_auth(ram_payer_);
   check(is_account(owner), "owner account does not exist");
   const currency_stats st = stat_of(symbol.code());
   check(st.supply.symbol == symbol, symbol_precision_mismatch_msg);
   accounts holdings(get_self(), owner.value);
   if (!holdings.contains(symbol_key{ symbol.code().raw() })) {
      adjust_account(owner, asset{ 0, symbol }, ram_payer_);
   }
}

void liq::close(name owner, symbol symbol) {
   require_auth(owner);
   accounts holdings(get_self(), owner.value);
   const symbol_key key{ symbol.code().raw() };
   const auto row = holdings.try_get(key);
   check(row.has_value(), "Balance row already deleted or never existed. Action won't have any effect.");
   check(row->balance.amount == 0, "Cannot close because the balance is not zero.");
   check(opp::shadow::owed(*row, current_index(symbol.code())) == 0, "Cannot close because yield is still owed; claim first.");
   holdings.erase(key);
}

void liq::claim(name holder, symbol_code sym) {
   require_auth(holder);
   // A custody contract pulling what its own row earned keeps the WIRE in custody; anyone else is refused
   // while the cord is pulled.
   if (!andon::is_custody(holder)) andon::check_clear(andon::ANDON_ACCOUNT);
   accounts holdings(get_self(), holder.value);
   const symbol_key key{ sym.raw() };
   const auto row = holdings.try_get(key);
   check(row.has_value(), "no balance object found");
   const u128     index = current_index(sym);
   const uint64_t owed  = opp::shadow::owed(*row, index);
   holdings.modify(ram_payer, key, [&](opp::shadow::account& a) {
      a.index_checkpoint = index;
      a.owed_wire        = 0;
   });
   if (owed == 0) return;

   yieldidxs indexes(get_self());
   indexes.modify(ram_payer, key, [&](opp::shadow::yield_index& y) {
      check(y.pot >= owed, "pot underfunded");
      y.pot -= owed;
   });
   action(active_of(get_self()), TOKEN_ACCOUNT, "transfer"_n,
          std::make_tuple(get_self(), holder, asset{ static_cast<int64_t>(owed), WIRE_SYM },
                          std::string{ CLAIM_MEMO })).send();
}

void liq::addyield(name from, asset quantity, symbol_code target) {
   require_auth(from);
   check(quantity.symbol == WIRE_SYM, "yield must be in WIRE");
   check(quantity.amount > 0, "yield must be positive");
   const currency_stats st = stat_of(target);
   check(st.supply.amount > 0, "no holders to distribute to");

   distribute(target, static_cast<uint64_t>(quantity.amount));
   action(active_of(from), TOKEN_ACCOUNT, "transfer"_n,
          std::make_tuple(from, get_self(), quantity, std::string{ YIELD_MEMO })).send();
}

// ---------------------------------------------------------------------------
//  Launch ingestion
// ---------------------------------------------------------------------------

void liq::regliqpool(sysio::slug_name chain_code, sysio::slug_name token_code, symbol pair_symbol,
                     uint64_t initial_chain_amount, uint64_t initial_wire_amount, int32_t fee,
                     int64_t locked_shares, uint32_t conversion_horizon_sec, uint32_t depth_cap_bps,
                     int64_t clip_floor) {
   opp::require_privileged_self();
   check(epoch::in_bootstrap_window(), "regliqpool is bootstrap-window only");
   const auto st = find_stat_by_token(get_self(), token_code);
   check(st.has_value(), "token_code has no shadow symbol");
   check(st->chain_code == chain_code, "token_code belongs to another chain");
   check(st->pair_symbol == symbol_code{}, "the shadow already has a yield pool");
   check(pair_symbol.is_valid(), "invalid pair symbol");
   check(initial_chain_amount > 0 && initial_wire_amount > 0, "both seeds must be positive");
   check(initial_chain_amount <= static_cast<uint64_t>(asset::max_amount) &&
         initial_wire_amount  <= static_cast<uint64_t>(asset::max_amount), "seed exceeds the asset range");

   const symbol          sym    = st->supply.symbol;
   const asset           shadow{ static_cast<int64_t>(initial_chain_amount), sym };
   const asset           wire  { static_cast<int64_t>(initial_wire_amount),  WIRE_SYM };
   const extended_symbol shadow_symbol{ sym, get_self() };
   const extended_symbol wire_symbol  { WIRE_SYM, TOKEN_ACCOUNT };

   // The LCO liq is protocol-owned and already in outpost custody: its shadow is
   // minted to sysio, which seeds the pool with it and holds the pool's shares.
   check(grow_supply(sym.code(), initial_chain_amount), "supply exceeds the asset range");
   adjust_account(SYSTEM_ACCOUNT, shadow, ram_payer);
   stats statstable(get_self());
   statstable.modify(ram_payer, symbol_key{ sym.code().raw() }, [&](currency_stats& s) {
      s.pair_symbol = pair_symbol.code();
   });

   const std::vector<permission_level> sysio_billed{ payer_of(SYSTEM_ACCOUNT), active_of(SYSTEM_ACCOUNT) };
   action(sysio_billed, SWAP_ACCOUNT, "openext"_n,
          std::make_tuple(SYSTEM_ACCOUNT, SYSTEM_ACCOUNT, shadow_symbol)).send();
   action(sysio_billed, SWAP_ACCOUNT, "openext"_n,
          std::make_tuple(SYSTEM_ACCOUNT, SYSTEM_ACCOUNT, wire_symbol)).send();
   // The shadow has no pair yet, so its seed carries the swap's own authority.
   action(std::vector<permission_level>{ active_of(SYSTEM_ACCOUNT), active_of(SWAP_ACCOUNT) },
          get_self(), "transfer"_n,
          std::make_tuple(SYSTEM_ACCOUNT, SWAP_ACCOUNT, shadow, std::string{})).send();
   // The WIRE side is the T5 dex earmark, drained from the treasury.
   action(active_of(SYSTEM_ACCOUNT), TOKEN_ACCOUNT, "transfer"_n,
          std::make_tuple(SYSTEM_ACCOUNT, SWAP_ACCOUNT, wire, std::string{})).send();
   action(std::vector<permission_level>{ payer_of(SYSTEM_ACCOUNT), active_of(SYSTEM_ACCOUNT), active_of(SWAP_ACCOUNT) },
          SWAP_ACCOUNT, "inittoken"_n,
          std::make_tuple(SYSTEM_ACCOUNT, pair_symbol, extended_asset{ shadow, get_self() },
                          extended_asset{ wire, TOKEN_ACCOUNT }, fee, SYSTEM_ACCOUNT,
                          asset{ locked_shares, pair_symbol }, std::optional<extended_symbol>{ shadow_symbol })).send();
   action(active_of(SYSTEM_ACCOUNT), SWAP_ACCOUNT, "setyield"_n,
          std::make_tuple(pair_symbol.code(), conversion_horizon_sec, depth_cap_bps, clip_floor)).send();
}

// ---------------------------------------------------------------------------
//  Internals
// ---------------------------------------------------------------------------

liq::currency_stats liq::stat_of(symbol_code sym) const {
   stats statstable(get_self());
   return statstable.get(symbol_key{ sym.raw() }, "shadow symbol does not exist");
}

liq::currency_stats liq::stat_by_token(sysio::slug_name token_code) const {
   const auto st = find_stat_by_token(get_self(), token_code);
   check(st.has_value(), "token_code has no shadow symbol");
   return *st;
}

ChainKind liq::kind_of_chain(sysio::slug_name chain_code) const {
   const auto kind = sysio::chains::outpost_kind_of(CHAINS_ACCOUNT, chain_code);
   check(kind.has_value(), "chain_code is not an active outpost");
   return *kind;
}

u128 liq::current_index(symbol_code sym) const {
   yieldidxs indexes(get_self());
   const auto idx = indexes.try_get(symbol_key{ sym.raw() });
   return idx ? idx->index : 0;
}

void liq::settle_and_adjust(opp::shadow::account& row, u128 index, const asset& delta) {
   row.owed_wire        = opp::shadow::owed(row, index);   // settle before mutate
   row.index_checkpoint = index;
   row.balance         += delta;
   check(row.balance.amount >= 0, overdrawn_balance_msg);
}

void liq::adjust_account(name owner, const asset& delta, name payer) {
   accounts holdings(get_self(), owner.value);
   const symbol_key key{ delta.symbol.code().raw() };
   const u128 index = current_index(delta.symbol.code());
   const auto row = holdings.try_get(key);
   if (!row) {
      check(delta.amount >= 0, "no balance object found");
      holdings.emplace(payer, key, opp::shadow::account{ delta, index, 0 });
      return;
   }
   opp::shadow::account updated = *row;
   settle_and_adjust(updated, index, delta);
   holdings.modify(payer, key, [&](opp::shadow::account& a) { a = updated; });
}

bool liq::grow_supply(symbol_code sym, uint64_t quantity) {
   stats statstable(get_self());
   const symbol_key key{ sym.raw() };
   const currency_stats st = statstable.get(key, "shadow symbol does not exist");
   if (quantity > headroom_of(get_self(), st)) return false;
   statstable.modify(ram_payer, key, [&](currency_stats& s) { s.supply.amount += static_cast<int64_t>(quantity); });
   return true;
}

void liq::distribute(symbol_code sym, uint64_t quantity) {
   const currency_stats st = stat_of(sym);
   check(st.supply.amount > 0, "no holders to distribute to");
   yieldidxs indexes(get_self());
   const symbol_key key{ sym.raw() };
   opp::shadow::yield_index idx = indexes.try_get(key).value_or(opp::shadow::yield_index{});
   const u128 total = static_cast<u128>(quantity) * opp::shadow::YIELD_INDEX_SCALE + idx.carry;
   idx.index += total / static_cast<u128>(st.supply.amount);
   idx.carry  = static_cast<uint64_t>(total % static_cast<u128>(st.supply.amount));
   idx.pot    = opp::safe::add_sat_u64(idx.pot, quantity);
   indexes.upsert(ram_payer, key, idx);
}

} // namespace sysio
