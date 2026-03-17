#include <boost/test/unit_test.hpp>

#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/signer.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/utility.hpp>
#include <fc/variant.hpp>

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

   fc::variant test_pubkey_variant{test_public_key};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(test_public_key.to_string({}), test_public_key2.to_string({}));

   fc::variant test_privkey_variant{test_private_key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(test_private_key.to_string({}), test_private_key2.to_string({}));
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

   fc::variant test_pubkey_variant{test_public_key};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(test_public_key.to_string({}, true), test_public_key2.to_string({}, true));

   fc::variant test_privkey_variant{test_private_key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(test_private_key.to_string({}, true), test_private_key2.to_string({}, true));
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

   fc::variant test_pubkey_variant{test_public_key};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(test_public_key.to_string({}), test_public_key2.to_string({}));

   fc::variant test_privkey_variant{test_private_key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(test_private_key.to_string({}), test_private_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_k1_recovery) try {
   auto payload = "Test Cases";
   auto digest = sha256::hash(payload, const_strlen(payload));
   auto key = private_key::generate();
   auto pub = key.get_public_key();
   auto sig = key.sign(digest);

   auto recovered_pub = public_key::recover(sig, digest);

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}), fc::crypto::signature::from_string(sig.to_string({})).to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}, true), fc::crypto::signature::from_string(sig.to_string({}, true)).to_string({}, true));

   fc::variant sig_variant{sig};
   signature sig2 = sig_variant.as<signature>();
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}, true), sig2.to_string({}, true));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1_recovery) try {
   auto payload = "Test Cases";
   auto digest = sha256::hash(payload, const_strlen(payload));
   auto key = private_key::generate(private_key::key_type::r1);
   auto pub = key.get_public_key();
   auto sig = key.sign(digest);

   auto recovered_pub = public_key::recover(sig, digest);

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}), fc::crypto::signature::from_string(sig.to_string({})).to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}, true), fc::crypto::signature::from_string(sig.to_string({}, true)).to_string({}, true));

   fc::variant sig_variant{sig};
   signature sig2 = sig_variant.as<signature>();
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}, true), sig2.to_string({}, true));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_k1_recyle) try {
   auto key = private_key::generate();
   auto pub = key.get_public_key();
   auto pub_str = pub.to_string({});
   auto recycled_pub = public_key::from_string(pub_str);

   BOOST_CHECK_EQUAL(pub.to_string({}), recycled_pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_r1_recyle) try {
   auto key = private_key::generate(private_key::key_type::r1);
   auto pub = key.get_public_key();
   auto pub_str = pub.to_string({});
   auto recycled_pub = public_key::from_string(pub_str);

   BOOST_CHECK_EQUAL(pub.to_string({}), recycled_pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::em);
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

   fc::variant test_pubkey_variant{pub};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(pub.to_string({}), test_public_key2.to_string({}));

   fc::variant test_privkey_variant{key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(key.to_string({}), test_private_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_alt) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::em);
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

   fc::variant test_pubkey_variant{pub};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(pub.to_string({}, true), test_public_key2.to_string({}, true));

   fc::variant test_privkey_variant{key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(key.to_string({}, true), test_private_key2.to_string({}, true));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_recovery_of_trx) try {
   auto        payload    = "Test Cases";
   auto        digest_raw = fc::sha256::hash(payload); // pretend payload is a transaction
   auto        key     = fc::crypto::private_key::generate(private_key::key_type::em);
   auto        pub     = key.get_public_key();
   auto        sig     = key.sign(digest_raw);
   std::string sig_str = sig.to_string({});
   BOOST_TEST(fc::em::public_key::is_canonical(sig.get<em::signature_shim>()._data));

   auto recovered_pub = public_key(em::public_key_shim(sig.get<em::signature_shim>().recover(digest_raw)));

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(sig_str, fc::crypto::signature::from_string(sig_str, fc::crypto::signature::sig_type::em).to_string({}));
   auto alt_sig_str = sig.to_string({}, true);
   BOOST_TEST(alt_sig_str.starts_with("SIG_EM_"));
   BOOST_CHECK_EQUAL(alt_sig_str, fc::crypto::signature::from_string(alt_sig_str).to_string({}, true));

   fc::variant sig_variant{sig};
   signature sig2 = sig_variant.as<signature>();
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}, true), sig2.to_string({}, true));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_recovery_of_eth) try {
   auto        payload    = "Test Cases";
   auto        digest_raw = ethereum::hash_message(ethereum::to_uint8_span(payload)); // pretend payload is an eth transaction (not EIP-191)
   auto        key     = fc::crypto::private_key::generate(private_key::key_type::em);
   auto        pub     = key.get_public_key();
   auto        sig     = signature(signature::storage_type(key.get<em::private_key_shim>().sign_keccak256(digest_raw)));
   std::string sig_str = sig.to_string({});
   BOOST_TEST(fc::em::public_key::is_canonical(sig.get<em::signature_shim>()._data));

   auto recovered_pub = public_key(em::public_key_shim(sig.get<em::signature_shim>().recover_eth(digest_raw)));

   BOOST_CHECK_EQUAL(recovered_pub.to_string({}), pub.to_string({}));
   BOOST_CHECK_EQUAL(sig_str, fc::crypto::signature::from_string(sig_str, fc::crypto::signature::sig_type::em).to_string({}));
   auto alt_sig_str = sig.to_string({}, true);
   BOOST_TEST(alt_sig_str.starts_with("SIG_EM_"));
   BOOST_CHECK_EQUAL(alt_sig_str, fc::crypto::signature::from_string(alt_sig_str).to_string({}, true));

   fc::variant sig_variant{sig};
   signature sig2 = sig_variant.as<signature>();
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
   BOOST_CHECK_EQUAL(sig.to_string({}, true), sig2.to_string({}, true));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_em_is_canonical) try {
   fc::sha256 msg = fc::sha256::hash(std::string("hello canonical world"));

   // Generate a private key
   auto priv = fc::crypto::private_key::generate(private_key::key_type::em);
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
   fc::variant test_pubkey_variant{pub_key};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(pub_key.to_string({}), test_public_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_ed_priv_str) try {
   auto priv_str = "4YFq9y5f5hi77Bq8kDCE6VgqoAqKGSQN87yW9YeGybpNfqKUG4WxnwhboHGUeXjY7g8262mhL1kCCM9yy8uGvdj7";
   auto priv_key = fc::crypto::private_key::from_string(priv_str, private_key::key_type::ed);
   BOOST_CHECK_EQUAL(priv_str, priv_key.to_string({}));
   BOOST_TEST(priv_key.to_string({}, true).starts_with("PVT_ED_"));
   BOOST_TEST(priv_key.to_string({}, true).ends_with(priv_str));
   auto priv_key2 = fc::crypto::private_key::from_string(priv_key.to_string({}, true));
   BOOST_CHECK_EQUAL(priv_key.to_string({}), priv_key2.to_string({}));
   fc::variant test_privkey_variant{priv_key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(priv_key.to_string({}), test_private_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_ed_sig_str) try {
   // Generate a real 96-byte ED25519 signature (64 sig + 32 embedded pubkey)
   auto priv = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto digest = fc::sha256::hash(std::string("test_ed_sig_str"));
   auto sig = priv.sign(digest);
   auto sig_str = sig.to_string({});
   BOOST_TEST(!sig_str.empty());
   // Roundtrip: base58 -> signature -> base58
   auto sig_rt = fc::crypto::signature::from_string(sig_str, signature::sig_type::ed);
   BOOST_CHECK_EQUAL(sig_str, sig_rt.to_string({}));
   // Prefixed form
   BOOST_TEST(sig.to_string({}, true).starts_with("SIG_ED_"));
   BOOST_TEST(sig.to_string({}, true).ends_with(sig_str));
   auto sig2 = fc::crypto::signature::from_string(sig.to_string({}, true));
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
   // Variant roundtrip
   fc::variant test_sig_variant{sig};
   signature test_sig2 = test_sig_variant.as<signature>();
   BOOST_CHECK_EQUAL(sig.to_string({}), test_sig2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_bls_pub_str) try {
   auto pub_str = "PUB_BLS_no1gZTuy-0iL_FVrc6q4ow5KtTtdv6ZQ3Cq73Jwl32qRNC9AEQaWsESaoN4Y9NAVRmRGnbEekzgo6YlwbztPeoWhWzvHiOALTFKegRXlRxVbM4naOg33cZOSdS25i_MXywteRA";
   auto pub_key = fc::crypto::public_key::from_string(pub_str, public_key::key_type::bls);
   BOOST_CHECK_EQUAL(pub_str, pub_key.to_string({}));
   BOOST_TEST(pub_key.to_string({}, true).starts_with("PUB_BLS_"));
   BOOST_TEST(pub_key.to_string({}, true).ends_with(pub_str));
   auto pub_key2 = fc::crypto::public_key::from_string(pub_key.to_string({}, true));
   BOOST_CHECK_EQUAL(pub_key.to_string({}), pub_key2.to_string({}));
   fc::variant test_pubkey_variant{pub_key};
   public_key test_public_key2 = test_pubkey_variant.as<public_key>();
   BOOST_CHECK_EQUAL(pub_key.to_string({}), test_public_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_bls_priv_str) try {
   auto priv_str = "PVT_BLS_y_iMu9QYlZXK_Cdb-NEfSOQfJeWzm1-f-7p6V5MsiwsL1SQr";
   auto priv_key = fc::crypto::private_key::from_string(priv_str, private_key::key_type::bls);
   BOOST_CHECK_EQUAL(priv_str, priv_key.to_string({}));
   BOOST_TEST(priv_key.to_string({}, true).starts_with("PVT_BLS_"));
   BOOST_TEST(priv_key.to_string({}, true).ends_with(priv_str));
   auto priv_key2 = fc::crypto::private_key::from_string(priv_key.to_string({}, true));
   BOOST_CHECK_EQUAL(priv_key.to_string({}), priv_key2.to_string({}));
   fc::variant test_privkey_variant{priv_key};
   private_key test_private_key2 = test_privkey_variant.as<private_key>();
   BOOST_CHECK_EQUAL(priv_key.to_string({}), test_private_key2.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_bls_sig_str) try {
   auto sig_str = "SIG_BLS_Mo64VWr_Wxg6E1Tfsh0X_gTlBRce4P8qezw7-ylE6ydXDTzlzd8tmrKUMIEIN8YD2D0C68Fs0KrNuV_NVCDKn_lhnYuO6-X0WnwX6eIcQRdESO106gyTe-HPg13kAgUAV4sWdsR7ZNyEPft48-KWoTyyhpnhI0RPoy9ddooXob2jUAvICmQwwPpu_fTuJo8OQ02rBQdgux6jEw9b9TkVZCTrL3kznQvhbrbQxjhv3L-IPx6tthhiRKxgYKM8vFoF4aO-QQ";
   auto sig = fc::crypto::signature::from_string(sig_str, signature::sig_type::bls);
   BOOST_CHECK_EQUAL(sig_str, sig.to_string({}));
   BOOST_TEST(sig.to_string({}, true).starts_with("SIG_BLS_"));
   BOOST_TEST(sig.to_string({}, true).ends_with(sig_str));
   auto sig2 = fc::crypto::signature::from_string(sig.to_string({}, true));
   BOOST_CHECK_EQUAL(sig.to_string({}), sig2.to_string({}));
   fc::variant test_sig_variant{sig};
   signature test_sig2 = test_sig_variant.as<signature>();
   BOOST_CHECK_EQUAL(sig.to_string({}), test_sig2.to_string({}));
} FC_LOG_AND_RETHROW();

// --- sign_eth (shim-level): recovery round-trip with multiple messages ---
BOOST_AUTO_TEST_CASE(test_sign_eth_recovery_roundtrip) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::em);
   auto pub = key.get_public_key();
   auto& em_key = key.get<em::private_key_shim>();

   for (auto msg : {"hello", "world", "", "a]longer\npayload with special chars!@#$"}) {
      auto digest = ethereum::hash_message(ethereum::to_uint8_span(msg));
      auto sig = signature(signature::storage_type(em_key.sign_keccak256(digest)));

      // signature must be canonical (low-s)
      BOOST_TEST(em::public_key::is_canonical(sig.get<em::signature_shim>()._data));

      // recover via shim-level API and compare
      auto recovered = public_key(public_key::storage_type(sig.get<em::signature_shim>().recover_eth(digest)));
      BOOST_CHECK_EQUAL(recovered.to_string({}), pub.to_string({}));
   }
} FC_LOG_AND_RETHROW();

// --- sign_eth (shim-level): signing the same message twice produces the same signature (deterministic RFC-6979 nonce) ---
BOOST_AUTO_TEST_CASE(test_sign_eth_deterministic) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::em);
   auto& em_key = key.get<em::private_key_shim>();
   auto digest = ethereum::hash_message(ethereum::to_uint8_span("determinism check"));

   auto sig1 = signature(signature::storage_type(em_key.sign_keccak256(digest)));
   auto sig2 = signature(signature::storage_type(em_key.sign_keccak256(digest)));

   BOOST_CHECK_EQUAL(sig1.to_string({}), sig2.to_string({}));
} FC_LOG_AND_RETHROW();

// --- sign_solana (shim-level) + verify_solana round-trip ---
BOOST_AUTO_TEST_CASE(test_sign_solana_verify_roundtrip) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto pub = key.get_public_key();
   auto& ed_key = key.get<ed::private_key_shim>();
   auto& ed_pub = pub.get<ed::public_key_shim>();

   for (auto msg : {"hello", "solana tx payload", ""}) {
      auto span = ethereum::to_uint8_span(msg);
      auto sig = ed_key.sign_raw(span.data(), span.size());
      BOOST_TEST(sig.verify_solana(span.data(), span.size(), ed_pub));
   }
} FC_LOG_AND_RETHROW();

// --- verify_solana rejects a tampered message ---
BOOST_AUTO_TEST_CASE(test_verify_solana_rejects_tampered_message) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto pub = key.get_public_key();
   auto& ed_key = key.get<ed::private_key_shim>();
   auto& ed_pub = pub.get<ed::public_key_shim>();

   auto msg = ethereum::to_uint8_span("original message");
   auto sig = ed_key.sign_raw(msg.data(), msg.size());

   // Verify passes on original
   BOOST_TEST(sig.verify_solana(msg.data(), msg.size(), ed_pub));

   // Verify fails on different message
   auto bad = ethereum::to_uint8_span("tampered message");
   BOOST_TEST(!sig.verify_solana(bad.data(), bad.size(), ed_pub));
} FC_LOG_AND_RETHROW();

