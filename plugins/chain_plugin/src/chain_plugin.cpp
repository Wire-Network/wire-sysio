#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain_plugin/trx_retry_db.hpp>
#include <sysio/chain_plugin/tracked_votes.hpp>
#include <sysio/chain/block_log.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/wasm_interface.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/contract_action_match.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/chain/deep_mind.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain_plugin/trx_finality_status_processing.hpp>
#include <sysio/chain/permission_link_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/block_header_state_utils.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/resource_monitor_plugin/resource_monitor_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <chainbase/environment.hpp>

#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
#include <sysio/chain/webassembly/native-module/native_module_overlay.hpp>
#endif

#include <boost/signals2/connection.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant.hpp>
#include <fc/crypto/hex.hpp>
#include <cstdlib>


const std::string deep_mind_logger_name("dmlog");
sysio::chain::deep_mind_handler _deep_mind_log;

namespace std {
   // declare operator<< for boost program options of vector<string>
   std::ostream& operator<<(std::ostream& osm, const std::vector<std::string>& v) {
      auto size = v.size();
      osm << "{";
      for (size_t i = 0; i < size; ++i) {
         osm << v[i];
         if (i < size - 1) {
            osm << ", ";
         }
      }
      osm << "}";
      return osm;
   }
}

namespace sysio {

//declare operator<< and validate function for read_mode in the same namespace as read_mode itself
namespace chain {

std::ostream& operator<<(std::ostream& osm, sysio::chain::db_read_mode m) {
   if ( m == sysio::chain::db_read_mode::HEAD ) {
      osm << "head";
   } else if ( m == sysio::chain::db_read_mode::IRREVERSIBLE ) {
      osm << "irreversible";
   } else if ( m == sysio::chain::db_read_mode::SPECULATIVE ) {
      osm << "speculative";
   }

   return osm;
}

void validate(boost::any& v,
              const std::vector<std::string>& values,
              sysio::chain::db_read_mode* /* target_type */,
              int)
{
  using namespace boost::program_options;

  // Make sure no previous assignment to 'v' was made.
  validators::check_first_occurrence(v);

  // Extract the first string from 'values'. If there is more than
  // one string, it's an error, and exception will be thrown.
  std::string const& s = validators::get_single_string(values);

  if ( s == "head" ) {
     v = boost::any(sysio::chain::db_read_mode::HEAD);
  } else if ( s == "irreversible" ) {
     v = boost::any(sysio::chain::db_read_mode::IRREVERSIBLE);
  } else if ( s == "speculative" ) {
     v = boost::any(sysio::chain::db_read_mode::SPECULATIVE);
  } else {
     throw validation_error(validation_error::invalid_option_value);
  }
}

std::ostream& operator<<(std::ostream& osm, sysio::chain::validation_mode m) {
   if ( m == sysio::chain::validation_mode::FULL ) {
      osm << "full";
   } else if ( m == sysio::chain::validation_mode::LIGHT ) {
      osm << "light";
   }

   return osm;
}

void validate(boost::any& v,
              const std::vector<std::string>& values,
              sysio::chain::validation_mode* /* target_type */,
              int)
{
  using namespace boost::program_options;

  // Make sure no previous assignment to 'v' was made.
  validators::check_first_occurrence(v);

  // Extract the first string from 'values'. If there is more than
  // one string, it's an error, and exception will be thrown.
  std::string const& s = validators::get_single_string(values);

  if ( s == "full" ) {
     v = boost::any(sysio::chain::validation_mode::FULL);
  } else if ( s == "light" ) {
     v = boost::any(sysio::chain::validation_mode::LIGHT);
  } else {
     throw validation_error(validation_error::invalid_option_value);
  }
}

void validate(boost::any& v,
              const std::vector<std::string>& values,
              wasm_interface::vm_oc_enable* /* target_type */,
              int)
{
  using namespace boost::program_options;

  // Make sure no previous assignment to 'v' was made.
  validators::check_first_occurrence(v);

  // Extract the first string from 'values'. If there is more than
  // one string, it's an error, and exception will be thrown.
  std::string s = validators::get_single_string(values);
  boost::algorithm::to_lower(s);

  if (s == "auto") {
     v = boost::any(wasm_interface::vm_oc_enable::oc_auto);
  } else if (s == "all" || s == "true" || s == "on" || s == "yes" || s == "1") {
     v = boost::any(wasm_interface::vm_oc_enable::oc_all);
  } else if (s == "none" || s == "false" || s == "off" || s == "no" || s == "0") {
     v = boost::any(wasm_interface::vm_oc_enable::oc_none);
  } else {
     throw validation_error(validation_error::invalid_option_value);
  }
}

} // namespace chain

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::chain::config;
using namespace sysio::chain::plugin_interface;
using vm_type = wasm_interface::vm_type;
using fc::flat_map;

using boost::signals2::scoped_connection;

class chain_plugin_impl {
public:
   chain_plugin_impl()
   :accepted_block_header_channel(app().get_channel<channels::accepted_block_header>())
   ,accepted_block_channel(app().get_channel<channels::accepted_block>())
   ,irreversible_block_channel(app().get_channel<channels::irreversible_block>())
   ,applied_transaction_channel(app().get_channel<channels::applied_transaction>())
   ,incoming_transaction_async_method(app().get_method<incoming::methods::transaction_async>())
   {}

   std::filesystem::path             finalizers_dir;
   std::filesystem::path             blocks_dir;
   std::filesystem::path             state_dir;
   bool                              readonly = false;
   flat_map<uint32_t, block_id_type> loaded_checkpoints;
   bool                              accept_votes = false;
   bool                              accept_transactions     = false;
   bool                              api_accept_transactions = true;
   bool                              account_queries_enabled = false;

   std::optional<controller::config> chain_config;
   std::optional<controller>         chain;
   std::optional<genesis_state>      genesis;
   std::optional<vm_type>            wasm_runtime;
   fc::microseconds                  abi_serializer_max_time_us;
   std::optional<std::filesystem::path>          snapshot_path;

   // --native-contract mappings: account -> path to .so
   std::vector<std::pair<chain::name, std::filesystem::path>> native_contracts;
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
   webassembly::native_module::native_module_overlay native_overlay_;
#endif


   // retained references to channels for easy publication
   channels::accepted_block_header::channel_type&  accepted_block_header_channel;
   channels::accepted_block::channel_type&         accepted_block_channel;
   channels::irreversible_block::channel_type&     irreversible_block_channel;
   channels::applied_transaction::channel_type&    applied_transaction_channel;

   // retained references to methods for easy calling
   incoming::methods::transaction_async::method_type& incoming_transaction_async_method;

   // method provider handles
   methods::get_block_by_id::method_type::handle                     get_block_by_id_provider;
   methods::get_head_block_id::method_type::handle                   get_head_block_id_provider;

   // scoped connections for chain controller
   std::optional<scoped_connection>                                   accepted_block_header_connection;
   std::optional<scoped_connection>                                   accepted_block_connection;
   std::optional<scoped_connection>                                   irreversible_block_connection;
   std::optional<scoped_connection>                                   applied_transaction_connection;
   std::optional<scoped_connection>                                   block_start_connection;

   std::optional<chain_apis::get_info_db>                             _get_info_db;
   std::optional<chain_apis::account_query_db>                        _account_query_db;
   std::optional<chain_apis::trx_retry_db>                            _trx_retry_db;
   chain_apis::trx_finality_status_processing_ptr                     _trx_finality_status_processing;
   std::optional<chain_apis::tracked_votes>                           _last_tracked_votes;

   static void handle_guard_exception(const chain::guard_exception& e);
   void do_hard_replay(const variables_map& options);
   void enable_accept_transactions();
   void plugin_initialize(const variables_map& options);
   void plugin_startup();

private:
   static void log_guard_exception(const chain::guard_exception& e);
};

chain_plugin::chain_plugin()
:my(new chain_plugin_impl()) {
   app().register_config_type<sysio::chain::db_read_mode>();
   app().register_config_type<sysio::chain::validation_mode>();
   app().register_config_type<chainbase::pinnable_mapped_file::map_mode>();
   app().register_config_type<sysio::chain::wasm_interface::vm_type>();
   app().register_config_type<sysio::chain::wasm_interface::vm_oc_enable>();
}

chain_plugin::~chain_plugin() = default;

void chain_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   // build wasm_runtime help text
   std::string wasm_runtime_opt = "Override default WASM runtime (";
   std::string wasm_runtime_desc;
   std::string delim;
#ifdef SYSIO_SYS_VM_JIT_RUNTIME_ENABLED
   wasm_runtime_opt += " \"sys-vm-jit\"";
   wasm_runtime_desc += "\"sys-vm-jit\" : A WebAssembly runtime that compiles WebAssembly code to native x86 code prior to execution.\n";
   delim = ", ";
#endif

#ifdef SYSIO_SYS_VM_RUNTIME_ENABLED
   wasm_runtime_opt += delim + "\"sys-vm\"";
   wasm_runtime_desc += "\"sys-vm\" : A WebAssembly interpreter.\n";
   delim = ", ";
#endif

   wasm_runtime_opt += ")\n" + wasm_runtime_desc;

   std::string default_wasm_runtime_str= sysio::chain::wasm_interface::vm_type_string(sysio::chain::config::default_wasm_runtime);

   cfg.add_options()
         ("blocks-dir", bpo::value<std::filesystem::path>()->default_value("blocks"),
          "the location of the blocks directory (absolute path or relative to application data dir)")
         ("blocks-log-stride", bpo::value<uint32_t>(),
         "split the block log file when the head block number is the multiple of the stride\n"
         "When the stride is reached, the current block log and index will be renamed '<blocks-retained-dir>/blocks-<start num>-<end num>.log/index'\n"
         "and a new current block log and index will be created with the most recent block. All files following\n"
         "this format will be used to construct an extended block log.")
         ("max-retained-block-files", bpo::value<uint32_t>(),
          "the maximum number of blocks files to retain so that the blocks in those files can be queried.\n"
          "When the number is reached, the oldest block file would be moved to archive dir or deleted if the archive dir is empty.\n"
          "The retained block log files should not be manipulated by users." )
         ("blocks-retained-dir", bpo::value<std::filesystem::path>(),
          "the location of the blocks retained directory (absolute path or relative to blocks dir).\n"
          "If the value is empty, it is set to the value of blocks dir.")
         ("blocks-archive-dir", bpo::value<std::filesystem::path>(),
          "the location of the blocks archive directory (absolute path or relative to blocks dir).\n"
          "If the value is empty, blocks files beyond the retained limit will be deleted.\n"
          "All files in the archive directory are completely under user's control, i.e. they won't be accessed by nodeop anymore.")
         ("state-dir", bpo::value<std::filesystem::path>()->default_value(config::default_state_dir_name),
          "the location of the state directory (absolute path or relative to application data dir)")
         ("finalizers-dir", bpo::value<std::filesystem::path>()->default_value(config::default_finalizers_dir_name),
          "the location of the finalizers safety data directory (absolute path or relative to application data dir)")
         ("protocol-features-dir", bpo::value<std::filesystem::path>()->default_value("protocol_features"),
          "the location of the protocol_features directory (absolute path or relative to application config dir)")
         ("checkpoint", bpo::value<vector<string>>()->composing(), "Pairs of [BLOCK_NUM,BLOCK_ID] that should be enforced as checkpoints.")
         ("wasm-runtime", bpo::value<sysio::chain::wasm_interface::vm_type>()->value_name("runtime")->notifier([](const auto& vm){
            if(vm == wasm_interface::vm_type::sys_vm_oc)
               wlog("sys-vm-oc-forced mode is not supported. It is for development purposes only");
         })->default_value(sysio::chain::config::default_wasm_runtime, default_wasm_runtime_str), wasm_runtime_opt.c_str()
         )
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
         ("native-contract", bpo::value<vector<string>>()->composing(),
          "Route execution of a contract through a native .so for debugger support.\n"
          "Format: account:/path/to/contract_native.so\n"
          "May be specified multiple times. Normal WASM runtime handles all other contracts.\n"
          "State data is automatically copied to .native-debug/ directories to protect originals.")
#endif
         ("profile-account", boost::program_options::value<vector<string>>()->composing(),
          "The name of an account whose code will be profiled")
         ("abi-serializer-max-time-ms", bpo::value<uint32_t>()->default_value(config::default_abi_serializer_max_time_us / 1000),
          "Override default maximum ABI serialization time allowed in ms")
         ("chain-state-db-size-mb", bpo::value<uint64_t>()->default_value(config::default_state_size / (1024  * 1024)), "Maximum size (in MiB) of the chain state database")
         ("chain-state-db-guard-size-mb", bpo::value<uint64_t>()->default_value(config::default_state_guard_size / (1024  * 1024)), "Safely shut down node when free space remaining in the chain state database drops below this size (in MiB).")
         ("signature-cpu-billable-pct", bpo::value<uint32_t>()->default_value(config::default_sig_cpu_bill_pct / config::percent_1),
          "Percentage of actual signature recovery cpu to bill. Whole number percentages, e.g. 50 for 50%")
         ("chain-threads", bpo::value<uint16_t>()->default_value(config::default_controller_thread_pool_size),
          "Number of worker threads in controller thread pool")
         ("vote-threads", bpo::value<uint16_t>(),
          "Number of worker threads in vote processor thread pool. If set to 0, voting disabled, votes are not propagatged on P2P network. Defaults to 4 on producer nodes.")
         ("contracts-console", bpo::bool_switch()->default_value(false),
          "print contract's output to console")
         ("deep-mind", bpo::bool_switch()->default_value(false),
          "print deeper information about chain operations")
         ("actor-whitelist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Account added to actor whitelist (may specify multiple times)")
         ("actor-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Account added to actor blacklist (may specify multiple times)")
         ("contract-whitelist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Contract account added to contract whitelist (may specify multiple times)")
         ("contract-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Contract account added to contract blacklist (may specify multiple times)")
         ("action-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Action (in the form code::action) added to action blacklist (may specify multiple times)")
         ("key-blacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Public key added to blacklist of keys that should not be included in authorities (may specify multiple times)")
         ("sender-bypass-whiteblacklist", boost::program_options::value<vector<string>>()->composing()->multitoken(),
          "Deferred transactions sent by accounts in this list do not have any of the subjective whitelist/blacklist checks applied to them (may specify multiple times)")
         ("read-mode", boost::program_options::value<sysio::chain::db_read_mode>()->default_value(sysio::chain::db_read_mode::HEAD),
          "Database read mode (\"head\", \"irreversible\", \"speculative\").\n"
          "In \"head\" mode: database contains state changes up to the head block; transactions received by the node are relayed if valid.\n"
          "In \"irreversible\" mode: database contains state changes up to the last irreversible block; "
          "transactions received by the node are speculatively executed and relayed if valid.\n"
          "In \"speculative\" mode: database contains state changes by transactions in the blockchain "
          "up to the head block as well as some transactions not yet included in the blockchain; transactions received by the node are relayed if valid.\n"
          )
         ( "api-accept-transactions", bpo::value<bool>()->default_value(true), "Allow API transactions to be evaluated and relayed if valid.")
         ("validation-mode", boost::program_options::value<sysio::chain::validation_mode>()->default_value(sysio::chain::validation_mode::FULL),
          "Chain validation mode (\"full\" or \"light\").\n"
          "In \"full\" mode all incoming blocks will be fully validated.\n"
          "In \"light\" mode all incoming blocks headers will be fully validated; transactions in those validated blocks will be trusted \n")
         ("disable-ram-billing-notify-checks", bpo::bool_switch()->default_value(false),
          "Disable the check which subjectively fails a transaction if a contract bills more RAM to another account within the context of a notification handler (i.e. when the receiver is not the code of the action).")
         ("maximum-variable-signature-length", bpo::value<uint32_t>()->default_value(16384u),
          "Subjectively limit the maximum length of variable components in a variable legnth signature to this size in bytes")
         ("trusted-producer", bpo::value<vector<string>>()->composing(), "Indicate a producer whose blocks headers signed by it will be fully validated, but transactions in those validated blocks will be trusted.")
         ("database-map-mode", bpo::value<chainbase::pinnable_mapped_file::map_mode>()->default_value(chainbase::pinnable_mapped_file::map_mode::mapped),
          "Database map mode (\"mapped\", \"mapped_private\", \"heap\", or \"locked\").\n"
          "In \"mapped\" mode database is memory mapped as a file.\n"
          "In \"mapped_private\" mode database is memory mapped as a file using a private mapping (no disk writeback until program exit).\n"
#ifndef _WIN32
          "In \"heap\" mode database is preloaded in to swappable memory and will use huge pages if available.\n"
          "In \"locked\" mode database is preloaded, locked in to memory, and will use huge pages if available.\n"
#endif
         )

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
         ("sys-vm-oc-cache-size-mb", bpo::value<uint64_t>()->default_value(sysvmoc::config().cache_size / (1024u*1024u)), "Maximum size (in MiB) of the SYS VM OC code cache")
         ("sys-vm-oc-compile-threads", bpo::value<uint64_t>()->default_value(1u)->notifier([](const auto t) {
               if(t == 0) {
                  elog("sys-vm-oc-compile-threads must be set to a non-zero value");
                  SYS_ASSERT(false, plugin_exception, "");
               }
         }), "Number of threads to use for SYS VM OC tier-up")
         ("sys-vm-oc-enable", bpo::value<chain::wasm_interface::vm_oc_enable>()->default_value(chain::wasm_interface::vm_oc_enable::oc_auto),
          "Enable SYS VM OC tier-up runtime ('auto', 'all', 'none').\n"
          "'auto' - SYS VM OC tier-up is enabled for sysio.* accounts, read-only trxs, and except on producers applying blocks.\n"
          "'all'  - SYS VM OC tier-up is enabled for all contract execution.\n"
          "'none' - SYS VM OC tier-up is completely disabled.\n")
         ("sys-vm-oc-whitelist", bpo::value<vector<string>>()->composing()->multitoken()->default_value(std::vector<string>{"wire"}),
          "SYS VM OC tier-up whitelist account suffixes for tier-up runtime 'auto'.")
#endif
         ("enable-account-queries", bpo::value<bool>()->default_value(false), "enable queries to find accounts by various metadata.")
         ("transaction-retry-max-storage-size-gb", bpo::value<uint64_t>(),
          "Maximum size (in GiB) allowed to be allocated for the Transaction Retry feature. Setting above 0 enables this feature.")
         ("transaction-retry-interval-sec", bpo::value<uint32_t>()->default_value(20),
          "How often, in seconds, to resend an incoming transaction to network if not seen in a block.\n"
          "Needs to be at least twice as large as p2p-dedup-cache-expire-time-sec.")
         ("transaction-retry-max-expiration-sec", bpo::value<uint32_t>()->default_value(120),
          "Maximum allowed transaction expiration for retry transactions, will retry transactions up to this value.\n"
          "Should be larger than transaction-retry-interval-sec.")
         ("transaction-finality-status-max-storage-size-gb", bpo::value<uint64_t>(),
          "Maximum size (in GiB) allowed to be allocated for the Transaction Finality Status feature. Setting above 0 enables this feature.")
         ("transaction-finality-status-success-duration-sec", bpo::value<uint64_t>()->default_value(config::default_max_transaction_finality_status_success_duration_sec),
          "Duration (in seconds) a successful transaction's Finality Status will remain available from being first identified.")
         ("transaction-finality-status-failure-duration-sec", bpo::value<uint64_t>()->default_value(config::default_max_transaction_finality_status_failure_duration_sec),
          "Duration (in seconds) a failed transaction's Finality Status will remain available from being first identified.")
         ("disable-replay-opts", bpo::bool_switch()->default_value(false),
          "disable optimizations that specifically target replay")
         ("integrity-hash-on-start", bpo::bool_switch(), "Log the state integrity hash on startup")
         ("integrity-hash-on-stop", bpo::bool_switch(), "Log the state integrity hash on shutdown");

