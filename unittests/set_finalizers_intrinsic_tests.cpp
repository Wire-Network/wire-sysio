// Direct negative-path coverage for the set_finalizers host function and
// finalizer_policy::validate() (invoked from set_finalizers). The sysio.bios
// setfinalizer action performs its own validation before the intrinsic, so the
// high-level testing helpers never exercise the intrinsic's checks. These tests
// deploy a small WAST contract that forwards the action payload directly into
// set_finalizers, letting us inject hand-packed bytes that trigger each check.

#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/testing/bls_utils.hpp>

#include <fc/io/raw.hpp>

#include <boost/test/unit_test.hpp>

using namespace sysio::testing;
using namespace sysio::chain;

namespace {

// Mirrors the wire format that set_finalizers unpacks. Must match the struct
// declared inside libraries/chain/webassembly/privileged.cpp exactly.
struct abi_finalizer_authority {
   std::string            description;
   uint64_t               weight = 0;
   std::vector<uint8_t>   public_key;
};

struct abi_finalizer_policy {
   uint64_t                             threshold = 0;
   std::vector<abi_finalizer_authority> finalizers;
};

} // namespace

FC_REFLECT(abi_finalizer_authority, (description)(weight)(public_key))
FC_REFLECT(abi_finalizer_policy, (threshold)(finalizers))

namespace {

// Forwards action payload bytes directly into set_finalizers with format=0.
// `apply(recv, code, action)` receives the action via the WASM ABI; the action
// data is whatever we pass to push_action, read via read_action_data / action_data_size.
constexpr const char set_finalizers_forward_wast[] = R"=====(
(module
 (import "env" "set_finalizers"      (func $set_finalizers      (param i64 i32 i32)))
 (import "env" "read_action_data"    (func $read_action_data    (param i32 i32) (result i32)))
 (import "env" "action_data_size"    (func $action_data_size    (result i32)))
 (memory $0 1)
 (export "apply" (func $apply))
 (func $apply (param $0 i64) (param $1 i64) (param $2 i64)
   (drop (call $read_action_data (i32.const 0) (call $action_data_size)))
   (call $set_finalizers (i64.const 0) (i32.const 0) (call $action_data_size))
 )
)
)=====";

// Forwards with format=1 to exercise the "unknown format" error path.
constexpr const char set_finalizers_bad_format_wast[] = R"=====(
(module
 (import "env" "set_finalizers"      (func $set_finalizers      (param i64 i32 i32)))
 (import "env" "read_action_data"    (func $read_action_data    (param i32 i32) (result i32)))
 (import "env" "action_data_size"    (func $action_data_size    (result i32)))
 (memory $0 1)
 (export "apply" (func $apply))
 (func $apply (param $0 i64) (param $1 i64) (param $2 i64)
   (drop (call $read_action_data (i32.const 0) (call $action_data_size)))
   (call $set_finalizers (i64.const 1) (i32.const 0) (call $action_data_size))
 )
)
)=====";

// Returns a valid 96-byte BLS public key derived from a name, in the
// affine-little-endian non-montgomery format expected by the intrinsic. The
// intrinsic calls `bls_public_key(std::span<const uint8_t,96>)` on the input
// bytes before validate() runs, which asserts on curve membership, so the
// negative tests for validate()-level checks must supply real keys.
// get_bls_key dispatches on the variant: `name` → SHA256(name) as seed.
std::vector<uint8_t> pk_for(sysio::chain::name key_name) {
   auto [_priv, pub, _pop, _sig] = get_bls_key(key_name);
   auto serialized = pub.serialize();
   return std::vector<uint8_t>(serialized.begin(), serialized.end());
}

struct set_finalizers_fixture {
   tester chain{setup_policy::preactivate_feature_and_new_bios};
   account_name alice_account{"alice"_n};

   set_finalizers_fixture() {
      chain.create_accounts({alice_account});
      chain.produce_block();
   }

   // set_privileged requires the account to have code deployed first.
   void set_code_and_produce(const char* wast) {
      chain.set_code(alice_account, wast);
      chain.set_privileged(alice_account);
      chain.produce_block();
   }

