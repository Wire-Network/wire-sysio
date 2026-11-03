#include <sysio/chain/account_object.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/webassembly/common.hpp>
#include <sysio/testing/tester.hpp>

#include "test_utils.hpp"

#include <fc/io/raw.hpp>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include <boost/test/unit_test.hpp>

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
   #include <sysio/chain/webassembly/native-module/native_context_stack.hpp>
/// The native exports that take a flag, as a contract built as a shared library calls them.
extern "C" {
void sysio_assert(uint32_t test, const char* msg);
void sysio_assert_message(uint32_t test, const char* msg, uint32_t msg_len);
void sysio_assert_code(uint32_t test, uint64_t code);
int32_t blake2_f(uint32_t rounds, const char* state, uint32_t state_len, const char* msg, uint32_t msg_len,
                 const char* t0_offset, uint32_t t0_len, const char* t1_offset, uint32_t t1_len, int32_t final,
                 char* result, uint32_t result_len);
void sha3(const char* data, uint32_t data_len, char* hash, uint32_t hash_len, int32_t keccak);
}
#endif

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using sysio::test_utils::to_wast_string;

// Every value a protocol feature might one day give a meaning has to abort until then, or contracts that pass it today
// would change behavior when the feature lands. These contracts are hand-written wast so they can pass what a compiled
// contract cannot, such as 2 for a bool.
namespace {

constexpr auto contract     = "intrinsic"_n;
constexpr auto other_account = "target"_n;

/// Where a test's packed argument sits in linear memory. Everything below it stays zero, which the calls use as buffers.
constexpr uint32_t data_offset = 1024;

/// A zero digest, in the zeroed memory below data_offset. Address 0 itself is refused as a null reference.
constexpr uint32_t digest_offset = 64;

/// Flags take 0 or 1. These cover the next value up, a set high bit, and the patterns a narrowing conversion would miss.
constexpr std::array<int32_t, 4> junk_flags{2, 256, 65536, -1};

/// Versions, formats and exit codes take 0 today.
constexpr std::array<int32_t, 3> junk_versions{1, 2, -1};

constexpr auto flag_error     = "intrinsic flag must be 0 or 1";
constexpr auto trailing_error = "trailing bytes after the packed argument";
constexpr char junk_byte      = 0;

/**
 * A contract whose apply makes one intrinsic call.
 * @param intrinsic name imported from env, bound to $f
 * @param signature its wast signature
 * @param call the body of apply
 * @param data placed at data_offset
 */
std::string wast_calling(std::string_view intrinsic, std::string_view signature, std::string_view call,
                         const bytes& data = {}) {
   return fmt::format(R"=====(
(module
 (import "env" "{}" (func $f {}))
 (memory $0 1)
 (data (i32.const {}) "{}")
 (export "apply" (func $apply))
 (func $apply (param $0 i64) (param $1 i64) (param $2 i64)
  {}
 )
)
)=====",
                      intrinsic, signature, data_offset, to_wast_string(data), call);
}

template <typename T>
bytes pack_with_junk(const T& value) {
   bytes packed = fc::raw::pack(value);
   packed.push_back(junk_byte);
   return packed;
}

struct value_tester : validating_tester {
   value_tester() {
      create_accounts({contract, other_account});
      const std::string placeholder = wast_calling("prints_l", "(param i32 i32)", "(call $f (i32.const 0) (i32.const 0))");
      set_code(contract, placeholder.c_str());
      set_code(other_account, placeholder.c_str());
      set_privileged(contract);
      produce_block();
   }

   /// A signed transaction with one action to the contract.
   signed_transaction call_to_contract() {
      signed_transaction trx;
      trx.actions.emplace_back(vector<permission_level>{{contract, config::active_name}}, contract, ""_n, bytes{});
      set_transaction_headers(trx);
      trx.sign(get_private_key(contract, "active"), get_chain_id());
      return trx;
   }

   /// Deploys the wast and pushes one action to it.
   void run(const std::string& wast) {
      set_code(contract, wast.c_str());
      produce_block();
      signed_transaction trx = call_to_contract();
      push_transaction(trx);
      produce_block();
   }
};

/// One intrinsic with a closed-set argument. `call` has a {} where the value goes.
struct value_case {
   std::string_view intrinsic;
   std::string_view signature;
   std::string_view call;

   std::string with(int64_t value) const {
      return wast_calling(intrinsic, signature, fmt::format(fmt::runtime(call), value));
   }
};

constexpr value_case sha3_keccak{"sha3", "(param i32 i32 i32 i32 i32)",
                                 "(call $f (i32.const 0) (i32.const 0) (i32.const 64) (i32.const 32) (i32.const {}))"};

constexpr value_case blake2_f_final{"blake2_f", "(param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)",
                                    R"((drop (call $f (i32.const 12) (i32.const 0) (i32.const 64) (i32.const 64)
                                       (i32.const 128) (i32.const 192) (i32.const 8) (i32.const 200) (i32.const 8)
                                       (i32.const {}) (i32.const 208) (i32.const 64))))"};

constexpr value_case sysio_assert_condition{"sysio_assert", "(param i32 i32)", "(call $f (i32.const {}) (i32.const 0))"};

constexpr value_case sysio_assert_message_condition{"sysio_assert_message", "(param i32 i32 i32)",
                                                    "(call $f (i32.const {}) (i32.const 0) (i32.const 0))"};

