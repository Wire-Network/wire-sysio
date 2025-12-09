#pragma once
#include <sysio/chain/wasm_interface.hpp>
#include <fc/utility.hpp>

namespace sysio::chain::config {

  typedef __uint128_t uint128_t;

  const static auto default_finalizers_dir_name = "finalizers";
  const static auto default_blocks_dir_name     = "blocks";
  const static auto reversible_blocks_dir_name  = "reversible";

  const static auto default_state_dir_name      = "state";
  const static auto fork_db_filename            = "fork_db.dat";
  const static auto safety_filename             = "safety.dat";
  const static auto chain_head_filename         = "chain_head.dat";
  static constexpr auto default_state_size            = 1*1024*1024*1024ll;
  static constexpr auto default_state_guard_size      =    128*1024*1024ll;


  static constexpr auto system_account_name    { "sysio"_n };
  static constexpr auto null_account_name      { "sysio.null"_n };
  static constexpr auto producers_account_name { "sysio.prods"_n };
  static constexpr auto roa_account_name       { "sysio.roa"_n };

  // Active permission of producers account requires greater than 2/3 of the producers to authorize
  static constexpr auto majority_producers_permission_name { "prod.major"_n }; // greater than 1/2 of producers needed to authorize
  static constexpr auto minority_producers_permission_name { "prod.minor"_n }; // greater than 1/3 of producers needed to authorize0

  static constexpr auto sysio_auth_scope       { "sysio.auth"_n };
  static constexpr auto sysio_all_scope        { "sysio.all"_n };

  static constexpr auto active_name     { "active"_n };
  static constexpr auto owner_name      { "owner"_n };
  static constexpr auto sysio_any_name  { "sysio.any"_n };
  static constexpr auto sysio_code_name { "sysio.code"_n };
  static constexpr auto sysio_payer_name { "sysio.payer"_n };

  static constexpr int      block_interval_ms = 500;
  static constexpr int      block_interval_us = block_interval_ms*1000;
  static constexpr uint64_t block_timestamp_epoch = 946684800000ll; // epoch is year 2000.
  static constexpr uint32_t genesis_num_supported_key_types = 5; // wire-sysio supports 5 at genesis
  static constexpr uint32_t k1_r1_num_supported_key_types = 2; // see interface::recover_key

  /** Percentages are fixed point with a denominator of 10,000 */
  static constexpr int percent_100 = 10000;
  static constexpr int percent_1   = 100;

  static constexpr uint32_t account_cpu_usage_average_window_ms  = 24*60*60*1000l;
  static constexpr uint32_t account_net_usage_average_window_ms  = 24*60*60*1000l;
  static constexpr uint32_t block_cpu_usage_average_window_ms    = 60*1000l;
  static constexpr uint32_t block_size_average_window_ms         = 60*1000l;
  static constexpr uint32_t maximum_elastic_resource_multiplier  = 1000;

  static constexpr uint32_t   rate_limiting_precision        = 1000*1000;

  static constexpr uint32_t   default_max_block_net_usage                  = 1024 * 1024; /// at 500ms blocks and 200byte trx, this enables ~10,000 TPS burst
  static constexpr uint32_t   default_target_block_net_usage_pct           = 10 * percent_1; /// we target 1000 TPS
  static constexpr uint32_t   default_max_transaction_net_usage            = default_max_block_net_usage / 2;
  static constexpr uint32_t   default_base_per_transaction_net_usage       = 12;  // 12 bytes (11 bytes for worst case of transaction_receipt_header + 1 byte for static_variant tag)
  static constexpr uint32_t   default_net_usage_leeway                     = 500; // is this reasonable?
  static constexpr uint32_t   default_context_free_discount_net_usage_num  = 0; // Wire does not support discount of context free data
  static constexpr uint32_t   default_context_free_discount_net_usage_den  = 1; // Wire does not support discount of context free data
  static constexpr uint32_t   transaction_id_net_usage                     = 32; // 32 bytes for the size of a transaction id

  static constexpr uint32_t   default_max_block_cpu_usage                  = 200'000; /// max block cpu usage in microseconds
  static constexpr uint32_t   default_target_block_cpu_usage_pct           = 10 * percent_1;
  static constexpr uint32_t   default_max_transaction_cpu_usage            = 3*default_max_block_cpu_usage/4; /// max trx cpu usage in microseconds
  static constexpr uint32_t   default_min_transaction_cpu_usage            = 100; /// min trx cpu usage in microseconds (10000 TPS equiv)
  static constexpr uint32_t   default_subjective_cpu_leeway_us             = 31000; /// default subjective cpu leeway in microseconds
  static constexpr uint32_t   default_subjective_cpu_us                    = 300000; /// default subjective cpu given to each account