   // Push an action on alice carrying `payload` as its data. Returns the trace.
   transaction_trace_ptr push_payload(const std::vector<char>& payload) {
      action a({{alice_account, permission_name("active")}},
               alice_account, action_name(), payload);
      signed_transaction trx;
      trx.actions.emplace_back(std::move(a));
      chain.set_transaction_headers(trx);
      trx.sign(chain.get_private_key(alice_account, "active"), chain.control->get_chain_id());
      return chain.push_transaction(trx);
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(set_finalizers_intrinsic_tests)

// packed_finalizer_format != 0 rejected by the intrinsic.
BOOST_FIXTURE_TEST_CASE(unknown_format_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_bad_format_wast);

   // Any bytes work — the format check fires before unpack.
   abi_finalizer_policy pol;
   pol.threshold = 1;
   pol.finalizers.push_back({.description = "f", .weight = 1, .public_key = pk_for("k1"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("unknown format"));
} FC_LOG_AND_RETHROW()

// public_key.size() != 96 rejected at the wire-format guard before validate().
BOOST_FIXTURE_TEST_CASE(bad_pubkey_length_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 1;
   // 95-byte key — invalid BLS size. The wire-format guard fires before curve validation.
   std::vector<uint8_t> short_pk(95, 0x42);
   pol.finalizers.push_back({.description = "f", .weight = 1, .public_key = std::move(short_pk)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("Invalid bls public key length"));
} FC_LOG_AND_RETHROW()

// Empty finalizers rejected by validate().
BOOST_FIXTURE_TEST_CASE(empty_finalizers_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 1;
   // No finalizers.
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("finalizers must not be empty"));
} FC_LOG_AND_RETHROW()

// finalizers.size() > config::max_finalizers rejected by validate().
// Tested directly against finalizer_policy::validate() rather than through the
// WAST forwarder: max_finalizers = 65536 produces a ~7MB packed payload, which
// exceeds the default max_transaction_net_usage (512KB). The intrinsic wiring
// is already exercised by the other tests in this suite; this test verifies the
// specific branch in validate().
BOOST_AUTO_TEST_CASE(validate_rejects_too_many_finalizers) try {
   finalizer_policy pol;
   const size_t n = config::max_finalizers + 1;
   pol.generation = 1;
   pol.threshold = n; // satisfy BFT invariant so only the count check can fire
   pol.finalizers.reserve(n);
   auto [_priv, pub, _pop, _sig] = get_bls_key("shared"_n);
   for (size_t i = 0; i < n; ++i) {
      pol.finalizers.push_back({.description = "f", .weight = 1, .public_key = pub});
   }
   BOOST_CHECK_EXCEPTION(pol.validate(), invalid_finalizer_policy_exception,
      fc_exception_message_contains("exceeds max"));
} FC_LOG_AND_RETHROW()

// The default-constructed (all-zero) key is the BLS identity / point at
// infinity; validate() must reject it as a finalizer key.
BOOST_AUTO_TEST_CASE(validate_rejects_identity_public_key) try {
   finalizer_policy pol;
   pol.generation = 1;
   pol.threshold = 1;
   pol.finalizers.push_back({.description = "f", .weight = 1, .public_key = fc::crypto::bls::public_key{}});
   BOOST_CHECK_EXCEPTION(pol.validate(), invalid_finalizer_policy_exception,
      fc_exception_message_contains("identity point"));
} FC_LOG_AND_RETHROW()

// The all-zero 96-byte encoding deserializes to the identity point WITHOUT a
// curve-membership check (the zero encoding short-circuits it), so the
// bls_public_key constructor in set_finalizers does not throw for it. The
// validate() identity check is what rejects it at the intrinsic boundary.
BOOST_FIXTURE_TEST_CASE(identity_public_key_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 1;
   pol.finalizers.push_back({.description = "f", .weight = 1, .public_key = std::vector<uint8_t>(96, 0)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("identity point"));
} FC_LOG_AND_RETHROW()

// description.size() > config::max_finalizer_description_size rejected by validate().
BOOST_FIXTURE_TEST_CASE(description_too_long_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 1;
   pol.finalizers.push_back({
      .description = std::string(config::max_finalizer_description_size + 1, 'x'),
      .weight = 1,
      .public_key = pk_for("k1"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("description size"));
} FC_LOG_AND_RETHROW()

// weight == 0 rejected by validate().
BOOST_FIXTURE_TEST_CASE(zero_weight_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 1;
   pol.finalizers.push_back({.description = "f", .weight = 0, .public_key = pk_for("k1"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("weight must be positive"));
} FC_LOG_AND_RETHROW()

// uint64 weight-sum overflow rejected by validate().
BOOST_FIXTURE_TEST_CASE(weight_overflow_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   // threshold arbitrary but satisfy BFT if no overflow (won't be reached).
   pol.threshold = 1;
   pol.finalizers.push_back({.description = "f", .weight = std::numeric_limits<uint64_t>::max(),
                             .public_key = pk_for("k1"_n)});
   pol.finalizers.push_back({.description = "g", .weight = 1,
                             .public_key = pk_for("k2"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("sum of weights overflows"));
} FC_LOG_AND_RETHROW()

// Duplicate public_key rejected by validate().
BOOST_FIXTURE_TEST_CASE(duplicate_pubkey_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 2;
   auto pk = pk_for("dup"_n);
   pol.finalizers.push_back({.description = "a", .weight = 1, .public_key = pk});
   pol.finalizers.push_back({.description = "b", .weight = 1, .public_key = pk});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("duplicate public_key"));
} FC_LOG_AND_RETHROW()

// threshold == 0 rejected (0 is not > 0/2 for sum > 0).
BOOST_FIXTURE_TEST_CASE(threshold_zero_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 0;
   pol.finalizers.push_back({.description = "f", .weight = 3, .public_key = pk_for("k1"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("threshold"));
} FC_LOG_AND_RETHROW()

// threshold > sum rejected.
BOOST_FIXTURE_TEST_CASE(threshold_exceeds_sum_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 10;
   pol.finalizers.push_back({.description = "f", .weight = 3, .public_key = pk_for("k1"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("threshold"));
} FC_LOG_AND_RETHROW()

// threshold == sum/2 exactly (BFT boundary) rejected.
// For sum=4, threshold=2 fails: 2 > 4/2 = 2 is false.
BOOST_FIXTURE_TEST_CASE(threshold_at_half_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 2;
   pol.finalizers.push_back({.description = "f", .weight = 2, .public_key = pk_for("k1"_n)});
   pol.finalizers.push_back({.description = "g", .weight = 2, .public_key = pk_for("k2"_n)});
   auto bytes = fc::raw::pack(pol);

   BOOST_CHECK_EXCEPTION(push_payload(bytes), wasm_execution_error,
      fc_exception_message_contains("threshold"));
} FC_LOG_AND_RETHROW()

// Happy path — confirms the forwarding contract actually reaches validate() and
// that a well-formed policy is accepted. This guards against the negative tests
// all passing for the wrong reason (e.g. the fixture itself rejecting the
// action before the intrinsic is reached).
BOOST_FIXTURE_TEST_CASE(well_formed_policy_accepted_test, set_finalizers_fixture) try {
   set_code_and_produce(set_finalizers_forward_wast);

   abi_finalizer_policy pol;
   pol.threshold = 2; // > sum/2 = 1, <= sum = 2
   pol.finalizers.push_back({.description = "a", .weight = 1, .public_key = pk_for("k1"_n)});
   pol.finalizers.push_back({.description = "b", .weight = 1, .public_key = pk_for("k2"_n)});
   auto bytes = fc::raw::pack(pol);

   // Should not throw. push_payload returns a trace — any exception during intrinsic
   // would have been re-raised as wasm_execution_error.
   BOOST_REQUIRE_NO_THROW(push_payload(bytes));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
