#include <sysio/sub_chain_plugin/sub_chain_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/root_processor.hpp>
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
   std::shared_ptr<root_processor>                   brp;
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
         [self=this]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t ) {
            self->my->root_txn_ident->signal_applied_transaction(std::get<0>(t), std::get<1>(t));
         } ) );
      my->accepted_block_connection.emplace( chain.accepted_block.connect(
         [self=this]( const block_state_ptr& blk ) {
            self->my->root_txn_ident->signal_accepted_block(blk);
         } ) );
      my->block_start_connection.emplace( chain.block_start.connect(
         [self=this]( uint32_t block_num ) {
            self->my->root_txn_ident->signal_block_start(block_num);
         } ) );

      my->brp = chain_plug->chain().create_root_processor();

   } FC_LOG_AND_RETHROW()
}
void sub_chain_plugin::plugin_startup() {
    ilog("sub_chain_plugin starting up, adding /v3/sub_chain/get_last_s_id");

    app().get_plugin<http_plugin>().add_api({
        {"/v3/sub_chain/get_last_s_id",
         api_category::chain_ro,
         [this](string&&, string&& body, auto&& cb) mutable { // Ensure correct use of auto for callback type inference
            try {
                checksum256_type last_s_id = get_prev_s_id();
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
      std::move(matches),
      *my->brp
   );

}
void sub_chain_plugin::plugin_shutdown() {
    ilog("sub_chain_plugin shutting down");
    // Cleanup code
}

account_name& sub_chain_plugin::get_contract_name() const {
   static account_name contract_name = "sub_chain"_n;
   return contract_name;
}

checksum256_type& sub_chain_plugin::get_prev_s_id() const {
   static checksum256_type id;
   return id;
}
void sub_chain_plugin::update_prev_s_id(const checksum256_type& new_s_id) {
}

/**
 * Calculate the S-Root from a vector of relevant s-transactions.
 * 
 * S-Root Fields (according to doc)
   - Previous Block S-ID
   - Current Block S-Root
 * 
 * @param transactions a vector of relevant s-transactions
 * @return the merkle S-Root calculated from the orderd s-leaves
 */
checksum256_type sub_chain_plugin::calculate_s_root(const std::vector<transaction>& transactions) {
   return checksum256_type{};
}

/**
 * Extract the block number from an S-ID.
 * Takes the 32 least-significant bits from the previous S-ID to get the previous S-Block number
 * @param s_id the S-ID to extract the block number from
 * @return the previous block number
 */
uint32_t sub_chain_plugin::extract_s_block_number(const checksum256_type& s_id) {
    return 0;
}

/**
 * Compute the new S-ID from the previous S-ID and the current S-Root.
 * 
 * 
 * @param curr_s_root the current S-Root
 * @return the new S-ID
 */
checksum256_type sub_chain_plugin::compute_curr_s_id(const checksum256_type& curr_s_root) {
    return checksum256_type();
}

/**
 * Find transactions that are relevant for the S-Root calculation.
 * @param chain the active chain controller
 * @return a vector of transactions deemed relevant
 */
std::vector<sysio::chain::transaction> sub_chain_plugin::find_relevant_transactions(sysio::chain::controller& curr_chain) {
      return std::vector<sysio::chain::transaction>{};
}

/**
 * Determine if a transaction is relevant to the S-Root process.
 * @param trx the transaction to check
 * @return true if relevant, false otherwise
 */
bool sub_chain_plugin::is_relevant_s_root_transaction(const transaction& trx) {
   return false;
}

} // namespace sysio