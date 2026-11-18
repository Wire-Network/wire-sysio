// unittests/signature_recovery_tests.cpp

#include <boost/test/unit_test.hpp>
#include <cstring>                         // for std::strcmp
#include "test_signature_utils.hpp"

#include <sysio/chain/transaction.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/merkle.hpp>
#include <sysio/testing/tester.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/raw.hpp>
#include <boost/container/flat_set.hpp>

using namespace sysio::chain;
using namespace sysio::testing;
using fc::crypto::private_key;
using fc::crypto::public_key;

namespace {

/// The account whose active authority is a WebAuthn key.
constexpr account_name webauthn_account = "wasigner"_n;

/// The sysio.bios action that requires the authorization of the account it names.
constexpr action_name reqauth_action = "reqauth"_n;

/// A chain on which webauthn_account's active authority is a WebAuthn key.
struct webauthn_signer_tester : validating_tester {
   webauthn_signer_tester() {
      create_account(webauthn_account);
      set_authority(webauthn_account, config::active_name, authority(key().get_public_key()));
      produce_block();
   }

   /// The WebAuthn key of webauthn_account's active authority.
   static mock::webauthn_private_key key() {
      return get_private_key<mock::webauthn_private_key>(webauthn_account, "active");
   }

   /// An unsigned transaction whose one action requires webauthn_account's active authority.
   signed_transaction reqauth() {
      signed_transaction trx;
      trx.actions.emplace_back(vector<permission_level>{{webauthn_account, config::active_name}},
                               config::system_account_name, reqauth_action, fc::raw::pack(webauthn_account));
      set_transaction_headers(trx);
      return trx;
   }

   /// A WebAuthn signature of @p trx by key() whose variable-size component is @p variable_size bytes.
   signature_type sign(const signed_transaction& trx, size_t variable_size) const {
      return key().sign_with_variable_size(trx.sig_digest(get_chain_id()), variable_size);
   }
};

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(signature_recovery_tests)

struct sig_fixture {
   sig_fixture(): chain_id(fc::sha256().str()) {}
   chain_id_type chain_id;
};

/// 1. Pure recoverable still works (order-agnostic)
BOOST_FIXTURE_TEST_CASE(pure_recoverable_sigs, sig_fixture) {
   auto p1  = private_key::generate();
   auto p2  = private_key::generate();
   auto trx = test::make_signed_trx({p1, p2}, chain_id);

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(
     chain_id,
     fc::time_point::maximum(),
     keys,
     /*allowDuplicates*/false
   );

   BOOST_REQUIRE_EQUAL(keys.size(), 2u);
   BOOST_CHECK(keys.count(p1.get_public_key()) == 1);
   BOOST_CHECK(keys.count(p2.get_public_key()) == 1);
}

/// 2. Duplicate recoverable -> tx_duplicate_sig
BOOST_FIXTURE_TEST_CASE(duplicate_sig_rejected, sig_fixture) {
   auto priv = private_key::generate();
   auto trx  = test::make_signed_trx({priv, priv}, chain_id);

   boost::container::flat_set<public_key> keys;
   bool caught = false;
   try {
      trx.get_signature_keys(chain_id,
                             fc::time_point::maximum(),
                             keys,
                             false);
   } catch (const fc::exception& e) {
      if (std::strcmp(e.name(), "tx_duplicate_sig") == 0) {
         caught = true;
      }
   }
   if (!caught) {
      BOOST_FAIL("Expected tx_duplicate_sig but none thrown");
   }
}

/// 3a. Invalid recovery byte -> fc::exception
BOOST_FIXTURE_TEST_CASE(invalid_recovery_byte_rejected, sig_fixture) {
   auto p1  = private_key::generate();
   auto trx = test::make_signed_trx({p1}, chain_id);

   auto packed = fc::raw::pack(trx.signatures[0]);
   packed[1] = 0;  // Invalid: must be 27-30 for k1
   fc::crypto::signature bad;
   fc::datastream<const char*> ds(packed.data(), packed.size());
   fc::raw::unpack(ds, bad);
   trx.signatures[0] = bad;

   boost::container::flat_set<public_key> keys;
   BOOST_CHECK_THROW(
     trx.get_signature_keys(chain_id,
                            fc::time_point::maximum(),
                            keys,
                            false),
     fc::exception
   );
}

/// 3b. Corrupted signature data -> fc::exception
BOOST_FIXTURE_TEST_CASE(corrupted_sig_data_rejected, sig_fixture) {
   auto p1  = private_key::generate();
   auto trx = test::make_signed_trx({p1}, chain_id);

   auto packed = fc::raw::pack(trx.signatures[0]);
   std::memset(packed.data() + 2, 0, 64);
   fc::crypto::signature bad;
   fc::datastream<const char*> ds(packed.data(), packed.size());
   fc::raw::unpack(ds, bad);
   trx.signatures[0] = bad;

   boost::container::flat_set<public_key> keys;
   BOOST_CHECK_THROW(
     trx.get_signature_keys(chain_id,
                            fc::time_point::maximum(),
                            keys,
                            false),
     fc::exception
   );
}

/// 4. Timeout path -> any fc::exception
BOOST_FIXTURE_TEST_CASE(signature_deadline_timeout, sig_fixture) {
   auto priv = private_key::generate();
   auto trx  = test::make_signed_trx({priv}, chain_id);

   auto past = fc::time_point::now() - fc::microseconds(1);
   boost::container::flat_set<public_key> keys;
   BOOST_CHECK_THROW(
     trx.get_signature_keys(chain_id, past, keys, false),
     fc::exception
   );
}

/// 5. Zero signatures -> empty
BOOST_FIXTURE_TEST_CASE(zero_sigs_zero_ext, sig_fixture) {
   signed_transaction trx;
   trx.set_reference_block(block_id_type());
   trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(3600));

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(chain_id,
                          fc::time_point::maximum(),
                          keys,
                          false);
   BOOST_CHECK(keys.empty());
}

