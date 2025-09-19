#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/snapshot_scheduler.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/chain/thread_utils.hpp>
#include <sysio/chain/unapplied_transaction_queue.hpp>
#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>
#include <sysio/chain/s_root_extension.hpp>

#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/time.hpp>

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/signals2/connection.hpp>

#include <cstdint>
#include <iostream>
#include <algorithm>
#include <mutex>


namespace sysio {
   static auto _batch_operator_plugin = application::register_plugin<batch_operator_plugin>();

class batch_operator_plugin_impl {
   public:
};

batch_operator_plugin::batch_operator_plugin():my(new batch_operator_plugin_impl()){}
batch_operator_plugin::~batch_operator_plugin(){}

void batch_operator_plugin::set_program_options(options_description&, options_description& cfg) {
   auto default_priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("nathan")));

   boost::program_options::options_description batch_operator_opts;
   
   batch_operator_opts.add_options()
   ("batch-operator-signature-provider", boost::program_options::value<vector<string>>()->composing()->multitoken()->default_value(
               {default_priv_key.get_public_key().to_string({}) + "=KEY:" + default_priv_key.to_string({})},
                default_priv_key.get_public_key().to_string({}) + "=KEY:" + default_priv_key.to_string({})),
               app().get_plugin<signature_provider_plugin>().signature_provider_help_text())
         ;
}

void batch_operator_plugin::plugin_initialize(const variables_map& options) {
   try {
      if( options.count( "batch-operator-signature-provider" )) {
         // Handle the option
      }
   }
   FC_LOG_AND_RETHROW()
}

void batch_operator_plugin::plugin_startup() {
   // Make the magic happen
}

void batch_operator_plugin::plugin_shutdown() {
   // OK, that's enough magic
}

}
