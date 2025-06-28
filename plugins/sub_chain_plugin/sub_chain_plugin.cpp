#include <sysio/sub_chain_plugin/sub_chain_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/sub_chain_plugin/contract_action_match.hpp>
#include <sysio/sub_chain_plugin/root_txn_identification.hpp>

#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>
#include <vector>
#include <variant>
#include <sysio/chain/merkle.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger_config.hpp>

namespace sysio {
   static auto _sub_chain_plugin = application::register_plugin<sub_chain_plugin>();

using namespace chain;
using contract_action_matches = std::vector<contract_action_match>;

struct sub_chain_plugin_impl {
   std::shared_ptr<root_txn_identification>          root_txn_ident;
   std::optional<boost::signals2::scoped_connection> accepted_block_connection;
   std::optional<boost::signals2::scoped_connection> applied_transaction_connection;
   std::optional<boost::signals2::scoped_connection> block_start_connection;

};

sub_chain_plugin::sub_chain_plugin(): my(new sub_chain_plugin_impl()) {}

sub_chain_plugin::~sub_chain_plugin() {
   my->accepted_block_connection.reset();
   my->applied_transaction_connection.reset();
   my->block_start_connection.reset();
}

void sub_chain_plugin::set_program_options(options_description&, options_description& cfg) {
}

void sub_chain_plugin::plugin_initialize(const variables_map& options) {
   try {
      auto chain_plug = app().find_plugin<chain_plugin>();
      chain::controller& chain = chain_plug->chain();
      my->applied_transaction_connection.emplace( chain.applied_transaction.connect(
         [self=this,&chain]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t ) {
            if( chain.is_builtin_activated( chain::builtin_protocol_feature_t::multiple_state_roots_supported ) )
               self->my->root_txn_ident->signal_applied_transaction(std::get<0>(t), std::get<1>(t));
         } ) );
      my->accepted_block_connection.emplace( chain.accepted_block.connect(
         [self=this,&chain]( const block_state_ptr& blk ) {
            if( chain.is_builtin_activated( chain::builtin_protocol_feature_t::multiple_state_roots_supported ) )
               self->my->root_txn_ident->signal_accepted_block(blk);
         } ) );
      my->block_start_connection.emplace( chain.block_start.connect(
         [self=this,&chain]( uint32_t block_num ) {
            if( chain.is_builtin_activated( chain::builtin_protocol_feature_t::multiple_state_roots_supported ) )
               self->my->root_txn_ident->signal_block_start(block_num);
         } ) );

   } FC_LOG_AND_RETHROW()
}
void sub_chain_plugin::plugin_startup() {
    ilog("sub_chain_plugin starting up, adding /v3/sub_chain/get_last_s_id");

    app().get_plugin<http_plugin>().add_api({
        {"/v3/sub_chain/get_last_s_id",
         api_category::chain_ro,
         [this](string&&, string&& body, auto&& cb) mutable { // Ensure correct use of auto for callback type inference
            try {
                checksum256_type last_s_id;
                cb(200, last_s_id.str());
            } catch (fc::exception& e) {
                cb(500, "{\"error\": \"internal_error\", \"details\": \"" + e.to_detail_string() + "\"}");
            }
        }
        }}, appbase::exec_queue::read_only);

   contract_action_matches matches;
   matches.push_back(contract_action_match("s"_n, "utl"_n, contract_action_match::match_type::suffix));
   matches[0].add_action("batchw"_n, contract_action_match::match_type::exact);
   matches[0].add_action("snoop"_n, contract_action_match::match_type::exact);
   matches.push_back(contract_action_match("op"_n, "sysio"_n, contract_action_match::match_type::suffix));
   matches[1].add_action("regproducer"_n, contract_action_match::match_type::exact);
   matches[1].add_action("unregprod"_n, contract_action_match::match_type::exact);

   my->root_txn_ident = std::make_shared<root_txn_identification>(
      std::move(matches)
   );
   auto chain_plug = app().find_plugin<chain_plugin>();
   chain_plug->chain().create_root_processor(my->root_txn_ident);

}
void sub_chain_plugin::plugin_shutdown() {
    ilog("sub_chain_plugin shutting down");
    // Cleanup code
}

} // namespace sysio