/// 6. Max signatures edge -> all accepted
BOOST_FIXTURE_TEST_CASE(max_sigs_and_ext, sig_fixture) {
   constexpr size_t MAX = 32;
   std::vector<private_key> privs;
   privs.reserve(MAX);
   for (size_t i = 0; i < MAX; ++i)
      privs.push_back(private_key::generate());

   auto trx = test::make_signed_trx(privs, chain_id);

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(chain_id,
                          fc::time_point::maximum(),
                          keys,
                          false);
   BOOST_CHECK_EQUAL(keys.size(), MAX);
}

/// 7. ED25519 signature recovery works through get_signature_keys
BOOST_FIXTURE_TEST_CASE(ed_sig_recovery_works, sig_fixture) {
   auto ed_priv = private_key::generate(private_key::key_type::ed);
   auto ed_pub  = ed_priv.get_public_key();

   signed_transaction trx;
   trx.set_reference_block(block_id_type());
   trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(3600));

   auto digest = trx.sig_digest(chain_id);
   trx.signatures.emplace_back(ed_priv.sign(digest));

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(chain_id,
                          fc::time_point::maximum(),
                          keys,
                          false);

   BOOST_REQUIRE_EQUAL(keys.size(), 1u);
   BOOST_CHECK(keys.count(ed_pub) == 1);
}

/// 8. Mixed K1 and ED sigs on same transaction, both recovered
BOOST_FIXTURE_TEST_CASE(mixed_k1_and_ed_sigs, sig_fixture) {
   auto k1_priv = private_key::generate();
   auto ed_priv = private_key::generate(private_key::key_type::ed);
   auto k1_pub  = k1_priv.get_public_key();
   auto ed_pub  = ed_priv.get_public_key();

   signed_transaction trx;
   trx.set_reference_block(block_id_type());
   trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(3600));

   auto digest = trx.sig_digest(chain_id);
   trx.signatures.emplace_back(k1_priv.sign(digest));
   trx.signatures.emplace_back(ed_priv.sign(digest));

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(chain_id,
                          fc::time_point::maximum(),
                          keys,
                          false);

   BOOST_REQUIRE_EQUAL(keys.size(), 2u);
   BOOST_CHECK(keys.count(k1_pub) == 1);
   BOOST_CHECK(keys.count(ed_pub) == 1);
}