constexpr value_case sysio_assert_code_condition{"sysio_assert_code", "(param i32 i64)",
                                                 "(call $f (i32.const {}) (i64.const 7))"};

constexpr value_case sysio_exit_code{"sysio_exit", "(param i32)", "(call $f (i32.const {}))"};

constexpr value_case code_hash_version{"get_code_hash", "(param i64 i32 i32 i32) (result i32)",
                                       "(drop (call $f (get_local $0) (i32.const {}) (i32.const 0) (i32.const 64)))"};

constexpr value_case wasm_parameters_version{"get_wasm_parameters_packed", "(param i32 i32 i32) (result i32)",
                                             "(drop (call $f (i32.const 0) (i32.const 0) (i32.const {})))"};

constexpr value_case action_type{"get_action", "(param i32 i32 i32 i32) (result i32)",
                                 "(drop (call $f (i32.const {}) (i32.const 0) (i32.const 0) (i32.const 0)))"};

constexpr value_case producer_format{"set_proposed_producers_ex", "(param i64 i32 i32) (result i64)",
                                     "(drop (call $f (i64.const {}) (i32.const 0) (i32.const 1)))"};

constexpr value_case finalizer_format{"set_finalizers", "(param i64 i32 i32)",
                                      "(call $f (i64.const {}) (i32.const 0) (i32.const 1))"};

/// One packed argument, with the call that reads it from data_offset. `call` has a {} for its size.
struct packed_case {
   std::string_view intrinsic;
   std::string_view signature;
   std::string_view call;

   std::string with(const bytes& data) const {
      return wast_calling(intrinsic, signature, fmt::format(fmt::runtime(call), data_offset, data.size()), data);
   }
};

constexpr std::string_view one_span_signature = "(param i32 i32)";
constexpr std::string_view one_span_call      = "(call $f (i32.const {}) (i32.const {}))";

/// Calls with the packed argument in each of the three spans check_transaction_authorization reads.
constexpr std::string_view trx_auth_signature = "(param i32 i32 i32 i32 i32 i32) (result i32)";

/// The KV arguments with a closed range, on every call that takes one. One byte at address 8 serves as each key and
/// value, the empty spans start there too, and the two size outputs sit at 64 and 68.
constexpr std::string_view kv_lookup_signature = "(param i32 i64 i32 i32 i32 i32) (result i32)";
constexpr std::string_view kv_value_signature  = "(param i32 i64 i32 i32 i32 i32 i32) (result i32)";
constexpr std::string_view kv_move_signature   = "(param i32 i32 i32 i32 i32 i32 i32) (result i32)";
constexpr std::string_view kv_seek_signature   = "(param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)";
constexpr std::string_view kv_read_signature   = "(param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)";

constexpr std::array<value_case, 9> kv_table_id_calls{{
   {"kv_set", kv_value_signature,
    R"((drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)
       (i32.const 0))))"},
   {"kv_erase", "(param i32 i32 i32) (result i32)", "(drop (call $f (i32.const {}) (i32.const 8) (i32.const 1)))"},
   {"kv_get", kv_value_signature,
    R"((drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 0) (i32.const 8)
       (i32.const 1))))"},
   {"kv_idx_store", kv_lookup_signature,
    "(drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))"},
   {"kv_idx_update", "(param i32 i64 i32 i32 i32 i32 i32 i32) (result i32)",
    R"((drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)
       (i32.const 8) (i32.const 1))))"},
   {"kv_idx_erase", "(param i32 i32 i32 i32 i32) (result i32)",
    "(drop (call $f (i32.const {}) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)))"},
   {"kv_it_create", kv_lookup_signature,
    "(drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)))"},
   {"kv_idx_create", kv_lookup_signature,
    "(drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)))"},
   {"kv_idx_find", kv_lookup_signature,
    "(drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 1)))"},
}};

constexpr value_case kv_set_mode_argument{
   "kv_set", kv_value_signature,
   R"((drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)
      (i32.const {}))))"};

/// kv_set with a mode that is never valid, and the table id to fill in.
constexpr value_case kv_set_with_junk_mode{
   "kv_set", kv_value_signature,
   R"((drop (call $f (i32.const {}) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)
      (i32.const -1))))"};

constexpr std::string_view kv_move_call =
   R"((drop (call $f (i32.const {}) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0) (i32.const 64)
      (i32.const 68))))";
constexpr std::string_view kv_seek_call =
   R"((drop (call $f (i32.const {}) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)
      (i32.const 64) (i32.const 68))))";
constexpr std::string_view kv_read_call =
   R"((drop (call $f (i32.const {}) (i32.const 0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0) (i32.const 64)
      (i32.const 68))))";
constexpr std::string_view kv_destroy_call = "(call $f (i32.const {}))";

constexpr std::array<value_case, 5> kv_row_handle_calls{{{"kv_it_next", kv_move_signature, kv_move_call},
                                                         {"kv_it_prev", kv_move_signature, kv_move_call},
                                                         {"kv_it_lower_bound", kv_seek_signature, kv_seek_call},
                                                         {"kv_it_read", kv_read_signature, kv_read_call},
                                                         {"kv_it_destroy", "(param i32)", kv_destroy_call}}};

