#pragma once
/**
 * @file sysio.andon.hpp
 * @brief Depot emergency stop, authorized through native account permissions.
 *
 * Bootstrap links pull and clear to sysio.andon permissions whose authorities
 * delegate to governance and the panic account. Readers gate custody exits;
 * intake and OPP consensus continue. Privileged sysio.synd pulls automatically
 * on a custody shortfall with sysio.andon@active authorization.
 */
#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/time.hpp>
#include <algorithm>
#include <array>
#include <string>

namespace sysio {
   class [[sysio::contract("sysio.andon")]] andon : public contract {
   public:
      using contract::contract;
      /// Deployment account shared by every freeze reader.
      static constexpr name ANDON_ACCOUNT = "sysio.andon"_n;
      /// Governance pays for this privileged contract's singleton.
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;
      /// Custody destinations remain available for deposits during a freeze.
      static constexpr std::array<name, 3> CUSTODY_ACCOUNTS{"sysio.bond"_n, "sysio.synd"_n, "sysio.opreg"_n};
      /// Maximum stored reason/note length in bytes.
      static constexpr uint32_t MAX_TEXT_BYTES = 256;
      /// Whether an account may receive a custody deposit while frozen.
      static constexpr bool is_custody(name account) {
         return std::find(CUSTODY_ACCOUNTS.begin(), CUSTODY_ACCOUNTS.end(), account) != CUSTODY_ACCOUNTS.end();
      }
      /// Pull the cord; repeated pulls preserve the first reason and timestamp. Auth=get_self().
      [[sysio::action]] void pull(std::string reason);
      /// Clear the cord; repeated clears are no-ops. Auth=get_self().
      [[sysio::action]] void clear(std::string note);
      /// Shared layout: redeploy Andon and its readers together on a fresh deployment.
      struct [[sysio::table("cord")]] cord_state {
         bool pulled = false;       ///< Whether custody exits are stopped.
         time_point when{};         ///< Time of the last state transition.
         std::string reason;        ///< Reason or recovery note of that transition.
         SYSLIB_SERIALIZE(cord_state, (pulled)(when)(reason))
      };
      /// Depot emergency-stop singleton.
      using cord_t = kv::global<"cord"_n, cord_state>;
      /// An absent row reads as clear.
      static cord_state cord_of(name account) { return cord_t(account).get_or_default(cord_state{}); }
      /// The common freeze check used by custody readers.
      static bool pulled(name account) { return cord_of(account).pulled; }
      /// Refusal returned by signed actions that would move funds out of custody.
      static constexpr const char* FROZEN_MESSAGE = "the andon cord is pulled: funds cannot leave custody";
      /// Refuse a custody exit while the cord is pulled.
      static void check_clear(name account) { check(!pulled(account), FROZEN_MESSAGE); }
   };
} // namespace sysio