// --- verify_solana rejects wrong public key ---
BOOST_AUTO_TEST_CASE(test_verify_solana_rejects_wrong_key) try {
   auto key1 = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto pub1 = key1.get_public_key();
   auto key2 = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto pub2 = key2.get_public_key();

   auto msg = ethereum::to_uint8_span("signed by key1");
   auto sig = key1.get<ed::private_key_shim>().sign_raw(msg.data(), msg.size());

   BOOST_TEST( sig.verify_solana(msg.data(), msg.size(), pub1.get<ed::public_key_shim>()));
   BOOST_TEST(!sig.verify_solana(msg.data(), msg.size(), pub2.get<ed::public_key_shim>()));
} FC_LOG_AND_RETHROW();

// --- sign_solana (shim-level) on binary data (non-UTF8) ---
BOOST_AUTO_TEST_CASE(test_sign_solana_binary_payload) try {
   auto key = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto pub = key.get_public_key();
   auto& ed_key = key.get<ed::private_key_shim>();
   auto& ed_pub = pub.get<ed::public_key_shim>();

   std::array<uint8_t, 64> binary_msg{};
   for (size_t i = 0; i < binary_msg.size(); ++i)
      binary_msg[i] = static_cast<uint8_t>(i);

   auto sig = ed_key.sign_raw(binary_msg.data(), binary_msg.size());
   BOOST_TEST(sig.verify_solana(binary_msg.data(), binary_msg.size(), ed_pub));
} FC_LOG_AND_RETHROW();

