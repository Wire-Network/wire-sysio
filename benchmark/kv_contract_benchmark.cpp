#include <kv_bench_chain.hpp>
#include <test_contracts.hpp>

#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>

// Compares the KV access layers end to end through WASM. Each result is the average of the -r runs. Raw calls the
// intrinsics directly, Shim is multi_index, and Fast is multi_index on a trivially copyable row.
//
//    benchmark/benchmark -f kv_contract -r 20
//    benchmark/benchmark -f kv_token -r 20

using namespace sysio::chain::literals;
using fc::mutable_variant_object;
using sysio::testing::test_contracts;

namespace sysio::benchmark {
namespace {

/// bench_kv_db and bench_kv_shim, which share one interface.
namespace db_contract {
   constexpr auto raw_account       = "benchkv"_n;
   constexpr auto shim_account      = "benchshim"_n;
   constexpr auto action_populate   = "populate"_n;
   constexpr auto action_find_all   = "findall"_n;
   constexpr auto action_iter_all   = "iterall"_n;
   constexpr auto action_update_all = "updateall"_n;
   constexpr auto action_erase_all  = "eraseall"_n;
   namespace field { constexpr auto count = "count"; }
}

/// bench_kv_shim_token, bench_kv_token and bench_kv_fast_token, which share one interface.
namespace token_contract {
   constexpr auto shim_account     = "shimtoken"_n;
   constexpr auto raw_account      = "rawtoken"_n;
   constexpr auto fast_account     = "fasttoken"_n;
   constexpr auto action_setup     = "setup"_n;
   constexpr auto action_transfers = "dotransfers"_n;
   namespace field {
      constexpr auto num_accounts = "num_accounts";
      constexpr auto count        = "count";
   }
}

constexpr uint32_t token_accounts   = 100;
constexpr uint32_t warmup_rows      = 0; ///< findall on an empty table still compiles the contract
constexpr uint32_t warmup_transfers = 1;
constexpr int64_t  us_per_ms        = 1000;
constexpr int      time_decimals    = 2;
constexpr int      ratio_decimals   = 2;
constexpr int      column_width     = 14;
constexpr int      name_width       = 20;
constexpr int      rows_width       = 8;
constexpr int      table_width      = 66;

constexpr std::array<uint32_t, 2> contract_row_counts{100, 500};
constexpr std::array<uint32_t, 4> token_transfer_counts{10, 50, 100, 500};

/// Report order of the kv_contract operations.
constexpr std::array<std::string_view, 5> contract_operations{"Populate", "Find All", "Iterate All", "Update All",
                                                              "Erase All"};

/// Renders a duration in microseconds, switching to milliseconds at one millisecond.
std::string format_us(int64_t us) {
   std::ostringstream ss;
   if (us >= us_per_ms)
      ss << std::fixed << std::setprecision(time_decimals) << double(us) / us_per_ms << " ms";
   else
      ss << us << " us";
   return ss.str();
}

/// Prints one kv_contract result line.
void print_contract_row(std::string_view operation, uint32_t rows, int64_t raw_us, int64_t shim_us) {
   std::cout << std::setw(name_width) << operation << std::setw(rows_width) << rows << std::setw(column_width)
             << format_us(raw_us) << std::setw(column_width) << format_us(shim_us) << std::fixed
             << std::setprecision(ratio_decimals) << double(shim_us) / double(raw_us) << "x\n";
}

} // namespace

void kv_contract_benchmarking() {
   kv_bench_chain raw(db_contract::raw_account, test_contracts::bench_kv_db_wasm(), test_contracts::bench_kv_db_abi());
   kv_bench_chain shim(db_contract::shim_account, test_contracts::bench_kv_shim_wasm(),
                       test_contracts::bench_kv_shim_abi());
   const auto no_args = mutable_variant_object();

   // First run compiles the contract, so it is not measured.
   const auto compile_args = mutable_variant_object()(db_contract::field::count, warmup_rows);
   raw.run(db_contract::action_find_all, compile_args);
   shim.run(db_contract::action_find_all, compile_args);

   std::cout << "Average of " << get_num_runs() << " runs\n"
             << std::left << std::setw(name_width) << "Operation" << std::setw(rows_width) << "Rows"
             << std::setw(column_width) << "KV Raw" << std::setw(column_width) << "KV Shim"
             << "Shim/Raw\n" << std::string(table_width, '-') << "\n";

   for (uint32_t rows : contract_row_counts) {
      const auto args = mutable_variant_object()(db_contract::field::count, rows);
      std::array<int64_t, contract_operations.size()> raw_us{};
      std::array<int64_t, contract_operations.size()> shim_us{};

      for (uint32_t run = 0; run < get_num_runs(); ++run) {
         size_t operation = 0;
         const auto both = [&](chain::action_name action, const fc::variant_object& data) {
            raw_us[operation] += raw.run(action, data);
            shim_us[operation] += shim.run(action, data);
            ++operation;
         };
         const auto end_block = [&] {
            raw.produce_block();
            shim.produce_block();
         };

         both(db_contract::action_populate, args);
         end_block();
         both(db_contract::action_find_all, args);
         both(db_contract::action_iter_all, no_args);
         both(db_contract::action_update_all, args);
         end_block();
         both(db_contract::action_erase_all, args);
         end_block();
      }

      for (size_t operation = 0; operation < contract_operations.size(); ++operation)
         print_contract_row(contract_operations[operation], rows, raw_us[operation] / get_num_runs(),
                            shim_us[operation] / get_num_runs());
      std::cout << std::string(table_width, '-') << "\n";
   }
}

void kv_token_benchmarking() {
   kv_bench_chain shim(token_contract::shim_account, test_contracts::bench_kv_shim_token_wasm(),
                       test_contracts::bench_kv_shim_token_abi());
   kv_bench_chain raw(token_contract::raw_account, test_contracts::bench_kv_token_wasm(),
                      test_contracts::bench_kv_token_abi());
   kv_bench_chain fast(token_contract::fast_account, test_contracts::bench_kv_fast_token_wasm(),
                       test_contracts::bench_kv_fast_token_abi());
   const std::array<kv_bench_chain*, 3> chains{&shim, &raw, &fast};

   const auto run_all = [&](chain::action_name action, const fc::variant_object& data) {
      std::array<int64_t, chains.size()> us{};
      for (size_t i = 0; i < chains.size(); ++i) {
         us[i] = chains[i]->run(action, data);
         chains[i]->produce_block();
      }
      return us;
   };

   run_all(token_contract::action_setup, mutable_variant_object()(token_contract::field::num_accounts, token_accounts));
   // First run compiles the contract, so it is not measured.
   run_all(token_contract::action_transfers, mutable_variant_object()(token_contract::field::count, warmup_transfers));

   std::cout << "Average of " << get_num_runs() << " runs. Each transfer = 2 reads + 2 writes "
             << "(sub_balance + add_balance)\n"
             << std::left << std::setw(rows_width) << "Xfers" << std::setw(column_width) << "KV Shim"
             << std::setw(column_width) << "KV Raw" << std::setw(column_width) << "KV Fast"
             << std::setw(rows_width + 2) << "Shim/Raw" << "Fast/Raw\n" << std::string(table_width, '-') << "\n";

   for (uint32_t transfers : token_transfer_counts) {
      std::array<int64_t, chains.size()> total_us{};
      for (uint32_t run = 0; run < get_num_runs(); ++run) {
         const auto us = run_all(token_contract::action_transfers,
                                 mutable_variant_object()(token_contract::field::count, transfers));
         for (size_t i = 0; i < chains.size(); ++i)
            total_us[i] += us[i];
      }
      const int64_t shim_us = total_us[0] / get_num_runs();
      const int64_t raw_us  = total_us[1] / get_num_runs();
      const int64_t fast_us = total_us[2] / get_num_runs();
      std::cout << std::setw(rows_width) << transfers << std::setw(column_width) << format_us(shim_us)
                << std::setw(column_width) << format_us(raw_us) << std::setw(column_width) << format_us(fast_us)
                << std::fixed << std::setprecision(ratio_decimals) << std::setw(rows_width + 2)
                << double(shim_us) / double(raw_us) << double(fast_us) / double(raw_us) << "\n";
   }
}

} // namespace sysio::benchmark