    cfg.add_options()("block-log-retain-blocks", bpo::value<uint32_t>(), "If set to greater than 0, periodically prune the block log to store only configured number of most recent blocks.\n"
        "If set to 0, no blocks are be written to the block log; block log file is removed after startup.");


   cli.add_options()
         ("genesis-json", bpo::value<std::filesystem::path>(), "File to read Genesis State from")
         ("genesis-timestamp", bpo::value<string>(), "override the initial timestamp in the Genesis State file")
         ("print-genesis-json", bpo::bool_switch()->default_value(false),
          "extract genesis_state from blocks.log as JSON, print to console, and exit")
         ("extract-genesis-json", bpo::value<std::filesystem::path>(),
          "extract genesis_state from blocks.log as JSON, write into specified file, and exit")
         ("print-build-info", bpo::bool_switch()->default_value(false),
          "print build environment information to console as JSON and exit")
         ("extract-build-info", bpo::value<std::filesystem::path>(),
          "extract build environment information as JSON, write into specified file, and exit")
         ("force-all-checks", bpo::bool_switch()->default_value(false),
          "do not skip any validation checks while replaying blocks (useful for replaying blocks from untrusted source)")
         ("replay-blockchain", bpo::bool_switch()->default_value(false),
          "clear chain state database and replay all blocks")
         ("hard-replay-blockchain", bpo::bool_switch()->default_value(false),
          "clear chain state database, recover as many blocks as possible from the block log, and then replay those blocks")
         ("delete-all-blocks", bpo::bool_switch()->default_value(false),
          "clear chain state database and block log")
         ("truncate-at-block", bpo::value<uint32_t>()->default_value(0),
          "Stop hard replay / block log recovery at this block number (if non-zero). "
          "Can also be used with terminate-at-block to prune any received blocks from fork database on exit.")
         ("terminate-at-block", bpo::value<uint32_t>()->default_value(0),
          "Stops the node after reaching the specified block number (if non-zero). "
          "Use RPC endpoint /v1/producer/pause_at_block to pause at a specific block instead. "
          "Combine with truncate-at-block to prune blocks beyond the specified number from the fork database on exit.")
         ("snapshot", bpo::value<std::filesystem::path>(), "File to read Snapshot State from")
         ;

}

#define LOAD_VALUE_SET(options, op_name, container) \
if( options.count(op_name) ) { \
   const std::vector<std::string>& ops = options[op_name].as<std::vector<std::string>>(); \
   for( const auto& v : ops ) { \
      container.emplace( sysio::chain::name( v ) ); \
   } \
}

fc::time_point calculate_genesis_timestamp( string tstr ) {
   fc::time_point genesis_timestamp;
   if( strcasecmp (tstr.c_str(), "now") == 0 ) {
      genesis_timestamp = fc::time_point::now();
   } else {
      genesis_timestamp = time_point::from_iso_string( tstr );
   }

   auto epoch_us = genesis_timestamp.time_since_epoch().count();
   auto diff_us = epoch_us % config::block_interval_us;
   if (diff_us > 0) {
      auto delay_us = (config::block_interval_us - diff_us);
      genesis_timestamp += fc::microseconds(delay_us);
      dlog("pausing {} microseconds to the next interval", delay_us);
   }

   ilog( "Adjusting genesis timestamp to {}", genesis_timestamp );
   return genesis_timestamp;
}

void clear_directory_contents( const std::filesystem::path& p ) {
   using std::filesystem::directory_iterator;

   if( !std::filesystem::is_directory( p ) )
      return;

   for( directory_iterator enditr, itr{p}; itr != enditr; ++itr ) {
      std::filesystem::remove_all( itr->path() );
   }
}

namespace {
  // This can be removed when versions of sysio that support reversible chainbase state file no longer supported.
  void upgrade_from_reversible_to_fork_db(chain_plugin_impl* my) {
     std::filesystem::path old_fork_db = my->chain_config->state_dir / config::fork_db_filename;
     std::filesystem::path new_fork_db = my->blocks_dir / config::reversible_blocks_dir_name / config::fork_db_filename;
     if( std::filesystem::exists( old_fork_db ) && std::filesystem::is_regular_file( old_fork_db ) ) {
        bool copy_file = false;
        if( std::filesystem::exists( new_fork_db ) && std::filesystem::is_regular_file( new_fork_db ) ) {
           if( std::filesystem::last_write_time( old_fork_db ) > std::filesystem::last_write_time( new_fork_db ) ) {
              copy_file = true;
           }
        } else {
           copy_file = true;
           std::filesystem::create_directories( my->blocks_dir / config::reversible_blocks_dir_name );
        }
        if( copy_file ) {
           std::filesystem::rename( old_fork_db, new_fork_db );
        } else {
           std::filesystem::remove( old_fork_db );
        }
     }
  }
}

void
chain_plugin_impl::do_hard_replay(const variables_map& options) {
   ilog( "Hard replay requested: deleting state database" );
   clear_directory_contents( chain_config->state_dir );
   auto backup_dir = block_log::repair_log( blocks_dir, options.at( "truncate-at-block" ).as<uint32_t>(), config::reversible_blocks_dir_name);
}

