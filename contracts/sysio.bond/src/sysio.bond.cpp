#include <sysio.bond/sysio.bond.hpp>
#include <sysio.andon/sysio.andon.hpp>

#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/privileged.hpp>
#include <sysio/system.hpp>
#include <sysio.opp.common/depot_native_token.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio.opp.common/shadow_custody.hpp>
#include <sysio.opp.common/wire_asset.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace sysio {

namespace {

constexpr name settle_action = "settle"_n;
constexpr auto request_is_not_resolved_msg = "request is not resolved";


using opp::custody::depot_native_token;
using opp::custody::pull_depot_native;
using opp::custody::transfer_depot_native;
using opp::shadow::u128;

namespace custody = opp::shadow::custody;

// The contract's own rows -- `bondconfig`, `bondcounters`, `yieldpool` -- bill the sysio RAM pool,
// not this contract account (privileged-contract model, as sysio.opreg uses): the account stays
// finite at code+abi size. A request's rows bill the account that created them (`payer_for`): the
// issuer its `requests` and `escrows` rows, an underwriter its `bonds` row. Later writes by any signer
// keep the row's payer (`same_payer`), which a privileged contract may do.
constexpr name ram_payer = bond::SYSTEM_ACCOUNT;

/// Permission this contract sends its own `sysio.liq::addyield` under.
constexpr name active_permission = "active"_n;

/// Memo on the transfer that pulls a `request`'s bounty into escrow.
constexpr std::string_view request_bounty_memo = "sysio.bond::request bounty";

/// Memo on the transfer that pulls an `addbounty` raise into escrow.
constexpr std::string_view addbounty_memo = "sysio.bond::addbounty bounty";

/// Memo on the transfer that pulls an `accept` bond into custody.
constexpr std::string_view accept_memo = "sysio.bond::accept";

/// Memo on the transfer that pulls a `hold` bond into escrow.
constexpr std::string_view hold_memo = "sysio.bond::hold";

/// Memo on the ONE transfer `claim` pays the issuer an INVALID request's bonds in. Part of the
/// interface: an issuer contract recognizes the forfeit by it.
constexpr std::string_view forfeit_memo = "sysio.bond::forfeit";

/// Memo on the transfer that pays a claim in the request's token: a bond and its shares, and what a
/// ruling awarded the account out of the escrows.
constexpr std::string_view claim_memo = "sysio.bond::claim";

/// Memo on the transfer that pays the WIRE a claimed bond or escrow earned.
constexpr std::string_view claim_yield_memo = "sysio.bond::claim yield";

/// Message of the `check` a covered or bonded amount off the token's increment raises.
using bond_validation::bond_increment_msg;

/// Message of the `check` a token with fewer than BOND_INCREMENT_DECIMALS decimals raises.
using bond_validation::precision_msg;

/// Message of the `check` a token code that is neither WIRE nor a shadow LIQ symbol raises.
using bond_validation::unsupported_token_msg;

/// Message of the `check` a statement longer than MAX_STATEMENT_BYTES raises.
using bond_validation::statement_len_msg;

/// Message of the `check` a second request for the same issuer and statement raises.
using bond_validation::duplicate_msg;

/// Message of the `check` a zero challenge window raises.
using bond_validation::window_msg;

/// Message of the `check` `setconfig` raises for a hold percentage of zero or above one whole.
constexpr std::string_view hold_bps_range_msg = "hold_bps must be positive and at most the basis-point denominator";

/// Message of the `check` a zero amount raises where a positive one is required.
constexpr std::string_view amount_positive_msg = "amount must be positive";

/// Message of the `check` an amount no `asset` can carry raises.
using bond_validation::amount_range_msg;

/// Message of the `check` an action naming an unknown request raises.
constexpr std::string_view request_missing_msg = "request not found";

/// Message of the `check` an action that needs a live request raises on a terminal one.
constexpr std::string_view terminal_msg = "request is already resolved";

/// Message of the `check` `claim` raises when the request owes the account nothing, or is not
/// terminal yet.
constexpr std::string_view nothing_msg = "nothing to claim";

/// Message of the `check` `sweepyield` raises for WIRE, which is not a shadow token.
constexpr std::string_view wire_no_yield_msg = "WIRE collateral earns no shadow yield";

/// Message of the `check` `accept` raises on a request in any state but OPEN and BONDED.
constexpr std::string_view not_accepting_msg = "request is not accepting bonds";

/// Message of the `check` `accept` raises once bonded has reached covered.
constexpr std::string_view fully_bonded_msg = "request is fully bonded";

/// Message of the `check` `approve` raises on a request in any state but BONDED.
constexpr std::string_view not_bonded_msg = "request is not bonded";

/// Message of the `check` `approve` raises while the challenge window still runs.
constexpr std::string_view window_open_msg = "challenge window has not passed";

/// Message of the `check` `hold` raises for a beneficiary that is not an account.
constexpr std::string_view beneficiary_msg = "hold beneficiary is not an account";

/// Message of the `check` `hold` raises for this contract named as the beneficiary.
constexpr std::string_view beneficiary_self_msg = "hold beneficiary cannot be the contract";

/// Message of the `check` `hold` raises on a request in any state but OPEN and BONDED.
constexpr std::string_view hold_state_msg = "hold is only possible while the request is OPEN or BONDED";

/// Largest amount an `asset` carries, as the unsigned base units every row holds.
constexpr uint64_t max_asset_amount = static_cast<uint64_t>(asset::max_amount);

/// The account a row created on behalf of `account` bills: `account` itself, unless it is privileged.
/// A privileged account is a system contract (`sysio.synd` issuing requests for its envelopes), and a
/// system contract has no RAM quota of its own -- it lives on what `sysio.roa` gifts it for its code
/// and abi -- so its rows bill the `sysio` RAM pool, as every system contract's own rows do. The ONE
/// place that decides the payer of a request, escrow or bond row.
name payer_for(name account) {
   return is_privileged(account) ? ram_payer : account;
}

/// Resolve `token_code` against this contract's custody contracts: `sysio.token` for WIRE,
/// `sysio.liq` for a shadow LIQ symbol. `std::nullopt` for any other code.
std::optional<depot_native_token> resolve_bond_token(sysio::slug_name token_code) {
   return opp::custody::resolve_depot_native_token(bond::LIQ_ACCOUNT, bond::TOKEN_ACCOUNT, token_code);
}

/// True iff `amount` is a positive multiple of `increment`.
bool is_increment_multiple(uint64_t amount, uint64_t increment) {
   return amount > 0 && amount % increment == 0;
}

/// True iff `state` is APPROVED, VALID or INVALID: no action moves a request out of it.
bool is_terminal(bond::request_state state) {
   return state == bond::request_state::APPROVED || state == bond::request_state::VALID ||
          state == bond::request_state::INVALID;
}

/// True iff a sub-holder balance of `token_code` earns shadow yield: any bond token but WIRE. The
/// ONE place that decides it; every yield path asks here.
bool earns_shadow_yield(sysio::slug_name token_code) {
   return token_code != opp::wire::token_code;
}

/// Credit `amount` to a sub-holder `balance` of `token_code` that carries `pos`: through the
/// shadow-custody funnel for a shadow token, so the position settles at the old balance first; a
/// plain saturating add for WIRE, which earns nothing. The caller has checked the result stays
/// within the asset range.
void credit_holding(opp::shadow::custody::position& pos, uint64_t& balance, uint64_t amount,
                    sysio::slug_name token_code, const depot_native_token& custody) {
   if (earns_shadow_yield(token_code)) {
      opp::shadow::custody::settle_and_adjust(pos, balance, static_cast<int64_t>(amount), bond::LIQ_ACCOUNT,
                                              custody.sym.code());
   } else {
      balance = opp::safe::add_sat_u64(balance, amount);
   }
}

/// The custody of WIRE, the token every yield payout is made in.
depot_native_token wire_custody() {
   return depot_native_token{.contract = bond::TOKEN_ACCOUNT, .sym = opp::wire::asset_symbol};
}

/// `sysio.liq`'s live yield index for the shadow token of `custody`; 0 for WIRE, which earns
/// nothing.
u128 live_index_of(sysio::slug_name token_code, const depot_native_token& custody) {
   return earns_shadow_yield(token_code) ? custody::live_index(bond::LIQ_ACCOUNT, custody.sym.code()) : u128{0};
}

/// Lower a sub-holder `balance` of `token_code` by `amount`: for a shadow token its position is
/// settled at `index` first, so it keeps what the old balance earned up to that index; for WIRE a
/// plain subtraction. Clamps at zero; callers never debit more than the balance holds.
void debit_holding(custody::position& pos, uint64_t& balance, uint64_t amount, sysio::slug_name token_code,
                   u128 index) {
   if (earns_shadow_yield(token_code)) {
      custody::settle_and_adjust(pos, balance, -static_cast<int64_t>(amount), index);
   } else {
      balance = balance > amount ? balance - amount : 0;
   }
}

/// Lower request `request_id`'s escrow of `kind` by `amount`, settled at `index`. No row -- no bounty
/// was posted, or a hold bond rounded to zero -- or a zero amount changes nothing.
void debit_escrow(name self, uint64_t request_id, bond::escrow_kind kind, uint64_t amount,
                  sysio::slug_name token_code, u128 index) {
   if (amount == 0) return;
   bond::escrows_t        escrows(self);
   const bond::escrow_key key = bond::escrow_key_of(request_id, kind);
   auto                   row = escrows.try_get(key);
   if (!row) return;
   debit_holding(row->yield, row->amount, amount, token_code, index);
   escrows.set(same_payer, key, *row);
}

/// Record that a ruling awards `payout` base units of request `request_id`'s escrow of `kind` to
/// `payee`, to claim. No row, or a zero payout, records nothing.
void award_escrow(name self, uint64_t request_id, bond::escrow_kind kind, name payee, uint64_t payout) {
   if (payout == 0) return;
   bond::escrows_t        escrows(self);
   const bond::escrow_key key = bond::escrow_key_of(request_id, kind);
   auto                   row = escrows.try_get(key);
   if (!row) return;
   row->payee  = payee;
   row->payout = payout;
   escrows.set(same_payer, key, *row);
}

/// Add `amount` to the running total `total` of one claim transfer, refusing a total no `asset` can
/// carry.
void add_to_transfer(uint64_t& total, uint64_t amount) {
   check(amount <= max_asset_amount - total, amount_range_msg);
   total += amount;
}

/// The WIRE a position of `balance` units is owed once settled at `index`, without changing it; 0
/// for WIRE, which earns nothing.
uint64_t owed_at(custody::position pos, uint64_t balance, sysio::slug_name token_code, u128 index) {
   if (!earns_shadow_yield(token_code)) return 0;
   custody::settle(pos, balance, index);
   return pos.owed_wire;
}

/// True iff the shadow token `token_code` has a positive supply on `sysio.liq`: `addyield` refuses a
/// symbol without one.
bool has_supply(sysio::slug_name token_code) {
   const auto stat = liq::find_stat_by_token(bond::LIQ_ACCOUNT, token_code);
   return stat.has_value() && stat->supply.amount > 0;
}

/// The yield pool row of `token_code`, or a fresh one before its first pull.
bond::pool_row pool_of(name self, sysio::slug_name token_code) {
   bond::yieldpools_t pools(self);
   return pools.try_get(bond::pool_key{token_code.value})
      .value_or(bond::pool_row{.token_code = token_code, .pool = custody::yield_pool{}});
}

/// True iff no bond of terminal request `req` is still owed a share of its escrows: an INVALID
/// request pays the underwriters no share, and on an APPROVED or VALID one every bond row is paid.
bool shares_settled(name self, const bond::request_row& req) {
   if (req.state == bond::request_state::INVALID) return true;
   bond::bonds_t bonds(self);
   for (auto it = bonds.lower_bound(bond::bond_key{.request_id = req.id, .underwriter = name{}});
        it != bonds.end() && it->request_id == req.id; ++it) {
      if (!it->paid) return false;
   }
   return true;
}

/// True iff nothing is left to claim on terminal request `req`, `live_index` being the token's live
/// yield index. The forfeit must be claimed, and every bond row settled: a returned bond (APPROVED,
/// VALID) is owed its principal until claimed, a forfeited bond only the WIRE it earned up to
/// `resolved_index`. An escrow must have its payout claimed; after that it holds at most the division
/// remainder, and it is done once paid or owed nothing now. What that remainder earns after the prune
/// is slack.
bool fully_paid(name self, const bond::request_row& req, u128 live_index) {
   if (req.forfeit_pending > 0) return false;
   const bool    returns_bonds = req.state != bond::request_state::INVALID;
   bond::bonds_t bonds(self);
   for (auto it = bonds.lower_bound(bond::bond_key{.request_id = req.id, .underwriter = name{}});
        it != bonds.end() && it->request_id == req.id; ++it) {
      if (it->yield.owed_wire > 0) return false;
      if (it->paid) continue;
      if (returns_bonds || owed_at(it->yield, it->amount, req.token_code, req.resolved_index) > 0) return false;
   }
   bond::escrows_t escrows(self);
   for (const auto kind : magic_enum::enum_values<bond::escrow_kind>()) {
      const auto row = escrows.try_get(bond::escrow_key_of(req.id, kind));
      if (!row) continue;
      if (row->payout > 0 || row->yield.owed_wire > 0) return false;
      if (!row->paid && owed_at(row->yield, row->amount, req.token_code, live_index) > 0) return false;
   }
   return true;
}

/// Place `amount` of `token_code` from `funder`, the signing issuer, in request `request_id`'s escrow
/// of `kind`: create the row at balance 0 or raise the existing one through `credit_holding`, then
/// pull the tokens. The row bills `payer_for(funder)`.
void escrow_tokens(name self, uint64_t request_id, bond::escrow_kind kind, name funder, uint64_t amount,
                   sysio::slug_name token_code, const depot_native_token& custody, std::string_view memo) {
   bond::escrows_t        escrows(self);
   const bond::escrow_key key = bond::escrow_key_of(request_id, kind);
   bond::escrow_row       row = escrows.try_get(key).value_or(
      bond::escrow_row{.request_id = request_id, .kind = kind, .funder = funder, .amount = 0});
   check(amount <= max_asset_amount - row.amount, amount_range_msg);
   credit_holding(row.yield, row.amount, amount, token_code, custody);
   escrows.set(payer_for(funder), key, row);
   pull_depot_native(self, funder, custody, amount, memo);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

void bond::setconfig(uint32_t hold_bps) {
   require_auth(SYSTEM_ACCOUNT);
   check(hold_bps > 0 && hold_bps <= BPS_DENOMINATOR, hold_bps_range_msg);
   bondconfig_t config(get_self());
   bond_config  cfg = config.get_or_default(bond_config{});
   cfg.hold_bps     = hold_bps;
   config.set(cfg, ram_payer);
}

// ---------------------------------------------------------------------------
//  Requests
// ---------------------------------------------------------------------------

void bond::request(name issuer, name schema, std::vector<char> statement, sysio::slug_name token_code,
                   uint64_t covered, uint64_t bounty, uint32_t window_sec) {
   require_auth(issuer);
   const auto refusal = request_refusal(get_self(), issuer, schema, statement, token_code, covered, bounty, window_sec);
   check(refusal.empty(), refusal);
   const auto custody = resolve_bond_token(token_code);

   request_row row{
      .id               = 0,
      .issuer           = issuer,
      .schema           = schema,
      .statement        = std::move(statement),
      .statement_digest = checksum256{},
      .token_code       = token_code,
      .covered          = covered,
      .bonded           = 0,
      .bounty           = bounty,
      .window_sec       = window_sec,
      .state            = request_state::OPEN,
      .created_at       = current_time_point(),
      .bonded_at        = time_point{},
      .hold_bond        = 0,
      .hold_beneficiary = name{},
      .held_at          = time_point{},
      .resolved_at      = time_point{},
      .resolved_index   = 0,
      .forfeit_pending  = 0,
   };
   row.statement_digest = statement_digest_of(schema, row.statement);

   requests_t requests(get_self());
   const auto by_statement = requests.get_index<STATEMENT_INDEX>();
   check(by_statement.find(row.by_statement()) == by_statement.end(), duplicate_msg);

   bondcounters_t counters(get_self());
   bond_counters  c = counters.get_or_default(bond_counters{});
   row.id           = c.next_request_id++;
   counters.set(c, ram_payer);

   requests.emplace(payer_for(issuer), request_key{row.id}, row);

   if (bounty > 0) {
      escrow_tokens(get_self(), row.id, escrow_kind::BOUNTY, issuer, bounty, token_code, *custody,
                    request_bounty_memo);
   }
}

void bond::requestkeep(name issuer, name schema, std::vector<char> statement, sysio::slug_name token_code,
                       uint64_t covered, uint64_t bounty, uint32_t window_sec) {
   request(issuer, schema, statement, token_code, covered, bounty, window_sec);
   const auto row = find_request(get_self(), issuer, schema, statement);
   requests_t(get_self()).modify(same_payer, request_key{row->id}, [](request_row& r) {
      r.outcome_acknowledged = false;
   });
}

void bond::ack(uint64_t request_id) {
   requests_t requests(get_self());
   const auto row = requests.try_get(request_key{request_id});
   check(row.has_value(), request_missing_msg);
   require_auth(row->issuer);
   check(is_terminal(row->state), request_is_not_resolved_msg);
   requests.modify(same_payer, request_key{request_id}, [](request_row& r) { r.outcome_acknowledged = true; });
}

void bond::addbounty(uint64_t request_id, uint64_t amount) {
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   require_auth(req->issuer);
   check(amount > 0, amount_positive_msg);
   check(!is_terminal(req->state), terminal_msg);
   check(amount <= max_asset_amount - req->bounty, amount_range_msg);

   const auto custody = resolve_bond_token(req->token_code);
   check(custody.has_value(), unsupported_token_msg);

   requests.modify(same_payer, request_key{request_id}, [&](request_row& r) { r.bounty += amount; });
   escrow_tokens(get_self(), request_id, escrow_kind::BOUNTY, req->issuer, amount, req->token_code, *custody,
                 addbounty_memo);
}

void bond::accept(name underwriter, uint64_t request_id, uint64_t amount) {
   require_auth(underwriter);
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   // A BONDED request is exactly one whose bonded amount reached covered; every other state but
   // OPEN has left the bonding phase.
   check(req->state != request_state::BONDED, fully_bonded_msg);
   check(req->state == request_state::OPEN, not_accepting_msg);

   const auto custody = resolve_bond_token(req->token_code);
   check(custody.has_value(), unsupported_token_msg);
   const auto increment = increment_of(custody->sym);
   check(increment.has_value(), precision_msg);
   check(is_increment_multiple(amount, *increment), bond_increment_msg);

   // covered and bonded are both multiples of the increment, so the remainder is too: the reduced
   // amount stays on the increment.
   const uint64_t remainder = req->covered - req->bonded;
   check(remainder > 0, fully_bonded_msg);
   const uint64_t bonded_amount = std::min(amount, remainder);

   // bonded_amount <= covered <= max_asset_amount, and a row never exceeds its request's bonded,
   // so the credited balance stays within the asset range.
   bonds_t        bonds(get_self());
   const bond_key key{.request_id = request_id, .underwriter = underwriter};
   bond_row       row = bonds.try_get(key).value_or(
      bond_row{.request_id = request_id, .underwriter = underwriter, .amount = 0});
   credit_holding(row.yield, row.amount, bonded_amount, req->token_code, *custody);
   bonds.set(payer_for(underwriter), key, row);

   const bool fully_bonded = bonded_amount == remainder;
   requests.modify(same_payer, request_key{request_id}, [&](request_row& r) {
      r.bonded += bonded_amount;
      if (fully_bonded) {
         r.state     = request_state::BONDED;
         r.bonded_at = current_time_point();
      }
   });

   pull_depot_native(get_self(), underwriter, *custody, bonded_amount, accept_memo);
}

void bond::hold(uint64_t request_id, name beneficiary) {
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   require_auth(req->issuer);
   // A HELD request fails here too, so a second hold is refused before it charges anything.
   check(req->state == request_state::OPEN || req->state == request_state::BONDED, hold_state_msg);
   check(is_account(beneficiary), beneficiary_msg);
   check(beneficiary != get_self(), beneficiary_self_msg);

   const auto custody = resolve_bond_token(req->token_code);
   check(custody.has_value(), unsupported_token_msg);

   // With no config row the default carries DEFAULT_HOLD_BPS. hold_bps <= BPS_DENOMINATOR, so the
   // bond is at most `covered` and fits the asset range.
   bondconfig_t   config(get_self());
   const uint32_t hold_bps  = config.get_or_default(bond_config{}).hold_bps;
   const uint64_t hold_bond = hold_bond_of(req->covered, hold_bps);

   requests.modify(same_payer, request_key{request_id}, [&](request_row& r) {
      r.hold_bond        = hold_bond;
      r.hold_beneficiary = beneficiary;
      r.held_at          = current_time_point();
      r.state            = request_state::HELD;
   });
   // A hold bond rounds to zero on a covered amount below BPS_DENOMINATOR / hold_bps base units:
   // there is nothing to escrow or pull, and no HOLD_BOND row exists.
   if (hold_bond > 0) {
      escrow_tokens(get_self(), request_id, escrow_kind::HOLD_BOND, req->issuer, hold_bond, req->token_code,
                    *custody, hold_memo);
   }
}

void bond::approve(uint64_t request_id) {
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   check(req->state == request_state::BONDED, not_bonded_msg);
   const time_point now = current_time_point();
   check(now >= req->bonded_at + seconds(req->window_sec), window_open_msg);

   requests.modify(same_payer, request_key{request_id}, [&](request_row& r) {
      r.state       = request_state::APPROVED;
      r.resolved_at = now;
   });
}

// rslvvalid - the statement is true. Every bond is returned through `claim` with its stake's share
// of the bounty and of the hold bond. The shares no stake covers are recorded on the escrows for
// their payees to claim: the bounty's for `sysio`, the hold bond's for the issuer who posted it.
// Nothing is sent and no one is notified, so no party can make the ruling fail.
void bond::rslvvalid(uint64_t request_id) {
   require_auth(SYSTEM_ACCOUNT);
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   check(!is_terminal(req->state), terminal_msg);

   requests.modify(same_payer, request_key{request_id}, [&](request_row& r) {
      r.state       = request_state::VALID;
      r.resolved_at = current_time_point();
   });

   const uint64_t unbonded = req->covered - req->bonded;
   award_escrow(get_self(), request_id, escrow_kind::BOUNTY, SYSTEM_ACCOUNT,
                bond::share_of(unbonded, req->bounty, req->covered));
   award_escrow(get_self(), request_id, escrow_kind::HOLD_BOND, req->issuer,
                bond::share_of(unbonded, req->hold_bond, req->covered));
}

// rslvinvalid - the statement is false. Every bond becomes the issuer's forfeit, recorded on the
// request as ONE amount the issuer claims in one transfer, whatever the number of underwriters.
// After a hold the challenger named as beneficiary is awarded the hold bond and the bounty; without
// one the bounty is awarded back to the issuer. Nothing is sent and no one is notified. The bonds'
// positions stop at the ruling's index: the forfeit is no longer theirs.
void bond::rslvinvalid(uint64_t request_id) {
   require_auth(SYSTEM_ACCOUNT);
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   check(!is_terminal(req->state), terminal_msg);
   const auto custody = resolve_bond_token(req->token_code);
   check(custody.has_value(), unsupported_token_msg);

   const u128 index = live_index_of(req->token_code, *custody);
   const bool held  = req->state == request_state::HELD;
   requests.modify(same_payer, request_key{request_id}, [&](request_row& r) {
      r.state           = request_state::INVALID;
      r.resolved_at     = current_time_point();
      r.resolved_index  = index;
      r.forfeit_pending = r.bonded;
   });

   const name bounty_payee = held ? req->hold_beneficiary : req->issuer;
   award_escrow(get_self(), request_id, escrow_kind::BOUNTY, bounty_payee, req->bounty);
   if (held) award_escrow(get_self(), request_id, escrow_kind::HOLD_BOND, req->hold_beneficiary, req->hold_bond);
}

// ---------------------------------------------------------------------------
//  Yield and payouts
// ---------------------------------------------------------------------------

// sweepyield - pull the WIRE sysio.liq owes this contract's holder row into the token's pool.
//
// Permissionless: it only moves WIRE the contract is already owed into the contract, and changes no
// position's entitlement -- only what the pool can cover. `custody::pull` records exactly what the
// claim it sends pays, or with nothing owed does neither.
void bond::sweepyield(sysio::slug_name token_code) {
   const auto custody_token = resolve_bond_token(token_code);
   check(custody_token.has_value(), unsupported_token_msg);
   check(earns_shadow_yield(token_code), wire_no_yield_msg);

   pool_row pool = pool_of(get_self(), token_code);
   if (custody::pull(pool.pool, LIQ_ACCOUNT, get_self(), custody_token->sym.code()) > 0) {
      yieldpools_t(get_self()).set(ram_payer, pool_key{token_code.value}, pool);
   }
}

// claim - pay `account` what terminal request `request_id` owes it.
//
// Each account settles independently. LIQ credits invoke no recipient code. Earned WIRE is
// recorded into a backed claim balance and withdrawn separately; WIRE-denominated requests keep
// ordinary transfer claims.
//
// A returned bond and an escrow are still held here, so they settle at sysio.liq's LIVE index: what
// they earn up to the claim is their owner's. A forfeited bond settles at the request's
// `resolved_index`. The pool is pulled first, before anything else this action queues touches
// sysio.liq (the custody library's obligation 3), so what the rows take is covered by WIRE that
// arrives ahead of the payouts queued after it. The rows are marked paid before any transfer is
// queued, so a transfer-notify handler that re-enters `claim` finds nothing left. What the pool
// cannot cover stays owed on the original row and prevents pruning until paid.
//
// An escrow keeps earning while it holds shares or a payout not yet claimed, so its funder may claim
// more than once; the funder's claim marks it paid once no bond is owed a share of it and its payout
// is claimed, whatever division remainder it still holds.
void bond::claim(uint64_t request_id, name account) {
   // Every payout leaves custody: deferred, by refusal, until the cord clears. Nothing a ruling awards is
   // lost -- it stays recorded until claimed.
   andon::check_clear(andon::ANDON_ACCOUNT);
   requests_t requests(get_self());
   const auto req = requests.try_get(request_key{request_id});
   check(req.has_value(), request_missing_msg);
   check(is_terminal(req->state), nothing_msg);
   const auto custody_token = resolve_bond_token(req->token_code);
   check(custody_token.has_value(), unsupported_token_msg);

   const bool        shadow  = earns_shadow_yield(req->token_code);
   const bool        forfeit = req->state == request_state::INVALID;
   const u128        live    = live_index_of(req->token_code, *custody_token);
   const symbol_code sym     = custody_token->sym.code();
   pool_row          pool    = pool_of(get_self(), req->token_code);
   if (shadow) custody::pull(pool.pool, LIQ_ACCOUNT, get_self(), sym);

   bool     found        = false;
   uint64_t forfeit_out  = 0;   // an INVALID request's bonds, to its issuer
   uint64_t token_out    = 0;   // bond, shares and escrow payouts, in the request's token
   uint64_t wire_out     = 0;   // yield of the bond and of escrows the account funded
   uint64_t forfeit_wire = 0;   // yield of a forfeited bond, paid into sysio.liq

   if (forfeit && account == req->issuer && req->forfeit_pending > 0) {
      forfeit_out = req->forfeit_pending;
      requests.modify(same_payer, request_key{request_id}, [](request_row& r) { r.forfeit_pending = 0; });
      found = true;
   }

   bonds_t        bonds(get_self());
   const bond_key bkey{.request_id = request_id, .underwriter = account};
   if (auto row = bonds.try_get(bkey); row && (!row->paid || row->yield.owed_wire > 0)) {
      uint64_t earned = 0;
      bool     earns  = false;
      if (shadow) {
         if (!row->paid) custody::settle(row->yield, row->amount, forfeit ? req->resolved_index : live);
         earns  = row->yield.owed_wire > 0;
         earned = custody::take(row->yield, pool.pool);
      }
      bool paid = false;
      if (row->paid) {
         if (forfeit) forfeit_wire = earned;
         else wire_out = earned;
         paid = earned > 0;
      } else if (!forfeit) {
         const uint64_t bounty_share    = bond::share_of(row->amount, req->bounty, req->covered);
         const uint64_t hold_bond_share = bond::share_of(row->amount, req->hold_bond, req->covered);
         add_to_transfer(token_out, row->amount);
         add_to_transfer(token_out, bounty_share);
         add_to_transfer(token_out, hold_bond_share);
         wire_out = earned;
         debit_escrow(get_self(), request_id, escrow_kind::BOUNTY, bounty_share, req->token_code, live);
         debit_escrow(get_self(), request_id, escrow_kind::HOLD_BOND, hold_bond_share, req->token_code, live);
         paid = true;
      } else if (earns) {
         forfeit_wire = earned;
         paid         = true;
      }
      if (paid) {
         row->paid = true;
         bonds.set(same_payer, bkey, *row);
         found = true;
      }
   }

   // A ruling's award out of an escrow is paid to its payee; an escrow owes its funder only the
   // yield it earned, nothing for WIRE. Read after the bond above, so its own claim counts toward
   // `shares_settled`.
   escrows_t escrows(get_self());
   for (const auto kind : magic_enum::enum_values<escrow_kind>()) {
      const escrow_key ekey = bond::escrow_key_of(request_id, kind);
      auto             row  = escrows.try_get(ekey);
      if (!row) continue;
      bool changed = false;
      if (row->payee == account && row->payout > 0) {
         add_to_transfer(token_out, row->payout);
         debit_holding(row->yield, row->amount, row->payout, req->token_code, live);
         row->payout = 0;
         changed     = true;
      }
      if (shadow && (!row->paid || row->yield.owed_wire > 0) && row->funder == account) {
         if (!row->paid) custody::settle(row->yield, row->amount, live);
         if (row->yield.owed_wire > 0) {
            add_to_transfer(wire_out, custody::take(row->yield, pool.pool));
            row->paid = row->payout == 0 && shares_settled(get_self(), *req);
            changed   = true;
         }
      }
      if (changed) {
         escrows.set(same_payer, ekey, *row);
         found = true;
      }
   }
   check(found, nothing_msg);
   if (shadow) yieldpools_t(get_self()).set(ram_payer, pool_key{req->token_code.value}, pool);

   const auto pay = [&](uint64_t amount, std::string_view memo) {
      if (amount == 0) return;
      if (shadow) {
         action(permission_level{get_self(), active_permission}, LIQ_ACCOUNT, settle_action,
                std::make_tuple(get_self(), account, asset{static_cast<int64_t>(amount), custody_token->sym})).send();
      } else {
         transfer_depot_native(get_self(), account, *custody_token, amount, memo);
      }
   };
   pay(forfeit_out, forfeit_memo);
   pay(token_out, claim_memo);
   if (wire_out > 0) {
      wireclaims_t claims(get_self());
      const wire_key key{account};
      auto balance = claims.try_get(key).value_or(wire_claim{account, 0});
      add_to_transfer(balance.amount, wire_out);
      claims.set(ram_payer, key, balance);
   }
   // sysio.liq refuses a distribution to a symbol with no supply: the WIRE then stays in the contract.
   if (forfeit_wire > 0 && has_supply(req->token_code)) {
      action(permission_level{get_self(), active_permission}, LIQ_ACCOUNT, opp::shadow::ADDYIELD_ACTION,
             std::make_tuple(get_self(), asset(static_cast<int64_t>(forfeit_wire), opp::wire::asset_symbol), sym))
         .send();
   }
}

void bond::claimwire(name account) {
   andon::check_clear(andon::ANDON_ACCOUNT);
   wireclaims_t claims(get_self());
   const wire_key key{account};
   const auto balance = claims.try_get(key);
   check(balance && balance->amount > 0, nothing_msg);
   claims.erase(key);
   transfer_depot_native(get_self(), account, wire_custody(), balance->amount, claim_yield_memo);
}

// prune - erase terminal requests nothing is owed on, with their rows, and paid bond rows.
//
// Walks the requests in id order from `from_id` and acts on up to `limit` of them. A request still owed
// a claim, or ruled less than PRUNE_RETENTION_SEC ago, keeps its row -- an issuer that polls the row
// (sysio.bond notifies no one) always sees the ruling -- but its bond rows already paid out are erased:
// a paid row carries nothing any claim or check reads, and an underwriter's RAM must not wait on
// another party's unclaimed payout. A request whose token no longer resolves (so nothing about it can
// be settled) is skipped and left whole. What an erased escrow still holds is the division remainder,
// which stays in the contract.
void bond::prune(uint64_t from_id, uint32_t limit) {
   requests_t            requests(get_self());
   bonds_t               bonds(get_self());
   const time_point      now = current_time_point();
   std::vector<uint64_t> prunable;
   std::vector<bond_key> paid_bonds;
   uint32_t              acted = 0;
   for (auto it = requests.lower_bound(request_key{from_id}); it != requests.end() && acted < limit; ++it) {
      if (!is_terminal(it->state)) continue;
      const auto custody_token = resolve_bond_token(it->token_code);
      if (!custody_token) continue;
      const bool retained = now < it->resolved_at + seconds(PRUNE_RETENTION_SEC);
      if (!retained && it->outcome_acknowledged && fully_paid(get_self(), *it, live_index_of(it->token_code, *custody_token))) {
         prunable.push_back(it->id);
         ++acted;
         continue;
      }
      const size_t before = paid_bonds.size();
      for (auto bit = bonds.lower_bound(bond_key{.request_id = it->id, .underwriter = name{}});
           bit != bonds.end() && bit->request_id == it->id; ++bit) {
         if (bit->paid && bit->yield.owed_wire == 0) paid_bonds.push_back(bond_key{.request_id = it->id, .underwriter = bit->underwriter});
      }
      if (paid_bonds.size() != before) ++acted;
   }

   for (const bond_key& key : paid_bonds) bonds.erase(key);
   escrows_t escrows(get_self());
   for (const uint64_t id : prunable) {
      for (auto it = bonds.lower_bound(bond_key{.request_id = id, .underwriter = name{}});
           it != bonds.end() && it->request_id == id;) {
         it = bonds.erase(it);
      }
      for (const auto kind : magic_enum::enum_values<escrow_kind>()) {
         const escrow_key ekey = bond::escrow_key_of(id, kind);
         if (escrows.contains(ekey)) escrows.erase(ekey);
      }
      requests.erase(request_key{id});
   }
}

} // namespace sysio
