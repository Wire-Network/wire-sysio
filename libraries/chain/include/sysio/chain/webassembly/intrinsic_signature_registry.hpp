#pragma once
//
// Consensus-critical signature registry for host intrinsics.
//
// Each interface method that the aligned_span/aligned_ptr -> span cleanup
// will touch is pinned below via a static_assert on
// decltype(&interface::fn). Any accidental change to a signature during
// the cleanup -- a constness flip, a aligned_span <-> span swap, a param
// reordering, a aligned_ptr<T, Align> alignment change -- fires a build
// error here instead of silently drifting into production.
//
// The compiler diagnostic names the intrinsic; the next step when you see
// one fire is:
//   1. Confirm the signature change in interface.hpp is intentional.
//   2. Update this registry to match.
//   3. Bump the host-ABI version / protocol feature if the wasm-visible
//      shape of the intrinsic changed in a contract-observable way.
//   4. Regenerate reference data (deep-mind log, snapshots, consensus
//      blockchain) per CLAUDE.md's "Regenerating Test Reference Data".
//   5. Update wire-cdt-side declarations so contracts built against the
//      new CDT use the new signature.
//
// This file is intentionally redundant with interface.hpp. Point of the
// redundancy is that redundancy is exactly what catches silent drift.
//
// Coverage: every interface method that currently uses aligned_ptr or
// aligned_span is pinned (the cleanup's blast radius). The span<>-using
// intrinsics that stay span<> through the cleanup are also pinned so a
// later "normalize everything to char*+size_t" refactor is caught. The
// softfloat _sysio_f32/f64_* arithmetic intrinsics (plain float/double
// args only, no proxy types) are intentionally NOT pinned here -- the
// cleanup does not touch them and their signatures are boring.
//
// Include once per build (from libraries/chain/webassembly/runtimes/sys-vm.cpp)
// so the asserts compile exactly once rather than per-TU.

#include <sysio/chain/webassembly/interface.hpp>
#include <type_traits>

