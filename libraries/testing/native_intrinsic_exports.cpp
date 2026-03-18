// Native intrinsic exports for the native_module runtime.
//
// These extern "C" functions match the CDT C API signatures and are resolved
// at dlopen time by native-compiled contract .so files. Each dispatches
// through the thread-local webassembly::interface* context stack.
//
// The test executable (unit_test) exports these symbols via --dynamic-list.

#include <sysio/chain/webassembly/native-module/native_context_stack.hpp>
#include <sysio/chain/webassembly/common.hpp>
#include <sysio/chain/apply_context.hpp>

#include <cstdint>
#include <cstdlib>
#include <vector>

#define INTRINSIC_EXPORT extern "C" __attribute__((visibility("default")))

using namespace sysio::chain;
using sysio::chain::webassembly::native_module::native_context_stack;

// Helper: construct legacy_span from pointer + size
// legacy_span<const char> = argument_proxy<span<const char>> with constructor (void*, uint32_t)
// For native code, memory is already in host address space - no WASM bounds checking needed.

// ============================================================================
// System
// ============================================================================

INTRINSIC_EXPORT
void sysio_assert(uint32_t test, const char* msg) {
   native_context_stack::current()->sysio_assert(test, null_terminated_ptr{msg});
}

INTRINSIC_EXPORT
void sysio_assert_message(uint32_t test, const char* msg, uint32_t msg_len) {
   native_context_stack::current()->sysio_assert_message(test, legacy_span<const char>{(void*)msg, msg_len});
}

INTRINSIC_EXPORT
void sysio_assert_code(uint32_t test, uint64_t code) {
   native_context_stack::current()->sysio_assert_code(test, code);
}

INTRINSIC_EXPORT
[[noreturn]] void sysio_exit(int32_t code) {
   native_context_stack::current()->sysio_exit(code);
   __builtin_unreachable(); // sysio_exit throws, but compiler needs this
}

INTRINSIC_EXPORT
uint64_t current_time() {
   return native_context_stack::current()->current_time();
}

INTRINSIC_EXPORT
uint32_t get_block_num() {
   return native_context_stack::current()->get_block_num();
}

INTRINSIC_EXPORT
bool is_feature_activated(const void* feature_digest) {
   return native_context_stack::current()->is_feature_activated(legacy_ptr<const digest_type>{(void*)feature_digest});
}

INTRINSIC_EXPORT
uint64_t get_sender() {
   return native_context_stack::current()->get_sender().to_uint64_t();
}

INTRINSIC_EXPORT
int64_t get_ram_usage(uint64_t account) {
   return native_context_stack::current()->get_ram_usage(name{account});
}

// ============================================================================
// Action
// ============================================================================

INTRINSIC_EXPORT
uint32_t read_action_data(void* msg, uint32_t len) {
   return native_context_stack::current()->read_action_data(legacy_span<char>{msg, len});
}

INTRINSIC_EXPORT
uint32_t action_data_size() {
   return native_context_stack::current()->action_data_size();
}

INTRINSIC_EXPORT
uint64_t current_receiver() {
   return native_context_stack::current()->current_receiver().to_uint64_t();
}

INTRINSIC_EXPORT
uint64_t publication_time() {
   return native_context_stack::current()->publication_time();
}

INTRINSIC_EXPORT
void set_action_return_value(void* data, size_t size) {
   native_context_stack::current()->set_action_return_value(
      sysio::vm::span<const char>{static_cast<const char*>(data), size});
}

// ============================================================================
// Authorization
// ============================================================================

INTRINSIC_EXPORT
void require_auth(uint64_t account) {
   native_context_stack::current()->require_auth(name{account});
}

INTRINSIC_EXPORT
void require_auth2(uint64_t account, uint64_t permission) {
   native_context_stack::current()->require_auth2(name{account}, name{permission});
}

INTRINSIC_EXPORT
bool has_auth(uint64_t account) {
   return native_context_stack::current()->has_auth(name{account});
}

INTRINSIC_EXPORT
void require_recipient(uint64_t recipient) {
   native_context_stack::current()->require_recipient(name{recipient});
}

