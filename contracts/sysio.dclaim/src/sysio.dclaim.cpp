#include <sysio.dclaim/sysio.dclaim.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sysio {

namespace {

using opp::types::ChainKind;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

/// Exact-match scan over a uint128 secondary index: `lower_bound` then walk
/// while the narrowing key still matches, returning the first row the
/// predicate accepts (or `idx.end()`). The uint128 key only narrows; the
/// predicate resolves prefix collisions deterministically for unmapped-credit lookups.
template<class Index, class KeyFn, class MatchFn>
auto scan_find(Index& idx, uint128_t key, KeyFn key_of, MatchFn matches) {
   auto it = idx.lower_bound(key);
   for (; it != idx.end() && key_of(*it) == key; ++it) {
      if (matches(*it)) break;
   }
   if (it != idx.end() && key_of(*it) == key && matches(*it)) return it;
   return idx.end();
}

/// Allocate the next id from one of the monotonic counters. `pick` returns a
/// reference to the field to bump.
template<class Pick>
uint64_t next_id(name self, Pick pick) {
   dclaim::capcounters_t cnt(self);
   dclaim::cap_counters c = cnt.get_or_default(dclaim::cap_counters{});
   uint64_t& field = pick(c);
   uint64_t id = field++;
   cnt.set(c, ram_payer);
   return id;
}

/// Saturating WIRE credit. `asset::operator+=` aborts on overflow past `asset::max_amount`
/// (2^62-1). AuthX linkswept is reachable through OPP node-owner registration and must not
/// abort that delivery. Both imported and swept balances preserve saturation at the asset maximum.
inline void add_wire_capped(asset& balance, const asset& amt) {
   const int64_t room = asset::max_amount - balance.amount;   // balance.amount in [0, max_amount]
   balance.amount += (amt.amount <= room ? amt.amount : room);
}

/// Credit a pending account balance that remains claimable indefinitely.
void credit_pending(name self, name wacct, const asset& amt) {
   dclaim::pclaims_t pclaims(self);
   auto it = pclaims.find(dclaim::pclaim_key{wacct.value});
   if (it == pclaims.end()) {
      pclaims.emplace(ram_payer, dclaim::pclaim_key{wacct.value},
         dclaim::pending_claim{ .wire_account = wacct,
                               .balance = amt });
   } else {
      pclaims.modify(same_payer, dclaim::pclaim_key{wacct.value}, [&](auto& r) {
         add_wire_capped(r.balance, amt);
      });
   }
}

/// Park an imported WIRE credit by native identity until AuthX links it to a claim account.
void credit_unmapped(name self, ChainKind chain,
                     const std::vector<char>& addr, const asset& amt) {
   dclaim::unmapped_t unmapped(self);
   auto idx = unmapped.template get_index<"bychainad"_n>();
   auto it = scan_find(idx, dclaim::chain_addr_key(chain, addr),
                       [](const auto& r) { return r.by_chain_addr(); },
                       [&](const auto& r) {
                          return r.chain_kind == chain && r.native_pubkey == addr;
                       });
   if (it == idx.end()) {
      uint64_t id = next_id(self, [](dclaim::cap_counters& c) -> uint64_t& {
         return c.next_unmapped_id;
      });
      unmapped.emplace(ram_payer, dclaim::unmapped_key{id},
         dclaim::unmapped_token{ .id             = id,
                              .chain_kind     = chain,
                              .native_pubkey  = addr,
                              .balance        = amt });
   } else {
      uint64_t rid = it->id;
      unmapped.modify(same_payer, dclaim::unmapped_key{rid}, [&](auto& r) {
         add_wire_capped(r.balance, amt);
      });
   }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void dclaim::setconfig() {
   require_auth(get_self());
   capcfg_t cfg(get_self());
   if (!cfg.exists()) {
      cfg.set(cap_config{}, ram_payer);
   }
}

// ---------------------------------------------------------------------------
//  claim
// ---------------------------------------------------------------------------
void dclaim::claim(name wire_account) {
   require_auth(wire_account);

   pclaims_t pclaims(get_self());
   auto it = pclaims.find(pclaim_key{wire_account.value});
   check(it != pclaims.end(), "no pending claim");
   const asset payout = it->balance;
   check(payout.amount > 0, "zero pending balance");
   pclaims.erase(it);

   action(
      permission_level{ get_self(), "active"_n },
      TOKEN_ACCOUNT,
      "transfer"_n,
      std::make_tuple(get_self(), wire_account, payout, std::string("sysio.dclaim claim"))
   ).send();
}

// ---------------------------------------------------------------------------
//  linkswept — AuthX link completed: sweep unmapped -> pending.
// ---------------------------------------------------------------------------
void dclaim::linkswept(name wire_account, ChainKind chain, std::vector<char> native_pubkey) {
   require_auth(AUTHEX_ACCOUNT);

   // Sweep an unmapped balance into the staker's pending_claims row.
   unmapped_t unmapped(get_self());
   auto uidx = unmapped.template get_index<"bychainad"_n>();
   auto uit = scan_find(uidx, chain_addr_key(chain, native_pubkey),
                        [](const auto& r) { return r.by_chain_addr(); },
                        [&](const auto& r) {
                           return r.chain_kind == chain && r.native_pubkey == native_pubkey;
                        });
   if (uit != uidx.end()) {
      const asset bal = uit->balance;
      const uint64_t row_id = uit->id;
      unmapped.erase(unmapped_key{row_id});
      credit_pending(get_self(), wire_account, bal);
   }
}

// ---------------------------------------------------------------------------
//  importseed — bootstrap pre-launch holders into unmapped_tokens
// ---------------------------------------------------------------------------
void dclaim::importseed(ChainKind chain, std::vector<import_credit> credits) {
   require_auth(get_self());

   capcfg_t cfg(get_self());
   const cap_config current_cfg = cfg.get_or_default(cap_config{});
   check(!current_cfg.imported_complete, "import already finalized");

   if (credits.empty()) return;

   for (const auto& credit : credits) {
      check(credit.wire_atomic >= 0, "negative wire_atomic");
      check(!credit.native_address.empty(), "empty native_address");
      if (credit.wire_atomic == 0) continue;

      // Pre-launch holders remain unmapped until an AuthX link sweeps their credit.
      credit_unmapped(get_self(), chain, credit.native_address,
                  asset{ credit.wire_atomic, WIRE_SYM });
   }
}

// ---------------------------------------------------------------------------
//  importdone
// ---------------------------------------------------------------------------
void dclaim::importdone() {
   require_auth(get_self());
   capcfg_t cfg(get_self());
   cap_config current = cfg.get_or_default(cap_config{});
   check(!current.imported_complete, "import already finalized");
   current.imported_complete = true;
   cfg.set(current, ram_payer);
}

} // namespace sysio
