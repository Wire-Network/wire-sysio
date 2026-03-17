#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include "contracts.hpp"

#include <fc/variant_object.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/ripemd160.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

// Replicate the contract's pubkey_to_string for EM keys:
// "PUB_EM_" + hex(compressed_33_bytes)
static std::string contract_pubkey_to_string(const fc::crypto::public_key& pk) {
   const auto& shim = pk.get<fc::em::public_key_shim>();
   auto compressed = shim.serialize(); // std::array<char, 33>
   return "PUB_EM_" + fc::to_hex(compressed.data(), compressed.size());
}

// Build the message string exactly as the contract does
static std::string build_link_message(
   const fc::crypto::public_key& pub_key,
   const std::string& username,
   const std::string& chain_name,
   uint64_t nonce
) {
    auto pub_key_str = contract_pubkey_to_string(pub_key);
    return pub_key_str + "|" + username + "|" + chain_name + "|" + std::to_string(nonce) + "|createlink auth";
}

// ——— Tester class ———
class sysio_authex_tester : public tester {
public:
   static constexpr auto AUTHEX = "sysio.authex"_n;

   sysio_authex_tester() {
      produce_blocks( 2 );

      create_accounts( { "alice"_n, "bob"_n, "carol"_n, } );
      produce_blocks( 2 );

      set_code( AUTHEX, contracts::authex_wasm() );
      set_abi( AUTHEX, contracts::authex_abi().data() );
      set_privileged( AUTHEX );

      produce_blocks();

      const auto* accnt = control->find_account_metadata( AUTHEX );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_action( const account_name& signer, const action_name& name, const variant_object& data ) {
      string action_type_name = abi_ser.get_action_type(name);

      action act;
      act.account = AUTHEX;
      act.name    = name;
      act.data    = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function(abi_serializer_max_time)
      );

      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   action_result createlink(
      const account_name& signer,
      const std::string& chain_name,
      const std::string& username,
      const fc::crypto::signature& sig,
      const fc::crypto::public_key& pub_key,
      uint64_t nonce
   ) {
      return push_action( signer, "createlink"_n, mvo()
         ("chainName", chain_name)
         ("username",  username)
         ("sig",       sig)
         ("pubKey",    pub_key)
         ("nonce",     nonce)
      );
   }

   action_result clearlinks( const account_name& signer ) {
      return push_action( signer, "clearlinks"_n, mvo() );
   }

   uint64_t now_ms() {
      return control->head().block_time().time_since_epoch().count() / 1000;
   }

   struct em_link_data {
      fc::crypto::private_key priv;
      fc::crypto::public_key  pub;
      fc::crypto::signature   sig;
      uint64_t                nonce;
   };

   em_link_data make_eth_link(const std::string& username, uint64_t nonce) {
      auto priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
      auto pub  = priv.get_public_key();
      auto msg  = build_link_message(pub, username, "ethereum", nonce);

      // keccak(msg) → 32 bytes, same as what the contract computes
      auto msg_hash = fc::crypto::keccak256::hash(msg);

      // sign(sha256(keccak_bytes)) — sign_sha256 wraps with EIP-191 internally,
      // and assert_recover_key's recover also wraps with EIP-191, so they match.
      // k1_recover_uncompressed also applies EIP-191 for EM signatures.
      auto sig = priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));
      return { priv, pub, sig, nonce };
   }

   abi_serializer abi_ser;
};


BOOST_AUTO_TEST_SUITE(sysio_authex_tests)

// ——— clearlinks tests ———

