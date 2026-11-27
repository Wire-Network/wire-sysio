#pragma once

#include <sysio/batch_operator_plugin/debug_envelope_event.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>

#include <boost/signals2/signal.hpp>

namespace sysio {

   using boost::signals2::signal;

   /// Batch operator plugin — cranks Depot and Outpost contracts, ferries OPP
   /// message chains between WIRE chain and external blockchains (ETH, SOL).
   ///
   /// Every batch operator runs this plugin in perpetuity. Each outpost chain runs
   /// its own consensus epoch: the epoch scheduler (sysio.epoch) seats the
   /// operators serving that chain into groups (3 of 7 by default), and each of
   /// the chain's epochs one group is elected; its members execute that chain's
   /// epoch cycle (outbound then inbound). An operator relays only the chains it
   /// serves (`sysio.opreg::servchains`).
   ///
   /// With `batch-underwriter-account` it also runs, or only runs, the
   /// underwriter: it bonds each sysio.synd envelope request once the outpost
   /// confirms the envelope, then approves and claims the bond.
   class batch_operator_plugin : public appbase::plugin<batch_operator_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES(
         (chain_plugin)
         (cron_plugin)
         (outpost_ethereum_client_plugin)
         (outpost_solana_client_plugin)
      )

      batch_operator_plugin();
      virtual ~batch_operator_plugin();

      virtual void set_program_options(options_description& cli, options_description& cfg) override;
      void plugin_initialize(const variables_map& options);
      void plugin_startup();
      void plugin_shutdown();

      /// Signal emitted whenever an OPP envelope is computed.
      /// Only fires if there are active connections (checked via num_slots).
      signal<void(const opp::debugging::DebugEnvelopeEvent&)>& debugging_opp_envelope();

   private:
      struct impl;
      std::unique_ptr<impl> _impl;
   };

} // namespace sysio
