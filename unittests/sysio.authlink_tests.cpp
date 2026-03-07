#include <sysio/chain/abi_serializer.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/key_serdes.hpp>

#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

// ——— Helper: build the message string the contract expects ———
static std::string build_link_message(
   const fc::crypto::public_key& pub_key,
   const std::string& username,
   const std::string& chain_name,
   uint64_t nonce
) {
   // The contract builds: pubkey_to_string(pubKey) + "|" + username + "|" + chainName + "|" + nonce + "|createlink auth"
   std::string pk_str = pub_key.to_string();
   return pk_str + "|" + username + "|" + chain_name + "|" + std::to_string(nonce) + "|createlink auth";
}

// ——— Tester class ———
template<typename T>
class sysio_authlink_tester : public T {
public:
   static constexpr name contract_name{"authlnk"_n};

   sysio_authlink_tester() {
      T::produce_block();

      T::create_accounts( { "alice"_n, "bob"_n, contract_name } );
      T::produce_block();

      T::set_code( contract_name, test_contracts::sysio_authlink_wasm() );
      T::set_abi( contract_name, test_contracts::sysio_authlink_abi() );

      // The contract sends inline actions to itself with owner permission
      T::set_authority( contract_name, "owner"_n, T::get_public_key(contract_name, "owner"),
                        {} );
      T::link_authority( contract_name, contract_name, "owner"_n, "onlinkauth"_n );

      T::produce_block();

      const auto* accnt = T::control->find_account_metadata( contract_name );
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( T::abi_serializer_max_time ));
   }

   typename T::action_result push_action(
      const account_name& signer,
      const action_name& name,
      const variant_object& data
   ) {
      string action_type_name = abi_ser.get_action_type(name);

      action act;
      act.account = contract_name;
      act.name    = name;
      act.data    = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function( T::abi_serializer_max_time )
      );

      return T::push_contract_paid_action( std::move(act), signer.to_uint64_t() );
   }

   typename T::action_result createlink(
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

   typename T::action_result clearlinks(const account_name& signer) {
      return push_action( signer, "clearlinks"_n, mvo() );
   }

   fc::variant get_links_table() {
      vector<char> data = T::get_row_by_account(
         contract_name, contract_name, "links"_n, name(0)
      );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "links_s", data,
         abi_serializer::create_yield_function( T::abi_serializer_max_time )
      );
   }

   abi_serializer abi_ser;
};

using sysio_authlink_testers = boost::mpl::list<sysio_authlink_tester<savanna_tester>>;

BOOST_AUTO_TEST_SUITE(sysio_authlink_tests)

// ——— clearlinks action tests ———

BOOST_AUTO_TEST_CASE_TEMPLATE( clearlinks_requires_contract_auth, T, sysio_authlink_testers ) try {
   T chain;

   // Clearing links from a non-contract account should fail
   BOOST_REQUIRE_EQUAL(
      chain.wasm_assert_msg("missing authority of authlnk"),
      chain.clearlinks("alice"_n)
   );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE_TEMPLATE( clearlinks_succeeds_with_contract_auth, T, sysio_authlink_testers ) try {
   T chain;

   // Contract can clear its own links table
   BOOST_REQUIRE_EQUAL( chain.success(), chain.clearlinks(T::contract_name) );

} FC_LOG_AND_RETHROW()

// ——— createlink action tests — auth failures ———

BOOST_AUTO_TEST_CASE_TEMPLATE( createlink_requires_username_auth, T, sysio_authlink_testers ) try {
   T chain;

   // Generate an EM key pair for Ethereum
   auto em_priv = fc::crypto::private_key::generate<fc::crypto::em::private_key_shim>();
   auto em_pub  = em_priv.get_public_key();

   // Build the message the contract expects
   uint64_t nonce = chain.control->head_block_time().time_since_epoch().count() / 1000;
   auto msg = build_link_message(em_pub, "alice", "ethereum", nonce);

   // Keccak hash of the message
   auto msg_hash = fc::crypto::keccak256::hash(msg);

   // Sign with EM private key
   auto sig = em_priv.sign_compact(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));

   // bob tries to create a link for alice — should fail with missing auth
   BOOST_REQUIRE_THROW(
      chain.createlink("bob"_n, "ethereum", "alice", sig, em_pub, nonce),
      missing_auth_exception
   );

} FC_LOG_AND_RETHROW()

// ——— createlink action tests — invalid chain ———

BOOST_AUTO_TEST_CASE_TEMPLATE( createlink_invalid_chain, T, sysio_authlink_testers ) try {
   T chain;

   auto em_priv = fc::crypto::private_key::generate<fc::crypto::em::private_key_shim>();
   auto em_pub  = em_priv.get_public_key();

   uint64_t nonce = chain.control->head_block_time().time_since_epoch().count() / 1000;
   auto msg = build_link_message(em_pub, "alice", "bitcoin", nonce);
   auto msg_hash = fc::crypto::keccak256::hash(msg);
   auto sig = em_priv.sign_compact(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));

   BOOST_REQUIRE_EQUAL(
      chain.wasm_assert_msg("Invalid chain. See 'wnsmanager' contract for supported chains."),
      chain.createlink("alice"_n, "bitcoin", "alice", sig, em_pub, nonce)
   );

} FC_LOG_AND_RETHROW()

// ——— createlink action tests — nonce freshness ———

BOOST_AUTO_TEST_CASE_TEMPLATE( createlink_stale_nonce, T, sysio_authlink_testers ) try {
   T chain;

   auto em_priv = fc::crypto::private_key::generate<fc::crypto::em::private_key_shim>();
   auto em_pub  = em_priv.get_public_key();

   // Use a nonce from 20 minutes ago — should fail
   uint64_t now_ms = chain.control->head_block_time().time_since_epoch().count() / 1000;
   uint64_t stale_nonce = now_ms - (20 * 60 * 1000);

   auto msg = build_link_message(em_pub, "alice", "ethereum", stale_nonce);
   auto msg_hash = fc::crypto::keccak256::hash(msg);
   auto sig = em_priv.sign_compact(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));

   BOOST_REQUIRE_EQUAL(
      chain.wasm_assert_msg("Invalid nonce: must be within the last 10 minutes"),
      chain.createlink("alice"_n, "ethereum", "alice", sig, em_pub, stale_nonce)
   );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
