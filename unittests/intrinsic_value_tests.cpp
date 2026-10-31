#include <sysio/chain/account_object.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/webassembly/common.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/io/raw.hpp>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include <boost/test/unit_test.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

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

/// Escapes bytes for a wast data segment.
std::string to_wast_string(const bytes& data) {
   std::string escaped;
   escaped.reserve(data.size() * 3);
   for (const char c : data)
      fmt::format_to(std::back_inserter(escaped), "\\{:02x}", static_cast<uint8_t>(c));
   return escaped;
}

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

   /// Deploys the wast and pushes one action to it.
   void run(const std::string& wast) {
      set_code(contract, wast.c_str());
      produce_block();
      signed_transaction trx;
      trx.actions.emplace_back(vector<permission_level>{{contract, config::active_name}}, contract, ""_n, bytes{});
      set_transaction_headers(trx);
      trx.sign(get_private_key(contract, "active"), get_chain_id());
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

constexpr value_case permission_delay{"check_permission_authorization",
                                      "(param i64 i64 i32 i32 i32 i32 i64) (result i32)",
                                      R"((drop (call $f (get_local $0) (i64.const 0) (i32.const 0) (i32.const 0)
                                         (i32.const 0) (i32.const 0) (i64.const {}))))"};

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
   BOOST_CHECK_THROW(run(permission_delay.with(1)), action_validate_exception);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(packed_arguments_reject_trailing_bytes, value_tester) { try {
   const auto& gpo = control->get_global_properties();
   const action self_action{vector<permission_level>{{contract, config::active_name}}, contract, "x"_n, bytes{}};
   const action free_action{vector<permission_level>{}, contract, "x"_n, bytes{}};

   const bytes wasm_config_bytes = fc::raw::pack(gpo.wasm_configuration);
   const auto versioned_wasm_parameters = [&](uint32_t version) {
      bytes packed = fc::raw::pack(version);
      packed.insert(packed.end(), wasm_config_bytes.begin(), wasm_config_bytes.end());
      return packed;
   };

   const packed_case set_wasm_parameters{"set_wasm_parameters_packed", one_span_signature, one_span_call};
   BOOST_CHECK_THROW(run(set_wasm_parameters.with(versioned_wasm_parameters(1))), wasm_config_unknown_version);

   bytes wasm_parameters = versioned_wasm_parameters(0);
   wasm_parameters.push_back(junk_byte);

   const chain_config_v0 blockchain_parameters = gpo.configuration;
   constexpr std::string_view producers_ex_call = "(drop (call $f (i64.const 1) (i32.const {}) (i32.const {})))";
   constexpr std::string_view producers_call    = "(drop (call $f (i32.const {}) (i32.const {})))";
   constexpr std::string_view finalizers_call   = "(call $f (i64.const 0) (i32.const {}) (i32.const {}))";
   constexpr std::string_view parameters_call =
      "(drop (call $f (i32.const {}) (i32.const {}) (i32.const 0) (i32.const 0)))";

   const std::vector<legacy::producer_key> no_producer_keys;
   const std::vector<producer_authority> no_producers;
   const std::vector<fc::unsigned_int> no_ids;
   // threshold, then an empty finalizer list, then the junk byte
   const bytes finalizer_policy(sizeof(uint64_t) + 2, junk_byte);

   const std::vector<std::pair<packed_case, bytes>> cases{
      {{"send_inline", one_span_signature, one_span_call}, pack_with_junk(self_action)},
      {{"send_context_free_inline", one_span_signature, one_span_call}, pack_with_junk(free_action)},
      {set_wasm_parameters, wasm_parameters},
      {{"set_blockchain_parameters_packed", one_span_signature, one_span_call}, pack_with_junk(blockchain_parameters)},
      {{"set_parameters_packed", one_span_signature, one_span_call}, pack_with_junk(no_ids)},
      {{"get_parameters_packed", "(param i32 i32 i32 i32) (result i32)", parameters_call}, pack_with_junk(no_ids)},
      {{"set_proposed_producers", "(param i32 i32) (result i64)", producers_call}, pack_with_junk(no_producer_keys)},
      {{"set_proposed_producers_ex", "(param i64 i32 i32) (result i64)", producers_ex_call}, pack_with_junk(no_producers)},
      {{"set_finalizers", "(param i64 i32 i32)", finalizers_call}, finalizer_policy},
   };

   for (const auto& [argument, data] : cases) {
      BOOST_TEST_CONTEXT(argument.intrinsic) {
         BOOST_CHECK_EXCEPTION(run(argument.with(data)), wasm_execution_error,
                               fc_exception_message_contains(trailing_error));
      }
   }
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
      return wast_calling("check_permission_authorization", "(param i64 i64 i32 i32 i32 i32 i64) (result i32)",
                          fmt::format("(drop (call $f (get_local $0) (i64.const {}) (i32.const {}) (i32.const {}) "
                                      "(i32.const {}) (i32.const {}) (i64.const 0)))",
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

BOOST_AUTO_TEST_SUITE_END()