INTRINSIC_EXPORT
bool is_account(uint64_t account) {
   return native_context_stack::current()->is_account(name{account});
}

INTRINSIC_EXPORT
uint32_t get_code_hash(uint64_t account, uint32_t struct_version, char* result_buffer, size_t buffer_size) {
   return native_context_stack::current()->get_code_hash(
      name{account}, struct_version,
      sysio::vm::span<char>{result_buffer, buffer_size});
}

// ============================================================================
// Crypto
// ============================================================================

INTRINSIC_EXPORT
void assert_sha256(const char* data, uint32_t datalen, const void* hash_val) {
   native_context_stack::current()->assert_sha256(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<const fc::sha256>{(void*)hash_val});
}

INTRINSIC_EXPORT
void assert_sha1(const char* data, uint32_t datalen, const void* hash_val) {
   native_context_stack::current()->assert_sha1(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<const fc::sha1>{(void*)hash_val});
}

INTRINSIC_EXPORT
void assert_sha512(const char* data, uint32_t datalen, const void* hash_val) {
   native_context_stack::current()->assert_sha512(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<const fc::sha512>{(void*)hash_val});
}

INTRINSIC_EXPORT
void assert_ripemd160(const char* data, uint32_t datalen, const void* hash_val) {
   native_context_stack::current()->assert_ripemd160(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<const fc::ripemd160>{(void*)hash_val});
}

INTRINSIC_EXPORT
void sha256(const char* data, uint32_t datalen, void* hash_val) {
   native_context_stack::current()->sha256(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<fc::sha256>{hash_val});
}

INTRINSIC_EXPORT
void sha1(const char* data, uint32_t datalen, void* hash_val) {
   native_context_stack::current()->sha1(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<fc::sha1>{hash_val});
}

INTRINSIC_EXPORT
void sha512(const char* data, uint32_t datalen, void* hash_val) {
   native_context_stack::current()->sha512(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<fc::sha512>{hash_val});
}

INTRINSIC_EXPORT
void ripemd160(const char* data, uint32_t datalen, void* hash_val) {
   native_context_stack::current()->ripemd160(
      legacy_span<const char>{(void*)data, datalen},
      legacy_ptr<fc::ripemd160>{hash_val});
}

INTRINSIC_EXPORT
int32_t recover_key(const void* digest, const char* sig, size_t siglen, char* pub, size_t publen) {
   return native_context_stack::current()->recover_key(
      legacy_ptr<const fc::sha256>{(void*)digest},
      legacy_span<const char>{(void*)sig, (uint32_t)siglen},
      legacy_span<char>{(void*)pub, (uint32_t)publen});
}

INTRINSIC_EXPORT
void assert_recover_key(const void* digest, const char* sig, size_t siglen, const char* pub, size_t publen) {
   native_context_stack::current()->assert_recover_key(
      legacy_ptr<const fc::sha256>{(void*)digest},
      legacy_span<const char>{(void*)sig, (uint32_t)siglen},
      legacy_span<const char>{(void*)pub, (uint32_t)publen});
}

// ============================================================================
// Console / Print
// ============================================================================

INTRINSIC_EXPORT
void prints(const char* str) {
   native_context_stack::current()->prints(null_terminated_ptr{str});
}

INTRINSIC_EXPORT
void prints_l(const char* str, uint32_t len) {
   native_context_stack::current()->prints_l(legacy_span<const char>{(void*)str, len});
}

INTRINSIC_EXPORT
void printi(int64_t val) {
   native_context_stack::current()->printi(val);
}

INTRINSIC_EXPORT
void printui(uint64_t val) {
   native_context_stack::current()->printui(val);
}

INTRINSIC_EXPORT
void printi128(const void* val) {
   native_context_stack::current()->printi128(legacy_ptr<const __int128>{(void*)val});
}

INTRINSIC_EXPORT
void printui128(const void* val) {
   native_context_stack::current()->printui128(legacy_ptr<const unsigned __int128>{(void*)val});
}

