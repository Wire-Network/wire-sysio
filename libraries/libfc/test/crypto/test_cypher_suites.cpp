#include <boost/test/unit_test.hpp>

#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/utility.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>

using namespace fc::crypto;
using namespace fc;

BOOST_AUTO_TEST_SUITE(cypher_suites)
BOOST_AUTO_TEST_CASE(test_k1) try {
   auto private_key_string = std::string("5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
   auto expected_public_key = std::string("SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV");
   auto test_private_key = private_key(private_key_string);
   auto test_public_key = test_private_key.get_public_key();

   BOOST_CHECK_EQUAL(private_key_string, test_private_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key, test_public_key.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1) try {
   auto private_key_string = std::string("PVT_R1_iyQmnyPEGvFd8uffnk152WC2WryBjgTrg22fXQryuGL9mU6qW");
   auto expected_public_key = std::string("PUB_R1_6EPHFSKVYHBjQgxVGQPrwCxTg7BbZ69H9i4gztN9deKTEXYne4");
   auto test_private_key = private_key(private_key_string);
   auto test_public_key = test_private_key.get_public_key();

   BOOST_CHECK_EQUAL(private_key_string, test_private_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key, test_public_key.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_k1_recovery) try {
   auto payload = "Test Cases";
   auto digest = sha256::hash(payload, const_strlen(payload));
   auto key = private_key::generate<ecc::private_key_shim>();
   auto pub = key.get_public_key();
   auto sig = key.sign(digest);

   auto recovered_pub = public_key(sig, digest);
   std::cout << recovered_pub.to_string({}) << std::endl;

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1_recovery) try {
   auto payload = "Test Cases";
   auto digest = sha256::hash(payload, const_strlen(payload));
   auto key = private_key::generate<r1::private_key_shim>();
   auto pub = key.get_public_key();
   auto sig = key.sign(digest);

   auto recovered_pub = public_key(sig, digest);
   std::cout << recovered_pub.to_string({}) << std::endl;

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_k1_recyle) try {
   auto key = private_key::generate<ecc::private_key_shim>();
   auto pub = key.get_public_key();
   auto pub_str = pub.to_string({});
   auto recycled_pub = public_key(pub_str);

   std::cout << pub.to_string({}) << " -> " << recycled_pub.to_string({}) << std::endl;

   BOOST_CHECK_EQUAL(pub.to_string({}), recycled_pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1_recyle) try {
   auto key = private_key::generate<r1::private_key_shim>();
   auto pub = key.get_public_key();
   auto pub_str = pub.to_string({});
   auto recycled_pub = public_key(pub_str);

   std::cout << pub.to_string({}) << " -> " << recycled_pub.to_string({}) << std::endl;

   BOOST_CHECK_EQUAL(pub.to_string({}), recycled_pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em) try {
   auto key = fc::crypto::private_key::generate<em::private_key_shim>();
   auto pub = key.get_public_key();
   auto priv_str = key.to_string({});
   auto pub_str = pub.to_string({});

   auto recycled_priv = fc::crypto::private_key(priv_str);
   auto recycled_pub = recycled_priv.get_public_key();

   BOOST_CHECK_EQUAL(priv_str, recycled_priv.to_string({}));
   BOOST_CHECK_EQUAL(pub_str, recycled_pub.to_string({}));
   BOOST_TEST(pub.to_string({}).starts_with("PUB_EM_"));
   BOOST_TEST(key.to_string({}).starts_with("PVT_EM_"));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_recovery) try {
   auto        payload    = "Test Cases";
   auto        digest_raw = ethereum::hash_message(payload);
   sha256      digest(reinterpret_cast<const char*>(digest_raw.data()),digest_raw.size());
   auto        key     = fc::crypto::private_key::generate<em::private_key_shim>();
   auto        pub     = key.get_public_key();
   auto        sig     = key.sign(digest);
   std::string sig_str = sig.to_string({});
   BOOST_TEST(fc::em::public_key::is_canonical(sig.get<em::signature_shim>()._data));

   auto recovered_pub = public_key(em::public_key_shim(sig.get<em::signature_shim>().recover_ex(payload, true)));
   // TODO: @jglanz Debug in detail - above works, but using the facade doesn't
   //   auto recovered_pub = fc::crypto::public_key(sig, digest);

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(sig_str, fc::crypto::signature(sig_str).to_string({}));
   BOOST_TEST(sig_str.starts_with("SIG_EM_"));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_is_canonical) try {
   fc::sha256 msg = fc::sha256::hash(std::string("hello canonical world"));

   // Generate a private key
   auto priv = fc::crypto::private_key::generate<em::private_key_shim>();
   auto sig = priv.sign(msg);

   // Force S > n/2 to simulate a non-canonical signature
   signature non_canonical = sig;
   const_cast<em::compact_signature*>(&non_canonical.get<em::signature_shim>()._data)->at(32) ^= 0x80;  // flip highest bit of S to make it > n/2 artificially

   BOOST_TEST(em::public_key::is_canonical(sig.get<em::signature_shim>()._data));
   BOOST_TEST(!em::public_key::is_canonical(non_canonical.get<em::signature_shim>()._data));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()