#include "test-contracts/ed25519_verify_test/test_config.hpp"
#include "test_utils.hpp"
#include <crypto/ed25519/vectors.hpp>
#include <fc/scoped_exit.hpp>

#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/hex.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <test_contracts.hpp>
#include <tuple>

#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
   #include <sysio/chain/webassembly/native-module/native_context_stack.hpp>
/// Exercise the actual native export through the active context stack.
extern "C" int32_t ed25519_verify(const char* sig, uint32_t sig_len, const char* pub, uint32_t pub_len,
                                   const char* msg, uint32_t msg_len);
#endif

using namespace sysio::chain;
using namespace sysio::testing;

/// Host-side counterpart of the fixture's action arguments; no intrinsic-specific packing.
struct ed25519_input {
   bytes signature;
   bytes public_key;
   bytes message;
};
FC_REFLECT(ed25519_input, (signature)(public_key)(message))

namespace {
constexpr auto contract_account = "edverify"_n;
constexpr auto verify_action = "verify"_n;
constexpr auto batch_action = "batch"_n;
constexpr auto boundary_action = "boundary"_n;
constexpr auto badptr_action = "badptr"_n;
constexpr auto stress_action = "stress"_n;
constexpr auto clean_action = "clean"_n;
constexpr int32_t verification_success = 0;
constexpr int32_t verification_failure = -1;
constexpr size_t signature_size = crypto_sign_BYTES;
constexpr size_t key_size = crypto_sign_PUBLICKEYBYTES;
constexpr size_t scalar_size = signature_size / 2;
constexpr unsigned bits_per_byte = 8;
constexpr uint8_t byte_mask = 0xff;
constexpr uint32_t byte_values = 256;
constexpr uint32_t buffer_count = 3;
constexpr uint32_t contract_timeout_ms = 100;
constexpr uint32_t block_timeout_ms = 1'000;

/// A published known-answer vector, independent of the library used to generate larger test inputs.
struct rfc_vector {
   const char* message;
   const char* public_key;
   const char* signature;
};

/// https://www.rfc-editor.org/rfc/rfc8032.txt section 7.1, tests 1, 2, and 3 (empty, one-byte, and two-byte messages).
constexpr std::array rfc_vectors{
   rfc_vector{"",     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
              "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
              "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"  },
   rfc_vector{"72",   "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
              "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
              "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
   rfc_vector{"af82", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
              "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
              "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"}
};

/// RFC 8032 section 7.3: valid Ed25519ph must not be accepted as plain Ed25519.
constexpr rfc_vector ph_vector{"616263", "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
                               "98a70222f0b8121aa9d30f813d683f809e462b469c7ff87639499bb94e6dae41"
                               "31f85042463c2a355a2003d062adf5aaa10b8c61e636062aaad11c2a26083406"};

/// Decode trusted test-vector hex into raw bytes, with no transformation on the contract side.
bytes decode(const std::string& hex) {
   auto data = fc::from_hex(hex);
   return bytes(data.begin(), data.end());
}

/// Read a published vector without generating its signature with the implementation under test.
ed25519_input known_input(size_t index = 0) {
   const auto& v = rfc_vectors.at(index);
   return {decode(v.signature), decode(v.public_key), decode(v.message)};
}

/// Sign deterministic binary test messages directly, independently of fc's hex-digest signature API.
ed25519_input signed_input(const bytes& message, uint8_t seed_byte = 0) {
   BOOST_REQUIRE_GE(sodium_init(), 0);
   std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
   seed.fill(seed_byte);
   std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
   std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> pub{};
   std::array<unsigned char, crypto_sign_BYTES> sig{};
   BOOST_REQUIRE_EQUAL(crypto_sign_seed_keypair(pub.data(), secret.data(), seed.data()), 0);
   // Give libsodium a valid pointer even for an empty vector.
   constexpr unsigned char empty = 0;
   const auto* data = message.empty() ? &empty : reinterpret_cast<const unsigned char*>(message.data());
   BOOST_REQUIRE_EQUAL(crypto_sign_detached(sig.data(), nullptr, data, message.size(), secret.data()), 0);
   return {bytes(sig.begin(), sig.end()), bytes(pub.begin(), pub.end()), message};
}

/// Deploy the compiled fixture at genesis and provide typed raw-action calls.
struct ed25519_tester : tester {
   ed25519_tester() {
      create_accounts({contract_account});
      produce_block();
      set_code(contract_account, test_contracts::ed25519_verify_test_wasm());
      set_abi(contract_account, test_contracts::ed25519_verify_test_abi());
      produce_block();
   }

   /// Build a transaction with contract action arguments already ABI-packed.
   signed_transaction transaction(name action_name, const bytes& payload) {
      signed_transaction trx;
      trx.actions.emplace_back(
         vector<permission_level>{
            {contract_account, config::active_name}
      },
         contract_account, action_name, payload);
      set_transaction_headers(trx);
      trx.sign(get_private_key(contract_account, "active"), control->get_chain_id());
      return trx;
   }

   /// Execute and commit a successful fixture action, including ordinary signature rejection.
   void run(name action_name, const bytes& payload = {}) {
      auto trx = transaction(action_name, payload);
      BOOST_REQUIRE(push_transaction(trx)->receipt);
      produce_block();
   }

   /// Assert the contract observes the exact expected return code.
   void verify(const ed25519_input& entry, int32_t expected, bool unaligned = false) {
      run(verify_action, fc::raw::pack(std::make_tuple(entry, expected, unaligned)));
   }

   /// Construct a real context to exercise the native export with the same host implementation.
   template <typename F>
   void with_interface(F&& test) {
      auto trx = transaction(verify_action, fc::raw::pack(std::make_tuple(known_input(), verification_success, false)));
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      platform_timer timer;
      const accounts_billing_t previous_billing;
      transaction_context context(*control, packed, transaction_checktime_timer(timer), fc::time_point::now(),
                                  transaction_metadata::trx_type::input, std::nullopt, fc::time_point::maximum(),
                                  fc::microseconds::maximum(), true, previous_billing,
                                  cpu_usage_t{fc::unsigned_int{DEFAULT_BILLED_CPU_TIME_US}});
      context.init_for_input_trx();
      context.exec();
      apply_context action_context(*control, context, 1);
      webassembly::interface host(action_context);
      test(host, context);
   }
};

/// Select the stress action for the shared transaction deadline test helper.
struct stress_call {
   static name get_account() { return contract_account; }
   static name get_name() { return stress_action; }
};
} // namespace

BOOST_AUTO_TEST_SUITE(ed25519_intrinsic_tests)

/// Published vectors, binary/empty messages, unaligned spans, and context-free execution.
BOOST_FIXTURE_TEST_CASE(raw_messages_and_context_free, ed25519_tester) {
   for (size_t i = 0; i < rfc_vectors.size(); ++i)
      verify(known_input(i), verification_success, true);
   bytes binary(byte_values);
   for (uint32_t i = 0; i < byte_values; ++i)
      binary[i] = static_cast<char>(i);
   verify(signed_input(binary), verification_success);
   verify({decode(ph_vector.signature), decode(ph_vector.public_key), decode(ph_vector.message)}, verification_failure);
   auto hex_message = known_input(1);
   hex_message.message = decode("3732"); // ASCII "72" differs from the signed byte 0x72.
   verify(hex_message, verification_failure);
   auto trx = transaction(clean_action, {});
   trx.signatures.clear();
   trx.context_free_actions.emplace_back(vector<permission_level>{}, contract_account, verify_action,
                                         fc::raw::pack(std::make_tuple(known_input(), verification_success, false)));
   trx.sign(get_private_key(contract_account, "active"), control->get_chain_id());
   BOOST_REQUIRE(push_transaction(trx)->receipt);
}

/// Consensus acceptance goldens also run through a deployed contract, including wrong-sized signatures.
BOOST_FIXTURE_TEST_CASE(frozen_verification_rules, ed25519_tester) {
   for (const auto& vector : ed25519_test_vectors::cases) {
      BOOST_TEST_CONTEXT(vector.name) {
         verify({decode(vector.signature), decode(vector.public_key), decode(vector.message)},
                vector.valid ? verification_success : verification_failure);
      }
   }
}

/// Incorrect signatures, keys, and messages are ordinary -1 results, including noncanonical S.
BOOST_FIXTURE_TEST_CASE(invalid_signatures_and_keys, ed25519_tester) {
   const auto good = known_input(2);
   for (auto member : {&ed25519_input::signature, &ed25519_input::public_key, &ed25519_input::message}) {
      auto bad = good;
      (bad.*member).front() ^= 1;
      verify(bad, verification_failure);
   }
   const auto order = decode("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010");
   auto bad = good;
   std::copy(order.begin(), order.end(), bad.signature.begin() + scalar_size);
   verify(bad, verification_failure);
   bad = good;
   uint32_t carry = 0;
   for (size_t i = 0; i < scalar_size; ++i) {
      const uint32_t sum = uint32_t{static_cast<uint8_t>(bad.signature[scalar_size + i])} +
                           static_cast<uint8_t>(order[i]) + carry;
      bad.signature[scalar_size + i] = static_cast<char>(sum & byte_mask);
      carry = sum >> bits_per_byte;
   }
   BOOST_REQUIRE_EQUAL(carry, 0);
   verify(bad, verification_failure);
   ed25519_input identity{bytes(signature_size, 0), bytes(key_size, 0), good.message};
   identity.signature.front() = identity.public_key.front() = 1;
   verify(identity, verification_failure); // S=0, R=A=identity satisfies the equation but must be rejected.
}

/// Realistic proofs execute their loop in WASM and can reuse existing messages and keys.
BOOST_FIXTURE_TEST_CASE(batches_and_timings, ed25519_tester) {
   constexpr std::array<uint32_t, 3> sizes{1, 24, 68};
   run(batch_action, fc::raw::pack(std::make_tuple(std::vector<ed25519_input>{}, verification_failure)));
   for (auto count : sizes) {
      std::vector<ed25519_input> batch;
      for (uint32_t i = 0; i < count; ++i)
         batch.push_back(signed_input(bytes(i + 1, static_cast<char>(i)), i));
      const auto start = fc::time_point::now();
      run(batch_action, fc::raw::pack(std::make_tuple(batch, verification_success)));
      BOOST_TEST_MESSAGE(count << " independent verifications and block production: "
                               << (fc::time_point::now() - start).count() << " us");
      for (uint32_t index : {uint32_t{0}, count / 2, count - 1}) {
         auto bad = batch;
         bad[index].signature.front() ^= 1;
         run(batch_action, fc::raw::pack(std::make_tuple(bad, verification_failure)));
      }
      run(batch_action, fc::raw::pack(std::make_tuple(std::vector<ed25519_input>(count, known_input()), verification_success)));
   }
}

/// Every short signature/key and a trailing byte are rejected without assertion-based decoding.
BOOST_FIXTURE_TEST_CASE(invalid_lengths, ed25519_tester) {
   for (auto member : {&ed25519_input::signature, &ed25519_input::public_key}) {
      const auto good = known_input();
      for (size_t size = 0; size <= (good.*member).size() + 1; ++size) {
         auto input = good;
         (input.*member).resize(size);
         verify(input, size == (good.*member).size() ? verification_success : verification_failure);
      }
   }
}

/// Correct signatures on both sides of the consensus message-size boundary isolate the size policy.
BOOST_FIXTURE_TEST_CASE(message_size_limit, ed25519_tester) {
   constexpr uint32_t maximum = config::max_ed25519_message_size;
   static_assert(maximum == 65'536);
   for (uint32_t size : {maximum - 1, maximum, maximum + 1}) {
      bytes message(size, '\0');
      message.back() = 1;
      auto entry = signed_input(message);
      const auto expected = size <= maximum ? verification_success : verification_failure;
      verify(entry, expected);
      run(batch_action, fc::raw::pack(std::make_tuple(std::vector<ed25519_input>{known_input(), entry}, expected)));
   }
}

/// Each span is validated in full before host entry, even when its length would otherwise return -1.
BOOST_FIXTURE_TEST_CASE(wasm_memory_bounds, ed25519_tester) {
   for (uint32_t buffer = 0; buffer < buffer_count; ++buffer) {
      run(boundary_action, fc::raw::pack(std::make_tuple(known_input(2), buffer, false)));
      auto crossing = transaction(boundary_action, fc::raw::pack(std::make_tuple(known_input(2), buffer, true)));
      BOOST_CHECK_THROW(push_transaction(crossing), wasm_execution_error);
      auto wrapping = transaction(badptr_action, fc::raw::pack(std::make_tuple(buffer)));
      BOOST_CHECK_THROW(push_transaction(wrapping), wasm_execution_error);
   }
   constexpr uint32_t message_buffer = 2;
   run(boundary_action, fc::raw::pack(std::make_tuple(known_input(), message_buffer, false)));
   verify(known_input(), verification_success);
}

/// Pin the native export's six arguments and return codes through actual context-stack dispatch.
BOOST_FIXTURE_TEST_CASE(native_export, ed25519_tester) {
   with_interface([&](webassembly::interface& host, transaction_context&) {
      const auto input = known_input();
      const span<const char> sig{input.signature.data(), input.signature.size()};
      const span<const char> pub{input.public_key.data(), input.public_key.size()};
      const span<const char> msg{input.message.data(), input.message.size()};
      BOOST_CHECK_EQUAL(host.ed25519_verify(sig, pub, msg), verification_success);
      BOOST_CHECK_EQUAL(host.ed25519_verify({}, pub, msg), verification_failure);
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
      webassembly::native_module::native_context_stack::guard guard(&host);
      BOOST_CHECK_EQUAL(::ed25519_verify(sig.data(), sig.size(), pub.data(), pub.size(), msg.data(), msg.size()), verification_success);
      BOOST_CHECK_EQUAL(::ed25519_verify(sig.data(), 0, pub.data(), pub.size(), msg.data(), msg.size()), verification_failure);
#endif
   });
}

/// Expire the timer immediately before WASM entry, including a call that would ordinarily return -1.
BOOST_FIXTURE_TEST_CASE(already_expired_execution, ed25519_tester) {
   verify(known_input(), verification_success); // Warm runtime compilation before injecting expiry.
   auto& substitute = control->get_wasm_interface().substitute_apply;
   auto reset = fc::make_scoped_exit([&] { substitute = {}; });
   bool entered = false;
   substitute = [&](const digest_type&, uint8_t, uint8_t, apply_context& context) {
      if (context.get_receiver() == contract_account) {
         entered = true;
         context.trx_context.transaction_timer.set_expired();
      }
      return false;
   };
   for (const bool valid : {true, false}) {
      auto entry = known_input();
      if (!valid)
         entry.signature.clear();
      auto trx = transaction(verify_action, fc::raw::pack(std::make_tuple(entry, valid ? verification_success : verification_failure, false)));
      entered = false;
      BOOST_CHECK_THROW(push_transaction(trx), deadline_exception);
      BOOST_CHECK(entered);
   }
   substitute = {};
   verify(known_input(), verification_success);
}

/// A timed-out contract action must roll back writes and leave the chain usable.
BOOST_FIXTURE_TEST_CASE(contract_timeout_rolls_back, ed25519_tester) {
   // Compile/warm the contract before measuring its transaction time.
   verify(known_input(), verification_success);
   transaction_trace_ptr failed_trace;
   boost::signals2::scoped_connection connection = control->applied_transaction().connect(
      [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> event) {
         const auto& trace = std::get<0>(event);
         if (trace->except)
            failed_trace = trace;
      });
   BOOST_CHECK_THROW(sysio::test_utils::push_trx(*this, stress_call{}, 0, contract_timeout_ms, block_timeout_ms, false,
                                                 fc::raw::pack(std::make_tuple(known_input())), contract_account),
                     tx_cpu_usage_exceeded);
   BOOST_REQUIRE(failed_trace);
   BOOST_REQUIRE_EQUAL(failed_trace->action_traces.size(), 1);
   const auto& deltas = failed_trace->action_traces.front().account_ram_deltas;
   BOOST_REQUIRE(!deltas.empty()); // Prove the marker was written before the timeout.
   BOOST_CHECK_GT(deltas.begin()->delta, 0);
   run(clean_action);
   verify(known_input(), verification_success);
}

BOOST_AUTO_TEST_SUITE_END()