/// 9. ED sig with corrupted embedded pubkey is rejected
BOOST_FIXTURE_TEST_CASE(ed_sig_corrupted_pubkey_rejected, sig_fixture) {
   auto ed_priv = private_key::generate(private_key::key_type::ed);

   signed_transaction trx;
   trx.set_reference_block(block_id_type());
   trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(3600));

   auto digest = trx.sig_digest(chain_id);
   auto sig = ed_priv.sign(digest);

   // Corrupt the embedded pubkey bytes (first 32 bytes of ED sig data)
   auto packed = fc::raw::pack(sig);
   // variant index (1 byte) + 96 bytes sig data; pubkey starts at offset 1
   std::memset(packed.data() + 1, 0xFF, 32);
   fc::crypto::signature bad;
   fc::datastream<const char*> ds(packed.data(), packed.size());
   fc::raw::unpack(ds, bad);
   trx.signatures.emplace_back(bad);

   boost::container::flat_set<public_key> keys;
   BOOST_CHECK_THROW(
     trx.get_signature_keys(chain_id,
                            fc::time_point::maximum(),
                            keys,
                            false),
     fc::exception
   );
}

/// 10. Unknown transaction extensions are rejected by validate_and_extract_extensions
BOOST_FIXTURE_TEST_CASE(trx_extension_rejected, sig_fixture) {
   signed_transaction trx;
   trx.set_reference_block(block_id_type());
   trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(3600));

   // Add an extension with an unregistered ID
   trx.transaction_extensions.emplace_back(
      uint16_t(0x8000),
      fc::raw::pack(std::string("bogus"))
   );

   bool caught = false;
   try {
      trx.validate_and_extract_extensions();
   } catch (const fc::exception& e) {
      if (std::strcmp(e.name(), "invalid_transaction_extension") == 0) {
         caught = true;
      }
   }
   if (!caught) {
      BOOST_FAIL("Expected invalid_transaction_extension but none thrown");
   }
}

/// 11. Transaction with extensions rejected at consensus layer (init_for_input_trx)
BOOST_AUTO_TEST_CASE(trx_extension_rejected_on_push) {
   validating_tester chain;

   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{{"sysio"_n, config::active_name}},
      "sysio"_n, "reqactivated"_n,
      fc::raw::pack(fc::unsigned_int(0))
   );
   chain.set_transaction_headers(trx);

   // Add a bogus transaction extension
   trx.transaction_extensions.emplace_back(
      uint16_t(0x8000),
      fc::raw::pack(std::string("bogus"))
   );

   trx.sign(chain.get_private_key("sysio"_n, "active"), chain.get_chain_id());

   BOOST_CHECK_EXCEPTION(
      chain.push_transaction(trx),
      invalid_transaction_extension,
      fc_exception_message_is("transaction extensions are not currently supported")
   );
}

// Regression test for verify_signee's dedup assumption. verify_signee (block_state.cpp)
// collects recovered keys into std::set<public_key_type> and relies on set equality to
// detect "block signed by same key twice". The equality semantics come from
// public_key_type's defaulted <=>, which in turn compares the underlying std::variant
// of curve-specific shim types. If recover() ever produces two structurally-distinct
// public_key_type values that represent the same logical key (e.g. different internal
// encodings of the same curve point), the "same key twice" check would silently miss
// and keys_satisfy_and_relevant would over-count relevant signatures.
//
// This test recovers the same key via two fresh calls with identical inputs and
// asserts the set collapses them to one entry. It's never expected to fail today but
// catches a regression if recover() or the variant ordering changes.
BOOST_AUTO_TEST_CASE(recovered_keys_dedup_in_set) try {
   auto priv = fc::crypto::private_key::generate();
   auto digest = fc::sha256::hash("hello world");
   auto sig = priv.sign(digest);

   // Two fresh recover calls with the same inputs should yield the same public_key_type.
   auto pk1 = fc::crypto::public_key::recover(sig, digest);
   auto pk2 = fc::crypto::public_key::recover(sig, digest);
   BOOST_CHECK(pk1 == pk2);

   // Insertion into std::set should collapse them.
   std::set<fc::crypto::public_key> keys;
   auto [it1, inserted1] = keys.emplace(pk1);
   BOOST_CHECK(inserted1);
   auto [it2, inserted2] = keys.emplace(pk2);
   BOOST_CHECK(!inserted2); // second emplace must be rejected -- same key
   BOOST_REQUIRE_EQUAL(keys.size(), 1u);

   // Paired with verify_signee's direct call shape: recover into set via distinct
   // loop iterations and confirm count is 1.
   std::vector<fc::crypto::signature> sigs{sig, sig};
   std::set<fc::crypto::public_key> keys2;
   for (const auto& s : sigs) {
      auto [iter, inserted] = keys2.emplace(fc::crypto::public_key::recover(s, digest));
      (void)iter; (void)inserted;
   }
   BOOST_REQUIRE_EQUAL(keys2.size(), 1u);
} FC_LOG_AND_RETHROW()

