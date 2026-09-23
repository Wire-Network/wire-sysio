#include <sysio.liq/sysio.liq.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.token/sysio.token.hpp>
#include <sysio.tokens/sysio.tokens.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <sysio/print.hpp>
#include <zpp_bits.h>

#include <algorithm>

namespace sysio {

namespace {

using opp::types::AttestationType;
using opp::types::ChainKind;
using opp::types::TokenKind;
using u128 = opp::shadow::u128;

// System-owned rows bill the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses).
constexpr name ram_payer = "sysio"_n;

constexpr size_t MAX_MEMO_BYTES  = 256;

constexpr std::string_view YIELD_MEMO = "sysio.liq yield";
constexpr std::string_view CLAIM_MEMO = "sysio.liq claim";

/// The `sysio.payer` seat: an inline action to an unprivileged contract that bills a
/// row to `actor` must carry it beside `actor`'s active permission.
permission_level payer_of(name actor) { return permission_level{ actor, "sysio.payer"_n }; }
permission_level active_of(name actor) { return permission_level{ actor, "active"_n }; }

uint32_t current_epoch_index() {
   sysio::epoch::epochstate_t es(liq::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

bool is_bootstrap_window() {
   return current_epoch_index() == 0;
}

void require_priv_caller() {
   require_auth(current_receiver());
   check(is_privileged(current_receiver()), "sysio.liq: privileged account required");
}

liq::parked_key parked_key_of(symbol_code sym, ChainKind kind, const std::vector<char>& pubkey) {
   return liq::parked_key{ sym.raw(), static_cast<uint64_t>(magic_enum::enum_integer(kind)), pubkey };
}

/// The account `pubkey` has linked on `kind`, or an empty name.
name linked_account(ChainKind kind, const std::vector<char>& pubkey) {
   const auto pk = public_key_from_op_address(kind, pubkey);
   if (!pk) return name{};   // no link could hold these bytes
   sysio::authex::links_t links(liq::AUTHEX_ACCOUNT);
   auto by_pubkey = links.get_index<"bypubkey"_n>();
   auto it = by_pubkey.find(pubkey_to_checksum256(*pk));
   return it == by_pubkey.end() ? name{} : it->username;
}

/// The pubkey `account` has linked on `kind`, or nullopt.
std::optional<std::vector<char>> linked_pubkey(name account, ChainKind kind) {
   sysio::authex::links_t links(liq::AUTHEX_ACCOUNT);
   auto by_namechain = links.get_index<"bynamechain"_n>();
   auto it = by_namechain.find(to_namechain_key(account, kind));
   if (it == by_namechain.end()) return std::nullopt;
   return pubkey_to_bytes(it->pub_key);
}

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
   check(!stat_by_token(token_code).has_value(), "token_code already has a shadow symbol");
   statstable.emplace(ram_payer, symbol_key{ sym.code().raw() }, currency_stats{
      .supply      = asset{ 0, sym },
      .chain_code  = chain_code,
      .token_code  = token_code,
      .pair_symbol = symbol_code{},
   }, "symbol already exists");
}

void liq::setkicker(uint32_t bps) {
   require_auth(SYSTEM_ACCOUNT);
   check(bps <= opp::amm::BPS_TOTAL, "kicker_bps out of range");
   liqconfig_t config(get_self());
   liq_config cfg = config.get_or_default(liq_config{});
   cfg.kicker_bps = bps;
   config.set(cfg, ram_payer);
}

void liq::recredit(name holder, asset quantity) {
   require_auth(get_self());
   check(is_account(holder), "holder account does not exist");
   check(quantity.is_valid() && quantity.amount > 0, "quantity must be positive");
   const currency_stats st = stat_of(quantity.symbol.code());
   check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
   check(mint(quantity.symbol.code(), static_cast<uint64_t>(quantity.amount)), "supply exceeds the asset range");
   adjust_account(holder, quantity, ram_payer);
}

// ---------------------------------------------------------------------------
//  Inbound OPP effects
// ---------------------------------------------------------------------------

std::optional<liq::currency_stats> liq::resolve_inbound(const char* path, sysio::slug_name chain_code,
                                                       sysio::slug_name token_code, uint64_t amount) {
   const auto st = stat_by_token(token_code);
   if (!st) { drop(path, "token_code has no shadow symbol"); return std::nullopt; }
   if (st->chain_code != chain_code) { drop(path, "token_code belongs to another chain"); return std::nullopt; }
   if (amount == 0 || amount > static_cast<uint64_t>(opp::safe::depot_amount_max)) {
      drop(path, "amount out of range");
      return std::nullopt;
   }
   if (amount > static_cast<uint64_t>(asset::max_amount - st->supply.amount)) {
      drop(path, "supply exceeds the asset range");
      return std::nullopt;
   }
   return st;
}

void liq::mintsynd(sysio::slug_name chain_code, uint64_t sequence, name account,
                   sysio::slug_name token_code, uint64_t amount) {
   require_auth(MSGCH_ACCOUNT);
   if (!is_account(account)) { drop("mintsynd", "account does not exist"); return; }
   const auto st = resolve_inbound("mintsynd", chain_code, token_code, amount);
   if (!st) return;
   // Every check is behind us: the sequence is consumed only by a credit that lands.
   if (!admit_sequence(chain_code, sequence, 0)) { drop("mintsynd", "replayed sequence"); return; }
   const symbol_code sym = st->supply.symbol.code();
   check(mint(sym, amount), "supply exceeds the asset range");   // resolve_inbound bounded it
   adjust_account(account, asset{ static_cast<int64_t>(amount), st->supply.symbol }, ram_payer);
}

void liq::park(sysio::slug_name chain_code, uint64_t sequence, ChainKind chain_kind, std::vector<char> pubkey,
               sysio::slug_name token_code, uint64_t amount) {
   require_auth(MSGCH_ACCOUNT);
   if (!pubkey_fits(chain_kind, pubkey)) { drop("park", "pubkey does not fit the chain family"); return; }
   const auto st = resolve_inbound("park", chain_code, token_code, amount);
   if (!st) return;
   if (kind_of_chain(st->chain_code) != chain_kind) { drop("park", "chain_kind is not the token's chain"); return; }
   if (!admit_sequence(chain_code, sequence, 0)) { drop("park", "replayed sequence"); return; }
   const symbol_code sym = st->supply.symbol.code();
   check(mint(sym, amount), "supply exceeds the asset range");
   adjust_parked(parked_key_of(sym, chain_kind, pubkey), chain_kind, pubkey,
                 asset{ static_cast<int64_t>(amount), st->supply.symbol });
}

void liq::mintyield(sysio::slug_name chain_code, uint64_t sequence, uint64_t epoch,
                    sysio::slug_name token_code, uint64_t amount) {
   require_auth(MSGCH_ACCOUNT);
   const auto st = resolve_inbound("mintyield", chain_code, token_code, amount);
   if (!st) return;
   if (!admit_sequence(chain_code, sequence, epoch)) { drop("mintyield", "replayed sequence"); return; }
   const symbol_key key{ st->supply.symbol.code().raw() };
   liqpendings pendings(get_self());
   const pending_yield fresh{ asset{ static_cast<int64_t>(amount), st->supply.symbol } };
   pendings.upsert(ram_payer, key, fresh, [&](pending_yield& p) {
      // Saturate rather than abort: a pending balance this large is not reachable,
      // and the never-throw contract holds either way.
      const int64_t room = asset::max_amount - p.quantity.amount;
      p.quantity.amount += std::min<int64_t>(room, static_cast<int64_t>(amount));
   });
}

// ---------------------------------------------------------------------------
//  Cranks
// ---------------------------------------------------------------------------

void liq::queueyield(symbol_code sym) {
   liqpendings pendings(get_self());
   const symbol_key key{ sym.raw() };
   const auto pending = pendings.try_get(key);
   if (!pending || pending->quantity.amount <= 0) return;   // nothing queued: a cheap no-op for the crank

   const currency_stats st = stat_of(sym);
   check(st.pair_symbol != symbol_code{}, "no yield pool registered for this shadow");
   const asset quantity = pending->quantity;
   pendings.erase(key);

   // Minted to this contract and handed on in the same transaction, so its row is
   // settled at one index and accrues nothing on the way through.
   check(mint(sym, static_cast<uint64_t>(quantity.amount)), "supply exceeds the asset range");
   adjust_account(get_self(), quantity, ram_payer);

   action(std::vector<permission_level>{ payer_of(get_self()), active_of(get_self()) },
          SWAP_ACCOUNT, "fundyield"_n,
          std::make_tuple(get_self(), st.pair_symbol, quantity)).send();
   action(active_of(get_self()), get_self(), "transfer"_n,
          std::make_tuple(get_self(), SWAP_ACCOUNT, quantity, std::string{})).send();
}

void liq::sweep(name account, ChainKind chain_kind) {
   const auto pubkey = linked_pubkey(account, chain_kind);
   check(pubkey.has_value(), "account has no link for this chain");
   deliver_parked(account, chain_kind, *pubkey);
}

// ---------------------------------------------------------------------------
//  The token
// ---------------------------------------------------------------------------

void liq::transfer(name from, name to, asset quantity, string memo) {
   check(from != to, "cannot transfer to self");
   require_auth(from);
   check(is_account(to), "to account does not exist");
   const currency_stats st = stat_of(quantity.symbol.code());

   require_recipient(from);
   require_recipient(to);

   check(quantity.is_valid(), "invalid quantity");
   check(quantity.amount > 0, "must transfer positive quantity");
   check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
   check(memo.size() <= MAX_MEMO_BYTES, "memo has more than 256 bytes");

   adjust_account(from, -quantity, ram_payer);
   adjust_account(to, quantity, ram_payer);
}

void liq::open(name owner, symbol symbol, name ram_payer_) {
   require_auth(ram_payer_);
   check(is_account(owner), "owner account does not exist");
   const currency_stats st = stat_of(symbol.code());
   check(st.supply.symbol == symbol, "symbol precision mismatch");
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

   // The kicker: `kicker_bps` of the intake, requested from T5 -- only on the
   // swap's intake, the one that is yield (tickyield's proceeds). A donation
   // draws nothing: a near-sole holder could otherwise donate, claim it back
   // with the kicker on top, and repeat against the treasury. fundclaim caps
   // the draw and never throws, so what actually lands is folded in afterwards
   // by addkicker, measured against the balance the pull above will have left.
   if (from != SWAP_ACCOUNT) return;
   liqconfig_t config(get_self());
   const uint32_t kicker_bps = config.get_or_default(liq_config{}).kicker_bps;
   const uint64_t kicker = static_cast<uint64_t>(
      static_cast<u128>(quantity.amount) * kicker_bps / opp::amm::BPS_TOTAL);
   if (kicker == 0) return;
   const int64_t base_balance = wire_balance() + quantity.amount;
   action(active_of(get_self()), SYSTEM_ACCOUNT, "fundclaim"_n,
          std::make_tuple(get_self(), static_cast<int64_t>(kicker))).send();
   action(active_of(get_self()), get_self(), "addkicker"_n,
          std::make_tuple(target, base_balance, kicker)).send();
}

void liq::addkicker(symbol_code sym, int64_t base_balance, uint64_t requested) {
   require_auth(get_self());
   const int64_t received = wire_balance() - base_balance;
   if (received <= 0) return;   // T5 had nothing to give: base yield only
   const currency_stats st = stat_of(sym);
   if (st.supply.amount <= 0) return;
   distribute(sym, std::min<uint64_t>(static_cast<uint64_t>(received), requested));
}

void liq::linkswept(name account, ChainKind chain_kind, std::vector<char> pubkey) {
   require_auth(AUTHEX_ACCOUNT);
   if (!is_account(account) || !pubkey_fits(chain_kind, pubkey)) return;
   deliver_parked(account, chain_kind, pubkey);
}

void liq::desyndicate(name holder, asset quantity) {
   require_auth(holder);
   check(quantity.is_valid() && quantity.amount > 0, "quantity must be positive");
   const currency_stats st = stat_of(quantity.symbol.code());
   check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

   const ChainKind kind   = kind_of_chain(st.chain_code);
   const auto      pubkey = linked_pubkey(holder, kind);
   check(pubkey.has_value(), "holder is not AuthX-linked for the token's chain");

   // Settle, then burn: the row keeps every subunit of yield accrued to now.
   adjust_account(holder, -quantity, ram_payer);
   stats statstable(get_self());
   statstable.modify(ram_payer, symbol_key{ quantity.symbol.code().raw() }, [&](currency_stats& s) {
      s.supply -= quantity;
   });

   liqcounters_t counters(get_self());
   liq_counters c = counters.get_or_default(liq_counters{});
   const uint64_t request_id = c.next_request_id++;
   counters.set(c, ram_payer);

   opp::attestations::DesyndicateLIQ msg;
   msg.chain_code = st.chain_code.value;
   msg.user       = opp::types::ChainAddress{ kind, *pubkey };
   msg.amount     = opp::types::TokenAmount{ st.token_code.value, quantity.amount };
   msg.request_id = request_id;
   // `no_size{}`: raw protobuf bytes, the form the outpost decodes the attestation
   // `data` field as (the same encoding sysio.opreg's emitters use).
   std::vector<char> encoded;
   auto out = zpp::bits::out{ encoded, zpp::bits::no_size{} };
   (void)out(msg);

   action(active_of(get_self()), MSGCH_ACCOUNT, "queueout"_n,
          std::make_tuple(st.chain_code.value, AttestationType::ATTESTATION_TYPE_DESYNDICATE_LIQ, encoded)).send();
}

// ---------------------------------------------------------------------------
//  Launch ingestion
// ---------------------------------------------------------------------------

void liq::regliqpool(sysio::slug_name chain_code, sysio::slug_name token_code, symbol pair_symbol,
                     uint64_t initial_chain_amount, uint64_t initial_wire_amount, int32_t fee,
                     int64_t locked_shares, uint32_t conversion_horizon_sec, uint32_t depth_cap_bps,
                     int64_t clip_floor) {
   require_priv_caller();
   check(is_bootstrap_window(), "regliqpool is bootstrap-window only");
   const auto st = stat_by_token(token_code);
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
   check(mint(sym.code(), initial_chain_amount), "supply exceeds the asset range");
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

void liq::importsynd(sysio::slug_name chain_code, sysio::slug_name token_code, std::vector<import_credit> credits) {
   require_priv_caller();
   check(is_bootstrap_window(), "importsynd is bootstrap-window only");
   liqconfig_t config(get_self());
   check(!config.get_or_default(liq_config{}).import_complete, "import already finalized");
   const auto st = stat_by_token(token_code);
   check(st.has_value(), "token_code has no shadow symbol");
   check(st->chain_code == chain_code, "token_code belongs to another chain");
   const ChainKind kind = kind_of_chain(chain_code);
   const symbol_code sym = st->supply.symbol.code();

   for (const auto& credit : credits) {
      check(pubkey_fits(kind, credit.pubkey), "pubkey does not fit the chain family");
      if (credit.amount == 0) continue;
      check(mint(sym, credit.amount), "supply exceeds the asset range");
      credit_by_pubkey(sym, kind, credit.pubkey, credit.amount);
   }
}

void liq::importdone() {
   require_priv_caller();
   liqconfig_t config(get_self());
   liq_config cfg = config.get_or_default(liq_config{});
   check(!cfg.import_complete, "import already finalized");
   cfg.import_complete = true;
   config.set(cfg, ram_payer);
}

// ---------------------------------------------------------------------------
//  Internals
// ---------------------------------------------------------------------------

liq::currency_stats liq::stat_of(symbol_code sym) const {
   stats statstable(get_self());
   return statstable.get(symbol_key{ sym.raw() }, "shadow symbol does not exist");
}

std::optional<liq::currency_stats> liq::stat_by_token(sysio::slug_name token_code) const {
   stats statstable(get_self());
   auto by_token = statstable.get_index<"bytoken"_n>();
   auto it = by_token.find(token_code.value);
   if (it == by_token.end()) return std::nullopt;
   return *it;
}

ChainKind liq::kind_of_chain(sysio::slug_name chain_code) const {
   sysio::chains::chains_t chains(CHAINS_ACCOUNT);
   const auto row = chains.try_get(sysio::chains::chain_key{ chain_code });
   check(row.has_value() && row->active && !row->is_depot, "chain_code is not an active outpost");
   return row->kind;
}

u128 liq::current_index(symbol_code sym) const {
   yieldidxs indexes(get_self());
   const auto idx = indexes.try_get(symbol_key{ sym.raw() });
   return idx ? idx->index : 0;
}

int64_t liq::wire_balance() const {
   token::accounts holdings(TOKEN_ACCOUNT, get_self().value);
   const auto row = holdings.try_get(token::acct_key{ WIRE_SYM.code().raw() });
   return row ? row->balance.amount : 0;
}

void liq::settle_and_adjust(opp::shadow::account& row, u128 index, const asset& delta) {
   row.owed_wire        = opp::shadow::owed(row, index);   // settle before mutate
   row.index_checkpoint = index;
   row.balance         += delta;
   check(row.balance.amount >= 0, "overdrawn balance");
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

void liq::adjust_parked(const parked_key& key, ChainKind chain_kind, const std::vector<char>& pubkey,
                        const asset& delta) {
   parkeds parked(get_self());
   const u128 index = current_index(delta.symbol.code());
   const auto row = parked.try_get(key);
   if (!row) {
      check(delta.amount >= 0, "no parked row found");
      parked.emplace(ram_payer, key, parked_row{ chain_kind, pubkey, opp::shadow::account{ delta, index, 0 } });
      return;
   }
   parked_row updated = *row;
   settle_and_adjust(updated.holding, index, delta);
   parked.modify(ram_payer, key, [&](parked_row& p) { p = updated; });
}

bool liq::mint(symbol_code sym, uint64_t quantity) {
   stats statstable(get_self());
   const symbol_key key{ sym.raw() };
   const currency_stats st = statstable.get(key, "shadow symbol does not exist");
   if (quantity > static_cast<uint64_t>(asset::max_amount - st.supply.amount)) return false;
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

bool liq::admit_sequence(sysio::slug_name chain_code, uint64_t sequence, uint64_t epoch) {
   liqcursors cursors(get_self());
   const cursor_key key{ chain_code.value };
   const auto cursor = cursors.try_get(key);
   if (cursor && sequence <= cursor->last_sequence) return false;
   cursors.upsert(ram_payer, key, liq_cursor{ chain_code, sequence, epoch }, [&](liq_cursor& c) {
      c.last_sequence = sequence;
      if (epoch != 0) c.last_epoch = epoch;
   });
   return true;
}

void liq::deliver_parked(name account, ChainKind chain_kind, const std::vector<char>& pubkey) {
   stats statstable(get_self());
   parkeds parked(get_self());
   for (auto it = statstable.begin(); it != statstable.end(); ++it) {
      const symbol_code sym = it->supply.symbol.code();
      const parked_key key = parked_key_of(sym, chain_kind, pubkey);
      const auto row = parked.try_get(key);
      if (!row) continue;
      // Settle the parked row at the current index, then move both its balance
      // and its banked WIRE into the account's row, itself settled at that index.
      const u128     index  = current_index(sym);
      const uint64_t banked = opp::shadow::owed(row->holding, index);
      const asset    balance = row->holding.balance;
      parked.erase(key);
      adjust_account(account, balance, ram_payer);
      if (banked == 0) continue;
      accounts holdings(get_self(), account.value);
      holdings.modify(ram_payer, symbol_key{ sym.raw() }, [&](opp::shadow::account& a) {
         a.owed_wire = opp::safe::add_sat_u64(a.owed_wire, banked);
      });
   }
}

void liq::credit_by_pubkey(symbol_code sym, ChainKind chain_kind, const std::vector<char>& pubkey, uint64_t amount) {
   const asset quantity{ static_cast<int64_t>(amount), stat_of(sym).supply.symbol };
   const name account = linked_account(chain_kind, pubkey);
   if (account != name{}) {
      adjust_account(account, quantity, ram_payer);
   } else {
      adjust_parked(parked_key_of(sym, chain_kind, pubkey), chain_kind, pubkey, quantity);
   }
}

} // namespace sysio