  static constexpr uint32_t   default_max_trx_lifetime                     = 60*60; // 1 hour
  static constexpr uint32_t   default_deferred_trx_expiration_window       = 0; // deferred trx not supported by Wire
  static constexpr uint32_t   default_max_trx_delay                        = 45*24*3600; // 45 days
  static constexpr uint32_t   default_max_inline_action_size               = 512 * 1024;   // 512 KB
  static constexpr uint16_t   default_max_inline_action_depth              = 4;
  static constexpr uint16_t   default_max_auth_depth                       = 6;
  static constexpr uint32_t   default_sig_cpu_bill_pct                     = 50 * percent_1; // billable percentage of signature recovery
  static constexpr uint32_t   default_produce_block_offset_ms              = 450;
  static constexpr uint32_t   default_production_pause_vote_timeout_ms     = 6u*1000u; // 6 seconds
  static constexpr uint16_t   default_controller_thread_pool_size          = 2;
  static constexpr uint16_t   default_vote_thread_pool_size                = 4;
  static constexpr uint32_t   default_max_variable_signature_length        = 16384u;
  static constexpr uint32_t   default_max_action_return_value_size         = 256;
  static constexpr uint32_t   default_max_reversible_blocks                = 3600u;

  static constexpr uint32_t   default_max_transaction_finality_status_success_duration_sec = 180;
  static constexpr uint32_t   default_max_transaction_finality_status_failure_duration_sec = 180;

  static_assert(MAX_SIZE_OF_BYTE_ARRAYS == 20*1024*1024, "Changing MAX_SIZE_OF_BYTE_ARRAYS breaks consensus. Make sure this is expected");

  static constexpr uint32_t default_max_wasm_mutable_global_bytes = 1024;
  static constexpr uint32_t default_max_wasm_table_elements       = 1024;
  static constexpr uint32_t default_max_wasm_section_elements     = 8192;
  static constexpr uint32_t default_max_wasm_linear_memory_init   = 64*1024;
  static constexpr uint32_t default_max_wasm_func_local_bytes     = 8192;
  static constexpr uint32_t default_max_wasm_nested_structures    = 1024;
  static constexpr uint32_t default_max_wasm_symbol_bytes         = 8192;
  static constexpr uint32_t default_max_wasm_module_bytes         = 20*1024*1024;
  static constexpr uint32_t default_max_wasm_code_bytes           = 20*1024*1024;
  static constexpr uint32_t default_max_wasm_pages                = 528;
  static constexpr uint32_t default_max_wasm_call_depth           = 251;

  static constexpr uint32_t   min_net_usage_delta_between_base_and_max_for_trx  = 10*1024;
  // Should be large enough to allow recovery from badly set blockchain parameters without a hard fork
  // (unless net_usage_leeway is set to 0 and so are the net limits of all accounts that can help with resetting blockchain parameters).

  static constexpr uint32_t   fixed_net_overhead_of_packed_trx = 16; // is this reasonable?

  static constexpr uint32_t   fixed_overhead_shared_vector_ram_bytes = 16; ///< overhead accounts for fixed portion of size of shared_vector field
  static constexpr uint32_t   overhead_per_row_per_index_ram_bytes = 32;    ///< overhead accounts for basic tracking structures in a row per index
  static constexpr uint32_t   setcode_ram_bytes_multiplier       = 10;     ///< multiplier on contract size to account for multiple copies and cached compilation

  static constexpr uint32_t   hashing_checktime_block_size       = 10*1024;  /// call checktime from hashing intrinsic once per this number of bytes

#ifdef SYSIO_SYS_VM_JIT_RUNTIME_ENABLED
  static constexpr auto default_wasm_runtime = sysio::chain::wasm_interface::vm_type::sys_vm_jit;
#else
  const static sysio::chain::wasm_interface::vm_type default_wasm_runtime = sysio::chain::wasm_interface::vm_type::sys_vm;
#endif

  static constexpr uint32_t   default_abi_serializer_max_time_us = 15*1000; ///< default deadline for abi serialization methods

  /**
 *  The number of sequential blocks produced by a single producer
 */
  static constexpr int producer_repetitions = 12;
  static constexpr int max_producers = 125; // pre-savanna producer (proposer) limit
  static constexpr int max_proposers = 64*1024; // savanna proposer (producer) limit

  static constexpr size_t maximum_tracked_dpos_confirmations = 1024;     ///<
  static_assert(maximum_tracked_dpos_confirmations >= ((max_producers * 2 / 3) + 1) * producer_repetitions, "Settings never allow for DPOS irreversibility" );

  /**
   * Maximum number of finalizers in the finalizer set
   */
  static constexpr size_t max_finalizers = 64*1024; // largest allowed finalizer policy diff
  static constexpr size_t max_finalizer_description_size = 256;

  /**
 * The number of blocks produced per round is based upon all producers having a chance
 * to produce all of their consecutive blocks.
 */
  //const static int blocks_per_round = producer_count * producer_repetitions;

  static constexpr int irreversible_threshold_percent= 70 * percent_1;

  static constexpr uint64_t billable_alignment = 16;

  template<typename T>
  struct billable_size;

  template<typename T>
  constexpr uint64_t billable_size_v = ((billable_size<T>::value + billable_alignment - 1) / billable_alignment) * billable_alignment;


} // namespace sysio::chain::config

constexpr uint64_t SYS_PERCENT(uint64_t value, uint32_t percentage) {
   return (value * percentage) / sysio::chain::config::percent_100;
}

template<typename Number>
Number SYS_PERCENT_CEIL(Number value, uint32_t percentage) {
   return ((value * percentage) + sysio::chain::config::percent_100 - sysio::chain::config::percent_1)  / sysio::chain::config::percent_100;
}
