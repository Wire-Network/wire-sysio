#include <sysio.dclaim/sysio.dclaim.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sysio {

namespace {

using opp::types::ChainKind;

/// Deterministic wall-clock seconds (block time). Used for the claimable
/// window; epoch indices carried on the attestation are for audit only.
uint32_t now_sec() {
   return static_cast<uint32_t>(current_time_point().sec_since_epoch());
}

/// Exact-match scan over a uint128 secondary index: `lower_bound` then walk
/// while the narrowing key still matches, returning the first row the
/// predicate accepts (or `idx.end()`). The uint128 key only narrows; the
/// predicate resolves prefix collisions deterministically. One implementation
/// shared by the unmapped + cursor lookups (no duplicated scan loops).
template<class Index, class KeyFn, class MatchFn>
auto scan_find(Index& idx, uint128_t key, KeyFn key_of, MatchFn matches) {
   auto it = idx.lower_bound(key);
   for (; it != idx.end() && key_of(*it) == key; ++it) {
      if (matches(*it)) break;
   }
   if (it != idx.end() && key_of(*it) == key && matches(*it)) return it;
   return idx.end();
}

/// Current claimable-reward window (seconds) from config, default if unset.
uint32_t config_window(name self) {
   dclaim::capcfg_t cfg(self);
   return cfg.get_or_default(dclaim::cap_config{}).claim_window_sec;
}

/// Allocate the next id from one of the monotonic counters. `pick` returns a
/// reference to the field to bump.
template<class Pick>
uint64_t next_id(name self, Pick pick) {
   dclaim::capcounters_t cnt(self);
   dclaim::cap_counters c = cnt.get_or_default(dclaim::cap_counters{});
   uint64_t& field = pick(c);
   uint64_t id = field++;
   cnt.set(c, self);
   return id;
}

/// Credit `amt` WIRE to the staker. Linked (`wacct` set) -> `pending_claims`;
/// otherwise parked in `unmapped_tokens` keyed by (chain, addr). Either way
/// the row's expiry is refreshed to now + window. Shared by `onreward`,
/// `retryconvert`, `linkswept`, and `importseed` so the upsert + expiry logic
/// lives in exactly one place.
void credit_wire(name self, name wacct, ChainKind chain,
                 const std::vector<char>& addr, const asset& amt, uint32_t window) {
   const uint32_t exp = now_sec() + window;

   if (wacct.value != 0) {
      dclaim::pclaims_t pclaims(self);
      auto it = pclaims.find(dclaim::pclaim_key{wacct.value});
      if (it == pclaims.end()) {
         pclaims.emplace(self, dclaim::pclaim_key{wacct.value},
            dclaim::pending_claim{ .wire_account = wacct,
                                .balance      = amt,
                                .expires_at_sec = exp });
      } else {
         pclaims.modify(same_payer, dclaim::pclaim_key{wacct.value}, [&](auto& r) {
            r.balance        += amt;
            r.expires_at_sec  = exp;
         });
      }
      return;
   }

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
      unmapped.emplace(self, dclaim::unmapped_key{id},
         dclaim::unmapped_token{ .id             = id,
                              .chain_kind     = chain,
                              .native_pubkey  = addr,
                              .balance        = amt,
                              .expires_at_sec = exp });
   } else {
      uint64_t rid = it->id;
      unmapped.modify(same_payer, dclaim::unmapped_key{rid}, [&](auto& r) {
         r.balance        += amt;
         r.expires_at_sec  = exp;
      });
   }
}

