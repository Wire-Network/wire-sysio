/// Unit tests for `sysio::opp::depot::authex_link`, the host-side builder of the `sysio.authex::createlink`
/// ownership proof that clio's `system createlink` and the contract tests share.
///
/// The expected values are known answers from an implementation independent of libfc: Python's `cryptography`
/// package for Ed25519 (deterministic, so the whole Solana signature is pinned) and a reference keccak-256 for the
/// Ethereum digest. The Ethereum signature itself is not pinned here: libfc's EIP-191 signer has its own
/// known-answer test against eth_account (tests/clio_em_key_test.py), so this test checks that the signature
/// recovers to the key over the pinned digest. Whether the contract accepts these proofs is covered by
/// contracts/tests/sysio.authex_tests.cpp, which signs through the same helper.
///
/// Vectors (regenerate only deliberately, from the reference implementation):
///   account "alice", nonce 1767225600000 (2026-01-01T00:00:00Z)
///   ED key: the ed25519 seed 0x01..0x20
///   EM key: the secret 0x46 * 32 (the EIP-155 example key)

#include <boost/test/unit_test.hpp>

#include <sysio/opp/depot/authex_link.hpp>

#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/exception/exception.hpp>

#include <string>
#include <string_view>

namespace authex_link = sysio::opp::depot::authex_link;
namespace types = sysio::opp::types;

BOOST_AUTO_TEST_SUITE(authex_link_tests)

namespace {

constexpr std::string_view account = "alice";
constexpr uint64_t         nonce   = 1767225600000;

constexpr std::string_view ed_private =
   "PVT_ED_2Ana1pUpv2ZbMVkwF5FXapYeBEjdxDatLn7nvJkhgTSdZd8hbDHTd21as7EAsg7ypityqfsw2pMQKJcVDVcAEsd";
constexpr std::string_view ed_public  = "PUB_ED_9C6hybhQ6Aycep9jaUnP6uL9ZYvDjUp1aSkFWPUFJtpj";
constexpr std::string_view ed_message =
   "PUB_ED_9C6hybhQ6Aycep9jaUnP6uL9ZYvDjUp1aSkFWPUFJtpj|alice|3|1767225600000|createlink auth";
constexpr std::string_view ed_digest = "52576a21795026604e7957527a445e4159343658314f566e4854755c32472b4f";
constexpr std::string_view ed_signature =
   "SIG_ED_ivgGc8ismkQ6a83ZZQbDcigJvHZd8vpuDcjDsQUAGXeC1SPSWycUW2d4LFSE6GNhy8tvUM4n2z3azdGFUivSVLmfxUZ9iBWbgwPCG9TU1Jp"
   "JDWitWUg6DwH8L5fReUAWJ2v";

constexpr std::string_view em_private = "PVT_EM_0x4646464646464646464646464646464646464646464646464646464646464646";
constexpr std::string_view em_message =
   "PUB_EM_024bc2a31265153f07e70e0bab08724e6b85e217f8cd628ceb62974247bb493382|alice|2|1767225600000|createlink auth";
constexpr std::string_view em_digest = "53d6ccc910ded561976f11ec3863e5e1eeb8ed93f84a33d0381734e893b336c8";
/// The EM key written with the WIRE chain kind: a pairing the contract refuses, which tests still need to build.
constexpr std::string_view em_wire_message =
   "PUB_EM_024bc2a31265153f07e70e0bab08724e6b85e217f8cd628ceb62974247bb493382|alice|1|1767225600000|createlink auth";

fc::crypto::private_key parse_private(std::string_view key) {
   return fc::crypto::private_key::from_string(std::string{key});
}

/// A K1 key (the EIP-155 example secret in Wire's K1 form): valid for Wire signing, but createlink links no K1 key.
constexpr std::string_view k1_private_text = "PVT_K1_Xx4wsMynSZ39WiwEn8wzRL9trcKK33ZzwaZKJmymYx5UTjKvv";

fc::crypto::private_key k1_private() { return parse_private(k1_private_text); }

} // namespace

BOOST_AUTO_TEST_CASE(chain_kind_pairs_em_with_evm_and_ed_with_svm) {
   BOOST_CHECK_EQUAL(types::CHAIN_KIND_EVM, authex_link::chain_kind_for(parse_private(em_private).get_public_key()));
   BOOST_CHECK_EQUAL(types::CHAIN_KIND_SVM, authex_link::chain_kind_for(parse_private(ed_private).get_public_key()));
}

BOOST_AUTO_TEST_CASE(other_key_types_are_refused) {
   const auto k1 = k1_private();
   BOOST_CHECK_THROW(authex_link::chain_kind_for(k1.get_public_key()), fc::invalid_arg_exception);
   BOOST_CHECK_THROW(authex_link::message_public_key(k1.get_public_key()), fc::invalid_arg_exception);
   BOOST_CHECK_THROW(authex_link::signing_digest(em_message, k1.get_public_key()), fc::invalid_arg_exception);
   BOOST_CHECK_THROW(authex_link::sign(k1, account, nonce), fc::invalid_arg_exception);
   BOOST_CHECK_THROW(authex_link::sign(k1, account, types::CHAIN_KIND_EVM, nonce), fc::invalid_arg_exception);
}