void chain_plugin_impl::plugin_initialize(const variables_map& options) {
   try {
      ilog("initializing chain plugin");
      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      if (!sig_plug.has_signature_providers(std::array{crypto::chain_key_type_wire})) {
         sig_plug.register_default_signature_providers({crypto::chain_key_type_wire});
      }
      if (!sig_plug.has_signature_providers(std::array{crypto::chain_key_type_wire_bls})) {
         sig_plug.register_default_signature_providers({crypto::chain_key_type_wire_bls});
      }

      auto producer_sig_prov = sig_plug.query_providers(std::nullopt,std::nullopt,crypto::chain_key_type_wire).front();
      auto finalizer_sig_prov = sig_plug.query_providers(std::nullopt,std::nullopt,crypto::chain_key_type_wire_bls).front();

      chain_config = controller::config();

      if( options.at( "print-build-info" ).as<bool>() || options.contains( "extract-build-info") ) {
         if( options.at( "print-build-info" ).as<bool>() ) {
            ilog( "Build environment JSON:\n{}", json::to_pretty_string( chainbase::environment() ) );
         }
         if( options.contains( "extract-build-info") ) {
            auto p = options.at( "extract-build-info" ).as<std::filesystem::path>();

            if( p.is_relative()) {
               p = std::filesystem::current_path() / p;
            }

            SYS_ASSERT( fc::json::save_to_file( chainbase::environment(), p, true ), misc_exception,
                        "Error occurred while writing build info JSON to '{}'",
                        p.string()
            );

            ilog( "Saved build info JSON to '{}'", p.string() );
         }

         SYS_THROW( node_management_success, "reported build environment information" );
      }

      LOAD_VALUE_SET( options, "sender-bypass-whiteblacklist", chain_config->sender_bypass_whiteblacklist );
      LOAD_VALUE_SET( options, "actor-whitelist", chain_config->actor_whitelist );
      LOAD_VALUE_SET( options, "actor-blacklist", chain_config->actor_blacklist );
      LOAD_VALUE_SET( options, "contract-whitelist", chain_config->contract_whitelist );
      LOAD_VALUE_SET( options, "contract-blacklist", chain_config->contract_blacklist );
      LOAD_VALUE_SET( options, "sys-vm-oc-whitelist", chain_config->sys_vm_oc_whitelist_suffixes);

      LOAD_VALUE_SET( options, "trusted-producer", chain_config->trusted_producers );

      if (!chain_config->sys_vm_oc_whitelist_suffixes.empty()) {
         const auto& wl = chain_config->sys_vm_oc_whitelist_suffixes;
         std::string s = std::accumulate(std::next(wl.begin()), wl.end(),
                                         wl.begin()->to_string(),
                                         [](std::string a, account_name b) -> std::string {
                                            return std::move(a) + ", " + b.to_string();
                                         });
         ilog("sys-vm-oc-whitelist accounts: {}", s);
      }
      if( options.count( "action-blacklist" )) {
         const std::vector<std::string>& acts = options["action-blacklist"].as<std::vector<std::string>>();
         auto& list = chain_config->action_blacklist;
         for( const auto& a : acts ) {
            auto pos = a.find( "::" );
            SYS_ASSERT( pos != std::string::npos, plugin_config_exception, "Invalid entry in action-blacklist: '{}'", a);
            account_name code( a.substr( 0, pos ));
            action_name act( a.substr( pos + 2 ));
            list.emplace( code, act );
         }
      }

      if( options.contains( "key-blacklist" )) {
         const std::vector<std::string>& keys = options["key-blacklist"].as<std::vector<std::string>>();
         auto& list = chain_config->key_blacklist;
         for( const auto& key_str : keys ) {
            list.emplace( public_key_type::from_string(key_str) );
         }
      }

      if( options.count( "finalizers-dir" )) {
         auto fd = options.at( "finalizers-dir" ).as<std::filesystem::path>();
         if( fd.is_relative())
            finalizers_dir = app().data_dir() / fd;
         else
            finalizers_dir = fd;
      }

      if( options.count( "blocks-dir" )) {
         auto bld = options.at( "blocks-dir" ).as<std::filesystem::path>();
         if( bld.is_relative())
            blocks_dir = app().data_dir() / bld;
         else
            blocks_dir = bld;
      }

      if( options.contains( "state-dir" )) {
         auto sd = options.at( "state-dir" ).as<std::filesystem::path>();
         if( sd.is_relative())
            state_dir = app().data_dir() / sd;
         else
            state_dir = sd;
      }

      protocol_feature_set pfs;
      {
         std::filesystem::path protocol_features_dir;
         auto pfd = options.at( "protocol-features-dir" ).as<std::filesystem::path>();
         if( pfd.is_relative())
            protocol_features_dir = app().config_dir() / pfd;
         else
            protocol_features_dir = pfd;

         pfs = initialize_protocol_features( protocol_features_dir );
      }

      if( options.contains("checkpoint") ) {
         auto cps = options.at("checkpoint").as<vector<string>>();
         loaded_checkpoints.reserve(cps.size());
         for( const auto& cp : cps ) {
            auto item = fc::json::from_string(cp).as<std::pair<uint32_t,block_id_type>>();
            auto itr = loaded_checkpoints.find(item.first);
            if( itr != loaded_checkpoints.end() ) {
               SYS_ASSERT( itr->second == item.second,
                           plugin_config_exception,
                          "redefining existing checkpoint at block number {}: original: {} new: {}",
                          item.first, itr->second, item.second
               );
            } else {
               loaded_checkpoints[item.first] = item.second;
            }
         }
      }

      if( options.contains( "wasm-runtime" ))
         wasm_runtime = options.at( "wasm-runtime" ).as<vm_type>();

      LOAD_VALUE_SET( options, "profile-account", chain_config->profile_accounts );

      if( options.count( "native-contract" )) {
         const auto& nc_opts = options["native-contract"].as<std::vector<std::string>>();
         for( const auto& nc : nc_opts ) {
            auto colon = nc.find(':');
            SYS_ASSERT( colon != std::string::npos && colon > 0 && colon + 1 < nc.size(),
                        plugin_config_exception,
                        "Invalid --native-contract format '{}'. Expected account:/path/to/contract.so", nc );
            auto acct = chain::name( nc.substr(0, colon) );
            auto so_path = std::filesystem::path( nc.substr(colon + 1) );
            SYS_ASSERT( std::filesystem::exists(so_path), plugin_config_exception,
                        "Native contract .so not found: {}", so_path.string() );
            native_contracts.emplace_back( acct, std::move(so_path) );
            ilog( "Native contract debug: {} -> {}", acct, native_contracts.back().second.string() );
         }
      }

      abi_serializer_max_time_us = fc::microseconds(options.at("abi-serializer-max-time-ms").as<uint32_t>() * 1000);

      chain_config->finalizers_dir = finalizers_dir;
      chain_config->blocks_dir = blocks_dir;
      chain_config->state_dir = state_dir;
      chain_config->read_only = readonly;

      if (auto resmon_plugin = app().find_plugin<resource_monitor_plugin>()) {
        resmon_plugin->monitor_directory(chain_config->blocks_dir);
        resmon_plugin->monitor_directory(chain_config->state_dir);
      }

      if( options.contains( "chain-state-db-size-mb" )) {
         chain_config->state_size = options.at( "chain-state-db-size-mb" ).as<uint64_t>() * 1024 * 1024;
         SYS_ASSERT( chain_config->state_size <= 8ull * 1024 * 1024 * 1024 * 1024, plugin_config_exception,
                     "The maximum supported size for the chain state db (chain-state-db-size-mb) is 8TiB" );
      }

      if( options.contains( "chain-state-db-guard-size-mb" ))
         chain_config->state_guard_size = options.at( "chain-state-db-guard-size-mb" ).as<uint64_t>() * 1024 * 1024;

      if( options.contains( "transaction-finality-status-max-storage-size-gb" )) {
         const uint64_t max_storage_size = options.at( "transaction-finality-status-max-storage-size-gb" ).as<uint64_t>() * 1024 * 1024 * 1024;
         if (max_storage_size > 0) {
            const fc::microseconds success_duration = fc::seconds(options.at( "transaction-finality-status-success-duration-sec" ).as<uint64_t>());
            const fc::microseconds failure_duration = fc::seconds(options.at( "transaction-finality-status-failure-duration-sec" ).as<uint64_t>());
            _trx_finality_status_processing.reset(
               new chain_apis::trx_finality_status_processing(max_storage_size, success_duration, failure_duration));
         }
      }

      if( options.count( "chain-threads" )) {
         chain_config->chain_thread_pool_size = options.at( "chain-threads" ).as<uint16_t>();
         SYS_ASSERT( chain_config->chain_thread_pool_size > 0, plugin_config_exception,
                     "chain-threads {} must be greater than 0", chain_config->chain_thread_pool_size );
      }

      if (options.count("producer-name") || options.count("vote-threads")) {
         chain_config->vote_thread_pool_size = options.count("vote-threads") ? options.at("vote-threads").as<uint16_t>() : 0;
         if (chain_config->vote_thread_pool_size == 0 && options.count("producer-name")) {
            chain_config->vote_thread_pool_size = config::default_vote_thread_pool_size;
            ilog("Setting vote-threads to {} on producing node", chain_config->vote_thread_pool_size);
         }
         accept_votes = chain_config->vote_thread_pool_size > 0;
      }

      chain_config->sig_cpu_bill_pct = options.at("signature-cpu-billable-pct").as<uint32_t>();
      SYS_ASSERT( chain_config->sig_cpu_bill_pct >= 0 && chain_config->sig_cpu_bill_pct <= 100, plugin_config_exception,
                  "signature-cpu-billable-pct must be 0 - 100, {}", chain_config->sig_cpu_bill_pct );
      chain_config->sig_cpu_bill_pct *= config::percent_1;

      if( wasm_runtime )
         chain_config->wasm_runtime = *wasm_runtime;

      chain_config->force_all_checks = options.at( "force-all-checks" ).as<bool>();
      chain_config->disable_replay_opts = options.at( "disable-replay-opts" ).as<bool>();
      chain_config->contracts_console = options.at( "contracts-console" ).as<bool>();
      chain_config->allow_ram_billing_in_notify = options.at( "disable-ram-billing-notify-checks" ).as<bool>();

      chain_config->maximum_variable_signature_length = options.at( "maximum-variable-signature-length" ).as<uint32_t>();

      chain_config->terminate_at_block = options.at( "terminate-at-block" ).as<uint32_t>();
      chain_config->truncate_at_block = options.at( "truncate-at-block" ).as<uint32_t>();

      chain_config->num_configured_p2p_peers = options.count( "p2p-peer-address" );

      // move fork_db to new location
      upgrade_from_reversible_to_fork_db( this );

      bool has_partitioned_block_log_options = options.contains("blocks-retained-dir") ||  options.contains("blocks-archive-dir")
         || options.contains("blocks-log-stride") || options.contains("max-retained-block-files");
      bool has_retain_blocks_option = options.contains("block-log-retain-blocks");

      SYS_ASSERT(!has_partitioned_block_log_options || !has_retain_blocks_option, plugin_config_exception,
         "block-log-retain-blocks cannot be specified together with blocks-retained-dir, blocks-archive-dir or blocks-log-stride or max-retained-block-files.");

      std::filesystem::path retained_dir;
      if (has_partitioned_block_log_options) {
         retained_dir = options.contains("blocks-retained-dir") ? options.at("blocks-retained-dir").as<std::filesystem::path>()
                                                                 : std::filesystem::path("");
         if (retained_dir.is_relative())
            retained_dir = std::filesystem::path{blocks_dir}/retained_dir;

         chain_config->blog = sysio::chain::partitioned_blocklog_config{
            .retained_dir = retained_dir,
            .archive_dir  = options.contains("blocks-archive-dir") ? options.at("blocks-archive-dir").as<std::filesystem::path>()
                                                               : std::filesystem::path("archive"),
            .stride       = options.contains("blocks-log-stride") ? options.at("blocks-log-stride").as<uint32_t>()
                                                               : UINT32_MAX,
            .max_retained_files = options.contains("max-retained-block-files")
                                       ? options.at("max-retained-block-files").as<uint32_t>()
                                       : UINT32_MAX,
         };
      } else if(has_retain_blocks_option) {
         uint32_t block_log_retain_blocks = options.at("block-log-retain-blocks").as<uint32_t>();
         if (block_log_retain_blocks == 0) {
            chain_config->blog = sysio::chain::empty_blocklog_config{};
         } else {
            SYS_ASSERT(cfile::supports_hole_punching(), plugin_config_exception,
                       "block-log-retain-blocks cannot be greater than 0 because the file system does not support hole "
                       "punching");
            chain_config->blog = sysio::chain::prune_blocklog_config{ .prune_blocks = block_log_retain_blocks };
         }
      }



      if( options.contains( "extract-genesis-json" ) || options.at( "print-genesis-json" ).as<bool>()) {
         std::optional<genesis_state> gs;
         
         gs = block_log::extract_genesis_state( blocks_dir, retained_dir );
         SYS_ASSERT( gs,
                     plugin_config_exception,
                     "Block log at '{}' does not contain a genesis state, it only has the chain-id.",
                     (blocks_dir / "blocks.log").generic_string()
         );


         if( options.at( "print-genesis-json" ).as<bool>()) {
            ilog( "Genesis JSON:\n{}", json::to_pretty_string( *gs ) );
         }

         if( options.contains( "extract-genesis-json" )) {
            auto p = options.at( "extract-genesis-json" ).as<std::filesystem::path>();

            if( p.is_relative()) {
               p = std::filesystem::current_path() / p;
            }

            SYS_ASSERT( fc::json::save_to_file( *gs, p, true ),
                        misc_exception,
                        "Error occurred while writing genesis JSON to '{}'",
                        p.generic_string()
            );

            ilog( "Saved genesis JSON to '{}'", p.generic_string() );
         }

         SYS_THROW( extract_genesis_state_exception, "extracted genesis state from blocks.log" );
      }

      uint32_t truncate_at_block = options.at( "truncate-at-block" ).as<uint32_t>();
      uint32_t terminate_at_block = options.at( "terminate-at-block" ).as<uint32_t>();
      if (truncate_at_block > 0 && terminate_at_block > 0) {
         SYS_ASSERT(truncate_at_block == terminate_at_block, plugin_config_exception,
                    "truncate-at-block {} must match terminate-at-block {}",
                    truncate_at_block, terminate_at_block);
      } else if (truncate_at_block > 0 && !options.at( "hard-replay-blockchain" ).as<bool>()) {
         wlog("truncate-at-block only applicable to --hard-replay-blockchain unless specified with --terminate-at-block");
      }

      if( options.at( "delete-all-blocks" ).as<bool>()) {
         ilog( "Deleting state database and blocks" );
         clear_directory_contents( chain_config->state_dir );
         clear_directory_contents( blocks_dir );
      } else if( options.at( "hard-replay-blockchain" ).as<bool>()) {
         do_hard_replay(options);
      } else if( options.at( "replay-blockchain" ).as<bool>()) {
         ilog( "Replay requested: deleting state database" );
         if (!options.count( "snapshot" )) {
            auto first_block = block_log::extract_first_block_num(blocks_dir, retained_dir);
            SYS_ASSERT(first_block == 1, plugin_config_exception,
                       "replay-blockchain without snapshot requested without a full block log, first block: {}", first_block);
         }
         clear_directory_contents( chain_config->state_dir );
      }

      std::optional<chain_id_type> chain_id;
      if (options.contains( "snapshot" )) {
         snapshot_path = options.at( "snapshot" ).as<std::filesystem::path>();
         SYS_ASSERT( std::filesystem::exists(*snapshot_path), plugin_config_exception,
                     "Cannot load snapshot, {} does not exist", snapshot_path->generic_string() );

         // recover genesis information from the snapshot
         // used for validation code below; load_index() is cheap (no hash verification)
         threaded_snapshot_reader reader(*snapshot_path);
         reader.load_index();
         chain_id = controller::extract_chain_id(reader);

         SYS_ASSERT(
           !options.contains( "genesis-timestamp" ),
                 plugin_config_exception,
                 "--snapshot is incompatible with --genesis-timestamp as the snapshot contains genesis information");
         SYS_ASSERT(
           !options.contains( "genesis-json" ),
                     plugin_config_exception,
                     "--snapshot is incompatible with --genesis-json as the snapshot contains genesis information");

         auto shared_mem_path = chain_config->state_dir / "shared_memory.bin";
         auto chain_head_path = chain_config->state_dir / chain_head_filename;
         auto dedup_path      = chain_config->state_dir / config::transaction_dedup_filename;
         SYS_ASSERT(!std::filesystem::is_regular_file(shared_mem_path) &&
                    !std::filesystem::is_regular_file(chain_head_path) &&
                    !std::filesystem::is_regular_file(dedup_path),
                    plugin_config_exception,
                    "Snapshot can only be used to initialize an empty database, remove directory: {}",
                    chain_config->state_dir.generic_string());

         auto block_log_chain_id = block_log::extract_chain_id(blocks_dir, retained_dir);

         if (block_log_chain_id) {
            SYS_ASSERT( *chain_id == *block_log_chain_id,
                           plugin_config_exception,
                           "snapshot chain ID ({}) does not match the chain ID ({}) in the block log",
                           *chain_id,
                           *block_log_chain_id
               );
         }

      } else {

         chain_id = controller::extract_chain_id_from_db( chain_config->state_dir );

         auto chain_context = block_log::extract_chain_context( blocks_dir, retained_dir );
         std::optional<genesis_state> block_log_genesis;
         std::optional<chain_id_type> block_log_chain_id;

         if (chain_context) {
            std::visit(overloaded {
               [&](const genesis_state& gs) {
                  block_log_genesis = gs;
                  block_log_chain_id = gs.compute_chain_id();
               },
               [&](const chain_id_type& id) {
                  block_log_chain_id = id;
               }
            }, *chain_context);

            if( chain_id ) {
               SYS_ASSERT( *block_log_chain_id == *chain_id, block_log_exception,
                           "Chain ID in blocks.log ({}) does not match the existing "
                           " chain ID in state ({}).",
                           *block_log_chain_id,
                           *chain_id
               );
            } else if (block_log_genesis) {
               ilog( "Starting fresh blockchain state using genesis state extracted from blocks.log." );
               genesis = block_log_genesis;
               // Delay setting chain_id until later so that the code handling genesis-json below can know
               // that chain_id still only represents a chain ID extracted from the state (assuming it exists).
            }
         }

         if( options.contains( "genesis-json" ) ) {
            std::filesystem::path genesis_file = options.at( "genesis-json" ).as<std::filesystem::path>();
            if( genesis_file.is_relative()) {
               genesis_file = std::filesystem::current_path() / genesis_file;
            }

            SYS_ASSERT( std::filesystem::is_regular_file( genesis_file ),
                        plugin_config_exception,
                       "Specified genesis file '{}' does not exist.",
                       genesis_file.string());

            genesis_state provided_genesis = fc::json::from_file( genesis_file ).as<genesis_state>();

            if( options.contains( "genesis-timestamp" ) ) {
               provided_genesis.initial_timestamp = calculate_genesis_timestamp( options.at( "genesis-timestamp" ).as<string>() );

               ilog( "Using genesis state provided in '{}' but with adjusted genesis timestamp", genesis_file.string() );
            } else {
               ilog( "Using genesis state provided in '{}'", genesis_file.string() );
            }

            if( block_log_genesis ) {
               SYS_ASSERT( *block_log_genesis == provided_genesis, plugin_config_exception,
                           "Genesis state, provided via command line arguments, does not match the existing genesis state"
                           " in blocks.log. It is not necessary to provide genesis state arguments when a full blocks.log "
                           "file already exists."
               );
            } else {
               const auto& provided_genesis_chain_id = provided_genesis.compute_chain_id();
               if( chain_id ) {
                  SYS_ASSERT( provided_genesis_chain_id == *chain_id, plugin_config_exception,
                              "Genesis state, provided via command line arguments, has a chain ID ({}) "
                              "that does not match the existing chain ID in the database state ({}). "
                              "It is not necessary to provide genesis state arguments when an initialized database state already exists.",
                              provided_genesis_chain_id,
                              *chain_id
                  );
               } else {
                  if( block_log_chain_id ) {
                     SYS_ASSERT( provided_genesis_chain_id == *block_log_chain_id, plugin_config_exception,
                                 "Genesis state, provided via command line arguments, has a chain ID ({}) "
                                 "that does not match the existing chain ID in blocks.log ({}).",
                                 provided_genesis_chain_id,
                                 *block_log_chain_id
                     );
                  }

                  chain_id = provided_genesis_chain_id;

                  ilog( "Starting fresh blockchain state using provided genesis state." );
                  genesis = std::move(provided_genesis);
               }
            }
         } else {
            SYS_ASSERT(
              !options.contains( "genesis-timestamp" ),
                        plugin_config_exception,
                        "--genesis-timestamp is only valid if also passed in with --genesis-json");
         }

         if( !chain_id ) {
            if( genesis ) {
               // Uninitialized state database and genesis state extracted from block log
               chain_id = genesis->compute_chain_id();
            } else {
               // Uninitialized state database and no genesis state provided

               SYS_ASSERT( !block_log_chain_id, plugin_config_exception,
                           "Genesis state is necessary to initialize fresh blockchain state but genesis state could not be "
                           "found in the blocks log. Please either load from snapshot or find a blocks log that starts "
                           "from genesis."
               );

               ilog( "Starting fresh blockchain state using default genesis state." );
               genesis.emplace(producer_sig_prov->public_key, finalizer_sig_prov->public_key);
               chain_id = genesis->compute_chain_id();
            }
         }
      }

      if ( options.contains("read-mode") ) {
         chain_config->read_mode = options.at("read-mode").as<db_read_mode>();
      }
      api_accept_transactions = options.at( "api-accept-transactions" ).as<bool>();

      if( api_accept_transactions ) {
         enable_accept_transactions();
      }

      if ( options.contains("validation-mode") ) {
         chain_config->block_validation_mode = options.at("validation-mode").as<validation_mode>();
      }

      chain_config->db_map_mode = options.at("database-map-mode").as<pinnable_mapped_file::map_mode>();

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
      if( options.contains("sys-vm-oc-cache-size-mb") )
         chain_config->sysvmoc_config.cache_size = options.at( "sys-vm-oc-cache-size-mb" ).as<uint64_t>() * 1024u * 1024u;
      if( options.contains("sys-vm-oc-compile-threads") )
         chain_config->sysvmoc_config.threads = options.at("sys-vm-oc-compile-threads").as<uint64_t>();
      chain_config->sysvmoc_tierup = options["sys-vm-oc-enable"].as<chain::wasm_interface::vm_oc_enable>();
#endif

      account_queries_enabled = options.at("enable-account-queries").as<bool>();

      chain_config->integrity_hash_on_start = options.at("integrity-hash-on-start").as<bool>();
      chain_config->integrity_hash_on_stop = options.at("integrity-hash-on-stop").as<bool>();

      // Native debug mode: operate on copied state to protect originals
      if( !native_contracts.empty() ) {
         namespace fs = std::filesystem;
         auto debug_state  = fs::path(chain_config->state_dir.string()  + ".native-debug");
         auto debug_blocks = fs::path(chain_config->blocks_dir.string() + ".native-debug");

         // Remove stale copies from previous debug sessions
         if( fs::exists(debug_state) )  fs::remove_all(debug_state);
         if( fs::exists(debug_blocks) ) fs::remove_all(debug_blocks);

         ilog("Native debug mode: copying state data for safe debugging...");
         if( fs::exists(chain_config->state_dir) )
            fs::copy(chain_config->state_dir,  debug_state,  fs::copy_options::recursive);
         else
            fs::create_directories(debug_state);

         if( fs::exists(chain_config->blocks_dir) )
            fs::copy(chain_config->blocks_dir, debug_blocks, fs::copy_options::recursive);
         else
            fs::create_directories(debug_blocks);

         chain_config->state_dir  = debug_state;
         chain_config->blocks_dir = debug_blocks;
         blocks_dir               = debug_blocks;

         ilog("Native debug mode: operating on copied state data.\n"
              "  Original chain data is untouched. Debug copies:\n"
              "    state:  {}\n"
              "    blocks: {}\n"
              "  Delete these directories when done debugging.",
              debug_state.string(), debug_blocks.string());
      }

      chain.emplace( *chain_config, std::move(pfs), *chain_id );

      if( options.contains( "transaction-retry-max-storage-size-gb" )) {
         SYS_ASSERT( !options.contains( "producer-name"), plugin_config_exception,
                     "Transaction retry not allowed on producer nodes." );
         const uint64_t max_storage_size = options.at( "transaction-retry-max-storage-size-gb" ).as<uint64_t>() * 1024 * 1024 * 1024;
         if( max_storage_size > 0 ) {
            const uint32_t p2p_dedup_time_s = options.at( "p2p-dedup-cache-expire-time-sec" ).as<uint32_t>();
            const uint32_t trx_retry_interval = options.at( "transaction-retry-interval-sec" ).as<uint32_t>();
            const uint32_t trx_retry_max_expire = options.at( "transaction-retry-max-expiration-sec" ).as<uint32_t>();
            SYS_ASSERT( trx_retry_interval >= 2 * p2p_dedup_time_s, plugin_config_exception,
                        "transaction-retry-interval-sec {} must be greater than 2 times p2p-dedup-cache-expire-time-sec {}",
                        trx_retry_interval, p2p_dedup_time_s );
            SYS_ASSERT( trx_retry_max_expire > trx_retry_interval, plugin_config_exception,
                        "transaction-retry-max-expiration-sec {} should be configured larger than transaction-retry-interval-sec {}",
                        trx_retry_max_expire, trx_retry_interval );
            _trx_retry_db.emplace( *chain, max_storage_size,
                                       fc::seconds(trx_retry_interval), fc::seconds(trx_retry_max_expire),
                                       abi_serializer_max_time_us );
         }
      }

      _last_tracked_votes.emplace(*chain);

      bool chain_api_plugin_configured = false;
      if (options.count("plugin")) {
         const auto& v = options.at("plugin").as<std::vector<std::string>>();
         chain_api_plugin_configured = std::ranges::any_of(v, [](const std::string& p) { return p.find("sysio::chain_api_plugin") != std::string::npos; });
      }

      // only enable _get_info_db if chain_api_plugin enabled.
      _get_info_db.emplace(*chain, chain_api_plugin_configured);

      // initialize deep mind logging
      if ( options.at( "deep-mind" ).as<bool>() ) {
         // The actual `fc::dmlog_sink` implementation that is currently used by deep mind
         // logger is using `stdout` to prints it's log line out. Deep mind logging outputs
         // massive amount of data out of the process, which can lead under pressure to some
         // of the system calls (i.e. `fwrite`) to fail abruptly without fully writing the
         // entire line.
         //
         // Recovering from errors on a buffered (line or full) and continuing retrying write
         // is merely impossible to do right, because the buffer is actually held by the
         // underlying `libc` implementation nor the operation system.
         //
         // To ensure good functionalities of deep mind tracer, the `stdout` is made unbuffered
         // and the actual `fc::dmlog_sink` deals with retry when facing error, enabling a much
         // more robust deep mind output.
         //
         // Changing the standard `stdout` behavior from buffered to unbuffered can is disruptive
         // and can lead to weird scenarios in the logging process if `stdout` is used there too.
         //
         // In a future version, the `fc::dmlog_sink` implementation will switch to a `FIFO` file
         // approach, which will remove the dependency on `stdout` and hence this call.
         //
         // For the time being, when `deep-mind = true` is activated, we set `stdout` here to
         // be an unbuffered I/O stream.
         setbuf(stdout, NULL);

         //verify configuration is correct
         SYS_ASSERT( options.at("api-accept-transactions").as<bool>() == false, plugin_config_exception,
            "api-accept-transactions must be set to false in order to enable deep-mind logging.");

         SYS_ASSERT( options.at("p2p-accept-transactions").as<bool>() == false, plugin_config_exception,
            "p2p-accept-transactions must be set to false in order to enable deep-mind logging.");

         _deep_mind_log.update_logger( deep_mind_logger_name );
         chain->enable_deep_mind( &_deep_mind_log );
      }

      get_block_by_id_provider = app().get_method<methods::get_block_by_id>().register_provider(
            [this]( block_id_type id ) -> signed_block_ptr {
               return chain->fetch_block_by_id( id );
            } );

      get_head_block_id_provider = app().get_method<methods::get_head_block_id>().register_provider( [this]() {
         return chain->head().id();
      } );

      // relay signals to channels
      accepted_block_header_connection = chain->accepted_block_header().connect(
            [this]( const block_signal_params& t ) {
               accepted_block_header_channel.publish( priority::medium, t );
            } );

      accepted_block_connection = chain->accepted_block().connect( [this]( const block_signal_params& t ) {
         const auto& [ block, id ] = t;
         if (_account_query_db) {
            _account_query_db->commit_block(block);
         }

         if (_trx_retry_db) {
            _trx_retry_db->on_accepted_block(block->block_num());
         }

         if (_trx_finality_status_processing) {
            _trx_finality_status_processing->signal_accepted_block(block, id);
         }

         if (_last_tracked_votes) {
            _last_tracked_votes->on_accepted_block(block, id);
         }

         if (_get_info_db) {
            _get_info_db->on_accepted_block();
         }

         accepted_block_channel.publish( priority::high, t );
      } );

      irreversible_block_connection = chain->irreversible_block().connect( [this]( const block_signal_params& t ) {
         const auto& [ block, id ] = t;

         if (_trx_retry_db) {
            _trx_retry_db->on_irreversible_block(block);
         }

         if (_trx_finality_status_processing) {
            _trx_finality_status_processing->signal_irreversible_block(block, id);
         }

         if (_get_info_db) {
            _get_info_db->on_irreversible_block(block, id);
         }

         irreversible_block_channel.publish( priority::low, t );
      } );
      
      applied_transaction_connection = chain->applied_transaction().connect(
            [this]( std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t ) {
               const auto& [ trace, ptrx ] = t;
               if (_account_query_db) {
                  _account_query_db->cache_transaction_trace(trace);
               }

               if (_trx_retry_db) {
                  _trx_retry_db->on_applied_transaction(trace, ptrx);
               }

               if (_trx_finality_status_processing) {
                  _trx_finality_status_processing->signal_applied_transaction(trace, ptrx);
               }

               applied_transaction_channel.publish( priority::low, trace );
            } );

      if (_trx_finality_status_processing || _trx_retry_db) {
         block_start_connection = chain->block_start().connect(
            [this]( uint32_t block_num ) {
               if (_trx_retry_db) {
                  _trx_retry_db->on_block_start(block_num);
               }
               if (_trx_finality_status_processing) {
                  _trx_finality_status_processing->signal_block_start( block_num );
               }
            } );
      }

      contract_action_matches matches;
      matches.push_back(contract_action_match("s"_n, "utl"_n, contract_action_match::match_type::suffix));
      matches[0].add_action("batchw"_n, contract_action_match::match_type::exact);
      matches[0].add_action("snoop"_n, contract_action_match::match_type::exact);
      matches.push_back(contract_action_match("op"_n, "sysio"_n, contract_action_match::match_type::suffix));
      matches[1].add_action("regproducer"_n, contract_action_match::match_type::exact);
      matches[1].add_action("unregprod"_n, contract_action_match::match_type::exact);

      chain->initialize_root_extensions(std::move(matches));

      chain->add_indices();

   } FC_LOG_AND_RETHROW()
}