/// A WebAuthn signature whose variable-size component is exactly config::max_signature_variable_size authorizes a
/// transaction, on the producer and on the validating node.
BOOST_FIXTURE_TEST_CASE(webauthn_signature_at_variable_size_limit_accepted, webauthn_signer_tester) try {
   signed_transaction trx = reqauth();
   trx.signatures.push_back(sign(trx, config::max_signature_variable_size));
   BOOST_REQUIRE_EQUAL(trx.signatures.back().variable_size(), config::max_signature_variable_size);

   push_transaction(trx);
   produce_block();
} FC_LOG_AND_RETHROW()

/// A transaction carrying a signature one byte over config::max_signature_variable_size is rejected before its keys are
/// recovered.
BOOST_FIXTURE_TEST_CASE(webauthn_signature_over_variable_size_limit_rejected, webauthn_signer_tester) try {
   constexpr size_t oversized = config::max_signature_variable_size + 1;

   signed_transaction trx = reqauth();
   trx.signatures.push_back(sign(trx, oversized));
   BOOST_CHECK_THROW(push_transaction(trx), sig_variable_size_limit_exception);

   // Empty client data JSON fails recovery, so this throw shows the size is checked first.
   signed_transaction unrecoverable = reqauth();
   unrecoverable.signatures.emplace_back(signature_type::storage_type(
      fc::crypto::webauthn::signature(fc::crypto::r1::compact_signature{}, std::vector<uint8_t>(oversized), {})));
   BOOST_CHECK_THROW(push_transaction(unrecoverable), sig_variable_size_limit_exception);
} FC_LOG_AND_RETHROW()

/// A validating node rejects a block holding a transaction with a signature over config::max_signature_variable_size.
/// The producer would refuse that transaction, so its block is rebuilt around the same transaction re-signed by the
/// same key, with the signature's variable-size component one byte over the limit, then re-merkled and re-signed.
/// The original block, with the signature at the limit, is accepted afterwards.
BOOST_FIXTURE_TEST_CASE(block_with_webauthn_signature_over_variable_size_limit_rejected, webauthn_signer_tester) try {
   signed_transaction trx = reqauth();
   trx.signatures.push_back(sign(trx, config::max_signature_variable_size));
   push_transaction(trx);
   const signed_block_ptr block = produce_block_no_validation();
   BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);

   mutable_block_ptr oversized = block->clone();
   trx.signatures = {sign(trx, config::max_signature_variable_size + 1)};
   oversized->transactions.front().trx = packed_transaction(trx);
   deque<digest_type> receipt_digests;
   for (const transaction_receipt& receipt : oversized->transactions)
      receipt_digests.emplace_back(receipt.digest());
   oversized->transaction_mroot   = calculate_merkle(receipt_digests);
   oversized->producer_signatures = {get_private_key(oversized->producer, "active").sign(oversized->calculate_id())};

   BOOST_CHECK_THROW(validate_push_block(signed_block::create_signed_block(std::move(oversized))),
                     sig_variable_size_limit_exception);
   validate_push_block(block);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
