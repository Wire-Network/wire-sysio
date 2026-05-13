#include <sysio.cap/sysio.cap.hpp>

#include <cstring>
#include <tuple>
#include <string>

namespace sysio {

namespace {

using opp::types::ChainKind;

uint128_t make_chain_addr_key(ChainKind chain, const std::vector<char>& addr) {
   if (addr.empty()) return 0;
   uint64_t prefix = 0;
   const size_t n = addr.size() < sizeof(uint64_t) ? addr.size() : sizeof(uint64_t);
   std::memcpy(&prefix, addr.data(), n);
   return (static_cast<uint128_t>(chain) << 64) | prefix;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void cap::setconfig() {
   require_auth(get_self());
   capcfg_t cfg(get_self());
   if (!cfg.exists()) {
      cfg.set(cap_config{}, get_self());
   }
}

// ---------------------------------------------------------------------------
//  claim
// ---------------------------------------------------------------------------
void cap::claim(name wire_account) {
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
      std::make_tuple(get_self(), wire_account, payout, std::string("sysio.cap claim"))
   ).send();
}

// ---------------------------------------------------------------------------
//  linkswept
// ---------------------------------------------------------------------------
void cap::linkswept(name wire_account, ChainKind chain, std::vector<char> native_pubkey) {
   require_auth(AUTHEX_ACCOUNT);

   unmapped_t unmapped(get_self());
   auto idx = unmapped.template get_index<"bychainad"_n>();
   auto it = idx.find(make_chain_addr_key(chain, native_pubkey));
   if (it == idx.end()) return;

   const asset    credit_balance = it->balance;
   const uint64_t row_id         = it->id;
   unmapped.erase(unmapped_key{row_id});

   pclaims_t pclaims(get_self());
   auto pit = pclaims.find(pclaim_key{wire_account.value});
   if (pit == pclaims.end()) {
      pending_claim row;
      row.wire_account = wire_account;
      row.balance      = credit_balance;
      pclaims.emplace(get_self(), pclaim_key{wire_account.value}, row);
   } else {
      pclaims.modify(same_payer, pclaim_key{wire_account.value}, [&](auto& row) {
         row.balance += credit_balance;
      });
   }
}

// ---------------------------------------------------------------------------
//  importseed
// ---------------------------------------------------------------------------
void cap::importseed(ChainKind chain, std::vector<import_credit> credits) {
   require_auth(get_self());

   capcfg_t cfg(get_self());
   const cap_config current_cfg = cfg.get_or_default(cap_config{});
   check(!current_cfg.imported_complete, "import already finalized");

   if (credits.empty()) return;

   capcounters_t cnt_tbl(get_self());
   cap_counters counters = cnt_tbl.get_or_default(cap_counters{});

   unmapped_t unmapped(get_self());
   auto idx = unmapped.template get_index<"bychainad"_n>();

   for (const auto& credit : credits) {
      check(credit.wire_atomic >= 0, "negative wire_atomic");
      check(!credit.native_address.empty(), "empty native_address");
      if (credit.wire_atomic == 0) continue;

      const uint128_t lookup_key = make_chain_addr_key(chain, credit.native_address);
      const asset add_balance{credit.wire_atomic, WIRE_SYM};

      bool merged = false;
      for (auto it = idx.lower_bound(lookup_key); it != idx.end(); ++it) {
         if (it->by_chain_addr() != lookup_key) break;
         if (it->chain_kind == chain && it->native_pubkey == credit.native_address) {
            const uint64_t row_id = it->id;
            unmapped.modify(same_payer, unmapped_key{row_id}, [&](auto& row) {
               row.balance += add_balance;
            });
            merged = true;
            break;
         }
      }

      if (!merged) {
         unmapped_token row;
         row.id            = counters.next_unmapped_id++;
         row.chain_kind    = chain;
         row.native_pubkey = credit.native_address;
         row.balance       = add_balance;
         unmapped.emplace(get_self(), unmapped_key{row.id}, row);
      }
   }

   cnt_tbl.set(counters, get_self());
}

// ---------------------------------------------------------------------------
//  importdone
// ---------------------------------------------------------------------------
void cap::importdone() {
   require_auth(get_self());
   capcfg_t cfg(get_self());
   cap_config current = cfg.get_or_default(cap_config{});
   check(!current.imported_complete, "import already finalized");
   current.imported_complete = true;
   cfg.set(current, get_self());
}

} // namespace sysio