void chain_plugin::plugin_initialize(const variables_map& options) {
   handle_sighup(); // Sets loggers
   my->plugin_initialize(options);
}

void chain_plugin_impl::plugin_startup()
{ try {
   try {
      auto shutdown = []() {
         dlog("controller shutdown, quitting...");
         return app().quit();
      };
      auto check_shutdown = [](){ return app().is_quiting(); };
      if (snapshot_path)
         chain->startup(shutdown, check_shutdown, std::make_shared<threaded_snapshot_reader>(*snapshot_path));
      else if( genesis )
         chain->startup(shutdown, check_shutdown, *genesis);
      else
         chain->startup(shutdown, check_shutdown);
   } catch (const database_guard_exception& e) {
      log_guard_exception(e);
      // make sure to properly close the db
      chain.reset();
      throw;
   }

   if(!readonly) {
      ilog("starting chain in read/write mode");
   }

   if (genesis) {
      ilog("Blockchain started; head block is #{}, genesis timestamp is {}",
           chain->head().block_num(), genesis->initial_timestamp);
   }
   else {
      ilog("Blockchain started; head block is #{}", chain->head().block_num());
   }

#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
   // Set up native contract dispatch via substitute_apply
   if( !native_contracts.empty() ) {
      const auto& db = chain->db();

      for( const auto& [acct, so_path] : native_contracts ) {
         const auto* meta = db.find<account_metadata_object, by_name>(acct);
         SYS_ASSERT( meta, plugin_config_exception,
                     "Native contract account '{}' not found on chain", acct );
         SYS_ASSERT( meta->code_hash != digest_type(), plugin_config_exception,
                     "Native contract account '{}' has no deployed code", acct );

         ilog("Native debug: {} (code_hash={}) -> {}", acct, meta->code_hash, so_path.string());
         native_overlay_.load(meta->code_hash, so_path);
      }

      chain->get_wasm_interface().substitute_apply =
         [this](const digest_type& code_hash, uint8_t vm_type,
                uint8_t vm_version, apply_context& context) -> bool {
            return native_overlay_(code_hash, vm_type, vm_version, context);
         };

      ilog("Native debug: {} contract(s) configured for native execution", native_overlay_.size());
   }
#endif

   chain_config.reset();

   if (account_queries_enabled) {
      account_queries_enabled = false;
      try {
         _account_query_db.emplace(*chain);
         account_queries_enabled = true;
      } FC_LOG_AND_DROP("Unable to enable account queries");
   }

} FC_CAPTURE_AND_RETHROW("") }

void chain_plugin::plugin_startup() {
   my->plugin_startup();
}

void chain_plugin::plugin_shutdown() {
   dlog("shutdown");
}

void chain_plugin::handle_sighup() {
   _deep_mind_log.update_logger( deep_mind_logger_name );
   fc::logger::update(vote_logger.get_name(), vote_logger);
}

chain_apis::read_write::read_write(controller& db,
                                   std::optional<trx_retry_db>& trx_retry,
                                   const fc::microseconds& abi_serializer_max_time,
                                   const fc::microseconds& http_max_response_time,
                                   bool api_accept_transactions)
: db(db)
, trx_retry(trx_retry)
, abi_serializer_max_time(abi_serializer_max_time)
, http_max_response_time(http_max_response_time)
, api_accept_transactions(api_accept_transactions)
{
}

void chain_apis::read_write::validate() const {
   SYS_ASSERT( api_accept_transactions, missing_chain_api_plugin_exception,
               "Not allowed, node has api-accept-transactions = false" );
}

chain_apis::read_write chain_plugin::get_read_write_api(const fc::microseconds& http_max_response_time) {
   return chain_apis::read_write(chain(), my->_trx_retry_db, get_abi_serializer_max_time(), http_max_response_time, api_accept_transactions());
}

chain_apis::read_only chain_plugin::get_read_only_api(const fc::microseconds& http_max_response_time) const {
   return chain_apis::read_only(chain(), my->_get_info_db, my->_account_query_db, my->_last_tracked_votes, get_abi_serializer_max_time(), http_max_response_time, my->_trx_finality_status_processing.get());
}

void chain_plugin::accept_transaction(const chain::packed_transaction_ptr& trx, next_function<chain::transaction_trace_ptr> next) {
   my->incoming_transaction_async_method(trx, false, transaction_metadata::trx_type::input, false, std::move(next));
}

controller& chain_plugin::chain() { return *my->chain; }
const controller& chain_plugin::chain() const { return *my->chain; }

chain::chain_id_type chain_plugin::get_chain_id()const {
   return my->chain->get_chain_id();
}

fc::microseconds chain_plugin::get_abi_serializer_max_time() const {
   return my->abi_serializer_max_time_us;
}

bool chain_plugin::api_accept_transactions() const{
   return my->api_accept_transactions;
}

bool chain_plugin::accept_transactions() const {
   return my->accept_transactions;
}

void chain_plugin_impl::enable_accept_transactions() {
   accept_transactions = true;
}

void chain_plugin::enable_accept_transactions() {
   my->enable_accept_transactions();
}

bool chain_plugin::accept_votes() const {
   return my->accept_votes;
}


void chain_plugin_impl::log_guard_exception(const chain::guard_exception&e ) {
   if (e.code() == chain::database_guard_exception::code_value) {
      elog("Database has reached an unsafe level of usage, shutting down to avoid corrupting the database.  "
           "Please increase the value set for \"chain-state-db-size-mb\" and restart the process!");
   }

   dlog("Details: {}", e.to_detail_string());
}

void chain_plugin_impl::handle_guard_exception(const chain::guard_exception& e) {
   log_guard_exception(e);

   elog("database chain::guard_exception, quitting..."); // log string searched for in: tests/nodeop_under_min_avail_ram.py
   // quit the app
   app().quit();
}

void chain_plugin::handle_guard_exception(const chain::guard_exception& e) {
   chain_plugin_impl::handle_guard_exception(e);
}

void chain_apis::api_base::handle_db_exhaustion() {
   elog("database memory exhausted: increase chain-state-db-size-mb");
   //return 1 -- it's what programs/nodeop/main.cpp considers "BAD_ALLOC"
   std::_Exit(1);
}

void chain_apis::api_base::handle_bad_alloc() {
   elog("std::bad_alloc - memory exhausted");
   //return -2 -- it's what programs/nodeop/main.cpp reports for std::exception
   std::_Exit(-2);
}

bool chain_plugin::account_queries_enabled() const {
   return my->account_queries_enabled;
}

bool chain_plugin::transaction_finality_status_enabled() const {
   return my->_trx_finality_status_processing.get();
}