constexpr std::array<value_case, 5> kv_entry_handle_calls{{{"kv_idx_next", kv_move_signature, kv_move_call},
                                                           {"kv_idx_prev", kv_move_signature, kv_move_call},
                                                           {"kv_idx_lower_bound", kv_seek_signature, kv_seek_call},
                                                           {"kv_idx_read", kv_move_signature, kv_move_call},
                                                           {"kv_idx_destroy", "(param i32)", kv_destroy_call}}};

constexpr int32_t top_bit = std::numeric_limits<int32_t>::min();

/// Past the top table id, with the values a narrowing to uint16_t would turn into a live id.
constexpr std::array<int32_t, 4> junk_table_ids{65536, 65537, top_bit, -1};

/// Past the last kv_set mode, with the values a narrowing to a byte or a short would turn into a live mode.
constexpr std::array<int32_t, 5> junk_kv_modes{3, 256, 65536, top_bit, -1};

/**
 * The handles a call must refuse while `live` is open. Two are slots that were never opened, one is the same slot in
 * the other pool, and the rest set a reserved bit, which a check that only masked a handle down to its slot would take
 * for `live`. -1 means no handle.
 */
std::array<int32_t, 7> junk_handles(int32_t live) {
   constexpr int32_t unopened_slot = 1;
   constexpr int32_t top_slot      = kv_handle_slot_index_mask;
   constexpr int32_t low_reserved  = kv_handle_slot_index_mask + 1;
   constexpr int32_t high_reserved = kv_secondary_handle_tag << 1;
   constexpr int32_t other_pool    = kv_secondary_handle_tag;
   return {live | unopened_slot, live | top_slot, live ^ other_pool, live | low_reserved, live | high_reserved,
           live | top_bit,       -1};
}

/// The call `use` makes, after opening one iterator of each family so that slot 0 is live in both pools.
std::string wast_with_open_iterators(const value_case& use, int32_t handle) {
   return fmt::format(R"=====(
(module
 (import "env" "kv_it_create" (func $open_rows {0}))
 (import "env" "kv_idx_create" (func $open_entries {0}))
 (import "env" "{1}" (func $f {2}))
 (memory $0 1)
 (export "apply" (func $apply))
 (func $apply (param $0 i64) (param $1 i64) (param $2 i64)
  (drop (call $open_rows (i32.const 1) (get_local $0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)))
  (drop (call $open_entries (i32.const 1) (get_local $0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)))
  {3}
 )
)
)=====",
                      kv_lookup_signature, use.intrinsic, use.signature, fmt::format(fmt::runtime(use.call), handle));
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(intrinsic_value_tests)

