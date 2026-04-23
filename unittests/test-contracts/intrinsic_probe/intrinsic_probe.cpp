// Adversarial probe of the non-kv host intrinsics.
// Companion to unittests/test-contracts/kv_intrinsic_probe/ which covers the
// 22 kv_* intrinsics. This contract exercises the remaining host ABI with
// inputs that CDT's wrappers would never emit -- zero-length spans,
// wasm-boundary-crossing pointers, misaligned pointers for the
// legacy_ptr<fc::sha*>-style aligned-proxy intrinsics, pointer-aliased
// arguments, null pointer + non-zero size, and the specific edge values
// that the 128-bit compiler_builtins and softfloat entry points are
// expected to handle.
//
// Using extern "C" + __attribute__((sysio_wasm_import)) for every intrinsic
// at the call site (not via <sysio/crypto.hpp>, <sysio/privileged.hpp>, etc.)
// makes the ABI being exercised explicit at the point of use, and lets a
// reviewer see the adversarial input directly without tracing through CDT
// wrappers.
//
// One [[sysio::action]] per probe. Accepted-behavior actions return normally;
// rejection probes call the host intrinsic with adversarial input and rely on
// the host to throw a specific exception type that the Boost driver pins via
// BOOST_CHECK_THROW.

#include <sysio/sysio.hpp>
#include <sysio/transaction.hpp>  // raw::{read_transaction, get_action, get_context_free_data}
#include <cstring>
#include <cstdint>