namespace chain_apis {

const string read_only::KEYi64 = "i64";

get_info_db::get_info_results read_only::get_info(const read_only::get_info_params&, const fc::time_point&) const {
   SYS_ASSERT(gidb, plugin_config_exception, "get_info being accessed when not enabled");

   // To be able to run get_info on an http thread, get_info results are stored
   // in get_info_db and updated whenever accepted_block signal is received.
   return gidb->get_info();
}

read_only::get_transaction_status_results
read_only::get_transaction_status(const read_only::get_transaction_status_params& param, const fc::time_point&) const {
   SYS_ASSERT(trx_finality_status_proc, unsupported_feature, "Transaction Status Interface not enabled.  To enable, configure nodeop with '--transaction-finality-status-max-storage-size-gb <size>'.");

   trx_finality_status_processing::chain_state ch_state = trx_finality_status_proc->get_chain_state();

   const auto trx_st = trx_finality_status_proc->get_trx_state(param.id);
   // check if block_id is set to a valid value, since trx_finality_status_proc does not use optionals for the block data
   const auto trx_block_valid = trx_st && trx_st->block_id != chain::block_id_type{};

   return {
      trx_st ? trx_st->status : "UNKNOWN",
      trx_block_valid ? std::optional<uint32_t>(chain::block_header::num_from_id(trx_st->block_id)) : std::optional<uint32_t>{},
      trx_block_valid ? std::optional<chain::block_id_type>(trx_st->block_id) : std::optional<chain::block_id_type>{},
      trx_block_valid ? std::optional<fc::time_point>(trx_st->block_timestamp) : std::optional<fc::time_point>{},
      trx_st ? std::optional<fc::time_point>(trx_st->expiration) : std::optional<fc::time_point>{},
      chain::block_header::num_from_id(ch_state.head_id),
      ch_state.head_id,
      ch_state.head_block_timestamp,
      chain::block_header::num_from_id(ch_state.irr_id),
      ch_state.irr_id,
      ch_state.irr_block_timestamp,
      ch_state.earliest_tracked_block_id,
      chain::block_header::num_from_id(ch_state.earliest_tracked_block_id)
   };
}

read_only::get_activated_protocol_features_results
read_only::get_activated_protocol_features( const read_only::get_activated_protocol_features_params& params,
                                            const fc::time_point& deadline )const {
   read_only::get_activated_protocol_features_results result;
   const auto& pfm = db.get_protocol_feature_manager();

   uint32_t lower_bound_value = std::numeric_limits<uint32_t>::lowest();
   uint32_t upper_bound_value = std::numeric_limits<uint32_t>::max();

   if( params.lower_bound ) {
      lower_bound_value = *params.lower_bound;
   }

   if( params.upper_bound ) {
      upper_bound_value = *params.upper_bound;
   }

   if( upper_bound_value < lower_bound_value )
      return result;

   auto walk_range = [&]( auto itr, auto end_itr, auto&& convert_iterator ) {
      fc::mutable_variant_object mvo;
      mvo( "activation_ordinal", 0 );
      mvo( "activation_block_num", 0 );

      auto& activation_ordinal_value   = mvo["activation_ordinal"];
      auto& activation_block_num_value = mvo["activation_block_num"];

      // activated protocol features are naturally limited and unlikely to ever reach max_return_items
      for( ; itr != end_itr; ++itr ) {
         const auto& conv_itr = convert_iterator( itr );
         activation_ordinal_value   = conv_itr.activation_ordinal();
         activation_block_num_value = conv_itr.activation_block_num();

         result.activated_protocol_features.emplace_back( conv_itr->to_variant( false, &mvo ) );
      }
   };

   auto get_next_if_not_end = [&pfm]( auto&& itr ) {
      if( itr == pfm.cend() ) return itr;

      ++itr;
      return itr;
   };

   auto lower = ( params.search_by_block_num ? pfm.lower_bound( lower_bound_value )
                                             : pfm.at_activation_ordinal( lower_bound_value ) );

   auto upper = ( params.search_by_block_num ? pfm.upper_bound( upper_bound_value )
                                             : get_next_if_not_end( pfm.at_activation_ordinal( upper_bound_value ) ) );

   if( params.reverse ) {
      walk_range( std::make_reverse_iterator(upper), std::make_reverse_iterator(lower),
                  []( auto&& ritr ) { return --(ritr.base()); } );
   } else {
      walk_range( lower, upper, []( auto&& itr ) { return itr; } );
   }

   return result;
}

uint64_t read_only::get_table_index_name(const read_only::get_table_rows_params& p, bool& primary) {
   // see multi_index packing of index name
   const uint64_t table = p.table.to_uint64_t();
   uint64_t index = table & 0xFFFFFFFFFFFFFFF0ULL;
   SYS_ASSERT( index == table, chain::contract_table_query_exception, "Unsupported table name: {}", p.table );

   primary = false;
   uint64_t pos = 0;
   if (p.index_position.empty() || p.index_position == "first" || p.index_position == "primary" || p.index_position == "one") {
      primary = true;
   } else if (p.index_position.starts_with("sec") || p.index_position == "two") { // second, secondary
   } else if (p.index_position .starts_with("ter") || p.index_position.starts_with("th")) { // tertiary, ternary, third, three
      pos = 1;
   } else if (p.index_position.starts_with("fou")) { // four, fourth
      pos = 2;
   } else if (p.index_position.starts_with("fi")) { // five, fifth
      pos = 3;
   } else if (p.index_position.starts_with("six")) { // six, sixth
      pos = 4;
   } else if (p.index_position.starts_with("sev")) { // seven, seventh
      pos = 5;
   } else if (p.index_position.starts_with("eig")) { // eight, eighth
      pos = 6;
   } else if (p.index_position.starts_with("nin")) { // nine, ninth
      pos = 7;
   } else if (p.index_position.starts_with("ten")) { // ten, tenth
      pos = 8;
   } else {
      try {
         pos = fc::to_uint64( p.index_position );
      } catch(...) {
         SYS_ASSERT( false, chain::contract_table_query_exception, "Invalid index_position: {}", p.index_position);
      }
      if (pos < 2) {
         primary = true;
         pos = 0;
      } else {
         pos -= 2;
      }
   }
   index |= (pos & 0x000000000000000FULL);
   return index;
}

uint64_t convert_to_type(const sysio::name &n, const string &desc) {
   return n.to_uint64_t();
}

template<>
uint64_t convert_to_type(const string& str, const string& desc) {

   try {
      return boost::lexical_cast<uint64_t>(str.c_str(), str.size());
   } catch( ... ) { }

   try {
      auto trimmed_str = str;
      boost::trim(trimmed_str);
      name s(trimmed_str);
      return s.to_uint64_t();
   } catch( ... ) { }

   if (str.find(',') != string::npos) { // fix #6274 only match formats like 4,SYS
      try {
         auto symb = sysio::chain::symbol::from_string(str);
         return symb.value();
      } catch( ... ) { }
   }

   try {
      return ( sysio::chain::string_to_symbol( 0, str.c_str() ) >> 8 );
   } catch( ... ) {
      SYS_ASSERT( false, chain_type_exception, "Could not convert {} string '{}' to any of the following: "
                        "uint64_t, valid name, or valid symbol (with or without the precision)",
                  desc, str);
   }
}

template<>
double convert_to_type(const string& str, const string& desc) {
   double val{};
   try {
      val = fc::variant(str).as<double>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert {} string '{}' to key type.", desc, str )

   SYS_ASSERT( !std::isnan(val), chain::contract_table_query_exception,
               "Converted {} string '{}' to NaN which is not a permitted value for the key type", desc, str );

   return val;
}

template<typename Type>
string convert_to_string(const Type& source, const string& key_type, const string& encode_type, const string& desc) {
   try {
      return fc::variant(source).as<string>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert {} from '{}' to string.", desc, fc::json::to_log_string(source) )
}

template<>
string convert_to_string(const float128_t& source, const string& key_type, const string& encode_type, const string& desc) {
   try {
      float64_t f = f128_to_f64(source);
      return fc::variant(f).as<string>();
   } FC_RETHROW_EXCEPTIONS(warn, "Could not convert {} from '{}' to string.", desc, fc::json::to_log_string(source) )
}

abi_def get_abi( const controller& db, const name& account ) {
   const auto* accnt = db.find_account(account);
   SYS_ASSERT(accnt != nullptr, chain::account_query_exception, "Fail to retrieve account for {}", account );
   const account_metadata_object* code_accnt = db.find_account_metadata(account);
   abi_def abi;
   if (code_accnt)
      abi_serializer::to_abi(code_accnt->abi, abi);
   return abi;
}

string get_table_type( const abi_def& abi, const name& table_name ) {
   for( const auto& t : abi.tables ) {
      if( t.name == table_name ){
         return t.index_type;
      }
   }
   SYS_ASSERT( false, chain::contract_table_query_exception, "Table {} is not specified in the ABI", table_name );
}

const chain::table_def& get_kv_table_def( const abi_def& abi, const name& table_name ) {
   for( const auto& t : abi.tables ) {
      if( t.name == table_name )
         return t;
   }
   SYS_ASSERT( false, chain::contract_table_query_exception, "Table {} is not specified in the ABI", table_name );
}

read_only::get_table_rows_return_t
read_only::get_table_rows( const read_only::get_table_rows_params& p, const fc::time_point& deadline ) const {
   abi_def abi = sysio::chain_apis::get_abi( db, p.code );
   bool primary = false;
   auto table_with_index = get_table_index_name( p, primary );
   if( primary ) {
      SYS_ASSERT( p.table == table_with_index, chain::contract_table_query_exception, "Invalid table name {}", p.table );
      auto table_type = get_table_type( abi, p.table );
      if( table_type == KEYi64 || p.key_type == "i64" || p.key_type == "name" ) {
         return get_table_rows_ex(p, std::move(abi), deadline);
      }
      SYS_ASSERT( false, chain::contract_table_query_exception,  "Invalid table type {}", table_type );
   } else {
      // Secondary index query: extract index_id from index_position
      // get_table_index_name encodes pos in low nibble; position 2 (second) -> pos 0, etc.
      uint64_t pos = table_with_index & 0x000000000000000FULL;
      return get_table_rows_by_seckey(p, std::move(abi), pos, deadline);
   }
}

// ---------------------------------------------------------------------------
// Secondary key encoding/decoding for get_table_rows secondary index queries.
// Matches the CDT's _kv_multi_index_detail::encode_secondary specializations.
// ---------------------------------------------------------------------------

namespace {


// Encode a user-provided bound string into secondary key bytes.
std::vector<char> encode_sec_bound(const string& key_type, const string& bound_str, const string& encode_type) {
   std::vector<char> buf;
   if (bound_str.empty()) return buf;

   if (key_type == "i64" || key_type == "idx64" || key_type == "name") {
      uint64_t val;
      if (key_type == "name") {
         val = name(bound_str).to_uint64_t();
      } else {
         val = convert_to_type<uint64_t>(bound_str, "bound");
      }
      buf.resize(8);
      chain::kv_encode_be64(buf.data(), val);
   } else if (key_type == "i128" || key_type == "idx128") {
      // Parse as hex string (with optional 0x prefix)
      auto s = bound_str;
      if (s.starts_with("0x") || s.starts_with("0X")) s = s.substr(2);
      // Pad to 32 hex chars (16 bytes)
      while (s.size() < 32) s = "0" + s;
      SYS_ASSERT(s.size() == 32, chain::contract_table_query_exception,
                 "i128 bound must be 32 hex chars, got {}", s.size());
      buf.resize(16);
      fc::from_hex(s, buf.data(), 16);
   } else if (key_type == "sha256" || key_type == "i256" || key_type == "idx256") {
      // 32-byte hex, natural byte order (no word swap)
      auto s = bound_str;
      if (s.starts_with("0x") || s.starts_with("0X")) s = s.substr(2);
      SYS_ASSERT(s.size() == 64, chain::contract_table_query_exception,
                 "sha256/i256 bound must be 64 hex chars, got {}", s.size());
      buf.resize(32);
      fc::from_hex(s, buf.data(), 32);
   } else if (key_type == "float64") {
      double d = convert_to_type<double>(bound_str, "bound");
      uint64_t bits;
      memcpy(&bits, &d, 8);
      if (bits >> 63) bits = ~bits;
      else            bits ^= (uint64_t(1) << 63);
      buf.resize(8);
      chain::kv_encode_be64(buf.data(), bits);
   } else if (key_type == "float128") {
      // Parse as 0x-prefixed hex string (16 bytes LE), convert to BE, sign-magnitude
      auto s = bound_str;
      if (s.starts_with("0x") || s.starts_with("0X")) s = s.substr(2);
      SYS_ASSERT(s.size() == 32, chain::contract_table_query_exception,
                 "float128 bound must be 32 hex chars (16 bytes LE), got {}", s.size());
      char raw[16];
      fc::from_hex(s, raw, 16);
      buf.resize(16);
      // LE to BE
      for (int i = 0; i < 16; ++i) buf[i] = raw[15 - i];
      // Sign-magnitude transform
      if (static_cast<uint8_t>(buf[0]) & 0x80u)
         for (int i = 0; i < 16; ++i) buf[i] = ~buf[i];
      else
         buf[0] = static_cast<char>(static_cast<uint8_t>(buf[0]) ^ 0x80u);
   } else {
      SYS_ASSERT(false, chain::contract_table_query_exception, "Unsupported secondary key_type: {}", key_type);
   }
   return buf;
}

// Decode secondary key bytes back to a string for next_key response.
string decode_sec_key(const string& key_type, const char* data, size_t size, const string& encode_type) {
   if (key_type == "i64" || key_type == "idx64") {
      SYS_ASSERT(size == 8, chain::contract_table_query_exception, "Expected 8-byte secondary key for i64");
      return fc::variant(chain::kv_decode_be64(data)).as<string>();
   } else if (key_type == "name") {
      SYS_ASSERT(size == 8, chain::contract_table_query_exception, "Expected 8-byte secondary key for name");
      return name(chain::kv_decode_be64(data)).to_string();
   } else if (key_type == "i128" || key_type == "idx128") {
      SYS_ASSERT(size == 16, chain::contract_table_query_exception, "Expected 16-byte secondary key for i128");
      return "0x" + fc::to_hex(data, 16);
   } else if (key_type == "sha256" || key_type == "i256" || key_type == "idx256") {
      SYS_ASSERT(size == 32, chain::contract_table_query_exception, "Expected 32-byte secondary key for sha256");
      return fc::to_hex(data, 32);
   } else if (key_type == "float64") {
      SYS_ASSERT(size == 8, chain::contract_table_query_exception, "Expected 8-byte secondary key for float64");
      uint64_t bits = chain::kv_decode_be64(data);
      if (bits >> 63) bits ^= (uint64_t(1) << 63);
      else            bits = ~bits;
      double d;
      memcpy(&d, &bits, 8);
      return fc::variant(d).as<string>();
   } else if (key_type == "float128") {
      SYS_ASSERT(size == 16, chain::contract_table_query_exception, "Expected 16-byte secondary key for float128");
      // Reverse sign-magnitude transform
      char be[16];
      memcpy(be, data, 16);
      if (static_cast<uint8_t>(be[0]) & 0x80u)
         be[0] = static_cast<char>(static_cast<uint8_t>(be[0]) ^ 0x80u);
      else
         for (int i = 0; i < 16; ++i) be[i] = ~be[i];
      // BE to LE
      char le[16];
      for (int i = 0; i < 16; ++i) le[i] = be[15 - i];
      return "0x" + fc::to_hex(le, 16);
   }
   return fc::to_hex(data, size);
}

} // anonymous namespace

read_only::get_table_rows_return_t
read_only::get_table_rows_by_seckey( const read_only::get_table_rows_params& p,
                                     abi_def&& abi,
                                     uint64_t index_position,
                                     const fc::time_point& deadline ) const {
   fc::time_point params_deadline = p.time_limit_ms
      ? std::min(fc::time_point::now().safe_add(fc::milliseconds(*p.time_limit_ms)), deadline)
      : deadline;

   struct http_params_t {
      name table;
      bool shorten_abi_errors;
      bool json;
      bool show_payer;
      bool more = false;
      std::string next_key;
      vector<std::pair<vector<char>, name>> rows;
   };

   http_params_t hp { p.table, shorten_abi_errors, p.json, p.show_payer && *p.show_payer };

   const auto& d = db.db();
   const uint8_t index_id = static_cast<uint8_t>(index_position);
   const uint64_t scope = convert_to_type<uint64_t>(p.scope, "scope");

   // Encode bounds
   auto lb_bytes = encode_sec_bound(p.key_type, p.lower_bound, p.encode_type);
   auto ub_bytes = encode_sec_bound(p.key_type, p.upper_bound, p.encode_type);
   auto lb_sv = std::string_view(lb_bytes.data(), lb_bytes.size());
   auto ub_sv = std::string_view(ub_bytes.data(), ub_bytes.size());
   bool has_lower = !lb_bytes.empty();
   bool has_upper = !ub_bytes.empty();

   // CDT's kv_multi_index encodes secondary pri_key as [scope:8B BE][pk:8B BE]
   char scope_be[chain::kv_table_prefix_size];
   chain::kv_encode_be64(scope_be, scope);

   const auto& sec_idx = d.get_index<chain::kv_index_index, chain::by_code_table_idx_seckey>();

   uint32_t limit = p.limit;
   if (deadline != fc::time_point::maximum() && limit > max_return_items)
      limit = max_return_items;

   // Primary row lookup: fetch value from kv_object given (table, scope, pk)
   const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();
   auto fetch_value = [&](uint64_t pk) -> std::pair<vector<char>, name> {
      auto key = chain::make_kv_key(p.table.to_uint64_t(), scope, pk);
      auto kv_sv = key.to_string_view();
      auto itr = kv_idx.find(boost::make_tuple(p.code, chain::config::kv_format_standard, kv_sv));
      if (itr != kv_idx.end()) {
         vector<char> data(itr->value.size());
         if (itr->value.size() > 0)
            memcpy(data.data(), itr->value.data(), itr->value.size());
         return {std::move(data), itr->payer};
      }
      return {{}, name{}};
   };

   auto collect_next = [&](const chain::kv_index_object& obj) {
      hp.more = true;
      hp.next_key = decode_sec_key(p.key_type, obj.sec_key_data(), obj.sec_key_size, p.encode_type);
   };

   bool reverse = p.reverse && *p.reverse;

   if (!reverse) {
      // Forward iteration
      auto itr = has_lower
         ? sec_idx.lower_bound(boost::make_tuple(p.code, p.table, index_id, lb_sv))
         : sec_idx.lower_bound(boost::make_tuple(p.code, p.table, index_id));
      uint32_t count = 0;
      for (; itr != sec_idx.end(); ++itr) {
         if (itr->code != p.code || itr->table != p.table || itr->index_id != index_id)
            break;
         // Upper bound check on secondary key
         if (has_upper && itr->sec_key_view() > ub_sv)
            break;
         // Scope filter: first 8 bytes of pri_key must match scope
         if (itr->pri_key_size < chain::kv_prefix_size) continue;
         if (memcmp(itr->pri_key_data(), scope_be, chain::kv_table_prefix_size) != 0)
            continue;
         if (count >= limit) { collect_next(*itr); break; }
         // Extract primary key (bytes 8-15 of pri_key)
         uint64_t pk = chain::kv_decode_be64(itr->pri_key_data() + chain::kv_table_prefix_size);
         auto [data, payer] = fetch_value(pk);
         if (!data.empty()) {
            hp.rows.emplace_back(std::move(data), payer);
            ++count;
         }
         if (fc::time_point::now() >= params_deadline) {
            ++itr;
            if (itr != sec_idx.end() && itr->code == p.code && itr->table == p.table && itr->index_id == index_id)
               collect_next(*itr);
            break;
         }
      }
   } else {
      // Reverse iteration
      auto end_itr = has_upper
         ? sec_idx.upper_bound(boost::make_tuple(p.code, p.table, index_id, ub_sv))
         : sec_idx.upper_bound(boost::make_tuple(p.code, p.table, index_id));
      auto begin_itr = has_lower
         ? sec_idx.lower_bound(boost::make_tuple(p.code, p.table, index_id, lb_sv))
         : sec_idx.lower_bound(boost::make_tuple(p.code, p.table, index_id));
      auto ritr = boost::make_reverse_iterator(end_itr);
      auto rend = boost::make_reverse_iterator(begin_itr);
      uint32_t count = 0;
      for (; ritr != rend; ++ritr) {
         if (ritr->code != p.code || ritr->table != p.table || ritr->index_id != index_id)
            break;
         // Scope filter
         if (ritr->pri_key_size < chain::kv_prefix_size) continue;
         if (memcmp(ritr->pri_key_data(), scope_be, chain::kv_table_prefix_size) != 0)
            continue;
         if (count >= limit) { collect_next(*ritr); break; }
         uint64_t pk = chain::kv_decode_be64(ritr->pri_key_data() + chain::kv_table_prefix_size);
         auto [data, payer] = fetch_value(pk);
         if (!data.empty()) {
            hp.rows.emplace_back(std::move(data), payer);
            ++count;
         }
         if (fc::time_point::now() >= params_deadline) {
            ++ritr;
            if (ritr != rend && ritr->code == p.code && ritr->table == p.table && ritr->index_id == index_id)
               collect_next(*ritr);
            break;
         }
      }
   }

   // Phase 2: ABI decode on http thread pool (same pattern as get_table_rows_ex)
   return [p = std::move(hp), abi = std::move(abi), table_name = p.table,
           abi_serializer_max_time = abi_serializer_max_time]() mutable ->
      chain::t_or_exception<read_only::get_table_rows_result> {
      read_only::get_table_rows_result result;
      abi_serializer abis;
      abis.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
      auto table_type = abis.get_table_type(table_name);

      for (auto& row : p.rows) {
         fc::variant data_var;
         if (p.json) {
            data_var = abis.binary_to_variant(table_type, row.first,
                                              abi_serializer::create_yield_function(abi_serializer_max_time),
                                              p.shorten_abi_errors);
         } else {
            data_var = fc::variant(row.first);
         }
         if (p.show_payer) {
            result.rows.emplace_back(fc::mutable_variant_object("data", std::move(data_var))("payer", row.second));
         } else {
            result.rows.emplace_back(std::move(data_var));
         }
      }
      result.more = p.more;
      result.next_key = p.next_key;
      return result;
   };
}

read_only::get_kv_rows_return_t
read_only::get_kv_rows( const read_only::get_kv_rows_params& p, const fc::time_point& deadline ) const {
   abi_def abi = sysio::chain_apis::get_abi( db, p.code );
   const auto& tbl = get_kv_table_def( abi, p.table );

   // Capture key metadata for use in both phases.
   auto key_names = tbl.key_names;
   auto key_types = tbl.key_types;

   fc::time_point params_deadline = p.time_limit_ms
      ? std::min(fc::time_point::now().safe_add(fc::milliseconds(*p.time_limit_ms)), deadline)
      : deadline;

   // Phase 1: Collect raw rows on the main thread.
   // Phase 2 (the returned lambda): ABI-decode on the http thread pool.
   struct raw_row {
      std::vector<char> key;
      std::vector<char> value;
   };
   struct http_params_t {
      bool json;
      bool more;
      std::string next_key;
      std::vector<raw_row> rows;
   };

   http_params_t hp{ p.json, false, {}, {} };

   const auto& d = db.db();
   const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();

   // Parse bounds: when json=true, bounds are JSON key objects; when json=false, hex strings.
   std::vector<char> lb_bytes;
   if (!p.lower_bound.empty()) {
      if (p.json) {
         auto lb_var = fc::json::from_string(p.lower_bound);
         lb_bytes = chain::be_key_codec::encode_key(lb_var, key_names, key_types);
      } else {
         auto v = fc::from_hex(p.lower_bound);
         lb_bytes.assign(reinterpret_cast<const char*>(v.data()),
                         reinterpret_cast<const char*>(v.data()) + v.size());
      }
   }
   std::vector<char> ub_bytes;
   bool has_upper = !p.upper_bound.empty();
   if (has_upper) {
      if (p.json) {
         auto ub_var = fc::json::from_string(p.upper_bound);
         ub_bytes = chain::be_key_codec::encode_key(ub_var, key_names, key_types);
      } else {
         auto v = fc::from_hex(p.upper_bound);
         ub_bytes.assign(reinterpret_cast<const char*>(v.data()),
                         reinterpret_cast<const char*>(v.data()) + v.size());
      }
   }

   std::string_view lb_sv(lb_bytes.data(), lb_bytes.size());
   std::string_view ub_sv(ub_bytes.data(), ub_bytes.size());

   uint32_t limit = p.limit;
   bool reverse = p.reverse.has_value() && *p.reverse;

   auto collect_next_key = [&](const chain::kv_object& obj) {
      auto kv = obj.key_view();
      if (p.json) {
         try {
            hp.next_key = fc::json::to_string(
               chain::be_key_codec::decode_key(kv.data(), kv.size(), key_names, key_types),
               fc::time_point::maximum());
         } catch (...) {
            hp.next_key = fc::to_hex(kv.data(), static_cast<uint32_t>(kv.size()));
         }
      } else {
         hp.next_key = fc::to_hex(kv.data(), static_cast<uint32_t>(kv.size()));
      }
   };

   if (!reverse) {
      auto itr = kv_idx.lower_bound(boost::make_tuple(p.code, chain::config::kv_format_raw, lb_sv));
      uint32_t count = 0;
      while (itr != kv_idx.end() && itr->code == p.code &&
             itr->key_format == chain::config::kv_format_raw) {
         auto kv = itr->key_view();
         if (has_upper && kv >= ub_sv) break;

         if (count >= limit) {
            hp.more = true;
            collect_next_key(*itr);
            break;
         }

         raw_row row;
         row.key.assign(kv.data(), kv.data() + kv.size());
         row.value.assign(itr->value.data(), itr->value.data() + itr->value.size());
         hp.rows.emplace_back(std::move(row));

         ++count;
         ++itr;
         if (fc::time_point::now() >= params_deadline) {
            if (itr != kv_idx.end() && itr->code == p.code &&
                itr->key_format == chain::config::kv_format_raw) {
               auto next_kv = itr->key_view();
               if (!has_upper || next_kv < ub_sv) {
                  hp.more = true;
                  collect_next_key(*itr);
               }
            }
            break;
         }
      }
   } else {
      // Reverse iteration
      decltype(kv_idx.end()) itr;
      if (has_upper) {
         itr = kv_idx.lower_bound(boost::make_tuple(p.code, chain::config::kv_format_raw, ub_sv));
      } else {
         itr = kv_idx.lower_bound(
            boost::make_tuple(p.code, static_cast<uint8_t>(chain::config::kv_format_raw + 1), std::string_view()));
      }

      auto begin = kv_idx.lower_bound(
         boost::make_tuple(p.code, chain::config::kv_format_raw, std::string_view()));

      if (itr != begin) {
         uint32_t count = 0;
         do {
            --itr;
            if (itr->code != p.code || itr->key_format != chain::config::kv_format_raw)
               break;

            auto kv = itr->key_view();
            if (!lb_bytes.empty() && kv < lb_sv)
               break;

            if (count >= limit) {
               hp.more = true;
               collect_next_key(*itr);
               break;
            }

            raw_row row;
            row.key.assign(kv.data(), kv.data() + kv.size());
            row.value.assign(itr->value.data(), itr->value.data() + itr->value.size());
            hp.rows.emplace_back(std::move(row));

            ++count;
            if (itr == begin) {
               // No more entries before this one
               break;
            }

            if (fc::time_point::now() >= params_deadline) {
               if (itr != begin) {
                  auto prev = itr;
                  --prev;
                  if (prev->code == p.code && prev->key_format == chain::config::kv_format_raw) {
                     auto prev_kv = prev->key_view();
                     if (lb_bytes.empty() || prev_kv >= lb_sv) {
                        hp.more = true;
                        collect_next_key(*prev);
                     }
                  }
               }
               break;
            }
         } while (true);
      }
   }

   return [hp = std::move(hp), abi = std::move(abi), tbl_name = p.table,
           key_names = std::move(key_names), key_types = std::move(key_types),
           abi_serializer_max_time = abi_serializer_max_time,
           shorten_abi_errors = shorten_abi_errors]() mutable
      -> chain::t_or_exception<read_only::get_kv_rows_result> {
      read_only::get_kv_rows_result result;

      abi_serializer abis;
      abis.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
      auto value_type = abis.get_table_type(tbl_name);

      for (auto& row : hp.rows) {
         fc::mutable_variant_object obj;

         // Decode key -- fall back to hex if BE decode fails
         if (hp.json) {
            try {
               obj("key", chain::be_key_codec::decode_key(
                  row.key.data(), row.key.size(), key_names, key_types));
            } catch (...) {
               obj("key", fc::to_hex(row.key.data(), static_cast<uint32_t>(row.key.size())));
            }
         } else {
            obj("key", fc::to_hex(row.key.data(), static_cast<uint32_t>(row.key.size())));
         }

         // Decode value -- fall back to hex if ABI decode fails
         if (hp.json) {
            try {
               obj("value", abis.binary_to_variant(value_type, row.value,
                                                    abi_serializer::create_yield_function(abi_serializer_max_time),
                                                    shorten_abi_errors));
            } catch (...) {
               obj("value", fc::variant(row.value));
            }
         } else {
            obj("value", fc::variant(row.value));
         }

         result.rows.emplace_back(std::move(obj));
      }

      result.more = hp.more;
      result.next_key = hp.next_key;
      return result;
   };
}

read_only::get_table_by_scope_result read_only::get_table_by_scope( const read_only::get_table_by_scope_params& p,
                                                                    const fc::time_point& deadline )const {

   fc::time_point params_deadline = p.time_limit_ms ? std::min(fc::time_point::now().safe_add(fc::milliseconds(*p.time_limit_ms)), deadline) : deadline;

   read_only::get_table_by_scope_result result;
   const auto& d = db.db();

   uint64_t lower_table = 0;
   uint64_t lower_scope = 0;
   uint64_t upper_scope = std::numeric_limits<uint64_t>::max();

   // Parse lower_bound: supports "table:scope" (pagination token) or plain "scope"
   if (p.lower_bound.size()) {
      auto colon = p.lower_bound.find(':');
      if (colon != std::string::npos) {
         lower_table = convert_to_type<uint64_t>(p.lower_bound.substr(0, colon), "lower_bound table");
         lower_scope = convert_to_type<uint64_t>(p.lower_bound.substr(colon + 1), "lower_bound scope");
      } else {
         lower_scope = convert_to_type<uint64_t>(p.lower_bound, "lower_bound scope");
      }
   }

   uint64_t upper_table = std::numeric_limits<uint64_t>::max();
   if (p.upper_bound.size()) {
      auto colon = p.upper_bound.find(':');
      if (colon != std::string::npos) {
         upper_table = convert_to_type<uint64_t>(p.upper_bound.substr(0, colon), "upper_bound table");
         upper_scope = convert_to_type<uint64_t>(p.upper_bound.substr(colon + 1), "upper_bound scope");
      } else {
         upper_scope = convert_to_type<uint64_t>(p.upper_bound, "upper_bound scope");
      }
   }

   if (upper_scope < lower_scope && lower_table == 0)
      return result;

   uint32_t limit = p.limit;
   if (deadline != fc::time_point::maximum() && limit > max_return_items)
      limit = max_return_items;

   const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();

   // Build a seek key: [table:8B BE][scope:8B BE][pk:8B zeros]
   auto make_seek_key = [](uint64_t tbl, uint64_t scope) -> chain::kv_key_t {
      return chain::make_kv_key(tbl, scope, 0);
   };
   // Build a max seek key for reverse: [table:8B BE][scope:8B BE][pk:8B 0xFF]
   auto make_seek_key_max = [](uint64_t tbl, uint64_t scope) -> chain::kv_key_t {
      return chain::make_kv_key(tbl, scope, std::numeric_limits<uint64_t>::max());
   };

   // Check if an iterator points to a valid format=1 24-byte key for this code
   auto is_valid = [&](const auto& it) -> bool {
      return it != kv_idx.end() && it->code == p.code &&
             it->key_format == config::kv_format_standard &&
             it->key_size == chain::kv_key_size;
   };

   bool reverse = p.reverse && *p.reverse;

   if (!reverse) {
      // Forward seek-skip iteration.
      // Seek to the starting position considering table filter and bounds.
      chain::kv_key_t seek;
      if (p.table) {
         uint64_t start_scope = (lower_table == p.table.to_uint64_t()) ? lower_scope : 0;
         if (lower_table > p.table.to_uint64_t()) return result;
         start_scope = std::max(start_scope, lower_scope);
         seek = make_seek_key(p.table.to_uint64_t(), start_scope);
      } else {
         seek = make_seek_key(lower_table, lower_scope);
      }

      auto itr = kv_idx.lower_bound(
         boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));

      uint32_t count = 0;
      while (is_valid(itr)) {
         auto kv = itr->key_view();
         uint64_t tbl_raw   = chain::kv_decode_be64(kv.data());
         uint64_t scope_raw = chain::kv_decode_be64(kv.data() + 8);

         // Table filter: skip to target table or stop if past it
         if (p.table) {
            if (tbl_raw < p.table.to_uint64_t()) {
               // Seek forward to the target table
               seek = make_seek_key(p.table.to_uint64_t(), lower_scope);
               itr = kv_idx.lower_bound(
                  boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));
               continue;
            }
            if (tbl_raw > p.table.to_uint64_t()) break;
         }

         // Scope bounds
         if (scope_raw < lower_scope) {
            // Seek forward within this table to lower_scope
            seek = make_seek_key(tbl_raw, lower_scope);
            itr = kv_idx.lower_bound(
               boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));
            continue;
         }
         if (scope_raw > upper_scope) {
            // Skip to next table
            if (p.table) break; // single table, done
            if (tbl_raw == std::numeric_limits<uint64_t>::max()) break;
            seek = make_seek_key(tbl_raw + 1, lower_scope);
            itr = kv_idx.lower_bound(
               boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));
            continue;
         }

         // Emit this (table, scope) pair
         if (count >= limit) {
            result.more = name(tbl_raw).to_string() + ":" + name(scope_raw).to_string();
            break;
         }
         result.rows.push_back({p.code, name(scope_raw), name(tbl_raw)});
         ++count;

         // Seek-skip to next (table, scope): advance scope by 1, overflow to next table
         if (scope_raw < std::numeric_limits<uint64_t>::max()) {
            seek = make_seek_key(tbl_raw, scope_raw + 1);
         } else if (tbl_raw < std::numeric_limits<uint64_t>::max()) {
            seek = make_seek_key(tbl_raw + 1, p.table ? 0 : lower_scope);
         } else {
            break; // both maxed out
         }
         itr = kv_idx.lower_bound(
            boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));

