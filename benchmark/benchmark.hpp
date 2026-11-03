#pragma once

#include <functional>
#include <map>
#include <vector>
#include <limits>
#include <optional>

#include <fc/crypto/hex.hpp>

#include <sysio/chain/wasm_interface.hpp>

namespace sysio::benchmark {
using bytes = std::vector<char>;

void set_num_runs(uint32_t runs);
uint32_t get_num_runs();
/// Sets the table sizes the kv_ops and kv_table features measure.
void set_kv_rows(std::vector<uint32_t> rows);
/// Table sizes the kv_ops and kv_table features measure.
const std::vector<uint32_t>& get_kv_rows();
/// Sets the WASM runtime of the kv_contract, kv_token, kv_ops and kv_table features.
void set_wasm_runtime(chain::wasm_interface::vm_type runtime);
/// WASM runtime of the kv_contract, kv_token, kv_ops and kv_table features. Defaults to sys-vm-oc where it is
/// available.
chain::wasm_interface::vm_type get_wasm_runtime();
std::map<std::string, std::function<void()>> get_features();
void print_header();
/// Prints one result line. `total` is the sum over `runs`, in nanoseconds, and the line shows its average.
void print_results(std::string name, uint32_t runs, uint64_t total, uint64_t min, uint64_t max);
bytes to_bytes(const std::string& source);

void alt_bn_128_benchmarking();
void modexp_benchmarking();
void key_benchmarking();
void hash_benchmarking();
void blake2_benchmarking();
void bls_benchmarking();
void merkle_benchmarking();
void auth_benchmarking();
/// Times kv_object operations directly against chainbase, with no WASM or intrinsic layer.
void kv_chainbase_benchmarking();
/// Compares direct intrinsics with multi_index through WASM on small tables.
void kv_contract_benchmarking();
/// Compares token transfers on direct intrinsics and on multi_index through WASM.
void kv_token_benchmarking();
/// Times each multi_index operation at the sizes from get_kv_rows().
void kv_ops_benchmarking();
/// Times each kv::table operation at the sizes from get_kv_rows().
void kv_table_benchmarking();

void benchmarking(const std::string& name, const std::function<void()>& func, std::optional<size_t> num_runs = {});

} // benchmark
