#include <sysio.chains/sysio.chains.hpp>
#include <sysio.epoch/sysio.epoch.hpp>

namespace sysio {

namespace {

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

uint32_t get_current_epoch_index() {
   sysio::epoch::epochstate_t es(chains::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

bool is_bootstrap_window() {
   return get_current_epoch_index() == 0;
}

void require_priv_caller() {
   require_auth(current_receiver());
   sysio::check(sysio::is_privileged(current_receiver()),
                "sysio.chains: privileged account required");
}

} // namespace

void chains::regchain(opp::types::ChainKind kind,
                      sysio::slug_name       code,
                      uint32_t              external_chain_id,
                      std::string           name,
                      std::string           description) {
   require_priv_caller();

   sysio::check(kind != opp::types::CHAIN_KIND_UNKNOWN,
                "sysio.chains: kind must not be UNKNOWN");

   chains_t tbl(get_self());
   chain_key pk{code};
   sysio::check(tbl.find(pk) == tbl.end(),
                "sysio.chains: chain code already registered");

   // Enforce: at most one row with kind == WIRE (the depot self-row).
   if (kind == opp::types::CHAIN_KIND_WIRE) {
      auto by_kind_idx = tbl.template get_index<"bykind"_n>();
      const auto wire_kind_value = magic_enum::enum_integer(opp::types::CHAIN_KIND_WIRE);
      sysio::check(by_kind_idx.lower_bound(wire_kind_value) == by_kind_idx.upper_bound(wire_kind_value),
                   "sysio.chains: a WIRE chain (depot self-row) already exists");
   }

   const auto now = current_time_ms();
   const bool bootstrap = is_bootstrap_window();

   tbl.emplace(get_self(), pk, chain_row{
      .code               = code,
      .kind               = kind,
      .external_chain_id  = external_chain_id,
      .name               = std::move(name),
      .description        = std::move(description),
      .is_depot           = (kind == opp::types::CHAIN_KIND_WIRE),
      .active             = bootstrap,
      .registered_at_ms   = now,
      .activated_at_ms    = bootstrap ? now : 0,
   });
}

void chains::activchain(sysio::slug_name code) {
   require_priv_caller();

   chains_t tbl(get_self());
   chain_key pk{code};
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "sysio.chains: chain code not registered");
   sysio::check(!it->active, "sysio.chains: chain is already active");

   tbl.modify(get_self(), pk, [&](auto& row) {
      row.active          = true;
      row.activated_at_ms = current_time_ms();
   });
}

} // namespace sysio