         if (fc::time_point::now() >= params_deadline) {
            if (is_valid(itr)) {
               auto next_kv = itr->key_view();
               uint64_t nt = chain::kv_decode_be64(next_kv.data());
               uint64_t ns = chain::kv_decode_be64(next_kv.data() + 8);
               result.more = name(nt).to_string() + ":" + name(ns).to_string();
            }
            break;
         }
      }
   } else {
      // Reverse seek-skip iteration.
      // Start from the upper bound and work backward.
      chain::kv_key_t seek;
      if (p.table) {
         seek = make_seek_key_max(p.table.to_uint64_t(), upper_scope);
      } else {
         seek = make_seek_key_max(upper_table, upper_scope);
      }

      auto itr = kv_idx.upper_bound(
         boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));

      uint32_t count = 0;
      while (true) {
         // Move backward
         if (itr == kv_idx.begin()) break;
         --itr;
         if (!is_valid(itr)) break;

         auto kv = itr->key_view();
         uint64_t tbl_raw   = chain::kv_decode_be64(kv.data());
         uint64_t scope_raw = chain::kv_decode_be64(kv.data() + 8);

         // Table filter
         if (p.table) {
            if (tbl_raw > p.table.to_uint64_t()) continue; // upper_bound put us past, keep decrementing
            if (tbl_raw < p.table.to_uint64_t()) break;
         }

         // Scope bounds
         if (scope_raw > upper_scope) {
            // Seek backward to upper_scope within this table
            seek = make_seek_key_max(tbl_raw, upper_scope);
            itr = kv_idx.upper_bound(
               boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));
            continue; // will --itr at top of loop
         }
         if (scope_raw < lower_scope) {
            // Skip to previous table
            if (p.table) break;
            if (tbl_raw == 0) break;
            seek = make_seek_key_max(tbl_raw - 1, upper_scope);
            itr = kv_idx.upper_bound(
               boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));
            continue;
         }

         // Emit this (table, scope) pair
         if (count >= limit) {
            result.more = name(tbl_raw).to_string() + ":" + name(scope_raw).to_string();
            break;
         }
         result.rows.push_back({p.code, name(scope_raw), name(tbl_raw)});
         ++count;

         // Seek-skip to previous (table, scope): decrement scope, underflow to prev table
         if (scope_raw > 0) {
            seek = make_seek_key(tbl_raw, scope_raw - 1);
         } else if (tbl_raw > 0) {
            seek = make_seek_key(tbl_raw - 1, upper_scope);
         } else {
            break;
         }
         // Position just past the target so the --itr at top lands on it
         itr = kv_idx.upper_bound(
            boost::make_tuple(p.code, config::kv_format_standard, seek.to_string_view()));

         if (fc::time_point::now() >= params_deadline) {
            if (itr != kv_idx.begin()) {
               auto prev = itr;
               --prev;
               if (is_valid(prev)) {
                  auto prev_kv = prev->key_view();
                  uint64_t nt = chain::kv_decode_be64(prev_kv.data());
                  uint64_t ns = chain::kv_decode_be64(prev_kv.data() + 8);
                  result.more = name(nt).to_string() + ":" + name(ns).to_string();
               }
            }
            break;
         }
      }
   }

   return result;
}

vector<asset> read_only::get_currency_balance( const read_only::get_currency_balance_params& p, const fc::time_point& )const {
   const abi_def abi = sysio::chain_apis::get_abi( db, p.code );
   (void)get_table_type( abi, "accounts"_n );

   vector<asset> results;
   const auto& d = db.db();

   // KV storage: iterate [table("accounts"):8B][scope(account):8B][pk:8B]
   auto prefix = chain::make_kv_prefix("accounts"_n, p.account);

   const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();
   auto itr = kv_idx.lower_bound(boost::make_tuple(p.code, config::kv_format_standard, prefix.to_string_view()));

   while (itr != kv_idx.end() && itr->code == p.code) {
      auto kv = itr->key_view();
      if (!prefix.matches(kv) || kv.size() != chain::kv_key_size) break;

      SYS_ASSERT(itr->value.size() >= sizeof(asset), chain::asset_type_exception, "Invalid data on table");

      asset cursor;
      fc::datastream<const char*> ds(itr->value.data(), itr->value.size());
      fc::raw::unpack(ds, cursor);

      SYS_ASSERT(cursor.get_symbol().valid(), chain::asset_type_exception, "Invalid asset");

      if (!p.symbol || boost::iequals(cursor.symbol_name(), *p.symbol)) {
         results.emplace_back(cursor);
      }

      if (p.symbol && boost::iequals(cursor.symbol_name(), *p.symbol)) break;
      ++itr;
   }

   return results;
}

fc::variant read_only::get_currency_stats( const read_only::get_currency_stats_params& p, const fc::time_point& )const {
   fc::mutable_variant_object results;

   const abi_def abi = sysio::chain_apis::get_abi( db, p.code );
   (void)get_table_type( abi, name("stat") );

   uint64_t scope = ( sysio::chain::string_to_symbol( 0, boost::algorithm::to_upper_copy(p.symbol).c_str() ) >> 8 );

   walk_key_value_table(p.code, name(scope), "stat"_n, [&](const auto& obj){
      SYS_ASSERT( obj.value.size() >= sizeof(read_only::get_currency_stats_result), chain::asset_type_exception, "Invalid data on table");

      fc::datastream<const char *> ds(obj.value.data(), obj.value.size());
      read_only::get_currency_stats_result result;

      fc::raw::unpack(ds, result.supply);
      fc::raw::unpack(ds, result.max_supply);
      fc::raw::unpack(ds, result.issuer);

      results[result.supply.symbol_name()] = result;
      return true;
   });

   return results;
}

fc::variant get_global_row( const database& db, const abi_def& abi, const abi_serializer& abis, const fc::microseconds& abi_serializer_max_time_us, bool shorten_abi_errors ) {
   const auto table_type = get_table_type(abi, "global"_n);
   SYS_ASSERT(table_type == read_only::KEYi64, chain::contract_table_query_exception, "Invalid table type {} for table global", table_type);

   auto key = chain::make_kv_key("global"_n, config::system_account_name, name("global").to_uint64_t());
   auto key_sv = key.to_string_view();

   const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
   auto it = kv_idx.find(boost::make_tuple(config::system_account_name, config::kv_format_standard, key_sv));
   SYS_ASSERT(it != kv_idx.end(), chain::contract_table_query_exception, "Missing row in table global");

   vector<char> data(it->value.data(), it->value.data() + it->value.size());
   return abis.binary_to_variant(abis.get_table_type("global"_n), data, abi_serializer::create_yield_function( abi_serializer_max_time_us ), shorten_abi_errors );
}

