#include "test-contracts/ed25519_verify_test/test_config.hpp"
#include "test_utils.hpp"

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
/// Exercise the actual native export, including context-stack dispatch and exception propagation.
extern "C" bool ed25519_verify(const char* entries, uint32_t entries_len, uint32_t count);
#endif

using namespace sysio::chain;
using namespace sysio::testing;

namespace {
constexpr auto contract_account = "edverify"_n;
constexpr auto verify_action = "verify"_n;
constexpr auto boundary_action = "boundary"_n;
constexpr auto badptr_action = "badptr"_n;
constexpr auto stress_action = "stress"_n;
constexpr auto clean_action = "clean"_n;
constexpr size_t length_size = sizeof(uint32_t);
constexpr size_t signature_size = crypto_sign_BYTES;
constexpr size_t key_size = crypto_sign_PUBLICKEYBYTES;
constexpr size_t scalar_size = signature_size / 2;
constexpr size_t key_offset = length_size + signature_size;
constexpr size_t message_offset = key_offset + key_size;
constexpr unsigned bits_per_byte = 8;
constexpr uint8_t byte_mask = 0xff;
constexpr uint32_t byte_values = 256;
constexpr uint32_t direct_timeout_ms = 20;
constexpr uint32_t contract_timeout_ms = 100;
constexpr uint32_t block_timeout_ms = 1'000;
static_assert(message_offset == ed25519_test::empty_record_size);

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

/// Encode the intrinsic's fixed-width little-endian length without native-endian casts.
void set_length(bytes& record, uint32_t size) {
   BOOST_REQUIRE_GE(record.size(), length_size);
   for (size_t i = 0; i < length_size; ++i)
      record[i] = static_cast<char>((size >> (i * bits_per_byte)) & byte_mask);
}

/// Construct one record from separately supplied message, signature, and public key bytes.
bytes record(const bytes& message, const bytes& signature, const bytes& public_key) {
   BOOST_REQUIRE_EQUAL(signature.size(), signature_size);
   BOOST_REQUIRE_EQUAL(public_key.size(), key_size);
   bytes result(length_size);
   set_length(result, message.size());
   result.insert(result.end(), signature.begin(), signature.end());
   result.insert(result.end(), public_key.begin(), public_key.end());
   result.insert(result.end(), message.begin(), message.end());
   return result;
}

/// Pack a published vector without asking libsodium to create its signature.
bytes known_record(size_t index = 0) {
   const auto& v = rfc_vectors.at(index);
   return record(decode(v.message), decode(v.signature), decode(v.public_key));
}

/// Sign deterministic binary test messages directly, independently of fc's hex-digest signature API.
bytes signed_record(const bytes& message, uint8_t seed_byte = 0) {
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
   return record(message, bytes(sig.begin(), sig.end()), bytes(pub.begin(), pub.end()));
}

/// Assemble repeated records outside transaction execution for direct host timeout tests.
bytes repeat_record(const bytes& entry, uint32_t count) {
   bytes result;
   result.reserve(entry.size() * count);
   for (uint32_t i = 0; i < count; ++i)
      result.insert(result.end(), entry.begin(), entry.end());
   return result;
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

   /// Execute and commit a successful fixture action; a false verification result must still succeed.
   void run(name action_name, const bytes& payload = {}) {
      auto trx = transaction(action_name, payload);
      BOOST_REQUIRE(push_transaction(trx)->receipt);
      produce_block();
   }

   /// Assert the contract observes the exact expected boolean result.
   void verify(const bytes& entries, uint32_t count, bool expected, bool unaligned = false) {
      run(verify_action, fc::raw::pack(std::make_tuple(entries, count, expected, unaligned)));
   }

   /// Construct a real transaction/apply context so host-only tests cannot pass via WASM's watchdog.
   template <typename F>
   void with_interface(F&& test) {
      auto trx = transaction(verify_action, fc::raw::pack(std::make_tuple(known_record(), uint32_t{1}, true, false)));
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

/// Published vectors, arbitrary bytes, unaligned input, and a context-free contract invocation.
BOOST_FIXTURE_TEST_CASE(raw_messages_and_context_free, ed25519_tester) {
   for (size_t i = 0; i < rfc_vectors.size(); ++i) {
      BOOST_TEST_CONTEXT("RFC 8032 vector " << i + 1) {
         verify(known_record(i), 1, true, true);
      }
   }
   bytes binary(byte_values);
   for (uint32_t i = 0; i < byte_values; ++i)
      binary[i] = static_cast<char>(i);
   verify(signed_record(binary), 1, true);
   verify(record(decode(ph_vector.message), decode(ph_vector.signature), decode(ph_vector.public_key)), 1, false);

   auto raw = known_record(1);
   auto hex_message = decode("3732"); // ASCII "72" is different from the raw byte 0x72.
   raw.resize(message_offset);
   set_length(raw, hex_message.size());
   raw.insert(raw.end(), hex_message.begin(), hex_message.end());
   verify(raw, 1, false);

   auto trx = transaction(clean_action, {});
   trx.signatures.clear();
   trx.context_free_actions.emplace_back(vector<permission_level>{}, contract_account, verify_action,
                                         fc::raw::pack(std::make_tuple(known_record(), uint32_t{1}, true, false)));
   trx.sign(get_private_key(contract_account, "active"), control->get_chain_id());
   BOOST_REQUIRE(push_transaction(trx)->receipt);
}

/// Invalid cryptographic data must return false without aborting the caller's action.
BOOST_FIXTURE_TEST_CASE(invalid_signatures_and_keys, ed25519_tester) {
   const auto good = known_record(2);
   for (const size_t offset : {length_size, key_offset, message_offset}) {
      auto bad = good;
      bad[offset] ^= 1;
      verify(bad, 1, false);
   }
   auto bad = good;
   const auto order = decode("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010");
   std::copy(order.begin(), order.end(), bad.begin() + length_size + scalar_size);
   verify(bad, 1, false); // S == L is noncanonical.
   // S + L satisfies the same group equation as the valid signature, but its encoding must be rejected.
   bad = good;
   uint32_t carry = 0;
   for (size_t i = 0; i < scalar_size; ++i) {
      const size_t offset = length_size + scalar_size + i;
      const uint32_t sum = uint32_t{static_cast<uint8_t>(bad[offset])} + static_cast<uint8_t>(order[i]) + carry;
      bad[offset] = static_cast<char>(sum & byte_mask);
      carry = sum >> bits_per_byte;
   }
   BOOST_REQUIRE_EQUAL(carry, 0);
   verify(bad, 1, false);

   // With R = A = identity and S = 0, the group equation holds for any message. Reject this forgery.
   bytes identity_signature(signature_size, 0);
   bytes identity_key(key_size, 0);
   identity_signature.front() = 1;
   identity_key.front() = 1;
   verify(record(decode(rfc_vectors[1].message), identity_signature, identity_key), 1, false);
   for (const size_t offset : {length_size, key_offset}) {
      bad = good;
      std::fill_n(bad.begin() + offset, key_size, 0);
      bad[offset] = 1; // Edwards identity, a small-order R or public key.
      verify(bad, 1, false);
      std::fill_n(bad.begin() + offset, key_size, static_cast<char>(byte_mask));
      verify(bad, 1, false); // Noncanonical point encoding.
   }
}

/// Exercise realistic independent-message proofs and rejection at different positions.
BOOST_FIXTURE_TEST_CASE(batches_and_timings, ed25519_tester) {
   constexpr std::array<uint32_t, 3> sizes{1, 24, 68};
   for (const auto count : sizes) {
      bytes batch;
      std::vector<size_t> offsets;
      for (uint32_t i = 0; i < count; ++i) {
         offsets.push_back(batch.size());
         bytes message(i + 1, static_cast<char>(i));
         const auto entry = signed_record(message, i);
         batch.insert(batch.end(), entry.begin(), entry.end());
      }
      const auto start = fc::time_point::now();
      verify(batch, count, true);
      BOOST_TEST_MESSAGE(count << "-entry contract verification and block production: "
                               << (fc::time_point::now() - start).count() << " us");
      for (const uint32_t index : {uint32_t{0}, count / 2, count - 1}) {
         auto bad = batch;
         bad[offsets[index] + length_size] ^= 1;
         verify(bad, count, false);
      }
      verify(repeat_record(known_record(), count), count, true);
   }
}

/// Validate every truncated header position, message boundaries, and hostile length/count fields.
BOOST_FIXTURE_TEST_CASE(malformed_batches, ed25519_tester) {
   verify({}, 0, false);
   verify({}, 1, false);
   const auto good = known_record(2);
   for (size_t size = 0; size < good.size(); ++size)
      verify(bytes(good.begin(), good.begin() + size), 1, false);
   verify(good, 0, false);
   verify(good, 2, false);
   verify(good, std::numeric_limits<uint32_t>::max(), false);
   auto bad = good;
   bad.push_back(0);
   verify(bad, 1, false);
   auto two = repeat_record(good, 2);
   verify(two, 1, false);
   two.pop_back();
   verify(two, 2, false);
   for (const auto length : {uint32_t{0}, uint32_t{1}, uint32_t{3}, std::numeric_limits<uint32_t>::max()}) {
      bad = good;
      set_length(bad, length);
      verify(bad, 1, false);
   }
}

/// Both sides of the consensus size boundary use valid signatures, so only the size policy differs.
BOOST_FIXTURE_TEST_CASE(message_size_limit, ed25519_tester) {
   constexpr uint32_t maximum = config::max_ed25519_message_size;
   static_assert(maximum == 65'536);
   for (const uint32_t size : {maximum - 1, maximum, maximum + 1}) {
      bytes message(size, '\0');
      message.back() = 1;
      const auto entry = signed_record(message);
      verify(entry, 1, size <= maximum);
      auto batch = known_record();
      batch.insert(batch.end(), entry.begin(), entry.end());
      verify(batch, 2, size <= maximum);
   }
}

/// Runtime range validation must cover the entire packed buffer, including exact end-of-memory input.
BOOST_FIXTURE_TEST_CASE(wasm_memory_bounds, ed25519_tester) {
   run(boundary_action, fc::raw::pack(std::make_tuple(known_record(), false)));
   auto crossing = transaction(boundary_action, fc::raw::pack(std::make_tuple(known_record(), true)));
   BOOST_CHECK_THROW(push_transaction(crossing), wasm_execution_error);
   auto wrapping = transaction(badptr_action, {});
   BOOST_CHECK_THROW(push_transaction(wrapping), wasm_execution_error);
   verify(known_record(), 1, true);
}

/// Expiration is enforced inside the host, without returning to a WASM instruction/checkpoint.
BOOST_FIXTURE_TEST_CASE(host_deadlines_and_native_export, ed25519_tester) {
   const auto entry = known_record();
   const auto batch = repeat_record(entry, ed25519_test::stress_count);
   with_interface([&](webassembly::interface& host, transaction_context& context) {
      const span<const char> one{entry.data(), entry.size()};
      context.transaction_timer.stop();
      context.transaction_timer.start(fc::time_point::maximum());
      BOOST_REQUIRE(host.ed25519_verify(one, 1));
      for (const uint32_t count : {uint32_t{1}, uint32_t{24}, uint32_t{68}}) {
         const auto records = repeat_record(entry, count);
         const auto start = fc::time_point::now();
         BOOST_REQUIRE(host.ed25519_verify(span<const char>{records.data(), records.size()}, count));
         BOOST_TEST_MESSAGE(count << "-entry host verification: " << (fc::time_point::now() - start).count() << " us");
      }
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
      webassembly::native_module::native_context_stack::guard guard(&host);
      BOOST_REQUIRE(::ed25519_verify(entry.data(), entry.size(), 1));
      BOOST_CHECK(!::ed25519_verify(entry.data(), entry.size(), 0));
#endif
      context.transaction_timer.set_expired();
      BOOST_CHECK_THROW(host.ed25519_verify(one, 1), deadline_exception);
      BOOST_CHECK_THROW(host.ed25519_verify(span<const char>{entry.data(), 0}, 0), deadline_exception);
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
      BOOST_CHECK_THROW(::ed25519_verify(entry.data(), entry.size(), 1), deadline_exception);
#endif
      context.transaction_timer.stop();
      context.transaction_timer.start(fc::time_point::now() + fc::milliseconds(direct_timeout_ms));
      BOOST_CHECK_THROW(host.ed25519_verify(span<const char>{batch.data(), batch.size()}, ed25519_test::stress_count),
                        deadline_exception);
   });
}

/// A timed-out contract action must roll back writes and leave the chain usable.
BOOST_FIXTURE_TEST_CASE(contract_timeout_rolls_back, ed25519_tester) {
   // Compile/warm the contract before measuring its transaction time.
   verify(known_record(), 1, true);
   transaction_trace_ptr failed_trace;
   boost::signals2::scoped_connection connection = control->applied_transaction().connect(
      [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> event) {
         const auto& trace = std::get<0>(event);
         if (trace->except)
            failed_trace = trace;
      });
   BOOST_CHECK_THROW(sysio::test_utils::push_trx(*this, stress_call{}, 0, contract_timeout_ms, block_timeout_ms, false,
                                                 fc::raw::pack(std::make_tuple(known_record())), contract_account),
                     tx_cpu_usage_exceeded);
   BOOST_REQUIRE(failed_trace);
   BOOST_REQUIRE_EQUAL(failed_trace->action_traces.size(), 1);
   const auto& deltas = failed_trace->action_traces.front().account_ram_deltas;
   BOOST_REQUIRE(!deltas.empty()); // Prove the marker was written before the timeout.
   BOOST_CHECK_GT(deltas.begin()->delta, 0);
   run(clean_action);
   verify(known_record(), 1, true);
}

BOOST_AUTO_TEST_SUITE_END()
