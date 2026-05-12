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

uint128_t make_wire_chain_key(name wire_account, ChainKind chain) {
   return (static_cast<uint128_t>(wire_account.value) << 64)
        | static_cast<uint64_t>(chain);
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

   const asset credit = it->balance;
   const uint64_t row_id = it->id;
   unmapped.erase(unmapped_key{row_id});

   pclaims_t pclaims(get_self());
   auto pit = pclaims.find(pclaim_key{wire_account.value});
   if (pit == pclaims.end()) {
      pending_claim row;
      row.wire_account = wire_account;
      row.balance      = credit;
      pclaims.emplace(get_self(), pclaim_key{wire_account.value}, row);
   } else {
      pclaims.modify(same_payer, pclaim_key{wire_account.value}, [&](auto& row) {
         row.balance += credit;
      });
   }
}

// ---------------------------------------------------------------------------
//  flushcd
// ---------------------------------------------------------------------------
void cap::flushcd(uint32_t current_epoch) {
   require_auth(EPOCH_ACCOUNT);

   cdqueue_t cdqueue(get_self());
   auto idx = cdqueue.template get_index<"byeligible"_n>();
   auto it = idx.begin();
   while (it != idx.end() && it->eligible_at_epoch <= current_epoch) {
      const uint64_t row_id = it->request_id;
      ++it;
      cdqueue.erase(cd_key{row_id});
   }
}

// ---------------------------------------------------------------------------
//  available_stake
// ---------------------------------------------------------------------------
uint64_t cap::availstake(name wire_account, ChainKind chain) {
   uint64_t cooldown_locked = 0;
   cdqueue_t cdqueue(get_self());
   auto idx = cdqueue.template get_index<"bywirechn"_n>();
   const uint128_t key = make_wire_chain_key(wire_account, chain);
   for (auto it = idx.lower_bound(key); it != idx.end(); ++it) {
      if (it->wire_account != wire_account || it->chain_kind != chain) break;
      cooldown_locked += static_cast<uint64_t>(it->amount.amount);
   }
   (void)cooldown_locked;
   return 0;
}

} // namespace sysio
