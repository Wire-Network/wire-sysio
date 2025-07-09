#pragma once
#include <sysio/chain/application.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/controller.hpp>
#include <string>
#include <vector>
#include <fc/bitutil.hpp> // for fc::endian_reverse_u32

namespace sysio {

using namespace appbase;
namespace chain {
   struct root_processor;
   using root_processor_ptr = std::shared_ptr<root_processor>;
}

class sub_chain_plugin : public appbase::plugin<sub_chain_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES()
      sub_chain_plugin();
      virtual ~sub_chain_plugin();

      virtual void set_program_options(options_description&, options_description& cfg) override;
      void plugin_initialize(const variables_map& options);
      void plugin_startup();
      void plugin_shutdown();

   private:
   };

}
