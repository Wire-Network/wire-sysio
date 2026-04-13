#pragma once

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>

namespace sysio {

   class external_debugging_plugin : public appbase::plugin<external_debugging_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES((batch_operator_plugin))

      external_debugging_plugin();
      virtual ~external_debugging_plugin();

      virtual void set_program_options(options_description& cli, options_description& cfg) override;
      void plugin_initialize(const variables_map& options);
      void plugin_startup();
      void plugin_shutdown();

   private:
      struct impl;
      std::unique_ptr<impl> _impl;
   };

} // namespace sysio
