#include <sysio.andon/sysio.andon.hpp>

#include <sysio/print.hpp>
#include <sysio/system.hpp>
#include <sysio.epoch/sysio.epoch.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace sysio {

namespace {

/// The payer of both singletons: the `sysio` RAM pool (privileged-contract model, as sysio.synd uses).
/// A puller pulling inline, or the panic account, has no RAM this contract should draw on.
constexpr name ram_payer = andon::SYSTEM_ACCOUNT;

/// Refusals.
constexpr std::string_view not_account_message   = "account does not exist";
constexpr std::string_view already_puller_msg    = "contract is already a puller";
constexpr std::string_view too_many_pullers_msg  = "too many pullers";
constexpr std::string_view pull_authority_msg    = "only the panic account, sysio or a registered puller may pull";
constexpr std::string_view clear_authority_msg   = "only the panic account or sysio may clear";

/// What a pull of a pulled cord and a clear of a clear one print; neither changes anything.
constexpr std::string_view already_pulled_note  = "sysio.andon::pull: the cord is already pulled; nothing changes\n";
constexpr std::string_view already_clear_note   = "sysio.andon::clear: the cord is not pulled; nothing changes\n";

/// `text` cut to andon::MAX_TEXT_BYTES.
std::string bounded(std::string text) {
   if (text.size() > andon::MAX_TEXT_BYTES) text.resize(andon::MAX_TEXT_BYTES);
   return text;
}

} // anonymous namespace

void andon::setpanic(name account) {
   require_auth(SYSTEM_ACCOUNT);
   check(is_account(account), not_account_message.data());
   andonconfig_t config(get_self());
   andon_config  cfg = config.get_or_default(andon_config{});
   cfg.panic         = account;
   config.set(cfg, ram_payer);
}

void andon::addpuller(name contract) {
   require_auth(SYSTEM_ACCOUNT);
   check(is_account(contract), not_account_message.data());
   andonconfig_t config(get_self());
   andon_config  cfg = config.get_or_default(andon_config{});
   check(std::find(cfg.pullers.begin(), cfg.pullers.end(), contract) == cfg.pullers.end(), already_puller_msg.data());
   check(cfg.pullers.size() < MAX_PULLERS, too_many_pullers_msg.data());
   cfg.pullers.push_back(contract);
   config.set(cfg, ram_payer);
}

void andon::pull(name actor, std::string reason) {
   require_auth(actor);
   check(may_pull(get_self(), actor), pull_authority_msg.data());

   cord_t     cord(get_self());
   cord_state state = cord.get_or_default(cord_state{});
   if (state.pulled) {
      sysio::print(std::string(already_pulled_note));
      return;
   }
   state.pulled          = true;
   state.pulled_by       = actor;
   state.pulled_at       = current_time_point();
   state.reason          = bounded(std::move(reason));
   // A pull in the epoch the previous freeze was cleared in starts after it: that epoch is counted once.
   const uint32_t epoch_now = epoch::current_epoch_index();
   state.pulled_at_epoch    = state.pull_count > 0 ? std::max(epoch_now, state.cleared_at_epoch + 1) : epoch_now;
   ++state.pull_count;
   cord.set(state, ram_payer);
}

void andon::clear(name actor, std::string note) {
   require_auth(actor);
   const andon_config cfg = andonconfig_t(get_self()).get_or_default(andon_config{});
   check(actor == SYSTEM_ACCOUNT || (cfg.panic != name{} && actor == cfg.panic), clear_authority_msg.data());

   cord_t     cord(get_self());
   cord_state state = cord.get_or_default(cord_state{});
   if (!state.pulled) {
      sysio::print(std::string(already_clear_note));
      return;
   }
   const uint32_t epoch_now = epoch::current_epoch_index();
   state.pulled           = false;
   state.cleared_by       = actor;
   state.cleared_at       = current_time_point();
   state.note             = bounded(std::move(note));
   state.cleared_at_epoch = epoch_now;
   // The epochs from the first the pull freezes to the clear, both included; none when the freeze began
   // after this epoch (re-pulled in the epoch the previous freeze was cleared in, and cleared again in it).
   if (epoch_now >= state.pulled_at_epoch) state.frozen_epochs += uint64_t{epoch_now - state.pulled_at_epoch} + 1;
   cord.set(state, ram_payer);
}

} // namespace sysio
