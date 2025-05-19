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

class sub_chain_plugin_impl {
   public:
      sysio::chain::account_name contract_name;
      std::vector<sysio::chain::action_name> action_names;
      sysio::chain::checksum256_type prev_s_id;
};

class sub_chain_plugin : public appbase::plugin<sub_chain_plugin> {
   public:
      APPBASE_PLUGIN_REQUIRES()
      sub_chain_plugin();
      virtual ~sub_chain_plugin();

      virtual void set_program_options(options_description&, options_description& cfg) override;
      void plugin_initialize(const variables_map& options);
      void plugin_startup();
      void plugin_shutdown();

      sysio::chain::account_name& get_contract_name() const;
      sysio::chain::checksum256_type& get_prev_s_id() const;
      void update_prev_s_id(const sysio::chain::checksum256_type& new_s_id);

      static sysio::chain::checksum256_type calculate_s_root(const std::vector<sysio::chain::transaction>& transactions);
      sysio::chain::checksum256_type compute_curr_s_id(const sysio::chain::checksum256_type& curr_s_root);
      static uint32_t extract_s_block_number(const sysio::chain::checksum256_type& s_id);

      std::vector<sysio::chain::transaction> find_relevant_transactions(sysio::chain::controller& curr_chain);
      bool is_relevant_s_root_transaction(const sysio::chain::transaction& trx);

   private:
      std::unique_ptr<sub_chain_plugin_impl> my;
   };

}