// -----------------------------------------------------------------------------
// Raw host intrinsic declarations. Listed here instead of via <sysio/...> so
// every adversarial input is explicit about the ABI it is exercising.
// -----------------------------------------------------------------------------
extern "C" {

// --- Hash intrinsics (legacy_span<const char> data, legacy_ptr<[const] fc::sha*> hash) ---
__attribute__((sysio_wasm_import))
void sha256( const char* data, uint32_t length, void* hash );
__attribute__((sysio_wasm_import))
void sha1( const char* data, uint32_t length, void* hash );
__attribute__((sysio_wasm_import))
void sha512( const char* data, uint32_t length, void* hash );
__attribute__((sysio_wasm_import))
void ripemd160( const char* data, uint32_t length, void* hash );

__attribute__((sysio_wasm_import))
void assert_sha256( const char* data, uint32_t length, const void* hash );
__attribute__((sysio_wasm_import))
void assert_sha1( const char* data, uint32_t length, const void* hash );
__attribute__((sysio_wasm_import))
void assert_sha512( const char* data, uint32_t length, const void* hash );
__attribute__((sysio_wasm_import))
void assert_ripemd160( const char* data, uint32_t length, const void* hash );

// --- Signature recovery (legacy_ptr<const fc::sha256> digest, legacy_span<[const] char>) ---
__attribute__((sysio_wasm_import))
int32_t recover_key( const void* digest, const char* sig, uint32_t siglen,
                     char* pub, uint32_t publen );
__attribute__((sysio_wasm_import))
void    assert_recover_key( const void* digest, const char* sig, uint32_t siglen,
                            const char* pub, uint32_t publen );

// --- Privileged intrinsics registered with privileged_check precondition
//     (libraries/chain/webassembly/runtimes/sys-vm.cpp). Calling from a
//     non-privileged account throws `unaccessible_api` BEFORE the host body
//     runs; the body itself may have further guards (digest validity,
//     read-only tx, range checks on resource limits, etc.). ---
__attribute__((sysio_wasm_import))
void preactivate_feature( const void* feature_digest );  // legacy_ptr<const digest_type>

// --- P2 -- resource/auth/producer intrinsics ---
//  get_resource_limits: 3 x legacy_ptr<int64_t, 8> out-params
//  set_resource_limits: priv-gated, plain scalar args
//  check_transaction_authorization: legacy_span<const char> x 3
//  get_active_producers: legacy_span<account_name> out (uint64_t[])
//  set_proposed_producers[_ex]: priv-gated, legacy_span<const char>
//  get/set_blockchain_parameters_packed: legacy_span<[const] char>
__attribute__((sysio_wasm_import))
void get_resource_limits( uint64_t account, void* ram_bytes,
                          void* net_weight, void* cpu_weight );
__attribute__((sysio_wasm_import))
void set_resource_limits( uint64_t account, int64_t ram_bytes,
                          int64_t net_weight, int64_t cpu_weight );

__attribute__((sysio_wasm_import))
int32_t check_transaction_authorization( const char* trx_data,  uint32_t trx_len,
                                         const char* pubs_data, uint32_t pubs_len,
                                         const char* perms_data, uint32_t perms_len );

__attribute__((sysio_wasm_import))
int32_t get_active_producers( void* producers, uint32_t datalen );
__attribute__((sysio_wasm_import))
int64_t set_proposed_producers( const char* data, uint32_t datalen );

__attribute__((sysio_wasm_import))
uint32_t get_blockchain_parameters_packed( char* data, uint32_t datalen );
__attribute__((sysio_wasm_import))
void     set_blockchain_parameters_packed( const char* data, uint32_t datalen );

// set_action_return_value lives only in capi/sysio/action.h (no
// sysio::internal_use_do_not_use wrapper in contracts/sysio/action.hpp),
// so it is declared here directly. Signature matches the capi header.
__attribute__((sysio_wasm_import))
void set_action_return_value( void* return_value, uint32_t size );

// --- 128-bit integer compiler builtins (legacy_ptr<[u]int128_t> ret) ---
// Declaring the output as void* (rather than __int128*/&) lets the unaligned
// probes pass an intentionally misaligned address without triggering C++
// alignment UB on the caller side; the WASM import ABI is untyped pointers
// and the host's argument_proxy<__int128_t*, 16> handles alignment via
// memcpy on entry / exit.
__attribute__((sysio_wasm_import))
void __multi3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __divti3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __udivti3( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __modti3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __umodti3( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __ashlti3( void* ret, uint64_t low, uint64_t high, uint32_t shift );
__attribute__((sysio_wasm_import))
void __ashrti3( void* ret, uint64_t low, uint64_t high, uint32_t shift );
__attribute__((sysio_wasm_import))
void __lshlti3( void* ret, uint64_t low, uint64_t high, uint32_t shift );
__attribute__((sysio_wasm_import))
void __lshrti3( void* ret, uint64_t low, uint64_t high, uint32_t shift );

// --- float128 (quad) compiler builtins (legacy_ptr<float128_t> ret) ---
__attribute__((sysio_wasm_import))
void __addtf3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __subtf3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __multf3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __divtf3 ( void* ret, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
void __fixtfti( void* ret, uint64_t la, uint64_t ha );
// __cmptf2 has no legacy_ptr -- uint64_t quad-pairs in, int32_t out; no
// alignment concerns. Used by the probes below to verify float128 results
// without having to hand-compute the exact destination bit pattern.
__attribute__((sysio_wasm_import))
int32_t __cmptf2( uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );
__attribute__((sysio_wasm_import))
int32_t __unordtf2( uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb );

} // extern "C"

// action_data_size / read_action_data are the host intrinsics that feed the
// recover_key probes their (digest, sig, pub) triple from the Boost driver.
// CDT's <sysio/action.hpp> already declares them under sysio::internal_use_*
// with the same sysio_wasm_import attribute, so we reuse those wrappers
// directly (re-declaring them here collides with the CDT-provided decls).

using namespace sysio;

// CDT buries several host intrinsics (raw::prints / raw::sysio_assert / raw::send_inline /
// raw::get_action / raw::read_transaction / set_action_return_value / ...) in
// `sysio::internal_use_do_not_use`. Intentional: CDT wants contract authors
// to go through the C++ wrappers. The probe contract IS the "do not use"
// path -- we want the raw ABI without any CDT-side validation between us and
// the host -- so alias it once and call through.
namespace raw = ::sysio::internal_use_do_not_use;

namespace {
// -----------------------------------------------------------------------------
// Hash digest sizes. Named constants per CLAUDE.md "no magic literals".
// -----------------------------------------------------------------------------
constexpr uint32_t SHA1_SIZE   = 20;
constexpr uint32_t SHA256_SIZE = 32;
constexpr uint32_t SHA512_SIZE = 64;
constexpr uint32_t RIPE_SIZE   = 20;

// -----------------------------------------------------------------------------
// FIPS / SYSIO-canonical hash test vectors. Identical to the vectors in
// unittests/test-contracts/test_api/test_crypto.cpp so these probes can be
// cross-checked against that suite.
// -----------------------------------------------------------------------------
constexpr char TEST_ABC[]   = "abc";
constexpr uint32_t TEST_ABC_LEN = 3;  // excludes terminator; host sees only these bytes

constexpr unsigned char ABC_SHA1[SHA1_SIZE] = {
   0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
   0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
};
constexpr unsigned char ABC_SHA256[SHA256_SIZE] = {
   0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
   0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
   0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
   0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
};
constexpr unsigned char ABC_SHA512[SHA512_SIZE] = {
   0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba,
   0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
   0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2,
   0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
   0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8,
   0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
   0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e,
   0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f
};
constexpr unsigned char ABC_RIPE[RIPE_SIZE] = {
   0x8e, 0xb2, 0x08, 0xf7, 0xe0, 0x5d, 0x98, 0x7a, 0x9b, 0x04,
   0x4a, 0x8e, 0x98, 0xc6, 0xb0, 0x87, 0xf1, 0x5a, 0x0b, 0xfc
};

constexpr unsigned char EMPTY_SHA1[SHA1_SIZE] = {
   0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
   0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09
};
constexpr unsigned char EMPTY_SHA256[SHA256_SIZE] = {
   0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
   0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
   0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
   0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
};
constexpr unsigned char EMPTY_SHA512[SHA512_SIZE] = {
   0xcf, 0x83, 0xe1, 0x35, 0x7e, 0xef, 0xb8, 0xbd,
   0xf1, 0x54, 0x28, 0x50, 0xd6, 0x6d, 0x80, 0x07,
   0xd6, 0x20, 0xe4, 0x05, 0x0b, 0x57, 0x15, 0xdc,
   0x83, 0xf4, 0xa9, 0x21, 0xd3, 0x6c, 0xe9, 0xce,
   0x47, 0xd0, 0xd1, 0x3c, 0x5d, 0x85, 0xf2, 0xb0,
   0xff, 0x83, 0x18, 0xd2, 0x87, 0x7e, 0xec, 0x2f,
   0x63, 0xb9, 0x31, 0xbd, 0x47, 0x41, 0x7a, 0x81,
   0xa5, 0x38, 0x32, 0x7a, 0xf9, 0x27, 0xda, 0x3e
};
constexpr unsigned char EMPTY_RIPE[RIPE_SIZE] = {
   0x9c, 0x11, 0x85, 0xa5, 0xc5, 0xe9, 0xfc, 0x54, 0x61, 0x28,
   0x08, 0x97, 0x7e, 0xe8, 0xf5, 0x48, 0xb2, 0x25, 0x8d, 0x31
};

// -----------------------------------------------------------------------------
// Padding used by the unaligned-pointer probes. Size picks up +3 past any
// plausible natural alignment so that offsetting into the buffer lands the
// hash pointer on an address that no legacy_ptr alignment requirement will
// accept without the argument_proxy copy path engaging.
// -----------------------------------------------------------------------------
constexpr uint32_t UNALIGNED_OFFSET = 3;
constexpr uint32_t BIG_INPUT_LEN    = 1024;

// -----------------------------------------------------------------------------
// Adversarial "wrong" hash used by assert_* rejection probes. All zeros is
// guaranteed not to match any non-empty SHA-family or RIPEMD output.
// -----------------------------------------------------------------------------
constexpr unsigned char ZERO_HASH64[SHA512_SIZE] = {};

// -----------------------------------------------------------------------------
// recover_key action-data layout. Mirrors the shape used by
// test_api/test_crypto.cpp's sig_hash_key_header so the Boost driver can
// build the blob with fc::private_key / fc::signature packing.
// -----------------------------------------------------------------------------
struct sig_hash_key_header {
   unsigned char hash[SHA256_SIZE];  // 32-byte digest
   uint32_t      pk_len;             // bytes of packed public key following sig
   uint32_t      sig_len;            // bytes of packed signature following header
};
constexpr uint32_t RECOVER_BUF_CAPACITY = 512;
constexpr uint32_t MAX_RECOVERED_PUB    = 128;

// -----------------------------------------------------------------------------
// float128 (IEEE 754 binary128) bit patterns, split into (low, high) uint64_t
// pairs matching the host ABI. Computed by hand; validated by Python and by
// round-tripping through __cmptf2 in the golden probes.
//
// Layout: 1 sign | 15 exponent | 112 fraction  (bias = 16383)
//   1.0 = exp=16383=0x3FFF, frac=0     -> 0x3FFF_0000_0000_0000 << 64
//   2.0 = exp=16384=0x4000, frac=0     -> 0x4000_0000_0000_0000 << 64
//   3.0 = 1.5*2^1, exp=16384, frac bit[111]=1 -> 0x4000_8000_0000_0000 << 64
//   6.0 = 1.5*2^2, exp=16385, frac bit[111]=1 -> 0x4001_8000_0000_0000 << 64
//   +Inf = exp=0x7FFF, frac=0          -> 0x7FFF_0000_0000_0000 << 64
//   qNaN = exp=0x7FFF, frac bit[111]=1 -> 0x7FFF_8000_0000_0000 << 64
// -----------------------------------------------------------------------------
constexpr uint64_t FP128_ZERO_LO = 0x0000000000000000ULL;
constexpr uint64_t FP128_ZERO_HI = 0x0000000000000000ULL;
constexpr uint64_t FP128_ONE_LO  = 0x0000000000000000ULL;
constexpr uint64_t FP128_ONE_HI  = 0x3FFF000000000000ULL;
constexpr uint64_t FP128_TWO_LO  = 0x0000000000000000ULL;
constexpr uint64_t FP128_TWO_HI  = 0x4000000000000000ULL;
constexpr uint64_t FP128_THREE_LO = 0x0000000000000000ULL;
constexpr uint64_t FP128_THREE_HI = 0x4000800000000000ULL;
constexpr uint64_t FP128_SIX_LO  = 0x0000000000000000ULL;
constexpr uint64_t FP128_SIX_HI  = 0x4001800000000000ULL;
constexpr uint64_t FP128_INF_LO  = 0x0000000000000000ULL;
constexpr uint64_t FP128_INF_HI  = 0x7FFF000000000000ULL;
constexpr uint64_t FP128_NAN_LO  = 0x0000000000000000ULL;
constexpr uint64_t FP128_NAN_HI  = 0x7FFF800000000000ULL;

// Large finite float128 that comfortably exceeds INT128_MAX when truncated:
// 2^127 = exp=16383+127=0x407E, frac=0 -> 0x407E_0000_0000_0000 << 64.
constexpr uint64_t FP128_LARGE_LO = 0x0000000000000000ULL;
constexpr uint64_t FP128_LARGE_HI = 0x407E000000000000ULL;

// -----------------------------------------------------------------------------
// Bit-pattern constants for the 128-bit integer probes. Verification uses
// read-back memcpy into uint64_t pairs so we never rely on the compiler
// emitting a second call into the same intrinsic we are testing.
// -----------------------------------------------------------------------------
constexpr uint64_t I128_ZERO      = 0x0000000000000000ULL;
constexpr uint64_t I128_ONE_HI    = 0x0000000000000000ULL;
constexpr uint64_t U64_MAX        = 0xFFFFFFFFFFFFFFFFULL;

} // namespace

class [[sysio::contract("intrinsic_probe")]] intrinsic_probe : public contract {
public:
   using contract::contract;

   // =============================================================================
   // P1 -- sha256
   // =============================================================================

   // Golden. Verifies the host produces the FIPS-canonical SHA-256 of "abc".
   [[sysio::action]]
   void sha2ok() {
      unsigned char out[SHA256_SIZE] = {};
      sha256( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_SHA256, SHA256_SIZE) == 0, "sha256('abc') mismatch" );
   }

   // Zero-length input. legacy_span<const char> with size 0 must be accepted
   // and must produce the FIPS empty-string hash, regardless of whether data
   // is nullptr or a valid pointer.
   [[sysio::action]]
   void sha2em() {
      unsigned char out[SHA256_SIZE] = {};
      sha256( nullptr, 0, out );
      check( std::memcmp(out, EMPTY_SHA256, SHA256_SIZE) == 0,
             "sha256(empty) mismatch when data=nullptr" );

      // Non-null data pointer with length=0 must be treated identically.
      char placeholder = 'x';
      std::memset(out, 0, SHA256_SIZE);
      sha256( &placeholder, 0, out );
      check( std::memcmp(out, EMPTY_SHA256, SHA256_SIZE) == 0,
             "sha256(empty) mismatch when data=&x but length=0" );
   }

   // Large input (10KB of zeros). Tests that the host accepts a span sized
   // above any natural stack-buffer threshold without path changes.
   [[sysio::action]]
   void sha2big() {
      char buf[BIG_INPUT_LEN] = {};
      unsigned char out[SHA256_SIZE] = {};
      sha256( buf, BIG_INPUT_LEN, out );
      // SHA-256 over 10K of zeros is deterministic; we don't hardcode the
      // exact bytes (golden vectors above cover correctness) but pin that
      // the output is not simply uninitialized / still-zero, which would
      // mean the intrinsic silently no-op'd.
      unsigned char acc = 0;
      for ( unsigned char c : out ) acc = acc | c;
      check( acc != 0, "sha256 over 10K zeros produced an all-zero output" );
   }

   // Unaligned hash-output pointer. argument_proxy<fc::sha256*, 8> copies the
   // pointee into an aligned stack temporary, runs the intrinsic, then copies
   // back on destructor. Offsetting by UNALIGNED_OFFSET from a 16-aligned base
   // guarantees the pointer is not 8-aligned, forcing the copy-out path.
   [[sysio::action]]
   void sha2ual() {
      alignas(16) unsigned char buf[SHA256_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      sha256( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_SHA256, SHA256_SIZE) == 0,
             "sha256('abc') via unaligned out-ptr mismatch -- copy-out path regression" );
   }

   // =============================================================================
   // P1 -- sha1
   // =============================================================================

   [[sysio::action]]
   void sha1ok() {
      unsigned char out[SHA1_SIZE] = {};
      sha1( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_SHA1, SHA1_SIZE) == 0, "sha1('abc') mismatch" );
   }

   [[sysio::action]]
   void sha1em() {
      unsigned char out[SHA1_SIZE] = {};
      sha1( nullptr, 0, out );
      check( std::memcmp(out, EMPTY_SHA1, SHA1_SIZE) == 0,
             "sha1(empty) mismatch when data=nullptr" );
   }

   [[sysio::action]]
   void sha1big() {
      char buf[BIG_INPUT_LEN] = {};
      unsigned char out[SHA1_SIZE] = {};
      sha1( buf, BIG_INPUT_LEN, out );
      unsigned char acc = 0;
      for ( unsigned char c : out ) acc = acc | c;
      check( acc != 0, "sha1 over 10K zeros produced an all-zero output" );
   }

   [[sysio::action]]
   void sha1ual() {
      alignas(16) unsigned char buf[SHA1_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      sha1( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_SHA1, SHA1_SIZE) == 0,
             "sha1('abc') via unaligned out-ptr mismatch" );
   }

   // =============================================================================
   // P1 -- sha512
   // =============================================================================

   [[sysio::action]]
   void sha5ok() {
      unsigned char out[SHA512_SIZE] = {};
      sha512( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_SHA512, SHA512_SIZE) == 0, "sha512('abc') mismatch" );
   }

   [[sysio::action]]
   void sha5em() {
      unsigned char out[SHA512_SIZE] = {};
      sha512( nullptr, 0, out );
      check( std::memcmp(out, EMPTY_SHA512, SHA512_SIZE) == 0,
             "sha512(empty) mismatch when data=nullptr" );
   }

   [[sysio::action]]
   void sha5big() {
      char buf[BIG_INPUT_LEN] = {};
      unsigned char out[SHA512_SIZE] = {};
      sha512( buf, BIG_INPUT_LEN, out );
      unsigned char acc = 0;
      for ( unsigned char c : out ) acc = acc | c;
      check( acc != 0, "sha512 over 10K zeros produced an all-zero output" );
   }

   [[sysio::action]]
   void sha5ual() {
      alignas(16) unsigned char buf[SHA512_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      sha512( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_SHA512, SHA512_SIZE) == 0,
             "sha512('abc') via unaligned out-ptr mismatch" );
   }

   // =============================================================================
   // P1 -- ripemd160
   // =============================================================================

   [[sysio::action]]
   void ripeok() {
      unsigned char out[RIPE_SIZE] = {};
      ripemd160( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_RIPE, RIPE_SIZE) == 0, "ripemd160('abc') mismatch" );
   }

   [[sysio::action]]
   void ripeem() {
      unsigned char out[RIPE_SIZE] = {};
      ripemd160( nullptr, 0, out );
      check( std::memcmp(out, EMPTY_RIPE, RIPE_SIZE) == 0,
             "ripemd160(empty) mismatch when data=nullptr" );
   }

   [[sysio::action]]
   void ripebig() {
      char buf[BIG_INPUT_LEN] = {};
      unsigned char out[RIPE_SIZE] = {};
      ripemd160( buf, BIG_INPUT_LEN, out );
      unsigned char acc = 0;
      for ( unsigned char c : out ) acc = acc | c;
      check( acc != 0, "ripemd160 over 10K zeros produced an all-zero output" );
   }

   [[sysio::action]]
   void ripeual() {
      alignas(16) unsigned char buf[RIPE_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      ripemd160( TEST_ABC, TEST_ABC_LEN, out );
      check( std::memcmp(out, ABC_RIPE, RIPE_SIZE) == 0,
             "ripemd160('abc') via unaligned out-ptr mismatch" );
   }

   // =============================================================================
   // P1 -- assert_sha256
   //
   // Host throws crypto_api_exception "hash mismatch" if the computed digest
   // does not equal the provided digest. Both the accepted and rejection
   // paths exercise the legacy_ptr<const fc::sha256> copy-in path.
   // =============================================================================

   // Correct hash: no throw.
   [[sysio::action]]
   void asha2ok() {
      assert_sha256( TEST_ABC, TEST_ABC_LEN, ABC_SHA256 );
   }

   // Wrong hash: host must throw. If we reach check() below the host's
   // precondition silently accepted a mismatch -- regression.
   [[sysio::action]]
   void asha2ng() {
      assert_sha256( TEST_ABC, TEST_ABC_LEN, ZERO_HASH64 );
      check( false, "assert_sha256 with all-zero hash should have thrown" );
   }

   // Empty input with correct empty-string hash.
   [[sysio::action]]
   void asha2em() {
      assert_sha256( nullptr, 0, EMPTY_SHA256 );
   }

   // Unaligned const-hash pointer: copy-in path for const argument_proxy.
   [[sysio::action]]
   void asha2ua() {
      alignas(16) unsigned char buf[SHA256_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* unaligned = buf + UNALIGNED_OFFSET;
      std::memcpy( unaligned, ABC_SHA256, SHA256_SIZE );
      assert_sha256( TEST_ABC, TEST_ABC_LEN, unaligned );
   }

   // =============================================================================
   // P1 -- assert_sha1 / assert_sha512 / assert_ripemd160
   // =============================================================================

   [[sysio::action]] void asha1ok() {
      assert_sha1( TEST_ABC, TEST_ABC_LEN, ABC_SHA1 );
   }
   [[sysio::action]] void asha1ng() {
      assert_sha1( TEST_ABC, TEST_ABC_LEN, ZERO_HASH64 );
      check( false, "assert_sha1 with all-zero hash should have thrown" );
   }
   [[sysio::action]] void asha1em() {
      assert_sha1( nullptr, 0, EMPTY_SHA1 );
   }
   [[sysio::action]] void asha1ua() {
      alignas(16) unsigned char buf[SHA1_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* unaligned = buf + UNALIGNED_OFFSET;
      std::memcpy( unaligned, ABC_SHA1, SHA1_SIZE );
      assert_sha1( TEST_ABC, TEST_ABC_LEN, unaligned );
   }

   [[sysio::action]] void asha5ok() {
      assert_sha512( TEST_ABC, TEST_ABC_LEN, ABC_SHA512 );
   }
   [[sysio::action]] void asha5ng() {
      assert_sha512( TEST_ABC, TEST_ABC_LEN, ZERO_HASH64 );
      check( false, "assert_sha512 with all-zero hash should have thrown" );
   }
   [[sysio::action]] void asha5em() {
      assert_sha512( nullptr, 0, EMPTY_SHA512 );
   }
   [[sysio::action]] void asha5ua() {
      alignas(16) unsigned char buf[SHA512_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* unaligned = buf + UNALIGNED_OFFSET;
      std::memcpy( unaligned, ABC_SHA512, SHA512_SIZE );
      assert_sha512( TEST_ABC, TEST_ABC_LEN, unaligned );
   }

   [[sysio::action]] void aripeok() {
      assert_ripemd160( TEST_ABC, TEST_ABC_LEN, ABC_RIPE );
   }
   [[sysio::action]] void aripeng() {
      assert_ripemd160( TEST_ABC, TEST_ABC_LEN, ZERO_HASH64 );
      check( false, "assert_ripemd160 with all-zero hash should have thrown" );
   }
   [[sysio::action]] void aripeem() {
      assert_ripemd160( nullptr, 0, EMPTY_RIPE );
   }
   [[sysio::action]] void aripeua() {
      alignas(16) unsigned char buf[RIPE_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* unaligned = buf + UNALIGNED_OFFSET;
      std::memcpy( unaligned, ABC_RIPE, RIPE_SIZE );
      assert_ripemd160( TEST_ABC, TEST_ABC_LEN, unaligned );
   }

   // =============================================================================
   // P1 -- recover_key / assert_recover_key
   //
   // A deterministic (digest, sig, pub) triple is constructed by the Boost
   // driver via fc::private_key::regenerate and passed as action data so the
   // probe contract never embeds a brittle pre-computed secp256k1 vector.
   //
   // Failure-mode coverage (pinning host behavior path by path; these are the
   // surfaces the signature-recovery exception-cleanup follow-on PR will touch):
   //   - Structural unpack failures: empty / truncated / invalid variant tag.
   //   - secp256k1 recovery math failures: bad recovery byte / out-of-curve r,s.
   //   - Mathematically-valid-but-wrong sig: recovery succeeds, pub differs.
   //   - Small dest buffer: fixed-size (K1) asserts, variable-size truncates.
   //   - argument_proxy copy-in on the legacy_ptr<const fc::sha256> digest.
   // =============================================================================

   // Golden: host recovers the exact pub the driver embedded.
   [[sysio::action]]
   void recok() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n >= sizeof(sig_hash_key_header), "recover_key action data too short" );
      check( n <= RECOVER_BUF_CAPACITY, "recover_key action data larger than probe buffer" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;
      check( sizeof(*hdr) + hdr->sig_len + hdr->pk_len == n,
             "recover_key action data length does not match header" );

      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                                recovered, sizeof(recovered) );
      check( rc == static_cast<int32_t>(hdr->pk_len),
             "recover_key returned unexpected size" );
      check( std::memcmp(recovered, pub_ptr, hdr->pk_len) == 0,
             "recovered public key does not match driver-supplied pub" );
   }

   // Small pub buffer with a K1 signature. For fixed-size key types (k1/r1/em)
   // the host packs through fc::datastream<char*> which FC_ASSERTs when the
   // destination cannot hold the full serialization - so this must throw.
   // (Variable-size keys - wa, ed - silently truncate via memcpy; that path is
   // not reachable with the K1 sig the driver supplies and is noted in the
   // cleanup write-up as an API inconsistency worth normalizing.)
   [[sysio::action]]
   void recsmpub() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "recsmpub action data too large" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);

      char small_pub[4] = {};
      int32_t rc = recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                                small_pub, sizeof(small_pub) );
      (void)rc;
      check( false, "recover_key with K1 sig and 4-byte pub buffer must throw "
                    "(fc::datastream fixed-size pack FC_ASSERT)" );
   }

   // Unaligned digest pointer. legacy_ptr<const fc::sha256, 8> forces the
   // argument_proxy copy-in path when the wasm pointer is not 8-aligned.
   [[sysio::action]]
   void recuald() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "recuald action data too large" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;

      alignas(16) unsigned char digbuf[SHA256_SIZE + UNALIGNED_OFFSET + 8] = {};
      unsigned char* unaligned_digest = digbuf + UNALIGNED_OFFSET;
      std::memcpy( unaligned_digest, hdr->hash, SHA256_SIZE );

      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( unaligned_digest, sig_ptr, hdr->sig_len,
                                recovered, sizeof(recovered) );
      check( rc == static_cast<int32_t>(hdr->pk_len),
             "recover_key(unaligned digest) returned unexpected size" );
      check( std::memcmp(recovered, pub_ptr, hdr->pk_len) == 0,
             "recover_key(unaligned digest) pub mismatch -- copy-in path regression" );
   }

   // Structural corruption: byte 0 of the signature is the fc::raw variant
   // tag. Setting it to 0x7F (max single-byte unsigned_int) is guaranteed
   // above any currently-registered signature variant index, so fc::raw's
   // variant unpack must throw during sig decoding, before any secp256k1
   // math runs.
   [[sysio::action]]
   void recbadvar() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "recbadvar action data too large" );
      sysio::read_action_data( buf, n );

      auto* hdr = reinterpret_cast<sig_hash_key_header*>(buf);
      char* sig_ptr = reinterpret_cast<char*>(buf) + sizeof(*hdr);
      check( hdr->sig_len >= 1, "sig too short to corrupt variant tag" );
      sig_ptr[0] = static_cast<char>(0x7F);

      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                                recovered, sizeof(recovered) );
      (void)rc;
      check( false, "recover_key with invalid variant tag (0x7F) must throw" );
   }

   // secp256k1 recovery math: byte 1 of a K1 sig is the recovery byte
   // (canonical range [31, 35), legacy [27, 30]). Setting it outside the
   // accepted range triggers FC_THROW_EXCEPTION in elliptic_secp256k1.cpp
   // with message "unable to reconstruct public key from signature".
   [[sysio::action]]
   void recbadrec() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "recbadrec action data too large" );
      sysio::read_action_data( buf, n );

      auto* hdr = reinterpret_cast<sig_hash_key_header*>(buf);
      char* sig_ptr = reinterpret_cast<char*>(buf) + sizeof(*hdr);
      check( hdr->sig_len >= 2, "sig too short to corrupt recovery byte" );
      sig_ptr[1] = static_cast<char>(0x00);  // out of [27, 35)

      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                                recovered, sizeof(recovered) );
      (void)rc;
      check( false, "recover_key with out-of-range recovery byte must throw" );
   }

   // Valid structure, valid recovery byte, corrupted r/s. The math succeeds
   // and produces a DIFFERENT public key than the signer originally used.
   // Host MUST NOT throw here -- recover_key is not a verify function. This
   // is the single most important misuse to pin: a caller that assumes
   // recover_key verifies is broken by design; the only safe pattern is to
   // compare the returned pub against a known-authorized pub.
   [[sysio::action]]
   void recbadrs() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "recbadrs action data too large" );
      sysio::read_action_data( buf, n );

      auto* hdr = reinterpret_cast<sig_hash_key_header*>(buf);
      char* sig_ptr = reinterpret_cast<char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;
      check( hdr->sig_len >= 10,
             "sig too short to corrupt deep r/s bytes" );
      // Corrupt a byte well inside the r component. A small bit flip keeps
      // (r, s) on-curve with overwhelming probability -- we want the math to
      // succeed and return a different valid pub, not to fail validation.
      sig_ptr[9] = sig_ptr[9] ^ 0x01;

      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                                recovered, sizeof(recovered) );
      // If the math fails (rare but possible with this corruption), host
      // throws and we never reach here -- which is acceptable because the
      // property we care about is "never silently returns original pub".
      // The driver's BOOST_CHECK_NO_THROW will flag the corner case.
      check( rc == static_cast<int32_t>(hdr->pk_len),
             "recover_key(corrupt r) returned unexpected size" );
      check( std::memcmp(recovered, pub_ptr, hdr->pk_len) != 0,
             "recover_key on r-corrupted sig must NOT recover original pub" );
   }

   // Short sig (1 byte): valid variant tag but no shim content. fc::raw
   // unpack of the shim bytes hits datastream end, throws.
   [[sysio::action]]
   void recshort() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "recshort action data too large" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);

      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( hdr->hash, sig_ptr, 1, recovered, sizeof(recovered) );
      (void)rc;
      check( false, "recover_key with truncated sig (1 byte) must throw" );
   }

   // Zero-length sig: datastream runs dry before the variant tag itself.
   [[sysio::action]]
   void recempsig() {
      unsigned char dig[SHA256_SIZE] = {};
      char recovered[MAX_RECOVERED_PUB] = {};
      int32_t rc = recover_key( dig, nullptr, 0, recovered, sizeof(recovered) );
      (void)rc;
      check( false, "recover_key with empty signature must throw" );
   }

   // assert_recover_key: matching digest + sig + pub -> no throw.
   [[sysio::action]]
   void arecok() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "arecok action data too large" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;

      assert_recover_key( hdr->hash, sig_ptr, hdr->sig_len, pub_ptr, hdr->pk_len );
   }

   // assert_recover_key: recovery succeeds but recovered pub != supplied pub
   // -> SYS_ASSERT(check == p, crypto_api_exception, "...") fires.
   [[sysio::action]]
   void arecng() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "arecng action data too large" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;

      unsigned char wrong_pub[MAX_RECOVERED_PUB] = {};
      check( hdr->pk_len <= sizeof(wrong_pub), "pub too large for wrong-pub buffer" );
      std::memcpy(wrong_pub, pub_ptr, hdr->pk_len);
      // Flip a byte past the variant-tag byte so the variant type still
      // matches the sig's (preventing the earlier SYS_ASSERT on type match)
      // and the mismatch surfaces specifically as a pub-equality failure.
      if ( hdr->pk_len > 1 ) wrong_pub[1] = wrong_pub[1] ^ 0x01;
      assert_recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                          reinterpret_cast<const char*>(wrong_pub), hdr->pk_len );
      check( false, "assert_recover_key with altered pub should have thrown "
                    "\"Error expected key different than recovered key\"" );
   }

   // assert_recover_key: recovery math itself fails (bad recovery byte) -
   // host throws BEFORE the pub-equality compare. Pins that the error path
   // is the recovery failure, not the compare.
   [[sysio::action]]
   void arecbadrec() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "arecbadrec action data too large" );
      sysio::read_action_data( buf, n );

      auto* hdr = reinterpret_cast<sig_hash_key_header*>(buf);
      char* sig_ptr = reinterpret_cast<char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;
      check( hdr->sig_len >= 2, "sig too short to corrupt" );
      sig_ptr[1] = static_cast<char>(0x00);

      assert_recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                          pub_ptr, hdr->pk_len );
      check( false, "assert_recover_key with out-of-range recovery byte must throw" );
   }

   // assert_recover_key: digest+sig are K1, pub is forged to claim variant
   // R1 (index 1 in fc::public_key's variant). Host's explicit SYS_ASSERT
   // "Public key type does not match signature type" fires BEFORE the
   // secp256k1 recovery math runs.
   [[sysio::action]]
   void arecpubty() {
      unsigned char buf[RECOVER_BUF_CAPACITY] = {};
      uint32_t n = sysio::action_data_size();
      check( n <= RECOVER_BUF_CAPACITY, "arecpubty action data too large" );
      sysio::read_action_data( buf, n );

      const auto* hdr = reinterpret_cast<const sig_hash_key_header*>(buf);
      const char* sig_ptr = reinterpret_cast<const char*>(buf) + sizeof(*hdr);
      const char* pub_ptr = sig_ptr + hdr->sig_len;

      unsigned char forged[MAX_RECOVERED_PUB] = {};
      check( hdr->pk_len <= sizeof(forged), "pub too large for forged buffer" );
      std::memcpy(forged, pub_ptr, hdr->pk_len);
      forged[0] = 0x01;  // R1 variant tag; driver vector is K1 (tag 0)
      assert_recover_key( hdr->hash, sig_ptr, hdr->sig_len,
                          reinterpret_cast<const char*>(forged), hdr->pk_len );
      check( false, "assert_recover_key with pub variant mismatched to sig "
                    "variant must throw \"Public key type does not match signature type\"" );
   }

   // assert_recover_key with empty sig -> throws at unpack step.
   [[sysio::action]]
   void arecempsig() {
      unsigned char dig[SHA256_SIZE] = {};
      const char* pub_placeholder = "\x00";
      assert_recover_key( dig, nullptr, 0, pub_placeholder, 0 );
      check( false, "assert_recover_key with empty signature must throw" );
   }

   // =============================================================================
   // P1 -- preactivate_feature
   //
   // Registered with privileged_check in runtimes/sys-vm.cpp line 359. The
   // priv gate fires BEFORE the legacy_ptr<const digest_type> copy-in, so the
   // non-privileged probe never reaches the digest read. The priv body then
   // dispatches into controller::preactivate_feature which validates the
   // digest against known features.
   //
   // Driver supplies action data = 32-byte digest for the accepted path (a
   // real unactivated-feature digest it computes off the controller). The
   // rejection probes use local buffers since they trap before the digest
   // matters (non-priv) or use a hardcoded sentinel (not-a-feature).
   // =============================================================================

   // Privileged account + valid unactivated feature digest -> host accepts.
   // Driver computes the digest of a pre-registered unactivated feature and
   // passes it as the full 32-byte action data payload.
   [[sysio::action]]
   void preactok() {
      unsigned char digest[SHA256_SIZE] = {};
      uint32_t n = sysio::action_data_size();
      check( n == SHA256_SIZE, "preactok action data must be exactly 32 bytes" );
      sysio::read_action_data( digest, n );
      preactivate_feature( digest );
   }

   // Non-privileged account -> privileged_check SYS_ASSERT fires with
   // unaccessible_api. Digest bytes are irrelevant (never read).
   [[sysio::action]]
   void preactnp() {
      unsigned char dummy_digest[SHA256_SIZE] = {};
      preactivate_feature( dummy_digest );
      check( false, "preactivate_feature from non-priv account must throw "
                    "unaccessible_api" );
   }

   // Privileged account + bogus (all-zero) digest that does not correspond
   // to any registered protocol feature -> controller::preactivate_feature
   // rejects (fc::exception, "unknown feature digest" or similar).
   [[sysio::action]]
   void preactbog() {
      unsigned char zeros[SHA256_SIZE] = {};
      preactivate_feature( zeros );
      check( false, "preactivate_feature with all-zero digest must throw" );
   }

   // =============================================================================
   // P2 -- resource / auth / producer / blockchain-parameters intrinsics
   //
   // Covers the legacy_ptr<int64_t, 8> out-param path (get_resource_limits),
   // the legacy_span<const char> and legacy_span<account_name> producer
   // paths, and the privileged_check gating on each set_* op. Small-buffer
   // probes pin the "returns required size, does not overflow caller" contract
   // that get_blockchain_parameters_packed documents.
   // =============================================================================

   // get_resource_limits with 3 aligned out-pointers -> host writes 3 int64_t
   // values into the caller's buffer. Self-lookup -- self() always exists.
   [[sysio::action]]
   void reslimok() {
      int64_t ram = -99, net_w = -99, cpu_w = -99;
      get_resource_limits( get_self().value, &ram, &net_w, &cpu_w );
      // Any account has non-zero RAM after being created. net and cpu can be
      // any non-negative stake-weighted value. We pin only that the host
      // actually wrote the outputs (the default sentinel -99 is gone).
      check( ram != -99 && net_w != -99 && cpu_w != -99,
             "get_resource_limits did not write all three out-params" );
   }

   // get_resource_limits with 3 intentionally-misaligned out-pointers.
   // Offset +1, +3, +5 bytes into a 16-aligned buffer guarantees none of
   // the pointers is 8-aligned, forcing argument_proxy<int64_t*, 8>'s
   // copy-in + copy-back memcpy paths for each of the three out-params.
   [[sysio::action]]
   void reslimua() {
      alignas(16) unsigned char buf[64] = {};
      void* ram_ptr   = buf + 1;   // offset 1
      void* net_ptr   = buf + 9 + 3;   // offset 12
      void* cpu_ptr   = buf + 32 + 5;  // offset 37
      // Preload known sentinel via memcpy so we can detect host write-back.
      int64_t sentinel = -99;
      std::memcpy(ram_ptr, &sentinel, 8);
      std::memcpy(net_ptr, &sentinel, 8);
      std::memcpy(cpu_ptr, &sentinel, 8);

      get_resource_limits( get_self().value, ram_ptr, net_ptr, cpu_ptr );

      int64_t ram = 0, net_w = 0, cpu_w = 0;
      std::memcpy(&ram,   ram_ptr, 8);
      std::memcpy(&net_w, net_ptr, 8);
      std::memcpy(&cpu_w, cpu_ptr, 8);
      check( ram != -99 && net_w != -99 && cpu_w != -99,
             "get_resource_limits unaligned -- copy-back path regression" );
   }

   // set_resource_limits with a privileged receiver -> host body runs.
   // Read current limits first so we restore them after the probe and don't
   // leak state across the shared tester.
   [[sysio::action]]
   void setreslim() {
      int64_t ram = 0, net_w = 0, cpu_w = 0;
      get_resource_limits( get_self().value, &ram, &net_w, &cpu_w );
      // Bump RAM by 1, then restore. No-op against the test if the account
      // already has headroom in its RAM stake.
      set_resource_limits( get_self().value, ram + 1, net_w, cpu_w );
      set_resource_limits( get_self().value, ram,     net_w, cpu_w );
   }

   // set_resource_limits from a non-privileged account -> privileged_check
   // SYS_ASSERT fires with unaccessible_api BEFORE the host body runs.
   [[sysio::action]]
   void setresnp() {
      set_resource_limits( get_self().value, -1, -1, -1 );
      check( false, "set_resource_limits from non-priv account must throw "
                    "unaccessible_api" );
   }

   // get_active_producers with a buffer sized for at least one producer.
   // Host returns bytes written, capped by buffer capacity.
   [[sysio::action]]
   void actprdok() {
      uint64_t buf[64] = {};   // 64 slots * 8 bytes = 512 bytes
      int32_t rc = get_active_producers( buf, sizeof(buf) );
      check( rc >= 0, "get_active_producers returned negative size" );
      check( static_cast<uint32_t>(rc) <= sizeof(buf),
             "get_active_producers returned more than buffer capacity" );
   }

   // get_active_producers with size=0 -> host returns required bytes without
   // touching the buffer. Pins the "report required size via zero-size query"
   // contract.
   [[sysio::action]]
   void actprdsm() {
      int32_t rc = get_active_producers( nullptr, 0 );
      check( rc >= 0, "get_active_producers(nullptr, 0) must return required "
                      "size without writing" );
   }

   // set_proposed_producers from a non-privileged account -> throws
   // unaccessible_api. Payload bytes are irrelevant; the priv gate fires
   // first.
   [[sysio::action]]
   void sprodnp() {
      unsigned char dummy[1] = {0};
      (void) set_proposed_producers(
         reinterpret_cast<const char*>(dummy), sizeof(dummy) );
      check( false, "set_proposed_producers from non-priv account must throw "
                    "unaccessible_api" );
   }

   // get_blockchain_parameters_packed with size=0 -> host returns the
   // required byte count without touching the (null) buffer. Pins the
   // conventional "query size first, then allocate" pattern.
   [[sysio::action]]
   void bcpgetsm() {
      uint32_t rc = get_blockchain_parameters_packed( nullptr, 0 );
      check( rc > 0, "get_blockchain_parameters_packed(nullptr, 0) must "
                     "return non-zero required size" );
   }

   // get_blockchain_parameters_packed with a generous buffer -> host packs
   // the parameters and returns bytes written.
   [[sysio::action]]
   void bcpgetok() {
      char buf[512] = {};
      uint32_t required = get_blockchain_parameters_packed( nullptr, 0 );
      check( required <= sizeof(buf),
             "blockchain parameters size exceeds probe buffer" );
      uint32_t rc = get_blockchain_parameters_packed( buf, sizeof(buf) );
      check( rc == required,
             "get_blockchain_parameters_packed byte count mismatch between "
             "size-query and actual-read" );
   }

   // set_blockchain_parameters_packed from a non-privileged account ->
   // throws unaccessible_api.
   [[sysio::action]]
   void bcpsetnp() {
      unsigned char dummy[1] = {0};
      set_blockchain_parameters_packed(
         reinterpret_cast<const char*>(dummy), sizeof(dummy) );
      check( false, "set_blockchain_parameters_packed from non-priv account "
                    "must throw unaccessible_api" );
   }

   // check_transaction_authorization with all-empty spans. The host tries to
   // unpack the trx / pubs / perms buffers and fails; datastream runs dry.
   // Pins that an all-empty input is not silently accepted as "empty
   // authorization set is sufficient".
   [[sysio::action]]
   void chktrxem() {
      int32_t rc = check_transaction_authorization( nullptr, 0,
                                                    nullptr, 0,
                                                    nullptr, 0 );
      (void)rc;
      check( false, "check_transaction_authorization with all-empty spans "
                    "must throw during trx unpack" );
   }

   // =============================================================================
   // P1 -- compiler_builtins: 128-bit integer ops
   //
   // Each intrinsic has three probes: a golden path, an unaligned-output-ptr
   // path (exercising argument_proxy<[u]int128_t*, 16> copy-out), and an
   // edge-value path (shift-overshoot, divide-by-zero, INT_MIN-div semantics).
   // Verification uses memcpy read-back into uint64_t pairs so we never
   // emit a second call into the intrinsic we are testing.
   // =============================================================================

   // __multi3: golden. (1 << 40) * 1024 = (1 << 50). Result fits in low 64.
   [[sysio::action]]
   void mulok() {
      alignas(16) unsigned char buf[16] = {};
      __multi3( buf, 1ULL << 40, 0, 1024, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( lo == (1ULL << 50) && hi == 0, "__multi3(1<<40, 1024) != 1<<50" );
   }

   // __multi3: unaligned output pointer. Offsets the result by
   // UNALIGNED_OFFSET bytes into a 16-aligned buffer, forcing the host's
   // argument_proxy<uint128_t*, 16> copy-out memcpy path.
   [[sysio::action]]
   void mulua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __multi3( out, 1ULL << 40, 0, 1024, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( lo == (1ULL << 50) && hi == 0,
             "__multi3 unaligned out -- copy-out path regression" );
   }

   // __multi3: 2^65. U64_MAX * 2 = 2^65 - 2. Low = 0xFFFFFFFFFFFFFFFE, High = 1.
   // Pins the 64->128-bit carry path.
   [[sysio::action]]
   void muledge() {
      alignas(16) unsigned char buf[16] = {};
      __multi3( buf, U64_MAX, 0, 2, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( lo == 0xFFFFFFFFFFFFFFFEULL && hi == 1,
             "__multi3(U64_MAX, 2) carry path wrong" );
   }

   // __divti3: golden. 100 / 5 = 20.
   [[sysio::action]]
   void divok() {
      alignas(16) unsigned char buf[16] = {};
      __divti3( buf, 100, 0, 5, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( lo == 20 && hi == 0, "__divti3(100, 5) != 20" );
   }

   // __divti3: unaligned output.
   [[sysio::action]]
   void divua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __divti3( out, 100, 0, 5, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( lo == 20 && hi == 0,
             "__divti3 unaligned out -- copy-out path regression" );
   }

   // __divti3 with rhs=0: host SYS_ASSERT("divide by zero") throws
   // arithmetic_exception.
   [[sysio::action]]
   void divzero() {
      alignas(16) unsigned char buf[16] = {};
      __divti3( buf, 100, 0, 0, 0 );
      check( false, "__divti3 divide-by-zero should throw arithmetic_exception" );
   }

   // __udivti3: golden. 1000 / 4 = 250.
   [[sysio::action]]
   void udivok() {
      alignas(16) unsigned char buf[16] = {};
      __udivti3( buf, 1000, 0, 4, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( lo == 250 && hi == 0, "__udivti3(1000, 4) != 250" );
   }

   // __udivti3: unaligned output.
   [[sysio::action]]
   void udivua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __udivti3( out, 1000, 0, 4, 0 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( lo == 250 && hi == 0,
             "__udivti3 unaligned out -- copy-out path regression" );
   }

   // __udivti3 with rhs=0: throws arithmetic_exception.
   [[sysio::action]]
   void udivzero() {
      alignas(16) unsigned char buf[16] = {};
      __udivti3( buf, 1000, 0, 0, 0 );
      check( false, "__udivti3 divide-by-zero should throw" );
   }

   // __ashlti3: golden. 1 << 1 = 2.
   [[sysio::action]]
   void ashlok() {
      alignas(16) unsigned char buf[16] = {};
      __ashlti3( buf, 1, 0, 1 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( lo == 2 && hi == 0, "__ashlti3(1, 1) != 2" );
   }

   // __ashlti3: unaligned output.
   [[sysio::action]]
   void ashlua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __ashlti3( out, 1, 0, 1 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( lo == 2 && hi == 0,
             "__ashlti3 unaligned out -- copy-out path regression" );
   }

   // __ashlti3 with shift >= 128: host returns 0 (well-defined saturation
   // per compiler_builtins.cpp:__ashlti3). Pins the boundary.
   [[sysio::action]]
   void ashlover() {
      alignas(16) unsigned char buf[16] = {};
      __ashlti3( buf, U64_MAX, U64_MAX, 200 );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( lo == 0 && hi == 0,
             "__ashlti3 shift>=128 must saturate to 0 per host contract" );
   }

   // =============================================================================
   // P1 -- compiler_builtins: float128 (quad precision) ops
   //
   // Verification uses __cmptf2 / __unordtf2 (uint64_t pair args, no
   // legacy_ptr) to compare results against pre-computed bit patterns, so
   // the probes exercise the intrinsic under test without reintroducing a
   // second call to that same intrinsic for golden comparison.
   // =============================================================================

   // __addtf3: 1.0 + 2.0 == 3.0
   [[sysio::action]]
   void addfok() {
      alignas(16) unsigned char buf[16] = {};
      __addtf3( buf, FP128_ONE_LO, FP128_ONE_HI, FP128_TWO_LO, FP128_TWO_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( __cmptf2(lo, hi, FP128_THREE_LO, FP128_THREE_HI) == 0,
             "__addtf3(1.0, 2.0) != 3.0" );
   }

   // __addtf3: unaligned output.
   [[sysio::action]]
   void addfua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __addtf3( out, FP128_ONE_LO, FP128_ONE_HI, FP128_TWO_LO, FP128_TWO_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( __cmptf2(lo, hi, FP128_THREE_LO, FP128_THREE_HI) == 0,
             "__addtf3 unaligned out -- copy-out path regression" );
   }

   // __multf3: 2.0 * 3.0 == 6.0
   [[sysio::action]]
   void mulfok() {
      alignas(16) unsigned char buf[16] = {};
      __multf3( buf, FP128_TWO_LO, FP128_TWO_HI, FP128_THREE_LO, FP128_THREE_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( __cmptf2(lo, hi, FP128_SIX_LO, FP128_SIX_HI) == 0,
             "__multf3(2.0, 3.0) != 6.0" );
   }

   // __multf3: unaligned output.
   [[sysio::action]]
   void mulfua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __multf3( out, FP128_TWO_LO, FP128_TWO_HI, FP128_THREE_LO, FP128_THREE_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( __cmptf2(lo, hi, FP128_SIX_LO, FP128_SIX_HI) == 0,
             "__multf3 unaligned out -- copy-out path regression" );
   }

   // __multf3 with NaN operand: result is NaN (no throw). __unordtf2 returns
   // non-zero when either operand is NaN, so we compare the product to itself
   // via __unordtf2 -- NaN is not equal to itself, so unordered.
   [[sysio::action]]
   void mulfnan() {
      alignas(16) unsigned char buf[16] = {};
      __multf3( buf, FP128_NAN_LO, FP128_NAN_HI, FP128_TWO_LO, FP128_TWO_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( __unordtf2(lo, hi, lo, hi) != 0,
             "__multf3 NaN * finite should produce NaN (unordered with itself)" );
   }

   // __divtf3: 6.0 / 2.0 == 3.0
   [[sysio::action]]
   void divfok() {
      alignas(16) unsigned char buf[16] = {};
      __divtf3( buf, FP128_SIX_LO, FP128_SIX_HI, FP128_TWO_LO, FP128_TWO_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( __cmptf2(lo, hi, FP128_THREE_LO, FP128_THREE_HI) == 0,
             "__divtf3(6.0, 2.0) != 3.0" );
   }

   // __divtf3: unaligned output.
   [[sysio::action]]
   void divfua() {
      alignas(16) unsigned char buf[16 + UNALIGNED_OFFSET + 16] = {};
      unsigned char* out = buf + UNALIGNED_OFFSET;
      __divtf3( out, FP128_SIX_LO, FP128_SIX_HI, FP128_TWO_LO, FP128_TWO_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, out,     8);
      std::memcpy(&hi, out + 8, 8);
      check( __cmptf2(lo, hi, FP128_THREE_LO, FP128_THREE_HI) == 0,
             "__divtf3 unaligned out -- copy-out path regression" );
   }

   // __divtf3: 1.0 / 0.0 = +Inf (IEEE 754, NO throw).
   [[sysio::action]]
   void divfzero() {
      alignas(16) unsigned char buf[16] = {};
      __divtf3( buf, FP128_ONE_LO, FP128_ONE_HI, FP128_ZERO_LO, FP128_ZERO_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      check( __cmptf2(lo, hi, FP128_INF_LO, FP128_INF_HI) == 0,
             "__divtf3(1.0, 0.0) must produce +Inf, not throw" );
   }

   // __fixtfti: large finite float128 (2^127) truncated to int128. Host's
   // softfloat f128_to_i128 saturates on overflow per the impl -- pin that
   // the call returns (no throw) and produces some definite bit pattern
   // rather than silent UB.
   [[sysio::action]]
   void fixovfl() {
      alignas(16) unsigned char buf[16] = {};
      __fixtfti( buf, FP128_LARGE_LO, FP128_LARGE_HI );
      uint64_t lo = 0, hi = 0;
      std::memcpy(&lo, buf,     8);
      std::memcpy(&hi, buf + 8, 8);
      // Saturation ceiling for int128 is INT128_MAX = 0x7FFF_FFFF... / hi
      // = 0x7FFFFFFFFFFFFFFF, lo = 0xFFFFFFFFFFFFFFFF. The specific value
      // is a host-impl choice; what we pin is: the call did not silently
      // return all zeros (which would mean the intrinsic ran uninitialized).
      check( !(lo == 0 && hi == 0),
             "__fixtfti(2^127) must not return 0 -- expect saturated int128" );
   }

   // =============================================================================
   // P3 -- console / IO / action-data intrinsics
   //
   // Covers the remaining legacy_span / null_terminated_ptr surface. raw::prints /
   // raw::sysio_assert take null_terminated_ptr (host walks memory for \0), so the
   // validator cost is per-call proportional to the string length. The *_l /
   // *_message variants use legacy_span<const char> with explicit size and
   // are the path the cleanup PR will keep. Remaining legacy_span<char>
   // readers (read_action_data, get_context_free_data, raw::get_action,
   // raw::read_transaction) document a "size=0 returns required size" contract
   // that the small-buffer probes here pin in place.
   //
   // CDT's capi/sysio/{print,system,action,transaction}.h declares all these
   // intrinsics globally with __attribute__((sysio_wasm_import)), so we call
   // them directly rather than re-declaring in this file's extern "C" block.
   // =============================================================================

   // raw::prints: null_terminated_ptr. Host walks memory until \0 -- probe with
   // a normal C string and an empty C string (single \0 byte).
   [[sysio::action]]
   void printok() {
      raw::prints( "probe-print-test" );
      raw::prints( "" );  // empty C string
   }

   // raw::prints_l: legacy_span<const char>. Zero-length legal (no bytes printed).
   // Non-null data with length 0 also legal and must behave identically.
   [[sysio::action]]
   void printlem() {
      raw::prints_l( nullptr, 0 );
      const char msg[] = "xyz";
      raw::prints_l( msg, 3 );
   }

   // printhex: legacy_span<const char> of raw bytes. Zero-length legal.
   [[sysio::action]]
   void phxok() {
      const unsigned char data[] = { 0xde, 0xad, 0xbe, 0xef };
      printhex( data, sizeof(data) );
      printhex( nullptr, 0 );
   }

   // raw::sysio_assert with test != 0: no-op. Pins that the null_terminated_ptr
   // msg is NOT walked when test is truthy (host short-circuits).
   [[sysio::action]]
   void sasok() {
      raw::sysio_assert( 1, "should not throw" );
   }

   // raw::sysio_assert with test == 0: host walks the null_terminated_ptr msg,
   // wraps in sysio_assert_message_exception, throws.
   [[sysio::action]]
   void sasng() {
      raw::sysio_assert( 0, "probe-sysio-assert-should-throw" );
      check( false, "raw::sysio_assert(0, ...) did not throw" );
   }

   // raw::sysio_assert_message with test != 0: no-op.
   [[sysio::action]]
   void samok() {
      const char msg[] = "ok";
      raw::sysio_assert_message( 1, msg, sizeof(msg) - 1 );
   }

   // raw::sysio_assert_message with test == 0: throws with msg included in the
   // exception string.
   [[sysio::action]]
   void samng() {
      const char msg[] = "probe-assert-message-should-throw";
      raw::sysio_assert_message( 0, msg, sizeof(msg) - 1 );
      check( false, "raw::sysio_assert_message(0, ...) did not throw" );
   }

   // raw::sysio_assert_message with test == 0 AND empty msg span. Pins that an
   // empty-message rejection does not crash on a zero-length legacy_span.
   [[sysio::action]]
   void samngem() {
      raw::sysio_assert_message( 0, nullptr, 0 );
      check( false, "raw::sysio_assert_message(0, nullptr, 0) did not throw" );
   }

   // read_action_data with buffer >= action_data_size. Pins the identity
   // "rc == action_data_size() when buffer fits".
   [[sysio::action]]
   void radok() {
      char buf[64] = {};
      uint32_t sz = action_data_size();
      check( sz <= sizeof(buf), "action data larger than probe buffer" );
      uint32_t rc = read_action_data( buf, sz );
      check( rc == sz, "read_action_data rc mismatches action_data_size" );
   }

   // read_action_data with size=0: returns 0 (empty-buffer identity). Legacy
   // span<char> with size 0 must be accepted.
   [[sysio::action]]
   void radsm() {
      uint32_t rc = read_action_data( nullptr, 0 );
      check( rc == 0, "read_action_data(nullptr, 0) should return 0" );
   }

   // raw::get_action with type=1 (action) index=0 -> current action serialized
   // size. Positive on success.
   [[sysio::action]]
   void gacok() {
      char buf[512] = {};
      int rc = raw::get_action( 1, 0, buf, sizeof(buf) );
      check( rc > 0, "raw::get_action(type=1, index=0) should return positive size" );
   }

   // raw::get_action with invalid type. apply_context::get_action asserts
   // act_ptr != nullptr AFTER the type-vs-0/1 branches, so type=99 (neither
   // context-free nor action) reaches the SYS_ASSERT and throws
   // action_not_found_exception. The -1 sentinel is only for valid-type,
   // out-of-range INDEX.
   [[sysio::action]]
   void gacbad() {
      char buf[64] = {};
      int rc = raw::get_action( 99, 0, buf, sizeof(buf) );
      (void)rc;
      check( false, "raw::get_action(type=99) must throw action_not_found_exception" );
   }

   // raw::read_transaction with adequate buffer. The host returns bytes written.
   [[sysio::action]]
   void rtxok() {
      char buf[2048] = {};
      size_t needed = raw::read_transaction( nullptr, 0 );
      check( needed > 0,            "raw::read_transaction(nullptr, 0) returned 0" );
      check( needed <= sizeof(buf), "transaction larger than probe buffer" );
      size_t rc = raw::read_transaction( buf, sizeof(buf) );
      check( rc == needed, "raw::read_transaction bytes written != size query" );
   }

   // raw::read_transaction with size=0 -> returns required size without touching
   // the null buffer.
   [[sysio::action]]
   void rtxsm() {
      size_t rc = raw::read_transaction( nullptr, 0 );
      check( rc > 0, "raw::read_transaction(nullptr, 0) should return required size" );
   }

   // raw::send_inline with empty span -> host tries to unpack and fails. Pins
   // that a zero-length legacy_span is NOT silently converted to a default
   // action.
   [[sysio::action]]
   void sinlem() {
      raw::send_inline( nullptr, 0 );
      check( false, "raw::send_inline(empty span) must throw during action unpack" );
   }

   // set_action_return_value (uses plain span<const char>, not legacy) with
   // a normal byte buffer -> no throw, value is stored on the action trace.
   [[sysio::action]]
   void sarvok() {
      const char retval[] = "probe-return-value";
      set_action_return_value( const_cast<char*>(retval), sizeof(retval) - 1 );
   }

   // set_action_return_value with empty span. Pins that a zero-length
   // return value is legal.
   [[sysio::action]]
   void sarvem() {
      set_action_return_value( nullptr, 0 );
   }

   // =============================================================================
   // Wiring-only stub kept so the earlier build-smoke commit remains callable.
   // =============================================================================
   [[sysio::action]]
   void nop() {}
};