/// Dedupe at ingest. Returns true if `(chain_code, chain, addr)` has not seen
/// `ext_ref` (or any >= it) yet — and advances the cursor. Returns false for a
/// replay / out-of-order duplicate (`ext_ref <= last`). Advancing here (not at
/// conversion time) means a replay is rejected even while an earlier reward is
/// still staged awaiting a quote.
bool cursor_admit(name self, uint64_t chain_code, ChainKind chain,
                  const std::vector<char>& addr, uint64_t ext_ref) {
   dclaim::rwdcursors_t cur(self);
   auto idx = cur.template get_index<"bychaincode"_n>();
   auto it = scan_find(idx, dclaim::chaincode_addr_key(chain_code, chain, addr),
                       [](const auto& r) { return r.by_chaincode_addr(); },
                       [&](const auto& r) {
                          return r.chain_code == chain_code
                              && r.chain      == chain
                              && r.native_pubkey == addr;
                       });
   if (it == idx.end()) {
      uint64_t id = next_id(self, [](dclaim::cap_counters& c) -> uint64_t& {
         return c.next_cursor_id;
      });
      cur.emplace(self, dclaim::rwdcur_key{id},
         dclaim::reward_cursor{ .id         = id,
                             .chain_code = chain_code,
                             .chain      = chain,
                             .native_pubkey = addr,
                             .last_external_epoch_ref = ext_ref });
      return true;
   }
   if (ext_ref <= it->last_external_epoch_ref) return false;
   uint64_t rid = it->id;
   cur.modify(same_payer, dclaim::rwdcur_key{rid}, [&](auto& r) {
      r.last_external_epoch_ref = ext_ref;
   });
   return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void dclaim::setconfig() {
   require_auth(get_self());
   capcfg_t cfg(get_self());
   if (!cfg.exists()) {
      cfg.set(cap_config{}, get_self());
   }
}

// ---------------------------------------------------------------------------
//  setclmwindow
// ---------------------------------------------------------------------------
void dclaim::setclmwindow(uint32_t window_sec) {
   require_auth(get_self());
   check(window_sec > 0, "window_sec must be positive");
   capcfg_t cfg(get_self());
   cap_config c = cfg.get_or_default(cap_config{});
   c.claim_window_sec = window_sec;
   cfg.set(c, get_self());
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

   const uint32_t window = config_window(get_self());

   // Sweep an unmapped balance into the staker's pending_claims row.
   unmapped_t unmapped(get_self());
   auto uidx = unmapped.template get_index<"bychainad"_n>();
   auto uit = scan_find(uidx, chain_addr_key(chain, native_pubkey),
                        [](const auto& r) { return r.by_chain_addr(); },
                        [&](const auto& r) {
                           return r.chain_kind == chain && r.native_pubkey == native_pubkey;
                        });
   if (uit != uidx.end()) {
      const asset    bal    = uit->balance;
      const uint64_t row_id = uit->id;
      unmapped.erase(unmapped_key{row_id});
      // wire_account is set -> routed to pending_claims, expiry refreshed.
      credit_wire(get_self(), wire_account, chain, native_pubkey, bal, window);
   }
}

// ---------------------------------------------------------------------------
//  onreward — per-staker WIRE-side credit of a STAKING_REWARD
// ---------------------------------------------------------------------------
void dclaim::onreward(uint64_t              chain_code,
                   std::string           staker_wire_account,
                   opp::types::ChainKind reward_chain,
                   std::vector<char>     staker_native_addr,
                   uint64_t              reward_amount,
                   uint32_t              reward_epoch_index,
                   uint64_t              external_epoch_ref,
                   uint32_t              share_bps) {
   require_auth(MSGCH_ACCOUNT);

   // Tolerate degenerate input rather than aborting the inbound OPP envelope
   // (the verifier role lives upstream in msgch::evalcons; dclaim trusts but
   // must not break the message chain on a malformed row).
   if (reward_amount == 0 || staker_native_addr.empty()) return;

   // Dedupe at ingest so a replay / out-of-order duplicate is rejected.
   if (!cursor_admit(get_self(), chain_code, reward_chain,
                     staker_native_addr, external_epoch_ref)) {
      return;
   }

   name wacct;   // value 0 == not yet AuthX-linked
   if (!staker_wire_account.empty()) {
      wacct = name(staker_wire_account);
   }

   // reward_amount arrives already WIRE-denominated -- native -> WIRE
   // conversion and source-chain precision scaling are outpost-side -- so the
   // claim ledger is credited directly.
   credit_wire(get_self(), wacct, reward_chain, staker_native_addr,
               asset{ static_cast<int64_t>(reward_amount), WIRE_SYM },
               config_window(get_self()));

   // Pull funding from sysio.system's drainable pool so the dclaim balance
   // covers this credit immediately -- a staker can claim in the next block
   // rather than waiting for a pay-epoch. sysio.system::fundclaim caps to
   // the remaining pool and never throws, preserving the never-throw
   // contract for OPP inbound dispatch.
   action(
      permission_level{ get_self(), "active"_n },
      SYSTEM_ACCOUNT,
      "fundclaim"_n,
      std::make_tuple(static_cast<int64_t>(reward_amount))
   ).send();
}

// ---------------------------------------------------------------------------
//  flushexpired — prune expired rows; credited WIRE reverts to the capital
//  fund (it simply stays in the sysio.dclaim balance once the row is erased).
// ---------------------------------------------------------------------------
void dclaim::flushexpired(uint32_t max_rows) {
   const uint32_t cutoff = now_sec();
   uint32_t budget = max_rows;

   pclaims_t pclaims(get_self());
   for (auto it = pclaims.begin(); it != pclaims.end() && budget > 0; ) {
      const pending_claim row = *it;
      ++it;
      if (row.expires_at_sec != 0 && cutoff >= row.expires_at_sec) {
         pclaims.erase(pclaim_key{row.wire_account.value});
         --budget;
      }
   }

   unmapped_t unmapped(get_self());
   for (auto it = unmapped.begin(); it != unmapped.end() && budget > 0; ) {
      const unmapped_token row = *it;
      ++it;
      if (row.expires_at_sec != 0 && cutoff >= row.expires_at_sec) {
         unmapped.erase(unmapped_key{row.id});
         --budget;
      }
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

   const uint32_t window = current_cfg.claim_window_sec;

   for (const auto& credit : credits) {
      check(credit.wire_atomic >= 0, "negative wire_atomic");
      check(!credit.native_address.empty(), "empty native_address");
      if (credit.wire_atomic == 0) continue;

      // Pre-launch holders are unlinked by definition -> name{} routes the
      // credit to unmapped_tokens, with the same upsert + expiry path as
      // staking rewards (one implementation in credit_wire).
      credit_wire(get_self(), name{}, chain, credit.native_address,
                  asset{ credit.wire_atomic, WIRE_SYM }, window);
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
   cfg.set(current, get_self());
}

} // namespace sysio