BOOST_FIXTURE_TEST_CASE(flags_take_zero_or_one, value_tester) { try {
   for (const auto& flag : {sha3_keccak, blake2_f_final}) {
      BOOST_TEST_CONTEXT(flag.intrinsic) {
         run(flag.with(0));
         run(flag.with(1));
         for (const int32_t junk : junk_flags)
            BOOST_CHECK_EXCEPTION(run(flag.with(junk)), wasm_execution_error, fc_exception_message_contains(flag_error));
      }
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(set_privileged_takes_zero_or_one, value_tester) { try {
   const std::string call = fmt::format("(call $f (i64.const {}) (i32.const {{}}))", other_account.to_uint64_t());
   const value_case set_privileged_flag{"set_privileged", "(param i64 i32)", call};

   const auto target_is_privileged = [&] {
      return control->db().get<account_metadata_object, by_name>(other_account).is_privileged();
   };

   run(set_privileged_flag.with(1));
   BOOST_CHECK(target_is_privileged());
   run(set_privileged_flag.with(0));
   BOOST_CHECK(!target_is_privileged());
   for (const int32_t junk : junk_flags) {
      BOOST_CHECK_EXCEPTION(run(set_privileged_flag.with(junk)), wasm_execution_error,
                            fc_exception_message_contains(flag_error));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(assert_conditions_take_zero_or_one, value_tester) { try {
   for (const auto& condition : {sysio_assert_condition, sysio_assert_message_condition}) {
      BOOST_TEST_CONTEXT(condition.intrinsic) {
         run(condition.with(1));
         BOOST_CHECK_THROW(run(condition.with(0)), sysio_assert_message_exception);
         for (const int32_t junk : junk_flags) {
            BOOST_CHECK_EXCEPTION(run(condition.with(junk)), wasm_execution_error,
                                  fc_exception_message_contains(flag_error));
         }
      }
   }

   run(sysio_assert_code_condition.with(1));
   BOOST_CHECK_THROW(run(sysio_assert_code_condition.with(0)), sysio_assert_code_exception);
   for (const int32_t junk : junk_flags) {
      BOOST_CHECK_EXCEPTION(run(sysio_assert_code_condition.with(junk)), wasm_execution_error,
                            fc_exception_message_contains(flag_error));
   }
} FC_LOG_AND_RETHROW() }

#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
// A native contract's calls reach these exports without the wasm argument conversion, so each export checks its own
// flag.
BOOST_FIXTURE_TEST_CASE(native_exports_take_zero_or_one, value_tester) { try {
   constexpr uint32_t blake2_rounds   = 12;
   constexpr uint64_t assert_code     = 7;
   std::array<char, 64>  blake2_state{};
   std::array<char, 128> blake2_message{};
   std::array<char, 8>   blake2_offset{};
   std::array<char, 64>  blake2_result{};
   std::array<char, 32>  digest{};

   const auto call_blake2_f = [&](int32_t final) {
      ::blake2_f(blake2_rounds, blake2_state.data(), blake2_state.size(), blake2_message.data(), blake2_message.size(),
                 blake2_offset.data(), blake2_offset.size(), blake2_offset.data(), blake2_offset.size(), final,
                 blake2_result.data(), blake2_result.size());
   };
   const auto call_sha3 = [&](int32_t keccak) {
      ::sha3(blake2_state.data(), blake2_state.size(), digest.data(), digest.size(), keccak);
   };

   // Every junk value aborts and 1 passes. 0 is checked below on the two calls where it is not a failed assert.
   const auto check_flag = [](const auto& call) {
      for (const int32_t junk : junk_flags) {
         BOOST_TEST_CONTEXT("flag " << junk) {
            BOOST_CHECK_EXCEPTION(call(junk), wasm_execution_error, fc_exception_message_contains(flag_error));
         }
      }
      BOOST_CHECK_NO_THROW(call(1));
   };

   test_utils::with_interface(*this, call_to_contract(), [&](webassembly::interface& host, transaction_context&) {
      webassembly::native_module::native_context_stack::guard guard(&host);

      check_flag([](int32_t test) { ::sysio_assert(test, ""); });
      check_flag([](int32_t test) { ::sysio_assert_message(test, "", 0); });
      check_flag([&](int32_t test) { ::sysio_assert_code(test, assert_code); });
      check_flag(call_blake2_f);
      check_flag(call_sha3);
      BOOST_CHECK_NO_THROW(call_blake2_f(0));
      BOOST_CHECK_NO_THROW(call_sha3(0));
   });
} FC_LOG_AND_RETHROW() }
#endif

BOOST_AUTO_TEST_CASE(checked_bool_accepts_only_zero_and_one) { try {
   BOOST_CHECK(!checked_bool(0));
   BOOST_CHECK(checked_bool(1));
   for (const int32_t junk : junk_flags) {
      BOOST_CHECK_EXCEPTION(checked_bool(static_cast<uint32_t>(junk)), wasm_execution_error,
                            fc_exception_message_contains(flag_error));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(reserved_error_codes_abort_on_either_condition, value_tester) { try {
   constexpr value_case passing_assert{"sysio_assert_code", "(param i32 i64)", "(call $f (i32.const 1) (i64.const {}))"};
   constexpr value_case failing_assert{"sysio_assert_code", "(param i32 i64)", "(call $f (i32.const 0) (i64.const {}))"};
   const auto first_reserved = static_cast<int64_t>(static_cast<uint64_t>(system_error_code::generic_system_error));

   run(passing_assert.with(first_reserved - 1));
   BOOST_CHECK_THROW(run(passing_assert.with(first_reserved)), restricted_error_code_exception);
   BOOST_CHECK_THROW(run(failing_assert.with(first_reserved)), restricted_error_code_exception);
   BOOST_CHECK_THROW(run(passing_assert.with(-1)), restricted_error_code_exception);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(sha3_output_must_be_one_digest, value_tester) { try {
   constexpr value_case sha3_output{"sha3", "(param i32 i32 i32 i32 i32)",
                                    "(call $f (i32.const 0) (i32.const 0) (i32.const 64) (i32.const {}) (i32.const 1))"};
   run(sha3_output.with(32));
   for (const int32_t junk : {0, 16, 31, 33, 64}) {
      BOOST_CHECK_EXCEPTION(run(sha3_output.with(junk)), wasm_execution_error,
                            fc_exception_message_contains("sha3 output must be 32 bytes"));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(exit_code_must_be_zero, value_tester) { try {
   run(sysio_exit_code.with(0));
   for (const int32_t junk : junk_versions) {
      BOOST_CHECK_EXCEPTION(run(sysio_exit_code.with(junk)), wasm_execution_error,
                            fc_exception_message_contains("sysio_exit code must be 0"));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(struct_versions_must_be_known, value_tester) { try {
   run(code_hash_version.with(0));
   run(wasm_parameters_version.with(0));
   for (const int32_t junk : junk_versions) {
      BOOST_CHECK_EXCEPTION(run(code_hash_version.with(junk)), wasm_execution_error,
                            fc_exception_message_contains("Unknown struct version"));
      BOOST_CHECK_THROW(run(wasm_parameters_version.with(junk)), wasm_config_unknown_version);
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(closed_sets_reject_the_next_value, value_tester) { try {
   run(action_type.with(0));
   run(action_type.with(1));
   BOOST_CHECK_THROW(run(action_type.with(2)), action_not_found_exception);

   constexpr int64_t past_32_bits = int64_t{1} << 32;
   for (const int64_t junk : {int64_t{2}, past_32_bits}) {
      BOOST_CHECK_EXCEPTION(run(producer_format.with(junk)), wasm_execution_error,
                            fc_exception_message_contains("unknown format"));
   }
   for (const int64_t junk : {int64_t{1}, past_32_bits}) {
      BOOST_CHECK_EXCEPTION(run(finalizer_format.with(junk)), wasm_execution_error,
                            fc_exception_message_contains("unknown format"));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(packed_arguments_reject_trailing_bytes, value_tester) { try {
   const auto& gpo = control->get_global_properties();
   const vector<permission_level> contract_active{{contract, config::active_name}};
   const action self_action{contract_active, contract, "x"_n, bytes{}};
   const action free_action{vector<permission_level>{}, contract, "x"_n, bytes{}};

   const bytes wasm_config_bytes = fc::raw::pack(gpo.wasm_configuration);
   const auto versioned_wasm_parameters = [&](uint32_t version) {
      bytes packed = fc::raw::pack(version);
      packed.insert(packed.end(), wasm_config_bytes.begin(), wasm_config_bytes.end());
      return packed;
   };

   const packed_case set_wasm_parameters{"set_wasm_parameters_packed", one_span_signature, one_span_call};
   BOOST_CHECK_THROW(run(set_wasm_parameters.with(versioned_wasm_parameters(1))), wasm_config_unknown_version);

   const packed_case send_inline{"send_inline", one_span_signature, one_span_call};
   const packed_case send_context_free_inline{"send_context_free_inline", one_span_signature, one_span_call};
   const packed_case set_blockchain_parameters{"set_blockchain_parameters_packed", one_span_signature, one_span_call};
   const packed_case set_parameters{"set_parameters_packed", one_span_signature, one_span_call};

   bytes wasm_parameters = versioned_wasm_parameters(0);
   wasm_parameters.push_back(junk_byte);

   const chain_config_v0 blockchain_parameters = gpo.configuration;
   constexpr std::string_view producers_ex_call = "(drop (call $f (i64.const 1) (i32.const {}) (i32.const {})))";
   constexpr std::string_view producers_call    = "(drop (call $f (i32.const {}) (i32.const {})))";
   constexpr std::string_view finalizers_call   = "(call $f (i64.const 0) (i32.const {}) (i32.const {}))";
   constexpr std::string_view parameters_call =
      "(drop (call $f (i32.const {}) (i32.const {}) (i32.const 0) (i32.const 0)))";

   const packed_case get_parameters{"get_parameters_packed", "(param i32 i32 i32 i32) (result i32)", parameters_call};
   const packed_case set_proposed_producers{"set_proposed_producers", "(param i32 i32) (result i64)", producers_call};
   const packed_case set_proposed_producers_ex{"set_proposed_producers_ex", "(param i64 i32 i32) (result i64)",
                                               producers_ex_call};
   const packed_case set_finalizers{"set_finalizers", "(param i64 i32 i32)", finalizers_call};

   const std::vector<legacy::producer_key> no_producer_keys;
   const std::vector<producer_authority> no_producers;
   const std::vector<fc::unsigned_int> no_ids;
   // threshold, then an empty finalizer list
   const bytes empty_finalizer_policy(sizeof(uint64_t) + 1, junk_byte);
   bytes finalizer_policy = empty_finalizer_policy;
   finalizer_policy.push_back(junk_byte);

   const std::vector<std::pair<packed_case, bytes>> cases{
      {send_inline, pack_with_junk(self_action)},
      {send_context_free_inline, pack_with_junk(free_action)},
      {set_wasm_parameters, wasm_parameters},
      {set_blockchain_parameters, pack_with_junk(blockchain_parameters)},
      {set_parameters, pack_with_junk(no_ids)},
      {get_parameters, pack_with_junk(no_ids)},
      {set_proposed_producers, pack_with_junk(no_producer_keys)},
      {set_proposed_producers_ex, pack_with_junk(no_producers)},
      {set_finalizers, finalizer_policy},
   };

   for (const auto& [argument, data] : cases) {
      BOOST_TEST_CONTEXT(argument.intrinsic) {
         BOOST_CHECK_EXCEPTION(run(argument.with(data)), wasm_execution_error,
                               fc_exception_message_contains(trailing_error));
      }
   }

   // The loop above would also pass against a check that refused everything, so each argument has to read at its exact
   // size. The inline actions go to other_account, as a contract sending itself this call would repeat it forever.
   const action target_action{contract_active, other_account, "x"_n, bytes{}};
   const action free_target_action{vector<permission_level>{}, other_account, "x"_n, bytes{}};
   const std::vector<std::pair<packed_case, bytes>> exact_cases{
      {send_inline, fc::raw::pack(target_action)},
      {send_context_free_inline, fc::raw::pack(free_target_action)},
      {set_wasm_parameters, versioned_wasm_parameters(0)},
      {set_blockchain_parameters, fc::raw::pack(blockchain_parameters)},
      {set_parameters, fc::raw::pack(no_ids)},
      {get_parameters, fc::raw::pack(no_ids)},
   };
   for (const auto& [argument, data] : exact_cases) {
      BOOST_TEST_CONTEXT(argument.intrinsic) {
         BOOST_CHECK_NO_THROW(run(argument.with(data)));
      }
   }

   // The empty lists read at their exact size too, and stop at the validation behind the trailing check.
   BOOST_CHECK_EXCEPTION(run(set_proposed_producers.with(fc::raw::pack(no_producer_keys))), wasm_execution_error,
                         fc_exception_message_contains("producer schedule must not be empty"));
   BOOST_CHECK_EXCEPTION(run(set_proposed_producers_ex.with(fc::raw::pack(no_producers))), wasm_execution_error,
                         fc_exception_message_contains("producer schedule must not be empty"));
   BOOST_CHECK_EXCEPTION(run(set_finalizers.with(empty_finalizer_policy)), wasm_execution_error,
                         fc_exception_message_contains("finalizers must not be empty"));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(authorization_arguments_reject_trailing_bytes, value_tester) { try {
   const bytes trx          = fc::raw::pack(transaction{});
   const bytes empty_list   = fc::raw::pack(flat_set<public_key_type>{});
   const bytes junk_trx     = pack_with_junk(transaction{});
   const bytes junk_list    = pack_with_junk(flat_set<public_key_type>{});

   // Lays out trx, then keys, then permissions at data_offset and passes each span.
   const auto check_authorization = [&](const bytes& trx_data, const bytes& keys, const bytes& permissions) {
      bytes data = trx_data;
      data.insert(data.end(), keys.begin(), keys.end());
      data.insert(data.end(), permissions.begin(), permissions.end());
      const uint32_t keys_offset        = data_offset + trx_data.size();
      const uint32_t permissions_offset = keys_offset + keys.size();
      return wast_calling("check_transaction_authorization", trx_auth_signature,
                          fmt::format("(drop (call $f (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) "
                                      "(i32.const {}) (i32.const {})))",
                                      data_offset, trx_data.size(), keys_offset, keys.size(), permissions_offset,
                                      permissions.size()),
                          data);
   };

   run(check_authorization(trx, empty_list, empty_list));
   BOOST_CHECK_EXCEPTION(run(check_authorization(junk_trx, empty_list, empty_list)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));
   BOOST_CHECK_EXCEPTION(run(check_authorization(trx, junk_list, empty_list)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));
   BOOST_CHECK_EXCEPTION(run(check_authorization(trx, empty_list, junk_list)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));

   transaction with_extension;
   with_extension.transaction_extensions.emplace_back(uint16_t{0}, std::vector<char>{});
   BOOST_CHECK_THROW(run(check_authorization(fc::raw::pack(with_extension), empty_list, empty_list)),
                     invalid_transaction_extension);

   const auto check_permission = [&](const bytes& keys, const bytes& permissions) {
      bytes data = keys;
      data.insert(data.end(), permissions.begin(), permissions.end());
      return wast_calling("check_permission_authorization", "(param i64 i64 i32 i32 i32 i32) (result i32)",
                          fmt::format("(drop (call $f (get_local $0) (i64.const {}) (i32.const {}) (i32.const {}) "
                                      "(i32.const {}) (i32.const {})))",
                                      static_cast<int64_t>(config::active_name.to_uint64_t()), data_offset, keys.size(),
                                      data_offset + keys.size(), permissions.size()),
                          data);
   };

   run(check_permission(empty_list, empty_list));
   BOOST_CHECK_EXCEPTION(run(check_permission(junk_list, empty_list)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));
   BOOST_CHECK_EXCEPTION(run(check_permission(empty_list, junk_list)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));
} FC_LOG_AND_RETHROW() }

// A valid WebAuthn key reaches the check that it is not the signature's type, so that error shows the key unpacked.
BOOST_FIXTURE_TEST_CASE(webauthn_keys_reject_unknown_user_presence, value_tester) { try {
   using fc::crypto::webauthn::public_key;
   constexpr std::size_t compressed_key_size = 33;

   const auto key        = get_private_key(contract, "active");
   const bytes signature = fc::raw::pack(key.sign(digest_type{}));

   const auto assert_recover = [&](uint8_t presence) {
      bytes data = signature;
      data.push_back(static_cast<char>(magic_enum::enum_integer(fc::crypto::public_key::key_type::wa)));
      data.resize(data.size() + compressed_key_size, 0);
      data.push_back(static_cast<char>(presence));
      const bytes rpid = fc::raw::pack(std::string{"wire.network"});
      data.insert(data.end(), rpid.begin(), rpid.end());
      return wast_calling("assert_recover_key", "(param i32 i32 i32 i32 i32)",
                          fmt::format("(call $f (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))",
                                      digest_offset, data_offset, signature.size(), data_offset + signature.size(),
                                      data.size() - signature.size()),
                          data);
   };

   for (const auto presence : magic_enum::enum_values<public_key::user_presence_t>())
      BOOST_CHECK_THROW(run(assert_recover(magic_enum::enum_integer(presence))), crypto_api_exception);

   const uint8_t next_value = magic_enum::enum_count<public_key::user_presence_t>();
   for (const uint8_t junk : {next_value, uint8_t{255}}) {
      BOOST_CHECK_EXCEPTION(run(assert_recover(junk)), fc::assert_exception,
                            fc_exception_message_contains("unknown user presence"));
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(signature_arguments_reject_trailing_bytes, value_tester) { try {
   const auto key         = get_private_key(contract, "active");
   const bytes signature  = fc::raw::pack(key.sign(digest_type{}));
   const bytes public_key = fc::raw::pack(key.get_public_key());
   const bytes junk_signature  = pack_with_junk(key.sign(digest_type{}));
   const bytes junk_public_key = pack_with_junk(key.get_public_key());

   const auto assert_recover = [&](const bytes& sig, const bytes& pub) {
      bytes data = sig;
      data.insert(data.end(), pub.begin(), pub.end());
      return wast_calling("assert_recover_key", "(param i32 i32 i32 i32 i32)",
                          fmt::format("(call $f (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))",
                                      digest_offset, data_offset, sig.size(), data_offset + sig.size(), pub.size()),
                          data);
   };

   run(assert_recover(signature, public_key));
   BOOST_CHECK_EXCEPTION(run(assert_recover(junk_signature, public_key)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));
   BOOST_CHECK_EXCEPTION(run(assert_recover(signature, junk_public_key)), wasm_execution_error,
                         fc_exception_message_contains(trailing_error));
} FC_LOG_AND_RETHROW() }

// recover_key reports a malformed signature as -1 so a contract can refuse untrusted input without aborting.
BOOST_FIXTURE_TEST_CASE(recover_key_returns_failure_on_trailing_bytes, value_tester) { try {
   const auto recover = [](const bytes& sig, std::string_view comparison) {
      const auto call = fmt::format(
         "(if ({} (call $recover (i32.const {}) (i32.const {}) (i32.const {}) (i32.const 512) (i32.const 128)) "
         "(i32.const -1)) (then (unreachable)))",
         comparison, digest_offset, data_offset, sig.size());
      return fmt::format(R"=====(
(module
 (import "env" "recover_key" (func $recover (param i32 i32 i32 i32 i32) (result i32)))
 (memory $0 1)
 (data (i32.const {}) "{}")
 (export "apply" (func $apply))
 (func $apply (param $0 i64) (param $1 i64) (param $2 i64)
  {}
 )
)
)=====",
                         data_offset, to_wast_string(sig), call);
   };

   constexpr std::string_view recovered     = "i32.eq"; // traps if recover_key failed
   constexpr std::string_view not_recovered = "i32.ne"; // traps if recover_key succeeded

   const auto key = get_private_key(contract, "active");
   run(recover(fc::raw::pack(key.sign(digest_type{})), recovered));
   run(recover(pack_with_junk(key.sign(digest_type{})), not_recovered));
} FC_LOG_AND_RETHROW() }

// Each of these has a host check already. They hold every call to it, with the values a narrowing would let through.
BOOST_FIXTURE_TEST_CASE(kv_table_ids_stop_at_the_top, value_tester) { try {
   constexpr int32_t top_table_id = std::numeric_limits<uint16_t>::max();
   for (const auto& call : kv_table_id_calls) {
      BOOST_TEST_CONTEXT(call.intrinsic) {
         run(call.with(0));
         run(call.with(top_table_id));
         for (const int32_t junk : junk_table_ids)
            BOOST_CHECK_THROW(run(call.with(junk)), kv_invalid_table_id);
      }
   }
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(kv_set_modes_are_a_closed_set, value_tester) { try {
   for (const auto mode : magic_enum::enum_values<kv_set_mode>())
      run(kv_set_mode_argument.with(magic_enum::enum_integer(mode)));
   for (const int32_t junk : junk_kv_modes)
      BOOST_CHECK_THROW(run(kv_set_mode_argument.with(junk)), kv_invalid_mode);
} FC_LOG_AND_RETHROW() }

// Every node has to abort with the same exception, so with both arguments bad the table id is the one reported.
BOOST_FIXTURE_TEST_CASE(kv_set_checks_the_table_id_first, value_tester) { try {
   for (const int32_t junk : junk_table_ids)
      BOOST_CHECK_THROW(run(kv_set_with_junk_mode.with(junk)), kv_invalid_table_id);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(kv_handles_are_checked_on_every_call, value_tester) { try {
   const auto check_calls = [&](const auto& calls, int32_t live) {
      for (const auto& call : calls) {
         BOOST_TEST_CONTEXT(call.intrinsic) {
            run(wast_with_open_iterators(call, live));
            for (const int32_t junk : junk_handles(live)) {
               BOOST_TEST_CONTEXT("handle " << junk) {
                  BOOST_CHECK_THROW(run(wast_with_open_iterators(call, junk)), kv_invalid_iterator);
               }
            }
         }
      }
   };
   constexpr int32_t first_row_handle   = 0;
   constexpr int32_t first_entry_handle = kv_secondary_handle_tag;
   check_calls(kv_row_handle_calls, first_row_handle);
   check_calls(kv_entry_handle_calls, first_entry_handle);
} FC_LOG_AND_RETHROW() }

// `code` has no closed range: any uint64 is a name an account could hold, and one with no account is a plain miss.
BOOST_FIXTURE_TEST_CASE(kv_reads_take_any_code, value_tester) { try {
   constexpr value_case get_miss{
      "kv_get", kv_value_signature,
      R"((if (i32.ne (call $f (i32.const 1) (i64.const {}) (i32.const 8) (i32.const 1) (i32.const 0) (i32.const 8)
         (i32.const 1)) (i32.const -1)) (then (unreachable))))"};
   constexpr value_case find_miss{
      "kv_idx_find", kv_lookup_signature,
      R"((if (i32.ne (call $f (i32.const 1) (i64.const {}) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 1))
         (i32.const -1)) (then (unreachable))))"};
   constexpr value_case open_rows{
      "kv_it_create", kv_lookup_signature,
      "(drop (call $f (i32.const 1) (i64.const {}) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)))"};
   constexpr value_case open_entries{
      "kv_idx_create", kv_lookup_signature,
      "(drop (call $f (i32.const 1) (i64.const {}) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 0)))"};

   for (const int64_t code : {int64_t{0}, int64_t{1}, std::numeric_limits<int64_t>::min(), int64_t{-1}}) {
      for (const auto& call : {get_miss, find_miss, open_rows, open_entries}) {
         BOOST_TEST_CONTEXT(call.intrinsic << " code " << code) {
            run(call.with(code));
         }
      }
   }
} FC_LOG_AND_RETHROW() }

// No row can have an empty key, so a call that passes one aborts rather than misses.
BOOST_FIXTURE_TEST_CASE(kv_keys_must_not_be_empty, value_tester) { try {
   constexpr std::array<value_case, 7> empty_key_calls{{
      {"kv_get", kv_value_signature,
       R"((drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const {}) (i32.const 0) (i32.const 8)
          (i32.const 1))))"},
      {"kv_idx_update", "(param i32 i64 i32 i32 i32 i32 i32 i32) (result i32)",
       R"((drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const {}) (i32.const 8) (i32.const 1)
          (i32.const 8) (i32.const 1))))"},
      {"kv_idx_update", "(param i32 i64 i32 i32 i32 i32 i32 i32) (result i32)",
       R"((drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const {})
          (i32.const 8) (i32.const 1))))"},
      {"kv_idx_update", "(param i32 i64 i32 i32 i32 i32 i32 i32) (result i32)",
       R"((drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const 1)
          (i32.const 8) (i32.const {}))))"},
      {"kv_idx_erase", "(param i32 i32 i32 i32 i32) (result i32)",
       "(drop (call $f (i32.const 1) (i32.const 8) (i32.const {}) (i32.const 8) (i32.const 1)))"},
      {"kv_idx_erase", "(param i32 i32 i32 i32 i32) (result i32)",
       "(drop (call $f (i32.const 1) (i32.const 8) (i32.const 1) (i32.const 8) (i32.const {})))"},
      {"kv_idx_find", kv_lookup_signature,
       "(drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const 0) (i32.const 8) (i32.const {})))"},
   }};
   constexpr int64_t empty = 0, one_byte = 1;
   for (const auto& call : empty_key_calls) {
      BOOST_TEST_CONTEXT(call.call) {
         run(call.with(one_byte));
         BOOST_CHECK_THROW(run(call.with(empty)), kv_empty_key);
      }
   }
} FC_LOG_AND_RETHROW() }

// A prefix or a seek is not a key, so these stay legal.
BOOST_FIXTURE_TEST_CASE(kv_prefix_and_seek_may_be_empty, value_tester) { try {
   constexpr std::array<value_case, 3> open_calls{{
      {"kv_it_create", kv_lookup_signature,
       "(drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const {0}) (i32.const 8) (i32.const {0})))"},
      {"kv_idx_create", kv_lookup_signature,
       "(drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const {0}) (i32.const 8) (i32.const {0})))"},
      {"kv_idx_find", kv_lookup_signature,
       "(drop (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const {0}) (i32.const 8) (i32.const 1)))"},
   }};
   for (const auto& call : open_calls)
      run(call.with(0));

   // A seek of "b" under a prefix of "a".
   const bytes letters{'a', 'b'};
   run(wast_calling("kv_it_create", kv_lookup_signature,
                    fmt::format("(drop (call $f (i32.const 1) (get_local $0) (i32.const {}) (i32.const 1) "
                                "(i32.const {}) (i32.const 1)))",
                                data_offset, data_offset + 1),
                    letters));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(kv_idx_find_key_must_start_with_the_prefix, value_tester) { try {
   constexpr uint32_t key_offset = data_offset;
   constexpr std::string_view prefix_bytes = "ab";
   const auto find = [&](uint32_t prefix_size, uint32_t key_at, uint32_t key_size) {
      return wast_calling("kv_idx_find", kv_lookup_signature,
                          fmt::format("(drop (call $f (i32.const 1) (get_local $0) (i32.const {}) (i32.const {}) "
                                      "(i32.const {}) (i32.const {})))",
                                      key_offset, prefix_size, key_at, key_size),
                          bytes(prefix_bytes.begin(), prefix_bytes.end()));
   };
   run(find(2, key_offset, 2));                                     // the key is the prefix
   run(find(1, key_offset, 2));                                     // the key extends it
   BOOST_CHECK_THROW(run(find(2, key_offset, 1)), kv_key_outside_prefix); // shorter than the prefix
   BOOST_CHECK_THROW(run(find(1, key_offset + 1, 1)), kv_key_outside_prefix); // "b" lacks "a"
} FC_LOG_AND_RETHROW() }

// A key over the absolute limit still looks up as a plain miss: no row can hold it, but no check needs to say so.
BOOST_FIXTURE_TEST_CASE(kv_lookup_past_the_key_limit_is_a_miss, value_tester) { try {
   constexpr value_case get_long_key{
      "kv_get", kv_value_signature,
      R"((if (i32.ne (call $f (i32.const 1) (get_local $0) (i32.const 8) (i32.const {}) (i32.const 0) (i32.const 8)
         (i32.const 1)) (i32.const -1)) (then (unreachable))))"};
   run(get_long_key.with(int64_t{config::max_kv_key_size_limit} + 1));
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
