#pragma once
/**
 * @file require_privileged.hpp
 * @brief The privileged-caller gate a system contract puts on the actions only it may run for itself.
 */
#include <sysio/action.hpp>
#include <sysio/check.hpp>
#include <sysio/name.hpp>
#include <sysio/privileged.hpp>
#include <sysio/system.hpp>

#include <string>
#include <string_view>

namespace sysio::opp {

/// What follows the contract's name in the refusal `require_privileged_self` raises.
inline constexpr std::string_view PRIVILEGED_REQUIRED_SUFFIX = ": privileged account required";

/// Refuse unless the contract now executing (`current_receiver()`) signed the action and is
/// privileged: the gate of the launch-ingestion and registry-seeding actions a system contract runs
/// under its own authority (`sysio.liq::regliqpool`, `sysio.synd::importsynd`,
/// `sysio.reserv::regreserve`). The refusal names the contract: `<account>: privileged account
/// required`. The ONE such gate; every system contract that needs it calls this.
inline void require_privileged_self() {
   const name self = current_receiver();
   require_auth(self);
   check(is_privileged(self), self.to_string() + std::string(PRIVILEGED_REQUIRED_SUFFIX));
}

} // namespace sysio::opp