INTRINSIC_EXPORT
void printsf(float val) {
   native_context_stack::current()->printsf(::to_softfloat32(val));
}

INTRINSIC_EXPORT
void printdf(double val) {
   native_context_stack::current()->printdf(::to_softfloat64(val));
}

INTRINSIC_EXPORT
void printqf(const void* val) {
   native_context_stack::current()->printqf(legacy_ptr<const float128_t>{(void*)val});
}

INTRINSIC_EXPORT
void printn(uint64_t name_val) {
   native_context_stack::current()->printn(name{name_val});
}

INTRINSIC_EXPORT
void printhex(const void* data, uint32_t datalen) {
   native_context_stack::current()->printhex(legacy_span<const char>{(void*)data, datalen});
}

// ============================================================================
// Database - Primary i64 index
// ============================================================================

INTRINSIC_EXPORT
int32_t db_store_i64(uint64_t scope, uint64_t table, uint64_t payer, uint64_t id, const void* data, uint32_t len) {
   return native_context_stack::current()->db_store_i64(scope, table, payer, id,
      legacy_span<const char>{(void*)data, len});
}

INTRINSIC_EXPORT
void db_update_i64(int32_t itr, uint64_t payer, const void* data, uint32_t len) {
   native_context_stack::current()->db_update_i64(itr, payer,
      legacy_span<const char>{(void*)data, len});
}

INTRINSIC_EXPORT
void db_remove_i64(int32_t itr) {
   native_context_stack::current()->db_remove_i64(itr);
}

INTRINSIC_EXPORT
int32_t db_get_i64(int32_t itr, void* data, uint32_t len) {
   return native_context_stack::current()->db_get_i64(itr, legacy_span<char>{data, len});
}

