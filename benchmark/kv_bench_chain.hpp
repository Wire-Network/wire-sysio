#pragma once

#include <benchmark.hpp>

#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <memory>
#include <string>
#include <vector>

namespace sysio::benchmark {

/**
 * Test chain for the KV contract benchmarks. It runs the selected WASM runtime, lifts the CPU limits so one action
 * can run many operations, and deploys one contract on an account with unlimited resources.
 */
class kv_bench_chain {
public:
   static constexpr uint64_t default_state_size     = 1ull << 30; ///< 1 GiB
   static constexpr uint64_t state_size_granularity = 1ull << 20; ///< chainbase takes whole MiB
   static constexpr int64_t  unlimited              = -1;         ///< resource limit value for no limit

   kv_bench_chain(chain::account_name account, const std::vector<uint8_t>& wasm, const std::string& abi,
                  uint64_t state_size = default_state_size)
      : _account(account) {
      // prevent logging from interleaving with the benchmark output
      fc::logger::default_logger().set_log_level(fc::log_level::off);

      auto [config, genesis] = testing::tester::default_config(_tempdir);
      const uint64_t state_mib = (state_size + state_size_granularity - 1) / state_size_granularity;
      config.state_size   = state_mib * state_size_granularity;
      config.wasm_runtime = get_wasm_runtime();
      auto& limits = genesis.initial_configuration;
      limits.max_block_cpu_usage       = max_block_cpu_us;
      limits.max_transaction_cpu_usage = max_transaction_cpu_us;

      _chain = std::make_unique<testing::tester>(config, genesis);
      _chain->execute_setup_policy(testing::setup_policy::full);
      _chain->create_accounts({account}, false, true, false, false);
      _chain->control->get_mutable_resource_limits_manager().set_account_limits(account, unlimited, unlimited,
                                                                                unlimited, false);
      _chain->produce_block();
      _chain->set_code(account, wasm);
      _chain->set_abi(account, abi);
      _chain->produce_block();
   }

   /** Pushes one action on the contract and returns the time it took to execute, in microseconds. */
   int64_t run(chain::action_name action, const fc::variant_object& args) {
      return _chain->push_action(_account, action, _account, args)->action_traces.at(0).elapsed.count();
   }

   void produce_block() { _chain->produce_block(); }

   /// Bytes of the chain's state memory in use, whether by objects or by the allocator's own free lists.
   uint64_t state_bytes_used() const {
      const auto& db = _chain->control->db();
      return db.get_segment_manager()->get_size() - db.get_free_memory();
   }

   testing::tester& tester() { return *_chain; }

private:
   static constexpr uint32_t max_block_cpu_us       = 1'000'000'000;
   static constexpr uint32_t max_transaction_cpu_us = 900'000'000; ///< the chain requires room below the block limit

   fc::temp_directory               _tempdir;
   chain::account_name              _account;
   std::unique_ptr<testing::tester> _chain;
};

} // namespace sysio::benchmark