// --- Solana verify rejects non-ED key types via FC_ASSERT ---
BOOST_AUTO_TEST_CASE(test_verify_solana_rejects_non_ed_key) try {
   auto em_key = fc::crypto::private_key::generate(private_key::key_type::em);
   auto em_pub = em_key.get_public_key();

   auto ed_priv = fc::crypto::private_key::generate(private_key::key_type::ed);
   auto& ed_key = ed_priv.get<ed::private_key_shim>();
   auto msg = ethereum::to_uint8_span("test");
   auto ed_sig = ed_key.sign_raw(msg.data(), msg.size());

   // Wrap in high-level types
   auto sig = signature(signature::storage_type(ed_sig));

   // do_verify should FC_ASSERT because em_pub doesn't contain ed::public_key_shim
   using solana_traits = signer_traits<chain_kind_solana, chain_key_type_solana>;
   BOOST_CHECK_THROW(
      solana_traits::do_verify(em_pub, sig, ethereum::to_uint8_span("test")),
      fc::exception);
} FC_LOG_AND_RETHROW();

// ===========================================================================
// signer tests
// ===========================================================================

namespace {

/// Create a signature_provider_t from a private key
signature_provider_t make_provider(const private_key& key, chain_key_type_t key_type) {
   signature_provider_t p;
   p.key_type    = key_type;
   p.public_key  = key.get_public_key();
   p.private_key = key;
   p.sign        = [key](const sha256& d) { return key.sign(d); };
   return p;
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(test_eth_client_signer_sign_recover) try {
   auto key      = private_key::generate(private_key::key_type::em);
   auto pub      = key.get_public_key();
   auto provider = make_provider(key, chain_key_type_ethereum);

   eth_client_signer s(provider);

   auto payload = ethereum::to_uint8_span("eth transaction bytes");
   auto sig     = s.sign(payload);

   auto recovered = s.recover(sig, payload);
   BOOST_CHECK_EQUAL(recovered.to_string({}), pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_wire_eth_signer_sign_recover) try {
   auto key      = private_key::generate(private_key::key_type::em);
   auto pub      = key.get_public_key();
   auto provider = make_provider(key, chain_key_type_ethereum);

   wire_eth_signer s(provider);

   auto digest = sha256::hash("wire transaction digest");
   auto sig    = s.sign(digest);

   auto recovered = s.recover(sig, digest);
   BOOST_CHECK_EQUAL(recovered.to_string({}), pub.to_string({}));
} FC_LOG_AND_RETHROW();

// --- wire_eth_signer must produce the same signature as em::sign_sha256 (the shim-level Wire signing path) ---
BOOST_AUTO_TEST_CASE(test_wire_eth_signer_matches_sign_sha256) try {
   auto key      = private_key::generate(private_key::key_type::em);
   auto provider = make_provider(key, chain_key_type_ethereum);
   auto& em_key  = key.get<em::private_key_shim>();

   wire_eth_signer s(provider);

   for (auto msg : {"hello", "wire transaction", "deterministic signing check"}) {
      auto digest = sha256::hash(msg);

      // signer path
      auto signer_sig = s.sign(digest);

      // shim-level path (the ground truth)
      auto shim_sig = signature(signature::storage_type(em_key.sign_sha256(digest)));

      BOOST_CHECK_EQUAL(signer_sig.to_string({}), shim_sig.to_string({}));
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_sol_client_signer_sign_verify) try {
   auto key      = private_key::generate(private_key::key_type::ed);
   auto pub      = key.get_public_key();
   auto provider = make_provider(key, chain_key_type_solana);

   sol_client_signer s(provider);

   auto payload = ethereum::to_uint8_span("solana transaction bytes");
   auto sig     = s.sign(payload);

   BOOST_TEST(s.verify(sig, payload));

   // Tampered message should fail
   auto bad = ethereum::to_uint8_span("tampered bytes");
   BOOST_TEST(!s.verify(sig, bad));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_wire_signer_k1) try {
   auto key      = private_key::generate();
   auto pub      = key.get_public_key();
   auto provider = make_provider(key, chain_key_type_wire);

   wire_signer s(provider);

   auto digest = sha256::hash("wire k1 transaction");
   auto sig    = s.sign(digest);

   // K1 signatures are recoverable via the standard path
   auto recovered = public_key::recover(sig, digest);
   BOOST_CHECK_EQUAL(recovered.to_string({}), pub.to_string({}));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_wire_signer_ed) try {
   auto key      = private_key::generate(private_key::key_type::ed);
   auto pub      = key.get_public_key();
   auto provider = make_provider(key, chain_key_type_solana);

   wire_signer s(provider);

   auto digest = sha256::hash("wire ed transaction");
   auto sig    = s.sign(digest);

   // ED signatures use verify (not recover)
   auto& ed_sig = sig.get<ed::signature_shim>();
   auto& ed_pub = pub.get<ed::public_key_shim>();
   BOOST_TEST(ed_sig.verify(digest, ed_pub));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_wire_signer_sol) try {
   auto key      = private_key::generate(private_key::key_type::ed);
   auto pub      = key.get_public_key();
   auto provider = make_provider(key, chain_key_type_solana);

   wire_signer s(provider);

   auto digest = sha256::hash("wire sol transaction");
   auto sig    = s.sign(digest);

   // wire_signer with ED key signs hex-encoded sha256 via sign(sha256).
   // Verify using shim-level verify_solana with the same hex-encoded bytes.
   auto& ed_sig = sig.get<ed::signature_shim>();
   auto& ed_pub = pub.get<ed::public_key_shim>();
   auto hex   = digest.str();
   auto bytes = ethereum::to_uint8_span(hex);
   BOOST_TEST(ed_sig.verify_solana(bytes.data(), bytes.size(), ed_pub));

   // Tampered digest should fail
   auto bad_digest = sha256::hash("tampered");
   auto bad_hex    = bad_digest.str();
   auto bad_bytes  = ethereum::to_uint8_span(bad_hex);
   BOOST_TEST(!ed_sig.verify_solana(bad_bytes.data(), bad_bytes.size(), ed_pub));
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(test_signer_rejects_wrong_key_type) try {
   auto key      = private_key::generate();
   auto provider = make_provider(key, chain_key_type_wire);

   // Trying to use a K1 provider with eth_client_signer should fail
   BOOST_CHECK_THROW(eth_client_signer{provider}, fc::exception);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()