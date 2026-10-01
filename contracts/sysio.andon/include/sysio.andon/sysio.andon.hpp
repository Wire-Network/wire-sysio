#pragma once
/**
 * @file sysio.andon.hpp
 * @brief sysio.andon: the depot's emergency stop (the Andon cord).
 *
 * One contract holds one flag for the whole depot. While the cord is pulled no funds leave protocol
 * custody on the depot; OPP consensus, envelope deliveries and `sysio.epoch::advance` keep running, so
 * inbound funds keep arriving and stay held. The contracts that move funds read the flag themselves,
 * through `andon::pulled`, before they let anything out:
 *   - `sysio.swap` refuses every action that moves tokens;
 *   - `sysio.liq` refuses a `transfer` to anything but a custody contract, `claim` by anyone but a
 *     custody contract, and `queueyield`;
 *   - `sysio.bond` refuses `claim`;
 *   - `sysio.synd` defers every release, delivery from `parked` and burn to the first queue step after
 *     the cord clears, and refuses `desyndicate`, `sweep`, `dropenv` and `sweepyield`.
 * Transfers INTO a custody contract (`sysio.bond`, `sysio.synd`, `sysio.opreg`) still succeed, so bonds,
 * challenges and holds can be placed during a freeze.
 *
 * Authorities. `sysio` names the panic account (`setpanic`) and registers the contracts allowed to pull
 * the cord themselves (`addpuller`). The panic account, `sysio` or a registered puller pulls the cord;
 * only the panic account or `sysio` clears it. Pulling a pulled cord and clearing a clear one change
 * nothing: only the first of each is recorded.
 *
 * The cord records the depot epoch it was pulled at and cleared at, and the total number of epochs
 * during which it was pulled (`frozen_epochs_through`), so `sysio.synd`'s rate-limit buckets never
 * refill for an epoch during which the cord was pulled.
 *
 * A reader of an account where this contract is not deployed, or has never been pulled, reads the
 * cord as clear.
 *
 * Privileged (roa::setsyscode): its two singletons bill the `sysio` RAM pool whoever signs.
 */

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/time.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sysio {

   class [[sysio::contract("sysio.andon")]] andon : public contract {
   public:
      using contract::contract;

      /// The account the depot deploys this contract on: the one every reader of the cord names.
      static constexpr name ANDON_ACCOUNT  = "sysio.andon"_n;
      /// Configuration authority; it may also pull and clear the cord. Council proposals execute as it.
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      /// The underwriting contract: a custody contract.
      static constexpr name BOND_ACCOUNT  = "sysio.bond"_n;
      /// The syndication contract: a custody contract.
      static constexpr name SYND_ACCOUNT  = "sysio.synd"_n;
      /// The operator registry: a custody contract of operator collateral.
      static constexpr name OPREG_ACCOUNT = "sysio.opreg"_n;
      /// The custody contracts: a transfer into one of them is allowed while the cord is pulled.
      static constexpr std::array<name, 3> CUSTODY_ACCOUNTS{BOND_ACCOUNT, SYND_ACCOUNT, OPREG_ACCOUNT};

      /// Longest `reason` a pull and `note` a clear record, in bytes; a longer one is cut to it.
      static constexpr uint32_t MAX_TEXT_BYTES = 256;
      /// Most contracts `addpuller` registers.
      static constexpr uint32_t MAX_PULLERS    = 16;

      /// True iff `account` is one of CUSTODY_ACCOUNTS.
      static constexpr bool is_custody(name account) {
         return std::find(CUSTODY_ACCOUNTS.begin(), CUSTODY_ACCOUNTS.end(), account) != CUSTODY_ACCOUNTS.end();
      }

      // -----------------------------------------------------------------------
      //  Configuration
      // -----------------------------------------------------------------------

      /// Name `account`, an existing account, the panic account: it may pull and clear the cord. Replaces
      /// the previous one. Auth=sysio.
      [[sysio::action]] void setpanic(name account);

      /// Register `contract`, an existing account not yet registered, as a puller: it may pull the cord,
      /// not clear it. At most MAX_PULLERS. Auth=sysio.
      [[sysio::action]] void addpuller(name contract);

      // -----------------------------------------------------------------------
      //  The cord
      // -----------------------------------------------------------------------

      /// Pull the cord as `actor`, the panic account, `sysio` or a registered puller (`may_pull`), for
      /// `reason` (cut to MAX_TEXT_BYTES). Records who pulled, when, the first epoch it freezes and the
      /// reason, and counts the pull.
      /// Pulling a pulled cord changes nothing and prints so. Auth=actor.
      [[sysio::action]] void pull(name actor, std::string reason);

      /// Clear the cord as `actor`, the panic account or `sysio`, with `note` (cut to MAX_TEXT_BYTES).
      /// Records who cleared, when and the depot epoch, and adds the epochs of this freeze to
      /// `frozen_epochs`. Clearing a clear cord changes nothing and prints so. Auth=actor.
      [[sysio::action]] void clear(name actor, std::string note);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// The cord. The last pull and the last clear are kept; `pull_count` and `frozen_epochs` add up
      /// every freeze.
      ///
      /// LAYOUT IS SHARED STATE: `sysio.swap`, `sysio.liq`, `sysio.bond` and `sysio.synd` deserialize this
      /// row through `cord_of`. Changing it means this contract and every reader redeploy together.
      struct [[sysio::table("cord")]] cord_state {
         bool        pulled           = false;   ///< the cord is pulled now
         name        pulled_by;                  ///< who pulled it last
         time_point  pulled_at{};                ///< when it was pulled last
         std::string reason;                     ///< why it was pulled last
         /// The first depot epoch the last pull freezes: the epoch it was pulled at, or the epoch after the
         /// previous clear when it was pulled again in the epoch it was cleared (that epoch is counted once).
         uint32_t    pulled_at_epoch  = 0;
         name        cleared_by;                 ///< who cleared it last
         time_point  cleared_at{};               ///< when it was cleared last
         std::string note;                       ///< the note of the last clear
         uint32_t    cleared_at_epoch = 0;       ///< the depot epoch it was cleared at last
         uint64_t    pull_count       = 0;       ///< pulls recorded, every freeze counted once
         /// Epochs during which the cord was pulled, over every freeze already cleared, each epoch once: a
         /// freeze counts from its `pulled_at_epoch` to the epoch of its clear, both included, and nothing
         /// when it was re-pulled and cleared within the epoch the previous freeze already counted.
         uint64_t    frozen_epochs    = 0;
         SYSLIB_SERIALIZE(cord_state, (pulled)(pulled_by)(pulled_at)(reason)(pulled_at_epoch)(cleared_by)
                          (cleared_at)(note)(cleared_at_epoch)(pull_count)(frozen_epochs))
      };

      /// The cord singleton.
      using cord_t = kv::global<"cord"_n, cord_state>;

      /// Who may pull the cord besides `sysio`.
      struct [[sysio::table("andonconfig")]] andon_config {
         name              panic;     ///< the panic account; empty until `setpanic`
         std::vector<name> pullers;   ///< contracts `addpuller` registered, in registration order
         SYSLIB_SERIALIZE(andon_config, (panic)(pullers))
      };

      /// The configuration singleton.
      using andonconfig_t = kv::global<"andonconfig"_n, andon_config>;

      // -----------------------------------------------------------------------
      //  Readers, for the contracts the cord freezes
      // -----------------------------------------------------------------------

      /// The cord of the `sysio.andon` deployed at `andon_account`; a clear cord when the contract is not
      /// deployed there or has never been pulled. Never throws.
      static cord_state cord_of(name andon_account) {
         return cord_t(andon_account).get_or_default(cord_state{});
      }

      /// True iff the cord of `andon_account` is pulled now. Never throws. The ONE freeze test every
      /// frozen contract makes.
      static bool pulled(name andon_account) { return cord_of(andon_account).pulled; }

      /// True iff `actor` may pull the cord of the `sysio.andon` deployed at `andon_account`: `sysio`, the
      /// panic account, or a registered puller. Never throws. The ONE pull-authority test: `pull` itself
      /// uses it, and a contract that pulls inline from a path that must not throw checks it before
      /// sending `pull`.
      static bool may_pull(name andon_account, name actor) {
         if (actor == SYSTEM_ACCOUNT) return true;
         if (actor == name{}) return false;
         const andon_config cfg = andonconfig_t(andon_account).get_or_default(andon_config{});
         return actor == cfg.panic || std::find(cfg.pullers.begin(), cfg.pullers.end(), actor) != cfg.pullers.end();
      }

      /// The refusal every frozen signed action raises while the cord is pulled.
      static constexpr const char* FROZEN_MESSAGE = "the andon cord is pulled: funds cannot leave custody";

      /// Refuse with FROZEN_MESSAGE while the cord of `andon_account` is pulled: the gate of a signed
      /// action that would move funds out of custody. Never for an envelope or epoch path, where a frozen
      /// step is a printed no-op instead.
      static void check_clear(name andon_account) { check(!pulled(andon_account), FROZEN_MESSAGE); }

      /// Epochs during which the cord of `andon_account` was pulled, counting only epochs up to `epoch`, the
      /// depot's current epoch: every cleared freeze, plus, while pulled, the epochs from the pull to
      /// `epoch`, both included. Non-decreasing as the depot moves on, so the difference between two
      /// readings is the number of frozen epochs between them (an epoch in which the cord was pulled after
      /// the first reading counts as frozen for the second). Never throws.
      static uint64_t frozen_epochs_through(name andon_account, uint32_t epoch) {
         const cord_state cord   = cord_of(andon_account);
         uint64_t         frozen = cord.frozen_epochs;
         if (cord.pulled && epoch >= cord.pulled_at_epoch) frozen += uint64_t{epoch - cord.pulled_at_epoch} + 1;
         return frozen;
      }
   };

} // namespace sysio
