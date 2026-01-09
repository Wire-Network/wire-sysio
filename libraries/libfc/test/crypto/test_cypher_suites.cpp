#include <boost/test/unit_test.hpp>

#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/utility.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>

using namespace fc::crypto;
using namespace fc;

BOOST_AUTO_TEST_SUITE(cypher_suites)

BOOST_AUTO_TEST_CASE(test_parse) try {

} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_k1) try {
   auto private_key_string = std::string("5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
   auto expected_public_key_string = std::string("SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV");
   auto test_private_key = private_key::from_string(private_key_string);
   auto test_public_key = test_private_key.get_public_key();
   auto expected_public_key = public_key::from_string(expected_public_key_string);

   BOOST_CHECK_EQUAL(private_key_string, test_private_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key_string, test_public_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key.to_string({}), test_public_key.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_k1_alt) try {
   auto private_key_string = std::string("PVT_K1_2Nb6TgdiEJopY3dCFYhrA46NzXC3DSWzznes1Dc71oWF29tJTt");
   auto expected_public_key_string = std::string("PUB_K1_8mycTeevEWPgQhjMM9QcLyGmpc93pLQrhD9hJVmMiAP9WH6jd8");
   auto test_private_key = private_key::from_string(private_key_string);
   auto test_public_key = test_private_key.get_public_key();
   auto expected_public_key = public_key::from_string(expected_public_key_string);

   // to_string generates legacy format by default
   BOOST_CHECK_EQUAL("5KBsg98ingso5YRuESzbwCoDbbM7FMJbiorS6BGMLtf9QgL2bS9", test_private_key.to_string({}));
   BOOST_CHECK_EQUAL("SYS8mycTeevEWPgQhjMM9QcLyGmpc93pLQrhD9hJVmMiAP9WyPhdD", test_public_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key_string, test_public_key.to_string({}, true));
   BOOST_CHECK_EQUAL(expected_public_key.to_string({}), test_public_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key.to_string({}, true), test_public_key.to_string({}, true));
   BOOST_CHECK_EQUAL(private_key_string, test_private_key.to_string({}, true));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1) try {
   auto private_key_string = std::string("PVT_R1_iyQmnyPEGvFd8uffnk152WC2WryBjgTrg22fXQryuGL9mU6qW");
   auto expected_public_key_string = std::string("PUB_R1_6EPHFSKVYHBjQgxVGQPrwCxTg7BbZ69H9i4gztN9deKTEXYne4");
   auto test_private_key = private_key::from_string(private_key_string);
   auto test_public_key = test_private_key.get_public_key();
   auto expected_public_key = public_key::from_string(expected_public_key_string);

   BOOST_CHECK_EQUAL(private_key_string, test_private_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key_string, test_public_key.to_string({}));
   BOOST_CHECK_EQUAL(expected_public_key.to_string({}), test_public_key.to_string({}));
   // always includes prefix
   BOOST_CHECK_EQUAL(private_key_string, test_private_key.to_string({}, true));
   BOOST_CHECK_EQUAL(expected_public_key_string, test_public_key.to_string({}, true));
   BOOST_CHECK_EQUAL(expected_public_key.to_string({}), test_public_key.to_string({}, true));
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
   auto recycled_pub = public_key::from_string(pub_str);

   BOOST_CHECK_EQUAL(pub.to_string({}), recycled_pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1_recyle) try {
   auto key = private_key::generate<r1::private_key_shim>();
   auto pub = key.get_public_key();
   auto pub_str = pub.to_string({});
   auto recycled_pub = public_key::from_string(pub_str);

   BOOST_CHECK_EQUAL(pub.to_string({}), recycled_pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em) try {
   auto key = fc::crypto::private_key::generate<em::private_key_shim>();
   auto pub = key.get_public_key();
   auto priv_str = key.to_string({});
   auto pub_str = pub.to_string({});

   auto recycled_priv = fc::crypto::private_key::from_string(priv_str, private_key::key_type::em);
   auto recycled_pub = recycled_priv.get_public_key();
   auto pub_key = fc::crypto::public_key::from_string(pub_str, public_key::key_type::em);

   BOOST_CHECK_EQUAL(priv_str, recycled_priv.to_string({}));
   BOOST_CHECK_EQUAL(pub_str, recycled_pub.to_string({}));
   BOOST_CHECK_EQUAL(pub_str, pub_key.to_string({}));
   BOOST_TEST(pub.to_string({}).starts_with("0x"));
   BOOST_TEST(key.to_string({}).starts_with("0x"));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_alt) try {
   auto key = fc::crypto::private_key::generate<em::private_key_shim>();
   auto pub = key.get_public_key();
   auto priv_str = key.to_string({}, true);
   auto pub_str = pub.to_string({}, true);

   auto recycled_priv = fc::crypto::private_key::from_string(priv_str);
   auto recycled_pub = recycled_priv.get_public_key();
   auto pub_key = fc::crypto::public_key::from_string(pub_str);

   BOOST_CHECK_EQUAL(priv_str, recycled_priv.to_string({}, true));
   BOOST_CHECK_EQUAL(pub_str, recycled_pub.to_string({}, true));
   BOOST_CHECK_EQUAL(pub_str, pub_key.to_string({}, true));
   BOOST_TEST(pub.to_string({}, true).starts_with("PUB_EM_"));
   BOOST_TEST(key.to_string({}, true).starts_with("PVT_EM_"));
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

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(sig_str, fc::crypto::signature::from_string(sig_str, fc::crypto::signature::sig_type::em).to_string({}));
   auto alt_sig_str = sig.to_string({}, true);
   BOOST_TEST(alt_sig_str.starts_with("SIG_EM_"));
   BOOST_CHECK_EQUAL(alt_sig_str, fc::crypto::signature::from_string(alt_sig_str).to_string({}, true));
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

BOOST_AUTO_TEST_CASE(test_ed_pub_str) try {
   auto pub_str = "5oNDL3swdJJF1g9DzJiZ4ynHXgszjAEpUkxVYejchzrY";
   auto pub_key = fc::crypto::public_key::from_string(pub_str, public_key::key_type::ed);
   BOOST_CHECK_EQUAL(pub_str, pub_key.to_string({}));
   BOOST_TEST(pub_key.to_string({}, true).starts_with("PUB_ED_"));
   BOOST_TEST(pub_key.to_string({}, true).ends_with(pub_str));
   auto pub_key2 = fc::crypto::public_key::from_string(pub_key.to_string({}, true));
   BOOST_CHECK_EQUAL(pub_key.to_string({}), pub_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_ed_priv_str) try {
   auto priv_str = "4YFq9y5f5hi77Bq8kDCE6VgqoAqKGSQN87yW9YeGybpNfqKUG4WxnwhboHGUeXjY7g8262mhL1kCCM9yy8uGvdj7";
   auto priv_key = fc::crypto::private_key::from_string(priv_str, private_key::key_type::ed);
   BOOST_CHECK_EQUAL(priv_str, priv_key.to_string({}));
   BOOST_TEST(priv_key.to_string({}, true).starts_with("PVT_ED_"));
   BOOST_TEST(priv_key.to_string({}, true).ends_with(priv_str));
   auto priv_key2 = fc::crypto::private_key::from_string(priv_key.to_string({}, true));
   BOOST_CHECK_EQUAL(priv_key.to_string({}), priv_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_ed_sig_str) try {
   auto sig_str = "4cdd1oX7cfVALfr26tP52BZ6cSzrgnNGtYD7BFhm6FFeZV5sPTnRvg6NRn8yC6DbEikXcrNChBM5vVJnTgKhGhVu";
   auto sig = fc::crypto::signature::from_string(sig_str, signature::sig_type::ed);
   BOOST_CHECK_EQUAL(sig_str, sig.to_string({}));
   BOOST_TEST(sig.to_string({}, true).starts_with("SIG_ED_"));
   BOOST_TEST(sig.to_string({}, true).ends_with(sig_str));
   auto sig2 = fc::crypto::signature::from_string(sig.to_string({}, true));
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()