BOOST_AUTO_TEST_CASE(ed_message_spells_the_key_as_base58_of_the_raw_point) {
   const auto key = parse_private(ed_private).get_public_key();
   BOOST_CHECK_EQUAL(std::string{ed_public}, key.to_string({}, true));
   BOOST_CHECK_EQUAL(std::string{ed_public}, authex_link::message_public_key(key));
   BOOST_CHECK_EQUAL(std::string{ed_message}, authex_link::build_message(key, account, types::CHAIN_KIND_SVM, nonce));
}

BOOST_AUTO_TEST_CASE(em_message_spells_the_key_as_hex_of_the_compressed_point) {
   const auto key = parse_private(em_private).get_public_key();
   // fc displays an EM key as its uncompressed point; the message must not.
   BOOST_CHECK(key.to_string({}, true) != authex_link::message_public_key(key));
   BOOST_CHECK_EQUAL(std::string{em_message}, authex_link::build_message(key, account, types::CHAIN_KIND_EVM, nonce));
}

BOOST_AUTO_TEST_CASE(message_writes_the_chain_kind_given) {
   const auto key = parse_private(em_private).get_public_key();
   BOOST_CHECK_EQUAL(std::string{em_wire_message},
                     authex_link::build_message(key, account, types::CHAIN_KIND_WIRE, nonce));
}

BOOST_AUTO_TEST_CASE(message_keeps_a_dotted_account_name_verbatim) {
   const auto key     = parse_private(ed_private).get_public_key();
   const auto message = authex_link::build_message(key, "wire.ab12cd3", types::CHAIN_KIND_SVM, nonce);
   BOOST_CHECK(message.find("|wire.ab12cd3|") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ed_digest_is_sha256_mapped_into_printable_ascii) {
   const auto key    = parse_private(ed_private).get_public_key();
   const auto digest = authex_link::signing_digest(ed_message, key);
   BOOST_CHECK_EQUAL(std::string{ed_digest}, digest.str());
   for (size_t i = 0; i < fc::sha256::byte_size; ++i) {
      const auto byte = static_cast<unsigned char>(digest.data()[i]);
      BOOST_CHECK_GE(byte, authex_link::ed_digest_printable_min);
      BOOST_CHECK_LT(byte, authex_link::ed_digest_printable_min + authex_link::ed_digest_printable_range);
   }
}

BOOST_AUTO_TEST_CASE(em_digest_is_keccak256_of_the_message) {
   const auto key = parse_private(em_private).get_public_key();
   BOOST_CHECK_EQUAL(std::string{em_digest}, authex_link::signing_digest(em_message, key).str());
}

BOOST_AUTO_TEST_CASE(ed_signature_matches_the_reference_wallet) {
   const auto signature = authex_link::sign(parse_private(ed_private), account, nonce);
   BOOST_CHECK_EQUAL(std::string{ed_signature}, signature.to_string({}, true));
}

BOOST_AUTO_TEST_CASE(ed_signature_verifies_over_the_digest) {
   const auto priv      = parse_private(ed_private);
   const auto signature = authex_link::sign(priv, account, nonce);
   const auto digest    = authex_link::signing_digest(ed_message, priv.get_public_key());
   const auto recovered = fc::crypto::public_key::recover(signature, digest);
   BOOST_CHECK(recovered == priv.get_public_key());
}

BOOST_AUTO_TEST_CASE(em_signature_recovers_the_key_over_the_digest) {
   const auto priv      = parse_private(em_private);
   const auto signature = authex_link::sign(priv, account, nonce);
   const auto digest    = authex_link::signing_digest(em_message, priv.get_public_key());
   const auto recovered = fc::crypto::public_key::recover(signature, digest);
   BOOST_CHECK(recovered == priv.get_public_key());
}

BOOST_AUTO_TEST_CASE(default_chain_kind_signs_the_same_proof) {
   for (const auto key : {em_private, ed_private}) {
      const auto priv = parse_private(key);
      BOOST_CHECK(authex_link::sign(priv, account, nonce) ==
                  authex_link::sign(priv, account, authex_link::chain_kind_for(priv.get_public_key()), nonce));
   }
}

BOOST_AUTO_TEST_CASE(proof_depends_on_account_and_nonce) {
   const auto priv = parse_private(ed_private);
   const auto base = authex_link::sign(priv, account, nonce);
   BOOST_CHECK(!(base == authex_link::sign(priv, "bob", nonce)));
   BOOST_CHECK(!(base == authex_link::sign(priv, account, nonce + 1)));
}

BOOST_AUTO_TEST_SUITE_END()