read_only::get_finalizer_info_result read_only::get_finalizer_info( const read_only::get_finalizer_info_params& p, const fc::time_point& ) const {
   read_only::get_finalizer_info_result result;

   // Finalizer keys present in active_finalizer_policy and pending_finalizer_policy.
   // Use std::set for eliminating duplications.
   std::set<fc::crypto::bls::public_key> finalizer_keys;

   // Populate a particular finalizer policy
   auto add_policy_to_result = [&](const finalizer_policy_ptr& from_policy, fc::variant& to_policy) {
      if (from_policy) {
         // Use string format of public key for easy uses
         to_variant(*from_policy, to_policy);

         for (const auto& f: from_policy->finalizers) {
            finalizer_keys.insert(f.public_key);
         }
      }
   };

   // Populate active_finalizer_policy and pending_finalizer_policy
   add_policy_to_result(db.head_active_finalizer_policy(), result.active_finalizer_policy);
   add_policy_to_result(db.head_pending_finalizer_policy(), result.pending_finalizer_policy);

   // Populate last_tracked_votes
   if (last_tracked_votes) {
      for (const auto& k: finalizer_keys) {
         if (const auto& v = last_tracked_votes->get_last_vote_info(k)) {
            result.last_tracked_votes.emplace_back(*v);
         }
      }
   }

   // Sort last_tracked_votes by description
   std::sort( result.last_tracked_votes.begin(), result.last_tracked_votes.end(), []( const tracked_votes::vote_info& lhs, const tracked_votes::vote_info& rhs ) {
      return lhs.description < rhs.description;
   });

   return result;
}

read_only::get_producers_result
read_only::get_producers( const read_only::get_producers_params& params, const fc::time_point& deadline ) const {
   read_only::get_producers_result result;
   result.rows.reserve(db.active_producers().producers.size());

   for (const auto& p : db.active_producers().producers) {
      auto row = fc::mutable_variant_object()
         ("owner", p.producer_name)
         ("producer_authority", p.authority);

      // detect a legacy key and maintain API compatibility for those entries
      if (std::holds_alternative<block_signing_authority_v0>(p.authority)) {
         const auto& auth = std::get<block_signing_authority_v0>(p.authority);
         if (auth.keys.size() == 1 && auth.keys.back().weight == auth.threshold) {
            row("producer_key", auth.keys.back().key);
         }
      }

      result.rows.emplace_back(std::move(row));
   }

   return result;
}

read_only::get_producer_schedule_result read_only::get_producer_schedule( const read_only::get_producer_schedule_params& p, const fc::time_point& ) const {
   read_only::get_producer_schedule_result result;
   to_variant(db.active_producers(), result.active);
   if (const auto* pending = db.pending_producers())
      to_variant(*pending, result.pending);
   return result;
}

chain::signed_block_ptr read_only::get_raw_block(const read_only::get_raw_block_params& params, const fc::time_point&) const {
   signed_block_ptr block;
   std::optional<uint64_t> block_num;

   SYS_ASSERT( !params.block_num_or_id.empty() && params.block_num_or_id.size() <= 64,
               chain::block_id_type_exception,
               "Invalid Block number or ID, must be greater than 0 and less than 65 characters"
   );

   try {
      block_num = fc::to_uint64(params.block_num_or_id);
   } catch( ... ) {}

   if( block_num ) {
      block = db.fetch_block_by_number( *block_num );
   } else {
      try {
         block = db.fetch_block_by_id( fc::variant(params.block_num_or_id).as<block_id_type>() );
      } SYS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: {}", params.block_num_or_id)
   }

   SYS_ASSERT( block, unknown_block_exception, "Could not find block: {}", params.block_num_or_id);

   return block;
}

std::function<chain::t_or_exception<fc::variant>()> read_only::get_block(const get_raw_block_params& params, const fc::time_point& deadline) const {
   chain::signed_block_ptr block = get_raw_block(params, deadline);

   using return_type = t_or_exception<fc::variant>;
   return [this,
           resolver = get_serializers_cache(db, block, abi_serializer_max_time),
           block    = std::move(block)]() mutable -> return_type {
      try {
         return convert_block(block, resolver);
      } CATCH_AND_RETURN(return_type);
   };
}

read_only::get_block_header_result read_only::get_block_header(const read_only::get_block_header_params& params, const fc::time_point& deadline) const{
   std::optional<uint64_t> block_num;

   SYS_ASSERT( !params.block_num_or_id.empty() && params.block_num_or_id.size() <= 64,
               chain::block_id_type_exception,
               "Invalid Block number or ID, must be greater than 0 and less than 65 characters"
   );

   try {
      block_num = fc::to_uint64(params.block_num_or_id);
   } catch( ... ) {}

   if (!params.include_extensions) {
      std::optional<signed_block_header> header;

      if( block_num ) {
         header = db.fetch_block_header_by_number( *block_num );
      } else {
         try {
            header = db.fetch_block_header_by_id( fc::variant(params.block_num_or_id).as<block_id_type>() );
         } SYS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: {}", params.block_num_or_id)
      }
      SYS_ASSERT( header, unknown_block_exception, "Could not find block header: {}", params.block_num_or_id);
      return { header->calculate_id(), fc::variant{*header}, {}};
   } else {
      signed_block_ptr block;
      if( block_num ) {
         block = db.fetch_block_by_number( *block_num );
      } else {
         try {
            block = db.fetch_block_by_id( fc::variant(params.block_num_or_id).as<block_id_type>() );
         } SYS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: {}", params.block_num_or_id)
      }
      SYS_ASSERT( block, unknown_block_exception, "Could not find block header: {}", params.block_num_or_id);
      return { block->calculate_id(), fc::variant{static_cast<signed_block_header>(*block)}, block->block_extensions};
   }
}

abi_resolver
read_only::get_block_serializers( const chain::signed_block_ptr& block, const fc::microseconds& max_time ) const {
   return get_serializers_cache(db, block, max_time);
}

fc::variant read_only::convert_block( const chain::signed_block_ptr& block, abi_resolver& resolver ) const {
   fc::variant pretty_output;
   abi_serializer::to_variant( *block, pretty_output, resolver, abi_serializer_max_time );

   const auto block_id = block->calculate_id();
   uint32_t ref_block_prefix = block_id._hash[1];

   return fc::mutable_variant_object( std::move(pretty_output.get_object()) )
         ( "id", block_id )
         ( "block_num", block->block_num() )
         ( "ref_block_prefix", ref_block_prefix );
}

fc::variant read_only::get_block_info(const read_only::get_block_info_params& params, const fc::time_point&) const {

   std::optional<signed_block_header> block;
   try {
         block = db.fetch_block_header_by_number( params.block_num );
   } catch (...)   {
      // assert below will handle the invalid block num
   }

   SYS_ASSERT( block, unknown_block_exception, "Could not find block: {}", params.block_num);

   const auto id = block->calculate_id();
   const uint32_t ref_block_prefix = id._hash[1];

   return fc::mutable_variant_object ()
         ("block_num", block->block_num())
         ("ref_block_num", static_cast<uint16_t>(block->block_num()))
         ("id", id)
         ("timestamp", block->timestamp)
         ("producer", block->producer)
         ("previous", block->previous)
         ("transaction_mroot", block->transaction_mroot)
         ("finality_mroot", block->finality_mroot)
         ("qc_claim", block->qc_claim)
         ("producer_signatures", block->producer_signatures)
         ("ref_block_prefix", ref_block_prefix);
}

fc::variant read_only::get_block_header_state(const get_block_header_state_params& params, const fc::time_point&) const {
   signed_block_ptr sbp;
   std::optional<uint64_t> block_num;

   try {
      block_num = fc::to_uint64(params.block_num_or_id);
   } catch( ... ) {}

   if( block_num ) {
      sbp = db.fetch_block_by_number(*block_num);
   } else {
      try {
         sbp = db.fetch_block_by_id(block_id_type(params.block_num_or_id));
      } SYS_RETHROW_EXCEPTIONS(chain::block_id_type_exception, "Invalid block ID: {}", params.block_num_or_id)
   }

   SYS_ASSERT( sbp, unknown_block_exception, "Could not find block: {}", params.block_num_or_id );

   fc::mutable_variant_object result;
   result
      ("block_num", sbp->block_num())
      ("id", sbp->calculate_id())
      ("header", static_cast<const signed_block_header&>(*sbp))
   ;
   return result;
}

