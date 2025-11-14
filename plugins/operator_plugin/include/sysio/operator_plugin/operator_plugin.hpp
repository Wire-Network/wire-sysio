#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/operator_plugin/utils/event_emitter.hpp>

namespace sysio {

  using utils::event_emitter;

    class operator_plugin : public appbase::plugin<operator_plugin> {
    public:

      APPBASE_PLUGIN_REQUIRES((chain_plugin))
      operator_plugin() = default;
      virtual ~operator_plugin() = default;

      virtual void set_program_options(options_description& cli, options_description& cfg) override;

      virtual void plugin_initialize(const variables_map& options);

      virtual void plugin_startup();

      virtual void plugin_shutdown();

      struct {
        event_emitter<const chain::block_signal_params&> irreversible_block{};
      } events;

    private:
      std::optional<boost::signals2::scoped_connection> _irreversible_block_connection{std::nullopt};
  };



} // namespace sysio