namespace sysio::chain::webassembly {

// __VA_ARGS__ captures the full member-pointer type (commas within the
// signature would otherwise split across macro args).
#define SYS_PIN_INTRINSIC( NAME, ... )                                  \
   static_assert( ::std::is_same_v<decltype(&interface::NAME),          \
                                   __VA_ARGS__>,                        \
                  #NAME " host-intrinsic signature drifted -- update "  \
                  "intrinsic_signature_registry.hpp, bump protocol "    \
                  "feature, regen reference data, sync wire-cdt" )

// =============================================================================
// Context-free data
// =============================================================================
SYS_PIN_INTRINSIC( get_context_free_data,
                   int32_t (interface::*)(uint32_t, span<char>) const );

// =============================================================================
// Privileged
// =============================================================================
SYS_PIN_INTRINSIC( is_feature_active,
                   int32_t (interface::*)(int64_t) const );
SYS_PIN_INTRINSIC( activate_feature,
                   void (interface::*)(int64_t) const );
SYS_PIN_INTRINSIC( preactivate_feature,
                   void (interface::*)(aligned_ptr<const digest_type>) );
SYS_PIN_INTRINSIC( set_resource_limits,
                   void (interface::*)(account_name, int64_t, int64_t, int64_t) );
SYS_PIN_INTRINSIC( get_resource_limits,
                   void (interface::*)(account_name,
                                       aligned_ptr<int64_t, 8>,
                                       aligned_ptr<int64_t, 8>,
                                       aligned_ptr<int64_t, 8>) const );
SYS_PIN_INTRINSIC( get_wasm_parameters_packed,
                   uint32_t (interface::*)(span<char>, uint32_t) const );
SYS_PIN_INTRINSIC( set_wasm_parameters_packed,
                   void (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( set_proposed_producers,
                   int64_t (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( set_proposed_producers_ex,
                   int64_t (interface::*)(uint64_t, span<const char>) );
SYS_PIN_INTRINSIC( set_finalizers,
                   void (interface::*)(uint64_t, span<const char>) );
SYS_PIN_INTRINSIC( get_blockchain_parameters_packed,
                   uint32_t (interface::*)(span<char>) const );
SYS_PIN_INTRINSIC( set_blockchain_parameters_packed,
                   void (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( get_parameters_packed,
                   uint32_t (interface::*)(span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( set_parameters_packed,
                   void (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( is_privileged,
                   bool (interface::*)(account_name) const );
SYS_PIN_INTRINSIC( set_privileged,
                   void (interface::*)(account_name, bool) );

// =============================================================================
// Producers
// =============================================================================
SYS_PIN_INTRINSIC( get_active_producers,
                   int32_t (interface::*)(aligned_span<account_name>) const );

// =============================================================================
// Crypto -- hashes + signature recovery
// =============================================================================
SYS_PIN_INTRINSIC( assert_recover_key,
                   void (interface::*)(aligned_ptr<const fc::sha256>,
                                       span<const char>,
                                       span<const char>) const );
SYS_PIN_INTRINSIC( recover_key,
                   int32_t (interface::*)(aligned_ptr<const fc::sha256>,
                                          span<const char>,
                                          span<char>) const );
SYS_PIN_INTRINSIC( assert_sha256,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<const fc::sha256>) const );
SYS_PIN_INTRINSIC( assert_sha1,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<const fc::sha1>) const );
SYS_PIN_INTRINSIC( assert_sha512,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<const fc::sha512>) const );
SYS_PIN_INTRINSIC( assert_ripemd160,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<const fc::ripemd160>) const );
SYS_PIN_INTRINSIC( sha256,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<fc::sha256>) const );
SYS_PIN_INTRINSIC( sha1,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<fc::sha1>) const );
SYS_PIN_INTRINSIC( sha512,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<fc::sha512>) const );
SYS_PIN_INTRINSIC( ripemd160,
                   void (interface::*)(span<const char>,
                                       aligned_ptr<fc::ripemd160>) const );

// =============================================================================
// Permission + authorization
// =============================================================================
SYS_PIN_INTRINSIC( check_transaction_authorization,
                   bool (interface::*)(span<const char>,
                                       span<const char>,
                                       span<const char>) const );
SYS_PIN_INTRINSIC( check_permission_authorization,
                   bool (interface::*)(account_name, permission_name,
                                       span<const char>,
                                       span<const char>,
                                       uint64_t) const );
SYS_PIN_INTRINSIC( get_permission_lower_bound,
                   int32_t (interface::*)(account_name, permission_name, span<char>) );
SYS_PIN_INTRINSIC( require_auth,
                   void (interface::*)(account_name) const );
SYS_PIN_INTRINSIC( require_auth2,
                   void (interface::*)(account_name, permission_name) const );
SYS_PIN_INTRINSIC( has_auth,
                   bool (interface::*)(account_name) const );
SYS_PIN_INTRINSIC( require_recipient,
                   void (interface::*)(account_name) );
SYS_PIN_INTRINSIC( is_account,
                   bool (interface::*)(account_name) const );

// =============================================================================
// System
// =============================================================================
SYS_PIN_INTRINSIC( current_time,
                   uint64_t (interface::*)() const );
SYS_PIN_INTRINSIC( publication_time,
                   uint64_t (interface::*)() const );
SYS_PIN_INTRINSIC( is_feature_activated,
                   bool (interface::*)(aligned_ptr<const digest_type>) const );
SYS_PIN_INTRINSIC( get_sender,
                   name (interface::*)() const );
SYS_PIN_INTRINSIC( get_ram_usage,
                   int64_t (interface::*)(account_name) const );
SYS_PIN_INTRINSIC( abort,
                   void (interface::*)() const );
SYS_PIN_INTRINSIC( sysio_assert,
                   void (interface::*)(bool, null_terminated_ptr) const );
SYS_PIN_INTRINSIC( sysio_assert_message,
                   void (interface::*)(bool, span<const char>) const );
SYS_PIN_INTRINSIC( sysio_assert_code,
                   void (interface::*)(bool, uint64_t) const );
SYS_PIN_INTRINSIC( sysio_exit,
                   void (interface::*)(int32_t) const );

// =============================================================================
// Action
// =============================================================================
SYS_PIN_INTRINSIC( read_action_data,
                   int32_t (interface::*)(span<char>) const );
SYS_PIN_INTRINSIC( action_data_size,
                   int32_t (interface::*)() const );
SYS_PIN_INTRINSIC( set_action_return_value,
                   void (interface::*)(span<const char>) );

// =============================================================================
// Console (prints / printhex -- null_terminated_ptr + aligned_span readers)
// =============================================================================
SYS_PIN_INTRINSIC( prints,
                   void (interface::*)(null_terminated_ptr) );
SYS_PIN_INTRINSIC( prints_l,
                   void (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( printi,
                   void (interface::*)(int64_t) );
SYS_PIN_INTRINSIC( printui,
                   void (interface::*)(uint64_t) );
SYS_PIN_INTRINSIC( printi128,
                   void (interface::*)(aligned_ptr<const __int128>) );
SYS_PIN_INTRINSIC( printui128,
                   void (interface::*)(aligned_ptr<const unsigned __int128>) );
SYS_PIN_INTRINSIC( printsf,
                   void (interface::*)(float32_t) );
SYS_PIN_INTRINSIC( printdf,
                   void (interface::*)(float64_t) );
SYS_PIN_INTRINSIC( printqf,
                   void (interface::*)(aligned_ptr<const float128_t>) );
SYS_PIN_INTRINSIC( printn,
                   void (interface::*)(name) );
SYS_PIN_INTRINSIC( printhex,
                   void (interface::*)(span<const char>) );

// =============================================================================
// KV database (all 22 -- the biggest cleanup target by signature count)
// =============================================================================
SYS_PIN_INTRINSIC( kv_set,
                   int64_t (interface::*)(uint32_t, uint64_t,
                                          span<const char>,
                                          span<const char>) );
SYS_PIN_INTRINSIC( kv_get,
                   int32_t (interface::*)(uint32_t, uint64_t,
                                          span<const char>,
                                          span<char>) );
SYS_PIN_INTRINSIC( kv_erase,
                   int64_t (interface::*)(uint32_t, span<const char>) );
SYS_PIN_INTRINSIC( kv_contains,
                   int32_t (interface::*)(uint32_t, uint64_t,
                                          span<const char>) );
SYS_PIN_INTRINSIC( kv_it_create,
                   uint32_t (interface::*)(uint32_t, uint64_t,
                                           span<const char>) );
SYS_PIN_INTRINSIC( kv_it_destroy,
                   void (interface::*)(uint32_t) );
SYS_PIN_INTRINSIC( kv_it_status,
                   int32_t (interface::*)(uint32_t) );
SYS_PIN_INTRINSIC( kv_it_next,
                   int32_t (interface::*)(uint32_t) );
SYS_PIN_INTRINSIC( kv_it_prev,
                   int32_t (interface::*)(uint32_t) );
SYS_PIN_INTRINSIC( kv_it_lower_bound,
                   int32_t (interface::*)(uint32_t, span<const char>) );
SYS_PIN_INTRINSIC( kv_it_key,
                   int32_t (interface::*)(uint32_t, uint32_t,
                                          span<char>,
                                          aligned_ptr<uint32_t>) );
SYS_PIN_INTRINSIC( kv_it_value,
                   int32_t (interface::*)(uint32_t, uint32_t,
                                          span<char>,
                                          aligned_ptr<uint32_t>) );
SYS_PIN_INTRINSIC( kv_idx_store,
                   void (interface::*)(uint64_t, uint32_t,
                                       span<const char>,
                                       span<const char>) );
SYS_PIN_INTRINSIC( kv_idx_remove,
                   void (interface::*)(uint32_t,
                                       span<const char>,
                                       span<const char>) );
SYS_PIN_INTRINSIC( kv_idx_update,
                   void (interface::*)(uint64_t, uint32_t,
                                       span<const char>,
                                       span<const char>,
                                       span<const char>) );
SYS_PIN_INTRINSIC( kv_idx_find_secondary,
                   int32_t (interface::*)(uint64_t, uint32_t,
                                          span<const char>) );
SYS_PIN_INTRINSIC( kv_idx_lower_bound,
                   int32_t (interface::*)(uint64_t, uint32_t,
                                          span<const char>) );
SYS_PIN_INTRINSIC( kv_idx_next,
                   int32_t (interface::*)(uint32_t) );
SYS_PIN_INTRINSIC( kv_idx_prev,
                   int32_t (interface::*)(uint32_t) );
SYS_PIN_INTRINSIC( kv_idx_key,
                   int32_t (interface::*)(uint32_t, uint32_t,
                                          span<char>,
                                          aligned_ptr<uint32_t>) );
SYS_PIN_INTRINSIC( kv_idx_primary_key,
                   int32_t (interface::*)(uint32_t, uint32_t,
                                          span<char>,
                                          aligned_ptr<uint32_t>) );
SYS_PIN_INTRINSIC( kv_idx_destroy,
                   void (interface::*)(uint32_t) );

// =============================================================================
// Memory (struct-by-value params; pinned here because the argument unpack
// logic is consensus-critical)
// =============================================================================
SYS_PIN_INTRINSIC( memcpy,
                   void* (interface::*)(memcpy_params) const );
SYS_PIN_INTRINSIC( memmove,
                   void* (interface::*)(memcpy_params) const );
SYS_PIN_INTRINSIC( memcmp,
                   int32_t (interface::*)(memcmp_params) const );
SYS_PIN_INTRINSIC( memset,
                   void* (interface::*)(memset_params) const );

// =============================================================================
// Transaction
// =============================================================================
SYS_PIN_INTRINSIC( send_inline,
                   void (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( send_context_free_inline,
                   void (interface::*)(span<const char>) );
SYS_PIN_INTRINSIC( read_transaction,
                   int32_t (interface::*)(span<char>) const );
SYS_PIN_INTRINSIC( transaction_size,
                   int32_t (interface::*)() const );
SYS_PIN_INTRINSIC( expiration,
                   int32_t (interface::*)() const );
SYS_PIN_INTRINSIC( tapos_block_num,
                   int32_t (interface::*)() const );
SYS_PIN_INTRINSIC( tapos_block_prefix,
                   int32_t (interface::*)() const );
SYS_PIN_INTRINSIC( get_action,
                   int32_t (interface::*)(uint32_t, uint32_t, span<char>) const );

// =============================================================================
// alt_bn128 / mod_exp / blake2 / sha3 / k1_recover (post-launch span API)
// =============================================================================
SYS_PIN_INTRINSIC( alt_bn128_add,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( alt_bn128_mul,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( alt_bn128_pair,
                   int32_t (interface::*)(span<const char>) const );
SYS_PIN_INTRINSIC( mod_exp,
                   int32_t (interface::*)(span<const char>, span<const char>,
                                          span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( blake2_f,
                   int32_t (interface::*)(uint32_t,
                                          span<const char>, span<const char>,
                                          span<const char>, span<const char>,
                                          int32_t, span<char>) const );
SYS_PIN_INTRINSIC( blake2b_256,
                   int32_t (interface::*)(span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( sha3,
                   void (interface::*)(span<const char>, span<char>, int32_t) const );
SYS_PIN_INTRINSIC( k1_recover,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );

// =============================================================================
// BLS12-381
// =============================================================================
SYS_PIN_INTRINSIC( bls_g1_add,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( bls_g2_add,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( bls_g1_weighted_sum,
                   int32_t (interface::*)(span<const char>, span<const char>,
                                          const uint32_t, span<char>) const );
SYS_PIN_INTRINSIC( bls_g2_weighted_sum,
                   int32_t (interface::*)(span<const char>, span<const char>,
                                          const uint32_t, span<char>) const );
SYS_PIN_INTRINSIC( bls_pairing,
                   int32_t (interface::*)(span<const char>, span<const char>,
                                          const uint32_t, span<char>) const );
SYS_PIN_INTRINSIC( bls_g1_map,
                   int32_t (interface::*)(span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( bls_g2_map,
                   int32_t (interface::*)(span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( bls_fp_mod,
                   int32_t (interface::*)(span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( bls_fp_mul,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );
SYS_PIN_INTRINSIC( bls_fp_exp,
                   int32_t (interface::*)(span<const char>, span<const char>, span<char>) const );

// =============================================================================
// compiler_builtins: 128-bit integer ops (all aligned_ptr<[u]int128_t, 16>)
// =============================================================================
SYS_PIN_INTRINSIC( __ashlti3,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t, uint32_t) const );
SYS_PIN_INTRINSIC( __ashrti3,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t, uint32_t) const );
SYS_PIN_INTRINSIC( __lshlti3,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t, uint32_t) const );
SYS_PIN_INTRINSIC( __lshrti3,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t, uint32_t) const );
SYS_PIN_INTRINSIC( __divti3,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __udivti3,
                   void (interface::*)(aligned_ptr<uint128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __multi3,
                   void (interface::*)(aligned_ptr<uint128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __modti3,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __umodti3,
                   void (interface::*)(aligned_ptr<uint128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );

// =============================================================================
// compiler_builtins: float128 arithmetic + conversion (aligned_ptr<float128_t, 16>
// for output, uint64_t-pair for input)
// =============================================================================
SYS_PIN_INTRINSIC( __addtf3,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __subtf3,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __multf3,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __divtf3,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t, uint64_t,
                                       uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __negtf2,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __extendsftf2,
                   void (interface::*)(aligned_ptr<float128_t>, float) const );
SYS_PIN_INTRINSIC( __extenddftf2,
                   void (interface::*)(aligned_ptr<float128_t>, double) const );
SYS_PIN_INTRINSIC( __trunctfdf2,
                   double (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __trunctfsf2,
                   float (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixtfsi,
                   int32_t (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixtfdi,
                   int64_t (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixtfti,
                   void (interface::*)(aligned_ptr<int128_t>, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixunstfsi,
                   uint32_t (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixunstfdi,
                   uint64_t (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixunstfti,
                   void (interface::*)(aligned_ptr<uint128_t>, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __fixsfti,
                   void (interface::*)(aligned_ptr<int128_t>, float) const );
SYS_PIN_INTRINSIC( __fixdfti,
                   void (interface::*)(aligned_ptr<int128_t>, double) const );
SYS_PIN_INTRINSIC( __fixunssfti,
                   void (interface::*)(aligned_ptr<uint128_t>, float) const );
SYS_PIN_INTRINSIC( __fixunsdfti,
                   void (interface::*)(aligned_ptr<uint128_t>, double) const );
SYS_PIN_INTRINSIC( __floatsidf,
                   double (interface::*)(int32_t) const );
SYS_PIN_INTRINSIC( __floatsitf,
                   void (interface::*)(aligned_ptr<float128_t>, int32_t) const );
SYS_PIN_INTRINSIC( __floatditf,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t) const );
SYS_PIN_INTRINSIC( __floatunsitf,
                   void (interface::*)(aligned_ptr<float128_t>, uint32_t) const );
SYS_PIN_INTRINSIC( __floatunditf,
                   void (interface::*)(aligned_ptr<float128_t>, uint64_t) const );
SYS_PIN_INTRINSIC( __floattidf,
                   double (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __floatuntidf,
                   double (interface::*)(uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __cmptf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __eqtf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __netf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __getf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __gttf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __letf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __lttf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );
SYS_PIN_INTRINSIC( __unordtf2,
                   int32_t (interface::*)(uint64_t, uint64_t, uint64_t, uint64_t) const );

#undef SYS_PIN_INTRINSIC

} // namespace sysio::chain::webassembly