void read_write::push_transaction(const read_write::push_transaction_params& params, next_function<read_write::push_transaction_results> next) {
   try {
      auto ptrx = std::make_shared<packed_transaction>();
      auto resolver = caching_resolver(make_resolver(db, abi_serializer_max_time, throw_on_yield::yes));
      try {
         abi_serializer::from_variant(params, *ptrx, resolver, abi_serializer_max_time);
      } SYS_RETHROW_EXCEPTIONS(chain::packed_transaction_type_exception, "Invalid packed transaction")
      ptrx->decompress();

      static incoming::methods::transaction_async::method_type& on_incoming = app().get_method<incoming::methods::transaction_async>();
      on_incoming(ptrx, true, transaction_metadata::trx_type::input, false,
            [this, next](const next_function_variant<transaction_trace_ptr>& result) -> void {
         if (std::holds_alternative<fc::exception_ptr>(result)) {
            next(std::get<fc::exception_ptr>(result));
         } else {
            auto trx_trace_ptr = std::get<transaction_trace_ptr>(result);

            try {
               fc::variant output;
               try {
                  auto resolver = get_serializers_cache(db, trx_trace_ptr, abi_serializer_max_time);
                  abi_serializer::to_variant(*trx_trace_ptr, output, resolver, abi_serializer_max_time);

                  // Create map of (closest_unnotified_ancestor_action_ordinal, global_sequence) with action trace
                  std::map< std::pair<uint32_t, uint64_t>, fc::mutable_variant_object > act_traces_map;
                  for( const auto& act_trace : output["action_traces"].get_array() ) {
                     if (act_trace["receipt"].is_null() && act_trace["except"].is_null()) continue;
                     auto closest_unnotified_ancestor_action_ordinal =
                           act_trace["closest_unnotified_ancestor_action_ordinal"].as<fc::unsigned_int>().value;
                     auto global_sequence = act_trace["receipt"].is_null() ?
                                                std::numeric_limits<uint64_t>::max() :
                                                act_trace["receipt"]["global_sequence"].as<uint64_t>();
                     act_traces_map.emplace( std::make_pair( closest_unnotified_ancestor_action_ordinal,
                                                             global_sequence ),
                                             act_trace.get_object() );
                  }

                  std::function<fc::variants(uint32_t)> convert_act_trace_to_tree_struct =
                  [&](uint32_t closest_unnotified_ancestor_action_ordinal) {
                     fc::variants restructured_act_traces;
                     auto it = act_traces_map.lower_bound(
                                 std::make_pair( closest_unnotified_ancestor_action_ordinal, 0)
                     );
                     for( ;
                        it != act_traces_map.end() && it->first.first == closest_unnotified_ancestor_action_ordinal; ++it )
                     {
                        auto& act_trace_mvo = it->second;

                        auto action_ordinal = act_trace_mvo["action_ordinal"].as<fc::unsigned_int>().value;
                        act_trace_mvo["inline_traces"] = convert_act_trace_to_tree_struct(action_ordinal);
                        if (act_trace_mvo["receipt"].is_null()) {
                           act_trace_mvo["receipt"] = fc::mutable_variant_object()
                              ("abi_sequence", 0)
                              ("act_digest", digest_type::hash(trx_trace_ptr->action_traces[action_ordinal-1].act))
                              ("auth_sequence", flat_map<account_name,uint64_t>())
                              ("code_sequence", 0)
                              ("global_sequence", 0)
                              ("receiver", act_trace_mvo["receiver"])
                              ("recv_sequence", 0);
                        }
                        restructured_act_traces.push_back( std::move(act_trace_mvo) );
                     }
                     return restructured_act_traces;
                  };

                  fc::mutable_variant_object output_mvo(std::move(output.get_object()));
                  output_mvo["action_traces"] = convert_act_trace_to_tree_struct(0);

                  output = std::move(output_mvo);
               } catch( chain::abi_exception& ) {
                  output = *trx_trace_ptr;
               }

               const chain::transaction_id_type& id = trx_trace_ptr->id;
               next(read_write::push_transaction_results{id, output});
            } CATCH_AND_CALL(next);
         }
      });
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

static void push_recurse(read_write* rw, int index, const std::shared_ptr<read_write::push_transactions_params>& params, const std::shared_ptr<read_write::push_transactions_results>& results, const next_function<read_write::push_transactions_results>& next) {
   auto wrapped_next = [=](const next_function_variant<read_write::push_transaction_results>& result) {
      if (std::holds_alternative<fc::exception_ptr>(result)) {
         const auto& e = std::get<fc::exception_ptr>(result);
         results->emplace_back( read_write::push_transaction_results{ transaction_id_type(), fc::mutable_variant_object( "error", e->to_detail_string() ) } );
      } else if (std::holds_alternative<read_write::push_transaction_results>(result)) {
         const auto& r = std::get<read_write::push_transaction_results>(result);
         results->emplace_back( r );
      } else {
         assert(0);
      }

      size_t next_index = index + 1;
      if (next_index < params->size()) {
         push_recurse(rw, next_index, params, results, next );
      } else {
         next(*results);
      }
   };

   rw->push_transaction(params->at(index), wrapped_next);
}

void read_write::push_transactions(const read_write::push_transactions_params& params, next_function<read_write::push_transactions_results> next) {
   try {
      SYS_ASSERT( params.size() <= 1000, too_many_tx_at_once, "Attempt to push too many transactions at once" );
      auto params_copy = std::make_shared<read_write::push_transactions_params>(params.begin(), params.end());
      auto result = std::make_shared<read_write::push_transactions_results>();
      result->reserve(params.size());

      push_recurse(this, 0, params_copy, result, next);
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

// called from read-exclusive thread for read-only
template<class API, class Result>
void api_base::send_transaction_gen(API &api, send_transaction_params_t params, next_function<Result> next) {
   try {
      auto ptrx = std::make_shared<packed_transaction>();
      auto resolver = caching_resolver(make_resolver(api.db, api.abi_serializer_max_time, throw_on_yield::yes));
      try {
         abi_serializer::from_variant(params.transaction, *ptrx, resolver, api.abi_serializer_max_time);
      } SYS_RETHROW_EXCEPTIONS(packed_transaction_type_exception, "Invalid packed transaction")
      ptrx->decompress();

      bool retry = false;
      std::optional<uint16_t> retry_num_blocks;

      if constexpr (std::is_same_v<API, read_write>) {
         retry = params.retry_trx;
         retry_num_blocks = params.retry_trx_num_blocks;

         SYS_ASSERT( !retry || api.trx_retry.has_value(), unsupported_feature, "Transaction retry not enabled on node. transaction-retry-max-storage-size-gb is 0" );
         SYS_ASSERT( !retry || (ptrx->expiration() <= api.trx_retry->get_max_expiration_time()), tx_exp_too_far_exception,
                     "retry transaction expiration {} larger than allowed {}",
                     ptrx->expiration(), api.trx_retry->get_max_expiration_time() );
      }

      static incoming::methods::transaction_async::method_type& on_incoming = app().get_method<incoming::methods::transaction_async>();
      on_incoming(ptrx, true, params.trx_type, params.return_failure_trace,
            [&api, ptrx, next, retry, retry_num_blocks](const next_function_variant<transaction_trace_ptr>& result) -> void {
            if( std::holds_alternative<fc::exception_ptr>( result ) ) {
               next( std::get<fc::exception_ptr>( result ) );
            } else {
               try {
                  auto trx_trace_ptr = std::get<transaction_trace_ptr>( result );
                  bool retried = false;
                  if constexpr (std::is_same_v<API, read_write>) {
                     if( retry && api.trx_retry.has_value() && !trx_trace_ptr->except) {
                        // will be ack'ed via next later
                        api.trx_retry->track_transaction( ptrx, retry_num_blocks,
                             [ptrx, next](const next_function_variant<std::unique_ptr<fc::variant>>& result ) {
                                if( std::holds_alternative<fc::exception_ptr>( result ) ) {
                                   next( std::get<fc::exception_ptr>( result ) );
                                } else {
                                   fc::variant& output = *std::get<std::unique_ptr<fc::variant>>( result );
                                   next( Result{ptrx->id(), std::move( output )} );
                                }
                             } );
                        retried = true;
                     }
                  }
                  else {
                     (void)retry; // ref variable to avoid compilation warning
                     (void)retry_num_blocks; // ref variable to avoid compilation warning
                  }
                  if (!retried) {
                     // we are still on main thread here. The lambda passed to `next()` below will be executed on the http thread pool
                     using return_type = t_or_exception<Result>;
                     next([&api,
                           trx_trace_ptr,
                           resolver = get_serializers_cache(api.db, trx_trace_ptr, api.abi_serializer_max_time)]() mutable {
                        try {
                           fc::variant output;
                           try {
                              abi_serializer::to_variant(*trx_trace_ptr, output, resolver, api.abi_serializer_max_time);
                           } catch( abi_exception& ) {
                              output = *trx_trace_ptr;
                           }
                           const transaction_id_type& id = trx_trace_ptr->id;
                           return return_type(Result{id, std::move( output )});
                        } CATCH_AND_RETURN(return_type);
                     });
                  }
               } CATCH_AND_CALL( next );
            }
         });
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

void read_write::send_transaction(read_write::send_transaction_params params, next_function<read_write::send_transaction_results> next) {
   send_transaction_params_t gen_params { .return_failure_trace = false,
                                          .retry_trx            = false,
                                          .retry_trx_num_blocks = std::nullopt,
                                          .trx_type             = transaction_metadata::trx_type::input,
                                          .transaction          = std::move(params) };
   return send_transaction_gen(*this, std::move(gen_params), std::move(next));
}

void read_write::send_transaction2(read_write::send_transaction2_params params, next_function<read_write::send_transaction_results> next) {
   send_transaction_params_t gen_params  { .return_failure_trace = params.return_failure_trace,
                                           .retry_trx            = params.retry_trx,
                                           .retry_trx_num_blocks = std::move(params.retry_trx_num_blocks),
                                           .trx_type             = transaction_metadata::trx_type::input,
                                           .transaction          = std::move(params.transaction) };
   return send_transaction_gen(*this, std::move(gen_params), std::move(next));
}

read_only::get_abi_results read_only::get_abi( const get_abi_params& params, const fc::time_point& )const {
   try {
      get_abi_results result;
      result.account_name = params.account_name;
      const auto* accnt = db.find_account(params.account_name);
      SYS_ASSERT(accnt != nullptr, account_query_exception, "Account {} not found", params.account_name);

      if (const auto* accnt_metadata = db.find_account_metadata(params.account_name); accnt_metadata != nullptr) {
         if (abi_def abi; abi_serializer::to_abi(accnt_metadata->abi, abi)) {
            result.abi = std::move(abi);
         }
      }

      return result;
   } SYS_RETHROW_EXCEPTIONS(chain::account_query_exception, "unable to retrieve account abi")
}

read_only::get_code_results read_only::get_code( const get_code_params& params, const fc::time_point& )const {
   try {
   SYS_ASSERT( params.code_as_wasm, unsupported_feature, "Returning WAST from get_code is no longer supported" );
   get_code_results result;
   result.account_name = params.account_name;
   const auto* accnt_obj          = db.find_account( params.account_name );
   SYS_ASSERT(accnt_obj != nullptr, account_query_exception, "Account {} not found", params.account_name);
   const auto* accnt_metadata_obj = db.find_account_metadata( params.account_name );

   if (accnt_metadata_obj != nullptr) {
      if (accnt_metadata_obj->code_hash != digest_type()) {
         const auto& d        = db.db();
         const auto& code_obj = d.get<code_object, by_code_hash>(accnt_metadata_obj->code_hash);
         result.wasm          = string(code_obj.code.begin(), code_obj.code.end());
         result.code_hash     = code_obj.code_hash;
      }
      if (abi_def abi; abi_serializer::to_abi(accnt_metadata_obj->abi, abi)) {
         result.abi = std::move(abi);
      }
   }

   return result;
   } SYS_RETHROW_EXCEPTIONS(chain::account_query_exception, "unable to retrieve account code")
}

read_only::get_code_hash_results read_only::get_code_hash( const get_code_hash_params& params, const fc::time_point& )const {
   try {
      get_code_hash_results result;
      result.account_name   = params.account_name;
      const auto* accnt_obj = db.find_account(params.account_name);
      SYS_ASSERT(accnt_obj != nullptr, account_query_exception, "Account {} not found", params.account_name);
      const auto* accnt_metadata_obj = db.find_account_metadata(params.account_name);

      if (accnt_metadata_obj != nullptr && accnt_metadata_obj->code_hash != digest_type())
         result.code_hash = accnt_metadata_obj->code_hash;

      return result;
   } SYS_RETHROW_EXCEPTIONS(chain::account_query_exception, "unable to retrieve account code hash")
}

read_only::get_raw_code_and_abi_results read_only::get_raw_code_and_abi( const get_raw_code_and_abi_params& params, const fc::time_point& )const {
   try {
      get_raw_code_and_abi_results result;

      result.account_name   = params.account_name;
      const auto* accnt_obj = db.find_account(params.account_name);
      SYS_ASSERT(accnt_obj != nullptr, account_query_exception, "Account {} not found", params.account_name);
      const auto* accnt_metadata_obj = db.find_account_metadata(params.account_name);

      if (accnt_metadata_obj != nullptr) {
         if (accnt_metadata_obj->code_hash != digest_type()) {
            const auto& d        = db.db();
            const auto& code_obj = d.get<code_object, by_code_hash>(accnt_metadata_obj->code_hash);
            result.wasm          = blob{{code_obj.code.begin(), code_obj.code.end()}};
         }
         result.abi = blob{{accnt_metadata_obj->abi.begin(), accnt_metadata_obj->abi.end()}};
      }

      return result;
   } SYS_RETHROW_EXCEPTIONS(chain::account_query_exception, "unable to retrieve account code/abi")
}

read_only::get_raw_abi_results read_only::get_raw_abi( const get_raw_abi_params& params, const fc::time_point& )const {
   try {
      get_raw_abi_results result;
      result.account_name = params.account_name;

      const auto* accnt_obj = db.find_account(params.account_name);
      SYS_ASSERT(accnt_obj != nullptr, account_query_exception, "Account {} not found", params.account_name);
      const auto* accnt_metadata_obj = db.find_account_metadata(params.account_name);

      if (accnt_metadata_obj != nullptr) {
         result.abi_hash = fc::sha256::hash( accnt_metadata_obj->abi.data(), accnt_metadata_obj->abi.size() );
         if (accnt_metadata_obj->code_hash != digest_type())
            result.code_hash = accnt_metadata_obj->code_hash;
         if (!params.abi_hash || *params.abi_hash != result.abi_hash)
            result.abi = blob{{accnt_metadata_obj->abi.begin(), accnt_metadata_obj->abi.end()}};
      }

      return result;
   } SYS_RETHROW_EXCEPTIONS(chain::account_query_exception, "unable to retrieve account abi")
}

read_only::get_account_return_t read_only::get_account( const get_account_params& params, const fc::time_point& ) const {
   try {
   get_account_results result;
   result.account_name = params.account_name;

   const auto& d = db.db();
   const auto& rm = db.get_resource_limits_manager();

   result.head_block_num  = db.head().block_num();
   result.head_block_time = db.head().block_time();

   const auto* accnt_obj = db.find_account(params.account_name);
   SYS_ASSERT(accnt_obj != nullptr, account_query_exception, "Account {} not found", params.account_name);
   const auto* accnt_metadata_obj = db.find_account_metadata(params.account_name);

   // Get baseline resource limits from the resource manager
   rm.get_account_limits(result.account_name, result.ram_quota, result.net_weight, result.cpu_weight);

   result.privileged       = accnt_metadata_obj != nullptr && accnt_metadata_obj->is_privileged();
   result.last_code_update = accnt_metadata_obj != nullptr ? accnt_metadata_obj->last_code_update : fc::time_point{};

   uint32_t greylist_limit = db.is_resource_greylisted(result.account_name) ? 1 : config::maximum_elastic_resource_multiplier;
   const block_timestamp_type current_usage_time (db.head().block_time());
   result.net_limit.set( rm.get_account_net_limit_ex( result.account_name, greylist_limit, current_usage_time).first );
   result.cpu_limit.set( rm.get_account_cpu_limit_ex( result.account_name, greylist_limit, current_usage_time).first );
   result.ram_usage = rm.get_account_ram_usage( result.account_name );

   sysio::chain::resource_limits::account_resource_limit subjective_cpu_bill_limit;
   subjective_cpu_bill_limit.used = db.get_subjective_billing().get_subjective_bill( result.account_name, fc::time_point::now() ).count();
   result.subjective_cpu_bill_limit = subjective_cpu_bill_limit;

   // Gather permission and linked actions info as before
   const auto linked_action_map = ([&](){
      const auto& links = d.get_index<permission_link_index,by_permission_name>();
      auto iter = links.lower_bound(boost::make_tuple(params.account_name));

      std::multimap<name, linked_action> result_map;
      while (iter != links.end() && iter->account == params.account_name ) {
         auto action_name = iter->message_type.empty() ? std::optional<name>() : std::optional<name>(iter->message_type);
         result_map.emplace(iter->required_permission, linked_action{iter->code, action_name});
         ++iter;
      }

      return result_map;
   })();

   auto get_linked_actions = [&](chain::name perm_name) {
      auto link_bounds = linked_action_map.equal_range(perm_name);
      auto linked_actions = std::vector<linked_action>();
      linked_actions.reserve(linked_action_map.count(perm_name));
      for (auto link = link_bounds.first; link != link_bounds.second; ++link) {
         linked_actions.push_back(link->second);
      }
      return linked_actions;
   };

   const auto& permissions = d.get_index<permission_index,by_owner>();
   auto perm = permissions.lower_bound(boost::make_tuple(params.account_name));
   while (perm != permissions.end() && perm->owner == params.account_name) {
      name parent;

      // Don't lookup parent if null
      if( perm->parent._id ) {
         const auto* p = d.find<permission_object,by_id>( perm->parent );
         if( p ) {
            SYS_ASSERT(perm->owner == p->owner, invalid_parent_permission, "Invalid parent permission");
            parent = p->name;
         }
      }

      auto linked_actions = get_linked_actions(perm->name);
      result.permissions.push_back(permission{ perm->name, parent, perm->auth.to_authority(), std::move(linked_actions) });
      ++perm;
   }

   // add sysio.any linked authorizations
   result.sysio_any_linked_actions = get_linked_actions(chain::config::sysio_any_name);

   // Load ROA account code/ABI
   const auto* code_account = db.find_account_metadata(config::roa_account_name);
   struct http_params_t {
      std::optional<vector<char>> total_resources;
   };
   http_params_t http_params;

   if( abi_def abi; code_account != nullptr && abi_serializer::to_abi(code_account->abi, abi) ) {

      const auto token_code = "sysio.token"_n;

      auto core_symbol = extract_core_symbol();
      if (params.expected_core_symbol)
         core_symbol = *(params.expected_core_symbol);

      // KV: key = [table("accounts"):8B][scope(account):8B][pk(symbol_code):8B]
      {
         auto key = chain::make_kv_key("accounts"_n, params.account_name, core_symbol.to_symbol_code());
         auto key_sv = key.to_string_view();
         const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();
         auto it = kv_idx.find(boost::make_tuple(token_code, config::kv_format_standard, key_sv));
         if (it != kv_idx.end() && it->value.size() >= sizeof(asset)) {
            asset bal;
            fc::datastream<const char*> ds(it->value.data(), it->value.size());
            fc::raw::unpack(ds, bal);
            if (bal.get_symbol().valid() && bal.get_symbol() == core_symbol)
               result.core_liquid_balance = bal;
         }
      }

      auto lookup_object = [&](const name& table_name, const name& account_name) -> std::optional<vector<char>> {
         auto key = chain::make_kv_key(table_name, config::roa_account_name, account_name.to_uint64_t());
         auto key_sv = key.to_string_view();
         const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();
         auto it = kv_idx.find(boost::make_tuple(config::roa_account_name, config::kv_format_standard, key_sv));
         if (it != kv_idx.end()) {
            return vector<char>(it->value.data(), it->value.data() + it->value.size());
         }
         return {};
      };

      http_params.total_resources          = lookup_object("reslimit"_n, params.account_name);

      return [http_params = std::move(http_params), result = std::move(result), abi=std::move(abi), shorten_abi_errors=shorten_abi_errors,
              abi_serializer_max_time=abi_serializer_max_time]() mutable ->  chain::t_or_exception<read_only::get_account_results> {
         auto yield = [&]() { return abi_serializer::create_yield_function(abi_serializer_max_time); };
         abi_serializer abis(std::move(abi), yield());

         if (http_params.total_resources)
            result.total_resources = abis.binary_to_variant("reslimit", *http_params.total_resources, yield(), shorten_abi_errors);
         return std::move(result);
      };
   }
   return [result = std::move(result)]() mutable -> chain::t_or_exception<read_only::get_account_results> {
      return std::move(result);
   };
   } SYS_RETHROW_EXCEPTIONS(chain::account_query_exception, "unable to retrieve account info")
}

read_only::get_required_keys_result read_only::get_required_keys( const get_required_keys_params& params, const fc::time_point& )const {
   transaction pretty_input;
   auto resolver = caching_resolver(make_resolver(db, abi_serializer_max_time, throw_on_yield::yes));
   try {
      abi_serializer::from_variant(params.transaction, pretty_input, resolver, abi_serializer_max_time);
   } SYS_RETHROW_EXCEPTIONS(chain::transaction_type_exception, "Invalid transaction")

   SYS_ASSERT( pretty_input.delay_sec.value == 0, chain::transaction_type_exception, "delay_sec must be 0");
   auto required_keys_set = db.get_authorization_manager().get_required_keys( pretty_input, params.available_keys );
   get_required_keys_result result;
   result.required_keys = required_keys_set;
   return result;
}

void read_only::compute_transaction(compute_transaction_params params, next_function<compute_transaction_results> next) {
   send_transaction_params_t gen_params { .return_failure_trace = true,
                                          .retry_trx            = false,
                                          .retry_trx_num_blocks = std::nullopt,
                                          .trx_type             = transaction_metadata::trx_type::dry_run,
                                          .transaction          = std::move(params.transaction) };
   return send_transaction_gen(*this, std::move(gen_params), std::move(next));
}

void read_only::send_read_only_transaction(send_read_only_transaction_params params, next_function<send_read_only_transaction_results> next) {
   try {
      static bool read_only_enabled = app().executor().get_read_threads() > 0;
      SYS_ASSERT( read_only_enabled, unsupported_feature,
                  "read-only transactions execution not enabled on API node. Set read-only-threads > 0" );

      send_transaction_params_t gen_params { .return_failure_trace = false,
                                             .retry_trx            = false,
                                             .retry_trx_num_blocks = std::nullopt,
                                             .trx_type             = transaction_metadata::trx_type::read_only,
                                             .transaction          = std::move(params.transaction) };
      // run read-only trx exclusively on read-only threads
      app().executor().post(priority::low, exec_queue::read_exclusive, [this, gen_params{std::move(gen_params)}, next{std::move(next)}]() mutable {
         send_transaction_gen(*this, std::move(gen_params), std::move(next));
      });
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

read_only::get_transaction_id_result read_only::get_transaction_id( const read_only::get_transaction_id_params& params, const fc::time_point& ) const {
   return params.id();
}


account_query_db::get_accounts_by_authorizers_result
read_only::get_accounts_by_authorizers( const account_query_db::get_accounts_by_authorizers_params& args, const fc::time_point& ) const
{
   SYS_ASSERT(aqdb.has_value(), plugin_config_exception, "Account Queries being accessed when not enabled");
   return aqdb->get_accounts_by_authorizers(args);
}

namespace detail {
   struct ram_market_exchange_state_t {
      asset  ignore1;
      asset  ignore2;
      double ignore3;
      asset  core_symbol;
      double ignore4;
   };
}

chain::symbol read_only::extract_core_symbol()const {
   symbol core_symbol(0);

   // The following code makes assumptions about the contract deployed on sysio.token account and how it stores its data.
   const auto& d = db.db();

   auto prefix = chain::make_kv_table_prefix("stat"_n);

   const auto& kv_idx = d.get_index<chain::kv_index, chain::by_code_key>();
   auto itr = kv_idx.lower_bound(boost::make_tuple("sysio.token"_n, config::kv_format_standard, prefix.to_string_view()));
   if (itr != kv_idx.end() && itr->code == "sysio.token"_n) {
      auto kv = itr->key_view();
      if (kv.size() == chain::kv_key_size && prefix.matches(kv)) {
         vector<char> data(itr->value.data(), itr->value.data() + itr->value.size());
         fc::datastream<const char*> ds(data.data(), data.size());
         read_only::get_currency_stats_result result;
         fc::raw::unpack(ds, result.supply);
         fc::raw::unpack(ds, result.max_supply);
         fc::raw::unpack(ds, result.issuer);
         return result.max_supply.get_symbol();
      }
   }

   return core_symbol;
}

read_only::get_consensus_parameters_results
read_only::get_consensus_parameters(const get_consensus_parameters_params&, const fc::time_point& ) const {
   get_consensus_parameters_results results;

   to_variant(db.get_global_properties().configuration, results.chain_config);
   results.wasm_config = db.get_global_properties().wasm_configuration;

   return results;
}

} // namespace chain_apis

fc::variant chain_plugin::get_log_trx_trace(const transaction_trace_ptr& trx_trace ) const {
    fc::variant pretty_output;
    try {
        abi_serializer::to_log_variant(trx_trace, pretty_output,
                                       caching_resolver(make_resolver(chain(), get_abi_serializer_max_time(), throw_on_yield::no)),
                                       get_abi_serializer_max_time());
    } catch (...) {
        pretty_output = trx_trace;
    }
    return pretty_output;
}

fc::variant chain_plugin::get_log_trx(const transaction& trx) const {
    fc::variant pretty_output;
    try {
        abi_serializer::to_log_variant(trx, pretty_output,
                                       caching_resolver(make_resolver(chain(), get_abi_serializer_max_time(), throw_on_yield::no)),
                                       get_abi_serializer_max_time());
    } catch (...) {
        pretty_output = trx;
    }
    return pretty_output;
}

const controller::config& chain_plugin::chain_config() const {
   SYS_ASSERT(my->chain_config.has_value(), plugin_exception, "chain_config not initialized");
   return *my->chain_config;
}
} // namespace sysio

FC_REFLECT( sysio::chain_apis::detail::ram_market_exchange_state_t, (ignore1)(ignore2)(ignore3)(core_symbol)(ignore4) )