BOOST_FIXTURE_TEST_CASE( clearlinks_requires_contract_auth, sysio_authex_tester ) try {
   BOOST_REQUIRE_EQUAL(
      error("missing authority of sysio.authex"),
      clearlinks("alice"_n)
   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( clearlinks_succeeds_with_contract_auth, sysio_authex_tester ) try {
   BOOST_REQUIRE_EQUAL( success(), clearlinks(AUTHEX) );
} FC_LOG_AND_RETHROW()

// ——— createlink: auth failures ———

BOOST_FIXTURE_TEST_CASE( createlink_requires_username_auth, sysio_authex_tester ) try {
   auto link = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL(
      error("missing authority of alice"),
      createlink("bob"_n, "ethereum", "alice", link.sig, link.pub, link.nonce)
   );
} FC_LOG_AND_RETHROW()

// ——— createlink: invalid chain ———

BOOST_FIXTURE_TEST_CASE( createlink_invalid_chain, sysio_authex_tester ) try {
   auto priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   auto pub  = priv.get_public_key();
   uint64_t nonce = now_ms();
   auto msg = build_link_message(pub, "alice", "bitcoin", nonce);
   auto msg_hash = fc::crypto::keccak256::hash(msg);
   auto sig = priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Invalid chain. See 'wnsmanager' contract for supported chains."),
      createlink("alice"_n, "bitcoin", "alice", sig, pub, nonce)
   );
} FC_LOG_AND_RETHROW()

// ——— createlink: stale nonce ———

BOOST_FIXTURE_TEST_CASE( createlink_stale_nonce, sysio_authex_tester ) try {
   uint64_t stale_nonce = now_ms() - (20 * 60 * 1000);
   auto link = make_eth_link("alice", stale_nonce);

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Invalid nonce: must be within the last 10 minutes"),
      createlink("alice"_n, "ethereum", "alice", link.sig, link.pub, link.nonce)
   );
} FC_LOG_AND_RETHROW()

// ——— createlink: successful ethereum link ———

BOOST_FIXTURE_TEST_CASE( createlink_eth_success, sysio_authex_tester ) try {
   auto link = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, "ethereum", "alice", link.sig, link.pub, link.nonce) );
   produce_blocks();

   // Verify that alice now has a permission named "ex.eth" with the linked public key
   auto& auth_mgr = control->get_authorization_manager();
   const auto* perm = auth_mgr.find_permission({"alice"_n, "ex.eth"_n});
   BOOST_REQUIRE( perm != nullptr );

   auto auth = perm->auth.to_authority();
   BOOST_REQUIRE_EQUAL( auth.keys.size(), 1u );
   BOOST_REQUIRE_EQUAL( auth.keys[0].key, link.pub );
   BOOST_REQUIRE_EQUAL( auth.keys[0].weight, 1u );
   BOOST_REQUIRE_EQUAL( auth.threshold, 1u );
} FC_LOG_AND_RETHROW()

// ——— createlink: duplicate pubkey ———

BOOST_FIXTURE_TEST_CASE( createlink_duplicate_pubkey, sysio_authex_tester ) try {
   auto link1 = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, "ethereum", "alice", link1.sig, link1.pub, link1.nonce) );
   produce_blocks();

   // Bob tries to link the same pubkey
   uint64_t nonce2 = now_ms();
   auto msg2 = build_link_message(link1.pub, "bob", "ethereum", nonce2);
   auto hash2 = fc::crypto::keccak256::hash(msg2);
   auto sig2 = link1.priv.sign(fc::sha256(reinterpret_cast<const char*>(hash2.data()), 32));

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Public key already linked to a different account."),
      createlink("bob"_n, "ethereum", "bob", sig2, link1.pub, nonce2)
   );
} FC_LOG_AND_RETHROW()

// ——— createlink: duplicate chain for same user ———

BOOST_FIXTURE_TEST_CASE( createlink_duplicate_chain_for_user, sysio_authex_tester ) try {
   auto link1 = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, "ethereum", "alice", link1.sig, link1.pub, link1.nonce) );
   produce_blocks();

   auto link2 = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Account already has a link for this curve."),
      createlink("alice"_n, "ethereum", "alice", link2.sig, link2.pub, link2.nonce)
   );
} FC_LOG_AND_RETHROW()

// ——— clearlinks + re-create ———

BOOST_FIXTURE_TEST_CASE( clearlinks_then_recreate, sysio_authex_tester ) try {
   auto link1 = make_eth_link("alice", now_ms());
   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, "ethereum", "alice", link1.sig, link1.pub, link1.nonce) );
   produce_blocks();

   // Clear and re-create should work
   BOOST_REQUIRE_EQUAL( success(), clearlinks(AUTHEX) );
   produce_blocks();

   auto link2 = make_eth_link("alice", now_ms());
   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, "ethereum", "alice", link2.sig, link2.pub, link2.nonce) );
   produce_blocks();
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