INTRINSIC_EXPORT
int32_t db_next_i64(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_next_i64(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_previous_i64(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_previous_i64(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_find_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t id) {
   return native_context_stack::current()->db_find_i64(code, scope, table, id);
}

INTRINSIC_EXPORT
int32_t db_lowerbound_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t id) {
   return native_context_stack::current()->db_lowerbound_i64(code, scope, table, id);
}

INTRINSIC_EXPORT
int32_t db_upperbound_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t id) {
   return native_context_stack::current()->db_upperbound_i64(code, scope, table, id);
}

INTRINSIC_EXPORT
int32_t db_end_i64(uint64_t code, uint64_t scope, uint64_t table) {
   return native_context_stack::current()->db_end_i64(code, scope, table);
}

// ============================================================================
// Database - idx64 secondary index
// ============================================================================

INTRINSIC_EXPORT
int32_t db_idx64_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t id, const uint64_t* secondary) {
   return native_context_stack::current()->db_idx64_store(scope, table, payer, id,
      legacy_ptr<const uint64_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx64_update(int32_t itr, uint64_t payer, const uint64_t* secondary) {
   native_context_stack::current()->db_idx64_update(itr, payer,
      legacy_ptr<const uint64_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx64_remove(int32_t itr) {
   native_context_stack::current()->db_idx64_remove(itr);
}

INTRINSIC_EXPORT
int32_t db_idx64_find_secondary(uint64_t code, uint64_t scope, uint64_t table, const uint64_t* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx64_find_secondary(code, scope, table,
      legacy_ptr<const uint64_t>{(void*)secondary},
      legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx64_find_primary(uint64_t code, uint64_t scope, uint64_t table, uint64_t* secondary, uint64_t primary) {
   return native_context_stack::current()->db_idx64_find_primary(code, scope, table,
      legacy_ptr<uint64_t>{(void*)secondary}, primary);
}

INTRINSIC_EXPORT
int32_t db_idx64_lowerbound(uint64_t code, uint64_t scope, uint64_t table, uint64_t* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx64_lowerbound(code, scope, table,
      legacy_ptr<uint64_t, 8>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx64_upperbound(uint64_t code, uint64_t scope, uint64_t table, uint64_t* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx64_upperbound(code, scope, table,
      legacy_ptr<uint64_t, 8>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx64_end(uint64_t code, uint64_t scope, uint64_t table) {
   return native_context_stack::current()->db_idx64_end(code, scope, table);
}

INTRINSIC_EXPORT
int32_t db_idx64_next(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx64_next(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx64_previous(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx64_previous(itr, legacy_ptr<uint64_t>{(void*)primary});
}

// ============================================================================
// Database - idx128 secondary index
// ============================================================================

INTRINSIC_EXPORT
int32_t db_idx128_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t id, const __uint128_t* secondary) {
   return native_context_stack::current()->db_idx128_store(scope, table, payer, id,
      legacy_ptr<const uint128_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx128_update(int32_t itr, uint64_t payer, const __uint128_t* secondary) {
   native_context_stack::current()->db_idx128_update(itr, payer,
      legacy_ptr<const uint128_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx128_remove(int32_t itr) {
   native_context_stack::current()->db_idx128_remove(itr);
}

INTRINSIC_EXPORT
int32_t db_idx128_find_secondary(uint64_t code, uint64_t scope, uint64_t table, const __uint128_t* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx128_find_secondary(code, scope, table,
      legacy_ptr<const uint128_t>{(void*)secondary},
      legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx128_find_primary(uint64_t code, uint64_t scope, uint64_t table, __uint128_t* secondary, uint64_t primary) {
   return native_context_stack::current()->db_idx128_find_primary(code, scope, table,
      legacy_ptr<uint128_t>{(void*)secondary}, primary);
}

INTRINSIC_EXPORT
int32_t db_idx128_lowerbound(uint64_t code, uint64_t scope, uint64_t table, __uint128_t* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx128_lowerbound(code, scope, table,
      legacy_ptr<uint128_t, 16>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx128_upperbound(uint64_t code, uint64_t scope, uint64_t table, __uint128_t* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx128_upperbound(code, scope, table,
      legacy_ptr<uint128_t, 16>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx128_end(uint64_t code, uint64_t scope, uint64_t table) {
   return native_context_stack::current()->db_idx128_end(code, scope, table);
}

INTRINSIC_EXPORT
int32_t db_idx128_next(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx128_next(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx128_previous(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx128_previous(itr, legacy_ptr<uint64_t>{(void*)primary});
}

// ============================================================================
// Database - idx256 secondary index
// ============================================================================

INTRINSIC_EXPORT
int32_t db_idx256_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t id, const __uint128_t* data, uint32_t data_len) {
   return native_context_stack::current()->db_idx256_store(scope, table, payer, id,
      legacy_span<const uint128_t>{(void*)data, data_len});
}

INTRINSIC_EXPORT
void db_idx256_update(int32_t itr, uint64_t payer, const __uint128_t* data, uint32_t data_len) {
   native_context_stack::current()->db_idx256_update(itr, payer,
      legacy_span<const uint128_t>{(void*)data, data_len});
}

INTRINSIC_EXPORT
void db_idx256_remove(int32_t itr) {
   native_context_stack::current()->db_idx256_remove(itr);
}

INTRINSIC_EXPORT
int32_t db_idx256_find_secondary(uint64_t code, uint64_t scope, uint64_t table, const __uint128_t* data, uint32_t data_len, uint64_t* primary) {
   return native_context_stack::current()->db_idx256_find_secondary(code, scope, table,
      legacy_span<const uint128_t>{(void*)data, data_len},
      legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx256_find_primary(uint64_t code, uint64_t scope, uint64_t table, __uint128_t* data, uint32_t data_len, uint64_t primary) {
   return native_context_stack::current()->db_idx256_find_primary(code, scope, table,
      legacy_span<uint128_t>{(void*)data, data_len}, primary);
}

INTRINSIC_EXPORT
int32_t db_idx256_lowerbound(uint64_t code, uint64_t scope, uint64_t table, __uint128_t* data, uint32_t data_len, uint64_t* primary) {
   return native_context_stack::current()->db_idx256_lowerbound(code, scope, table,
      legacy_span<uint128_t>{(void*)data, data_len},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx256_upperbound(uint64_t code, uint64_t scope, uint64_t table, __uint128_t* data, uint32_t data_len, uint64_t* primary) {
   return native_context_stack::current()->db_idx256_upperbound(code, scope, table,
      legacy_span<uint128_t>{(void*)data, data_len},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx256_end(uint64_t code, uint64_t scope, uint64_t table) {
   return native_context_stack::current()->db_idx256_end(code, scope, table);
}

INTRINSIC_EXPORT
int32_t db_idx256_next(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx256_next(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx256_previous(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx256_previous(itr, legacy_ptr<uint64_t>{(void*)primary});
}

// ============================================================================
// Database - idx_double secondary index
// ============================================================================

INTRINSIC_EXPORT
int32_t db_idx_double_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t id, const double* secondary) {
   return native_context_stack::current()->db_idx_double_store(scope, table, payer, id,
      legacy_ptr<const float64_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx_double_update(int32_t itr, uint64_t payer, const double* secondary) {
   native_context_stack::current()->db_idx_double_update(itr, payer,
      legacy_ptr<const float64_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx_double_remove(int32_t itr) {
   native_context_stack::current()->db_idx_double_remove(itr);
}

INTRINSIC_EXPORT
int32_t db_idx_double_find_secondary(uint64_t code, uint64_t scope, uint64_t table, const double* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx_double_find_secondary(code, scope, table,
      legacy_ptr<const float64_t>{(void*)secondary},
      legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_double_find_primary(uint64_t code, uint64_t scope, uint64_t table, double* secondary, uint64_t primary) {
   return native_context_stack::current()->db_idx_double_find_primary(code, scope, table,
      legacy_ptr<float64_t>{(void*)secondary}, primary);
}

INTRINSIC_EXPORT
int32_t db_idx_double_lowerbound(uint64_t code, uint64_t scope, uint64_t table, double* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx_double_lowerbound(code, scope, table,
      legacy_ptr<float64_t, 8>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_double_upperbound(uint64_t code, uint64_t scope, uint64_t table, double* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx_double_upperbound(code, scope, table,
      legacy_ptr<float64_t, 8>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_double_end(uint64_t code, uint64_t scope, uint64_t table) {
   return native_context_stack::current()->db_idx_double_end(code, scope, table);
}

INTRINSIC_EXPORT
int32_t db_idx_double_next(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx_double_next(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_double_previous(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx_double_previous(itr, legacy_ptr<uint64_t>{(void*)primary});
}

// ============================================================================
// Database - idx_long_double secondary index
// ============================================================================

INTRINSIC_EXPORT
int32_t db_idx_long_double_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t id, const long double* secondary) {
   return native_context_stack::current()->db_idx_long_double_store(scope, table, payer, id,
      legacy_ptr<const float128_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx_long_double_update(int32_t itr, uint64_t payer, const long double* secondary) {
   native_context_stack::current()->db_idx_long_double_update(itr, payer,
      legacy_ptr<const float128_t>{(void*)secondary});
}

INTRINSIC_EXPORT
void db_idx_long_double_remove(int32_t itr) {
   native_context_stack::current()->db_idx_long_double_remove(itr);
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_find_secondary(uint64_t code, uint64_t scope, uint64_t table, const long double* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx_long_double_find_secondary(code, scope, table,
      legacy_ptr<const float128_t>{(void*)secondary},
      legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_find_primary(uint64_t code, uint64_t scope, uint64_t table, long double* secondary, uint64_t primary) {
   return native_context_stack::current()->db_idx_long_double_find_primary(code, scope, table,
      legacy_ptr<float128_t>{(void*)secondary}, primary);
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_lowerbound(uint64_t code, uint64_t scope, uint64_t table, long double* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx_long_double_lowerbound(code, scope, table,
      legacy_ptr<float128_t, 8>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_upperbound(uint64_t code, uint64_t scope, uint64_t table, long double* secondary, uint64_t* primary) {
   return native_context_stack::current()->db_idx_long_double_upperbound(code, scope, table,
      legacy_ptr<float128_t, 8>{(void*)secondary},
      legacy_ptr<uint64_t, 8>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_end(uint64_t code, uint64_t scope, uint64_t table) {
   return native_context_stack::current()->db_idx_long_double_end(code, scope, table);
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_next(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx_long_double_next(itr, legacy_ptr<uint64_t>{(void*)primary});
}

INTRINSIC_EXPORT
int32_t db_idx_long_double_previous(int32_t itr, uint64_t* primary) {
   return native_context_stack::current()->db_idx_long_double_previous(itr, legacy_ptr<uint64_t>{(void*)primary});
}

// ============================================================================
// Transaction
// ============================================================================

INTRINSIC_EXPORT
void send_inline(char* serialized_action, size_t size) {
   native_context_stack::current()->send_inline(legacy_span<const char>{(void*)serialized_action, (uint32_t)size});
}

INTRINSIC_EXPORT
void send_context_free_inline(char* serialized_action, size_t size) {
   native_context_stack::current()->send_context_free_inline(legacy_span<const char>{(void*)serialized_action, (uint32_t)size});
}

INTRINSIC_EXPORT
int32_t read_transaction(char* buffer, size_t buffer_size) {
   return native_context_stack::current()->read_transaction(legacy_span<char>{(void*)buffer, (uint32_t)buffer_size});
}

INTRINSIC_EXPORT
int32_t transaction_size() {
   return native_context_stack::current()->transaction_size();
}

INTRINSIC_EXPORT
int32_t expiration() {
   return native_context_stack::current()->expiration();
}

INTRINSIC_EXPORT
int32_t tapos_block_num() {
   return native_context_stack::current()->tapos_block_num();
}

INTRINSIC_EXPORT
int32_t tapos_block_prefix() {
   return native_context_stack::current()->tapos_block_prefix();
}

INTRINSIC_EXPORT
int32_t get_action(uint32_t type, uint32_t index, char* buffer, size_t buffer_size) {
   return native_context_stack::current()->get_action(type, index,
      legacy_span<char>{(void*)buffer, (uint32_t)buffer_size});
}

INTRINSIC_EXPORT
int32_t get_context_free_data(uint32_t index, char* buffer, size_t buffer_size) {
   return native_context_stack::current()->get_context_free_data(index,
      legacy_span<char>{(void*)buffer, (uint32_t)buffer_size});
}

// ============================================================================
// Privileged
// ============================================================================

INTRINSIC_EXPORT
void set_resource_limits(uint64_t account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight) {
   native_context_stack::current()->set_resource_limits(name{account}, ram_bytes, net_weight, cpu_weight);
}

INTRINSIC_EXPORT
void get_resource_limits(uint64_t account, int64_t* ram_bytes, int64_t* net_weight, int64_t* cpu_weight) {
   native_context_stack::current()->get_resource_limits(name{account},
      legacy_ptr<int64_t, 8>{(void*)ram_bytes},
      legacy_ptr<int64_t, 8>{(void*)net_weight},
      legacy_ptr<int64_t, 8>{(void*)cpu_weight});
}

INTRINSIC_EXPORT
uint32_t get_blockchain_parameters_packed(char* data, uint32_t datalen) {
   return native_context_stack::current()->get_blockchain_parameters_packed(
      legacy_span<char>{(void*)data, datalen});
}

INTRINSIC_EXPORT
void set_blockchain_parameters_packed(const char* data, uint32_t datalen) {
   native_context_stack::current()->set_blockchain_parameters_packed(
      legacy_span<const char>{(void*)data, datalen});
}

INTRINSIC_EXPORT
uint32_t get_parameters_packed(const char* ids, uint32_t ids_len, char* params, uint32_t params_len) {
   return native_context_stack::current()->get_parameters_packed(
      sysio::vm::span<const char>{ids, ids_len},
      sysio::vm::span<char>{params, params_len});
}

INTRINSIC_EXPORT
void set_parameters_packed(const char* params, uint32_t params_len) {
   native_context_stack::current()->set_parameters_packed(
      sysio::vm::span<const char>{params, params_len});
}

INTRINSIC_EXPORT
uint32_t get_wasm_parameters_packed(char* data, uint32_t datalen, uint32_t max_version) {
   return native_context_stack::current()->get_wasm_parameters_packed(
      legacy_span<char>{(void*)data, datalen}, max_version);
}

INTRINSIC_EXPORT
void set_wasm_parameters_packed(const char* data, uint32_t datalen) {
   native_context_stack::current()->set_wasm_parameters_packed(
      legacy_span<const char>{(void*)data, datalen});
}

INTRINSIC_EXPORT
bool is_privileged(uint64_t account) {
   return native_context_stack::current()->is_privileged(name{account});
}

INTRINSIC_EXPORT
void set_privileged(uint64_t account, bool is_priv) {
   native_context_stack::current()->set_privileged(name{account}, is_priv);
}

INTRINSIC_EXPORT
void preactivate_feature(const void* feature_digest) {
   native_context_stack::current()->preactivate_feature(legacy_ptr<const digest_type>{(void*)feature_digest});
}

INTRINSIC_EXPORT
int64_t set_proposed_producers(const char* data, size_t datalen) {
   return native_context_stack::current()->set_proposed_producers(
      legacy_span<const char>{(void*)data, (uint32_t)datalen});
}

INTRINSIC_EXPORT
int64_t set_proposed_producers_ex(uint64_t format, const char* data, size_t datalen) {
   return native_context_stack::current()->set_proposed_producers_ex(format,
      legacy_span<const char>{(void*)data, (uint32_t)datalen});
}

INTRINSIC_EXPORT
int32_t get_active_producers(uint64_t* producers, uint32_t datalen) {
   return native_context_stack::current()->get_active_producers(
      legacy_span<name>{(void*)producers, datalen});
}

INTRINSIC_EXPORT
void set_finalizers(uint64_t packed_finalizer_format, const char* data, size_t datalen) {
   native_context_stack::current()->set_finalizers(packed_finalizer_format,
      sysio::vm::span<const char>{data, datalen});
}

// ============================================================================
// Permission
// ============================================================================

INTRINSIC_EXPORT
int32_t check_transaction_authorization(const char* trx_data, uint32_t trx_size,
                                        const char* pubkeys_data, uint32_t pubkeys_size,
                                        const char* perms_data, uint32_t perms_size) {
   return native_context_stack::current()->check_transaction_authorization(
      legacy_span<const char>{(void*)trx_data, trx_size},
      legacy_span<const char>{(void*)pubkeys_data, pubkeys_size},
      legacy_span<const char>{(void*)perms_data, perms_size});
}

INTRINSIC_EXPORT
int32_t check_permission_authorization(uint64_t account, uint64_t permission,
                                       const char* pubkeys_data, uint32_t pubkeys_size,
                                       const char* perms_data, uint32_t perms_size,
                                       uint64_t delay_us) {
   return native_context_stack::current()->check_permission_authorization(
      name{account}, name{permission},
      legacy_span<const char>{(void*)pubkeys_data, pubkeys_size},
      legacy_span<const char>{(void*)perms_data, perms_size},
      delay_us);
}

// ============================================================================
// Crypto extensions
// ============================================================================

INTRINSIC_EXPORT
int32_t alt_bn128_add(const char* op1, uint32_t op1_len, const char* op2, uint32_t op2_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->alt_bn128_add(
      sysio::vm::span<const char>{op1, op1_len},
      sysio::vm::span<const char>{op2, op2_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t alt_bn128_mul(const char* g1, uint32_t g1_len, const char* scalar, uint32_t scalar_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->alt_bn128_mul(
      sysio::vm::span<const char>{g1, g1_len},
      sysio::vm::span<const char>{scalar, scalar_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t alt_bn128_pair(const char* pairs, uint32_t pairs_len) {
   return native_context_stack::current()->alt_bn128_pair(
      sysio::vm::span<const char>{pairs, pairs_len});
}

INTRINSIC_EXPORT
int32_t mod_exp(const char* base, uint32_t base_len, const char* exp, uint32_t exp_len, const char* mod, uint32_t mod_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->mod_exp(
      sysio::vm::span<const char>{base, base_len},
      sysio::vm::span<const char>{exp, exp_len},
      sysio::vm::span<const char>{mod, mod_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t blake2_f(uint32_t rounds, const char* state, uint32_t state_len, const char* msg, uint32_t msg_len, const char* t0_offset, uint32_t t0_len, const char* t1_offset, uint32_t t1_len, int32_t final, char* result, uint32_t result_len) {
   return native_context_stack::current()->blake2_f(rounds,
      sysio::vm::span<const char>{state, state_len},
      sysio::vm::span<const char>{msg, msg_len},
      sysio::vm::span<const char>{t0_offset, t0_len},
      sysio::vm::span<const char>{t1_offset, t1_len},
      final,
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
void sha3(const char* data, uint32_t data_len, char* hash, uint32_t hash_len, int32_t keccak) {
   native_context_stack::current()->sha3(
      sysio::vm::span<const char>{data, data_len},
      sysio::vm::span<char>{hash, hash_len},
      keccak);
}

INTRINSIC_EXPORT
int32_t k1_recover(const char* sig, uint32_t sig_len, const char* dig, uint32_t dig_len, char* pub, uint32_t pub_len) {
   return native_context_stack::current()->k1_recover(
      sysio::vm::span<const char>{sig, sig_len},
      sysio::vm::span<const char>{dig, dig_len},
      sysio::vm::span<char>{pub, pub_len});
}

INTRINSIC_EXPORT
int32_t blake2b_256(const char* data, uint32_t data_len, char* hash, uint32_t hash_len) {
   return native_context_stack::current()->blake2b_256(
      sysio::vm::span<const char>{data, data_len},
      sysio::vm::span<char>{hash, hash_len});
}

// ============================================================================
// BLS primitives
// ============================================================================

INTRINSIC_EXPORT
int32_t bls_g1_add(const char* op1, uint32_t op1_len, const char* op2, uint32_t op2_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_g1_add(
      sysio::vm::span<const char>{op1, op1_len},
      sysio::vm::span<const char>{op2, op2_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_g2_add(const char* op1, uint32_t op1_len, const char* op2, uint32_t op2_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_g2_add(
      sysio::vm::span<const char>{op1, op1_len},
      sysio::vm::span<const char>{op2, op2_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_g1_weighted_sum(const char* points, uint32_t points_len, const char* scalars, uint32_t scalars_len, uint32_t n, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_g1_weighted_sum(
      sysio::vm::span<const char>{points, points_len},
      sysio::vm::span<const char>{scalars, scalars_len},
      n,
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_g2_weighted_sum(const char* points, uint32_t points_len, const char* scalars, uint32_t scalars_len, uint32_t n, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_g2_weighted_sum(
      sysio::vm::span<const char>{points, points_len},
      sysio::vm::span<const char>{scalars, scalars_len},
      n,
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_pairing(const char* g1_points, uint32_t g1_len, const char* g2_points, uint32_t g2_len, uint32_t n, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_pairing(
      sysio::vm::span<const char>{g1_points, g1_len},
      sysio::vm::span<const char>{g2_points, g2_len},
      n,
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_g1_map(const char* e, uint32_t e_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_g1_map(
      sysio::vm::span<const char>{e, e_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_g2_map(const char* e, uint32_t e_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_g2_map(
      sysio::vm::span<const char>{e, e_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_fp_mod(const char* s, uint32_t s_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_fp_mod(
      sysio::vm::span<const char>{s, s_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_fp_mul(const char* op1, uint32_t op1_len, const char* op2, uint32_t op2_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_fp_mul(
      sysio::vm::span<const char>{op1, op1_len},
      sysio::vm::span<const char>{op2, op2_len},
      sysio::vm::span<char>{result, result_len});
}

INTRINSIC_EXPORT
int32_t bls_fp_exp(const char* base, uint32_t base_len, const char* exp_val, uint32_t exp_len, char* result, uint32_t result_len) {
   return native_context_stack::current()->bls_fp_exp(
      sysio::vm::span<const char>{base, base_len},
      sysio::vm::span<const char>{exp_val, exp_len},
      sysio::vm::span<char>{result, result_len});
}
