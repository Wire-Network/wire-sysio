#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/opp/debugging/debugging.pb.h>

#include <boost/signals2/signal.hpp>

namespace sysio {

   using boost::signals2::signal;

   namespace opp::debugging {
      /// Signal data: (epoch_index, endpoints_type, batch_op_name, envelope_data)
      using DebugEnvelopeEvent = std::tuple<
         uint64_t,
         ::sysio::opp::debugging::DebugOutpostEndpointsType,
         name,
         std::vector<char>
      >;
   }

   /// Batch operator plugin — cranks Depot and Outpost contracts, ferries OPP
   /// message chains between WIRE chain and external blockchains (ETH, SOL).
   ///
   /// All 21 batch operators run this plugin in perpetuity. The epoch scheduler
   /// (sysio.epoch) assigns operators into 3 groups of 7. Each epoch, one group
   /// is elected; those 7 execute the full epoch cycle (outbound then inbound).
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
