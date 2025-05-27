#include <sysio/sub_chain_plugin/sub_chain_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <fc/log/logger.hpp>
#include <vector>
#include <variant>
#include <sysio/chain/merkle.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger_config.hpp>

namespace sysio {
   static auto _sub_chain_plugin = application::register_plugin<sub_chain_plugin>();

using namespace chain;

struct sub_chain_plugin_impl {
      sysio::chain::account_name contract_name;
      std::vector<sysio::chain::action_name> action_names;
      sysio::chain::checksum256_type prev_s_id;
};

sub_chain_plugin::sub_chain_plugin(): my(new sub_chain_plugin_impl()) {}
sub_chain_plugin::~sub_chain_plugin() {}

void sub_chain_plugin::set_program_options(options_description&, options_description& cfg) {
    cfg.add_options()
        ("s-chain-contract", bpo::value<std::string>()->default_value("settle.wns"), "Contract name for identifying relevant S-transactions.")
        ("s-chain-actions", bpo::value<std::vector<std::string>>()->composing(), "List of action names for relevant S-transactions for a given s-chain-contract");
}

void sub_chain_plugin::plugin_initialize(const variables_map& options) {
   try {
      if (!options.count("s-chain-contract") || !options.count("s-chain-actions")) {
         SYS_ASSERT(false, plugin_config_exception, "sub_chain_plugin requires both --s-chain-contract and --s-chain-actions to be set");
      }
      else {
         std::string contract_name_str = options.at("s-chain-contract").as<std::string>();
         my->contract_name = sysio::chain::name(contract_name_str);  // Convert std::string to sysio::chain::name

         std::vector<std::string> actions = options.at("s-chain-actions").as<std::vector<std::string>>();
         my->action_names.clear();
         for (const std::string& action_name : actions) {
            my->action_names.push_back(sysio::chain::name(action_name));  // Convert std::string to action_name
         }
      }
      // my->chain_plug = app().find_plugin<chain_plugin>();
      // SYS_ASSERT( my->chain_plug, chain::missing_chain_plugin_exception, ""  );

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
}
void sub_chain_plugin::plugin_shutdown() {
    ilog("sub_chain_plugin shutting down");
    // Cleanup code
}

account_name& sub_chain_plugin::get_contract_name() const {
   return my->contract_name;
}
checksum256_type& sub_chain_plugin::get_prev_s_id() const {
   return my->prev_s_id;
}
void sub_chain_plugin::update_prev_s_id(const checksum256_type& new_s_id) {
   ilog("Updating OLD prev_s_id: \t${prev_s_id}", ("prev_s_id", my->prev_s_id));
   my->prev_s_id = new_s_id;
   ilog("Updated  NEW prev_s_id: \t${prev_s_id}", ("prev_s_id", my->prev_s_id));
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
   // Build s-leaves from relevant s-transactions
   vector<digest_type> s_leaves;
   for( const auto& trx : transactions ){
      s_leaves.emplace_back( trx.id() );
   }
   // Create and return the merkle S-Root 
   // return merkle( std::move(s_leaves) );
   return merkle(sysio::chain::deque<fc::sha256>(s_leaves.begin(), s_leaves.end()));

}

/**
 * Extract the block number from an S-ID.
 * Takes the 32 least-significant bits from the previous S-ID to get the previous S-Block number
 * @param s_id the S-ID to extract the block number from
 * @return the previous block number
 */
uint32_t sub_chain_plugin::extract_s_block_number(const checksum256_type& s_id) {
//    auto s_id_data = s_id.data();
//    return fc::endian_reverse_u32(s_id_data[0]);

    // Extract the pointer to the data from the checksum.
    auto s_id_data = s_id.data();
    
    // Since we are dealing with a checksum256_type, which is typically an array of bytes,
    // we need to correctly handle the conversion to uint32_t considering endianess.
    uint32_t s_block_number;
    
    // Copy the first 4 bytes of the data into a uint32_t variable.
    // memcpy ensures that we handle any alignment issues.
    std::memcpy(&s_block_number, s_id_data, sizeof(uint32_t));

    // Reverse the endianess if necessary.
    // You might need to adjust this based on how the data is stored (big-endian vs little-endian).
    return fc::endian_reverse_u32(s_block_number);
}

/**
 * Compute the new S-ID from the previous S-ID and the current S-Root.
 * 
 * 
 * @param curr_s_root the current S-Root
 * @return the new S-ID
 */
checksum256_type sub_chain_plugin::compute_curr_s_id(const checksum256_type& curr_s_root) {
    // ilog("Computing new S-ID from \n\tcurr_s_root: ${curr_s_root}\n\t${prev_s_id}", ("curr_s_root", curr_s_root)("prev_s_id", my->prev_s_id));

    // Serialize both the previous S-ID and the current S-Root
    auto data = fc::raw::pack(std::make_pair(my->prev_s_id, curr_s_root));
    // Hash the serialized data to generate the new S-ID
    checksum256_type curr_s_id = checksum256_type::hash(data);

    ilog("Computed interim S-ID: ${curr_s_id}", ("curr_s_id", curr_s_id));
    // Extract the block number from the previous S-ID and increment it by 1
    uint32_t prev_s_block_number = sub_chain_plugin::extract_s_block_number(my->prev_s_id);
    uint32_t next_s_block_number = prev_s_block_number + 1;

    ilog("Extracted prev_s_block_number from prev_s_id: ${prev_s_block_number}, next: ${next_s_block_number}", 
        ("prev_s_block_number", prev_s_block_number)("next_s_block_number", next_s_block_number));

    // Modify the first 4 bytes directly
    uint32_t next_s_block_number_reversed = fc::endian_reverse_u32(next_s_block_number);
    std::memcpy(curr_s_id.data(), &next_s_block_number_reversed, sizeof(uint32_t)); // Modify the first 4 bytes

    // ilog("Computed curr_s_id with block number embedded: ${c}", ("c", curr_s_id.str()));
    return curr_s_id;
}

/**
 * Find transactions that are relevant for the S-Root calculation.
 * @param chain the active chain controller
 * @return a vector of transactions deemed relevant
 */
std::vector<sysio::chain::transaction> sub_chain_plugin::find_relevant_transactions(sysio::chain::controller& curr_chain) {
    std::vector<sysio::chain::transaction> relevant_transactions;
    auto& pending_trx_receipts = curr_chain.get_pending_trx_receipts(); // Access pending transactions directly
    for (const auto& trx_receipt : pending_trx_receipts) {
        if (std::holds_alternative<sysio::chain::packed_transaction>(trx_receipt.trx)) {
            const auto& packed_trx = std::get<sysio::chain::packed_transaction>(trx_receipt.trx);
            const auto& trx = packed_trx.get_transaction();
            if (is_relevant_s_root_transaction(trx)) {
                relevant_transactions.push_back(trx);
            }
        }
    }
    return relevant_transactions;
}

/**
 * Determine if a transaction is relevant to the S-Root process.
 * @param trx the transaction to check
 * @return true if relevant, false otherwise
 */
bool sub_chain_plugin::is_relevant_s_root_transaction(const transaction& trx) {
   for (const auto& action : trx.actions) {
      if (action.account == my->contract_name && std::find(my->action_names.begin(), my->action_names.end(), action.name) != my->action_names.end()){
         return true;
      }
   }
   return false;
}

} // namespace sysio