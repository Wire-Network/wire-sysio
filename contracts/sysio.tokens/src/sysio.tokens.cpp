#include <sysio.tokens/sysio.tokens.hpp>
#include <sysio.epoch/sysio.epoch.hpp>

namespace sysio {

namespace {

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

uint32_t get_current_epoch_index() {
   sysio::epoch::epochstate_t es(tokens::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

bool is_bootstrap_window() {
   return get_current_epoch_index() == 0;
}

void require_priv_caller() {
   require_auth(current_receiver());
   sysio::check(sysio::is_privileged(current_receiver()),
                "sysio.tokens: privileged account required");
}

} // namespace

void tokens::regtoken(opp::types::TokenKind    kind,
                      sysio::slug_name          code,
                      std::string              symbol_name,
                      std::string              description,
                      uint32_t                 precision,
                      opp::types::ChainAddress address) {
   require_priv_caller();

   sysio::check(kind != opp::types::TOKEN_KIND_UNKNOWN,
                "sysio.tokens: token kind must not be UNKNOWN");

   tokens_t tbl(get_self());
   token_key pk{code};
   sysio::check(tbl.find(pk) == tbl.end(),
                "sysio.tokens: token code already registered");

   const auto now = current_time_ms();
   const bool bootstrap = is_bootstrap_window();

   tbl.emplace(ram_payer, pk, token_row{
      .code               = code,
      .kind               = kind,
      .symbol_name        = std::move(symbol_name),
      .description        = std::move(description),
      .precision          = precision,
      .address            = std::move(address),
      .active             = bootstrap,
      .registered_at_ms   = now,
      .activated_at_ms    = bootstrap ? now : 0,
   });
}

void tokens::activtoken(sysio::slug_name code) {
   require_priv_caller();

   tokens_t tbl(get_self());
   token_key pk{code};
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "sysio.tokens: token code not registered");
   sysio::check(!it->active, "sysio.tokens: token is already active");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.active          = true;
      row.activated_at_ms = current_time_ms();
   });
}

void tokens::regctok(sysio::slug_name   chain_code,
                      sysio::slug_name   token_code,
                      std::vector<char> contract_addr,
                      uint32_t          precision_override,
                      bool              is_native) {
   require_priv_caller();

   chaintokens_t tbl(get_self());
   chain_token_key pk{chain_code, token_code};
   sysio::check(tbl.find(pk) == tbl.end(),
                "sysio.tokens: (chain_code, token_code) binding already registered");

   const auto now = current_time_ms();
   const bool bootstrap = is_bootstrap_window();

   // Bootstrap inline-active path: enforce "exactly one is_native=true per chain".
   if (bootstrap && is_native) {
      auto by_chain_idx = tbl.template get_index<"bychain"_n>();
      auto it  = by_chain_idx.lower_bound(chain_code.value);
      auto end = by_chain_idx.upper_bound(chain_code.value);
      for (; it != end; ++it) {
         sysio::check(!(it->is_native && it->active),
                      "sysio.tokens: chain already has an active is_native binding");
      }
   }

   tbl.emplace(ram_payer, pk, chain_token_row{
      .chain_code         = chain_code,
      .token_code         = token_code,
      .contract_addr      = std::move(contract_addr),
      .precision_override = precision_override,
      .is_native          = is_native,
      .active             = bootstrap,
      .registered_at_ms   = now,
      .activated_at_ms    = bootstrap ? now : 0,
   });
}

void tokens::activctok(sysio::slug_name chain_code, sysio::slug_name token_code) {
   require_priv_caller();

   chaintokens_t tbl(get_self());
   chain_token_key pk{chain_code, token_code};
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(),
                "sysio.tokens: (chain_code, token_code) binding not registered");
   sysio::check(!it->active, "sysio.tokens: binding is already active");

   if (it->is_native) {
      auto by_chain_idx = tbl.template get_index<"bychain"_n>();
      auto cit  = by_chain_idx.lower_bound(chain_code.value);
      auto cend = by_chain_idx.upper_bound(chain_code.value);
      for (; cit != cend; ++cit) {
         if (cit->token_code == token_code) continue;
         sysio::check(!(cit->is_native && cit->active),
                      "sysio.tokens: chain already has an active is_native binding");
      }
   }

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.active          = true;
      row.activated_at_ms = current_time_ms();
   });
}

} // namespace sysio
