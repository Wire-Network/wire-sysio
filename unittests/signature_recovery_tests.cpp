// unittests/signature_recovery_tests.cpp

#include <boost/test/unit_test.hpp>
#include <cstring>                         // for std::strcmp
#include "test_signature_utils.hpp"

#include <sysio/chain/transaction.hpp>
#include <sysio/chain/exceptions.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/raw.hpp>
#include <boost/container/flat_set.hpp>

using namespace sysio::chain;
using fc::crypto::private_key;
using fc::crypto::public_key;

BOOST_AUTO_TEST_SUITE(signature_recovery_tests)

struct sig_fixture {
   sig_fixture(): chain_id(fc::sha256()) {}
   chain_id_type chain_id;
};

/// 1. Pure recoverable still works (order‐agnostic)
BOOST_FIXTURE_TEST_CASE(pure_recoverable_sigs, sig_fixture) {
   auto p1  = private_key::generate();
   auto p2  = private_key::generate();
   auto trx = test::make_signed_trx({p1, p2}, chain_id, /*include_ed_ext=*/false);

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(
     chain_id,
     fc::time_point::maximum(),
     keys,
     /*allowDuplicates*/false
   );

   BOOST_REQUIRE_EQUAL(keys.size(), 2u);
   // membership, order‐agnostic
   BOOST_CHECK(keys.count(p1.get_public_key()) == 1);
   BOOST_CHECK(keys.count(p2.get_public_key()) == 1);
}

/// 2. Duplicate recoverable → tx_duplicate_sig
BOOST_FIXTURE_TEST_CASE(duplicate_sig_rejected, sig_fixture) {
   auto priv = private_key::generate();
   auto trx  = test::make_signed_trx({priv, priv}, chain_id, false);

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

/// 3. Invalid recoverable (tampered) → any fc::exception
BOOST_FIXTURE_TEST_CASE(invalid_sig_rejected, sig_fixture) {
   auto p1  = private_key::generate();
   auto trx = test::make_signed_trx({p1}, chain_id, false);

   // Tamper with the single signature blob
   auto packed = fc::raw::pack(trx.signatures[0]);
   packed[2] ^= 0xFF;
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

/// 4. Timeout path → any fc::exception
BOOST_FIXTURE_TEST_CASE(signature_deadline_timeout, sig_fixture) {
   auto priv = private_key::generate();
   auto trx  = test::make_signed_trx({priv}, chain_id, false);

   auto past = fc::time_point::now() - fc::microseconds(1);
   boost::container::flat_set<public_key> keys;
   BOOST_CHECK_THROW(
     trx.get_signature_keys(chain_id, past, keys, false),
     fc::exception
   );
}

/// 5. Zero signatures → empty
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

/// 6. Max signatures edge → all accepted
BOOST_FIXTURE_TEST_CASE(max_sigs_and_ext, sig_fixture) {
   constexpr size_t MAX = 32;
   std::vector<private_key> privs;
   privs.reserve(MAX);
   for (size_t i = 0; i < MAX; ++i)
      privs.push_back(private_key::generate());

   auto trx = test::make_signed_trx(privs, chain_id, false);

   boost::container::flat_set<public_key> keys;
   trx.get_signature_keys(chain_id,
                          fc::time_point::maximum(),
                          keys,
                          false);
   BOOST_CHECK_EQUAL(keys.size(), MAX);
}

// --- ED extension mismatch cases (no ED signatures) ---

/// 7. ED extension without ED signature → unsatisfied_authorization
BOOST_FIXTURE_TEST_CASE(ed_extension_without_sig_rejected, sig_fixture) {
   auto p1  = private_key::generate();
   auto trx = test::make_signed_trx({p1}, chain_id, false);

   // wrap the raw shim in the proper public_key variant and append a single ED pubkey extension
   fc::crypto::public_key pk(test::hardcoded_ed_pubkey);
   trx.transaction_extensions.emplace_back(
      test::ED_EXTENSION_ID,
      fc::raw::pack(pk)
   );

   boost::container::flat_set<public_key> keys;
   bool caught = false;
   try {
      trx.get_signature_keys(chain_id,
                             fc::time_point::maximum(),
                             keys,
                             false);
   } catch (const fc::exception& e) {
      if (std::strcmp(e.name(), "unsatisfied_authorization") == 0) {
         caught = true;
      }
   }
   if (!caught) {
      BOOST_FAIL("Expected unsatisfied_authorization but none thrown");
   }
}

/// 8. Multiple ED extensions, no ED signatures → unsatisfied_authorization
BOOST_FIXTURE_TEST_CASE(multiple_ed_exts_no_sig_rejected, sig_fixture) {
   auto p1  = private_key::generate();
   auto trx = test::make_signed_trx({p1}, chain_id, false);

   fc::crypto::public_key pk(test::hardcoded_ed_pubkey);
   // append it twice
   trx.transaction_extensions.emplace_back(test::ED_EXTENSION_ID,
                                          fc::raw::pack(pk));
   trx.transaction_extensions.emplace_back(test::ED_EXTENSION_ID,
                                          fc::raw::pack(pk));

   boost::container::flat_set<public_key> keys;
   bool caught = false;
   try {
      trx.get_signature_keys(chain_id,
                             fc::time_point::maximum(),
                             keys,
                             true);
   } catch (const fc::exception& e) {
      if (std::strcmp(e.name(), "unsatisfied_authorization") == 0) {
         caught = true;
      }
   }
   if (!caught) {
      BOOST_FAIL("Expected unsatisfied_authorization but none thrown");
   }
}

/// 9. Duplicate ED extension → tx_duplicate_sig
BOOST_FIXTURE_TEST_CASE(duplicate_ed_extension_rejected, sig_fixture) {
   auto p1  = private_key::generate();
   auto trx = test::make_signed_trx({p1}, chain_id, false);

    fc::crypto::public_key pk(test::hardcoded_ed_pubkey);
    // duplicate
    trx.transaction_extensions.emplace_back(test::ED_EXTENSION_ID,
                                           fc::raw::pack(pk));
    trx.transaction_extensions.emplace_back(test::ED_EXTENSION_ID,
                                           fc::raw::pack(pk));

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

BOOST_AUTO_TEST_SUITE_END()
