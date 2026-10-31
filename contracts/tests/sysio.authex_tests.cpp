#include <random>

#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/resource_limits.hpp>
#include "contracts.hpp"

#include <fc/variant_object.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/io/raw.hpp>
#include <sysio/opp/opp.hpp>     // FC_REFLECT_ENUM for sysio::opp::types::ChainKind
#include "contract_test_support.hpp"
#include <magic_enum/magic_enum.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace fc::crypto;
using namespace std;

using mvo = fc::mutable_variant_object;
using ChainKind = sysio::opp::types::ChainKind;

using sysio_system::test_support::build_link_message;
using sysio_system::test_support::sign_createlink;
namespace secp256k1 = sysio_system::test_support::secp256k1;

namespace {

/// Raw ed25519 public-key bytes.
using ed_key_bytes = fc::crypto::ed::public_key_shim::data_type;

/// Bytes in a canonical EVM address.
constexpr size_t evm_address_size = 20;

/// What the host reports when an ed25519 signature does not verify against its embedded key.
constexpr std::string_view ed_verification_failure = "ED25519 signature verification failed during recovery";

secp256k1::coordinate zero_coordinate() { return {}; }

fc::em::public_key_data em_key_bytes(const fc::crypto::public_key& key) {
   return key.get<fc::em::public_key_shim>().serialize();
}

secp256k1::coordinate x_coordinate(const fc::em::public_key_data& key) {
   secp256k1::coordinate x;
   std::copy(key.begin() + 1, key.end(), x.begin());
   return x;
}

/// Wrap raw EM bytes in the host key type without validating them, as a malformed attestation would.
fc::crypto::public_key em_public_key(const fc::em::public_key_data& key) {
   return fc::crypto::public_key{fc::crypto::public_key::storage_type{fc::em::public_key_shim{key}}};
}

std::vector<char> evm_address_of(const fc::crypto::public_key& key) {
   const auto address = fc::crypto::ethereum::address_to_bytes(key);
   return {address.begin(), address.end()};
}

/// Solana's `Pubkey::default()`.
ed_key_bytes all_zero_ed_key() { return {}; }

/// The ed25519 identity point (y = 1), the order-1 member of the small-order set.
ed_key_bytes identity_ed_key() {
   ed_key_bytes key{};
   key[0] = 1;
   return key;
}

fc::crypto::public_key ed_public_key(const ed_key_bytes& key) {
   return fc::crypto::public_key{fc::crypto::public_key::storage_type{fc::crypto::ed::public_key_shim{key}}};
}

/// The ed25519 base point B, compressed (y = 4/5).
ed_key_bytes base_point() {
   ed_key_bytes b;
   b.fill(0x66);
   b[0] = 0x58;
   return b;
}

/// A signature that satisfies the cofactorless equation [S]B = R + [k]A for the identity key A and
/// every challenge k: R = B and S = 1, both canonical and R of full order. Only a verifier that
/// refuses small-order public keys rejects it, which is what libsodium's verify does. fc embeds the
/// claimed key ahead of R || S.
fc::crypto::signature identity_key_forgery(const ed_key_bytes& key) {
   fc::crypto::ed::signature_shim::data_type blob{};
   std::copy(key.begin(), key.end(), blob.begin());
   const auto r = base_point();
   std::copy(r.begin(), r.end(), blob.begin() + key.size());
   blob[key.size() + r.size()] = 1;   // S = 1, little-endian
   return fc::crypto::signature{fc::crypto::signature::storage_type{fc::crypto::ed::signature_shim{blob}}};
}

// ChainKind's action ABI is int32; fc's reflected-enum packer uses int64, so these pack it by value.
// Raw packing also carries keys the host's string renderer refuses.

std::vector<char> pack_createlink(ChainKind chain_kind, name account, const fc::crypto::signature& sig,
                                  const fc::crypto::public_key& pub_key, uint64_t nonce) {
   return fc::raw::pack(std::make_tuple(magic_enum::enum_integer(chain_kind), account, sig, pub_key, nonce));
}

std::vector<char> pack_recordlink(name account, ChainKind chain_kind, const fc::crypto::public_key& pub_key,
                                  const std::vector<char>& native_address) {
   return fc::raw::pack(std::make_tuple(account, magic_enum::enum_integer(chain_kind), pub_key, native_address));
}

} // anonymous namespace

// ——— Tester class ———
class sysio_authex_tester : public tester {
public:
   static constexpr auto AUTHEX = "sysio.authex"_n;
   static constexpr auto DCLAIM = "sysio.dclaim"_n;
   static constexpr auto MSGCH = "sysio.msgch"_n;

   sysio_authex_tester() {
      produce_blocks( 2 );

      create_accounts( { MSGCH, "alice"_n, "bob"_n, "carol"_n, } );
      produce_blocks( 2 );

      set_code( AUTHEX, contracts::authex_wasm() );
      set_abi( AUTHEX, contracts::authex_abi().data() );
      set_privileged( AUTHEX );

      produce_blocks();

      const auto* accnt = control->find_account_metadata( AUTHEX );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(abi, fixture_abi_yield_function());
   }

   void deploy_dclaim() {
      create_accounts( { DCLAIM } );
      set_code( DCLAIM, contracts::dclaim_wasm() );
      set_abi( DCLAIM, contracts::dclaim_abi().data() );
      set_privileged( DCLAIM );
      produce_blocks();
      dclaim_abi_ser.set_abi(load_abi(DCLAIM),
         fixture_abi_yield_function());
   }

   void set_dclaim_privileged(bool is_privileged) {
      base_tester::push_action(config::system_account_name, "setpriv"_n,
         config::system_account_name,
         mvo()("account", DCLAIM)("is_priv", is_privileged ? 1 : 0));
      produce_blocks();
   }

   abi_def load_abi(name account) {
      const auto* metadata = control->find_account_metadata(account);
      BOOST_REQUIRE(metadata != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(metadata->abi, abi), true);
      return abi;
   }

   action_result push_action( const account_name& signer, const action_name& name, const variant_object& data ) {
      string action_type_name = abi_ser.get_action_type(name);

      return push_raw_action(signer, name, abi_ser.variant_to_binary(
         action_type_name, data,
         fixture_abi_yield_function()
      ));
   }

   /// Submit wire bytes directly, including malformed keys the host's string renderer rejects.
   action_result push_raw_action(const account_name& signer, const action_name& name,
                                 const std::vector<char>& data) {
      action act;
      act.account = AUTHEX;
      act.name = name;
      act.data = data;
      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   action_result createlink(
      const account_name& signer,
      ChainKind chain_kind,
      const std::string& account,
      const fc::crypto::signature& sig,
      const fc::crypto::public_key& pub_key,
      uint64_t nonce
   ) {
      return push_action( signer, "createlink"_n, mvo()
         ("chain_kind", chain_kind)
         ("account",    account)
         ("sig",        sig)
         ("pub_key",    pub_key)
         ("nonce",      nonce)
      );
   }

   action_result recordlink(const account_name& signer, const std::string& account,
                            const fc::crypto::public_key& pub_key,
                            const std::vector<char>& native_address,
                            ChainKind chain_kind = ChainKind::CHAIN_KIND_EVM) {
      return push_action(signer, "recordlink"_n, mvo()
         ("account", account)
         ("chain_kind", chain_kind)
         ("pub_key", pub_key)
         ("native_address", native_address));
   }

   /// Import a pre-launch WIRE credit before testing authenticated link settlement.
   action_result import_credit(const std::vector<char>& native_address, uint64_t amount,
                               ChainKind chain_kind = ChainKind::CHAIN_KIND_EVM) {
      return sysio_system::test_support::push_contract_action_and_produce_block(
         *this, DCLAIM, dclaim_abi_ser, DCLAIM, "importseed"_n, mvo()
            ("chain", chain_kind)("credits", fc::variants{
               mvo()("native_address", native_address)("wire_atomic", static_cast<int64_t>(amount))}));
   }

   fc::variant get_dclaim_row(name table, const char* type, uint64_t id) {
      auto data = get_row_by_id(DCLAIM, DCLAIM, table, id);
      return data.empty() ? fc::variant()
         : dclaim_abi_ser.binary_to_variant(
              type, data, fixture_abi_yield_function());
   }

   fc::variant get_link(uint64_t id) {
      auto data = get_row_by_id(AUTHEX, AUTHEX, "links"_n, id);
      return data.empty() ? fc::variant()
         : abi_ser.binary_to_variant(
              "links_s", data, fixture_abi_yield_function());
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

   em_link_data make_eth_link(const std::string& account, uint64_t nonce) {
      auto priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
      auto pub  = priv.get_public_key();
      auto msg  = build_link_message(pub, account, ChainKind::CHAIN_KIND_EVM, nonce);

      // keccak(msg) → 32 bytes, same as what the contract computes
      auto msg_hash = fc::crypto::keccak256::hash(msg);

      // sign(sha256(keccak_bytes)) — sign_sha256 wraps with EIP-191 internally,
      // and assert_recover_key's recover also wraps with EIP-191, so they match.
      // k1_recover_uncompressed also applies EIP-191 for EM signatures.
      auto sig = priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));
      return { priv, pub, sig, nonce };
   }

   abi_serializer abi_ser;
   abi_serializer dclaim_abi_ser;
};


BOOST_AUTO_TEST_SUITE(sysio_authex_tests)

// ——— createlink: auth failures ———

BOOST_FIXTURE_TEST_CASE( createlink_requires_account_auth, sysio_authex_tester ) try {
   auto link = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL(
      error("missing authority of alice"),
      createlink("bob"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce)
   );
} FC_LOG_AND_RETHROW()

// ——— createlink: invalid chain ———

BOOST_FIXTURE_TEST_CASE( createlink_invalid_chain, sysio_authex_tester ) try {
   auto priv = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   auto pub  = priv.get_public_key();
   uint64_t nonce = now_ms();
   auto msg = build_link_message(pub, "alice", ChainKind::CHAIN_KIND_WIRE, nonce);
   auto msg_hash = fc::crypto::keccak256::hash(msg);
   auto sig = priv.sign(fc::sha256(reinterpret_cast<const char*>(msg_hash.data()), 32));

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Invalid chain_kind. Supported: CHAIN_KIND_EVM(2), CHAIN_KIND_SVM(3)."),
      createlink("alice"_n, ChainKind::CHAIN_KIND_WIRE, "alice", sig, pub, nonce)
   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_rejects_mismatched_chain_and_key, sysio_authex_tester ) try {
   auto link = make_eth_link("alice", now_ms());
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("chain_kind and pub_key must pair as EVM/EM or SVM/ED"),
      createlink("alice"_n, ChainKind::CHAIN_KIND_SVM, "alice", link.sig, link.pub, link.nonce)
   );
   BOOST_REQUIRE(get_link(0).is_null());
} FC_LOG_AND_RETHROW()

/// A key that cannot sign never passes createlink: proof of possession is the guard. The identity
/// key is offered the signature a cofactorless verifier accepts for it (R = B, S = 1), so the only
/// thing that can refuse it is the small-order check on the key itself. Solana's all-zero
/// `Pubkey::default()` (y = 0, a point of order 4) is refused as well.
BOOST_FIXTURE_TEST_CASE( createlink_rejects_svm_keys_that_cannot_sign, sysio_authex_tester ) try {
   deploy_dclaim();
   for (const auto& key_bytes : {identity_ed_key(), all_zero_ed_key()}) {
      const auto result = push_raw_action("alice"_n, "createlink"_n,
         pack_createlink(ChainKind::CHAIN_KIND_SVM, "alice"_n, identity_key_forgery(key_bytes),
                         ed_public_key(key_bytes), now_ms()));
      BOOST_REQUIRE_MESSAGE(result.find(ed_verification_failure) != std::string::npos, result);
      BOOST_REQUIRE(get_link(0).is_null());
   }
} FC_LOG_AND_RETHROW()

/// EM bytes off the curve have no private key, so no signature recovers to their x-coordinate.
BOOST_FIXTURE_TEST_CASE( createlink_rejects_evm_keys_that_cannot_sign, sysio_authex_tester ) try {
   deploy_dclaim();
   const auto signature = make_eth_link("alice", now_ms()).sig;
   for (const auto& key_bytes :
        {secp256k1::compressed_key(0x00, zero_coordinate()), secp256k1::compressed_key(0x02, zero_coordinate())}) {
      BOOST_REQUIRE(!secp256k1::is_point(key_bytes));
      const auto result = push_raw_action("alice"_n, "createlink"_n, pack_createlink(
         ChainKind::CHAIN_KIND_EVM, "alice"_n, signature, em_public_key(key_bytes), now_ms()));
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("EM key recovery failed: x-coordinate mismatch"), result);
      BOOST_REQUIRE(get_link(0).is_null());
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_rejects_k1_signature_for_em_key, sysio_authex_tester ) try {
   auto link = make_eth_link("alice", now_ms());
   // Use the same secp256k1 secret to produce an otherwise-valid raw-digest K1 signature.
   // Without the type gate, recovery would accept this signature without EIP-191 wrapping.
   auto k1_priv = fc::crypto::private_key::regenerate<fc::ecc::private_key_shim>(
      link.priv.get<fc::em::private_key_shim>().serialize());
   const auto msg = build_link_message(link.pub, "alice", ChainKind::CHAIN_KIND_EVM, link.nonce);
   const auto hash = fc::crypto::keccak256::hash(msg);
   const auto k1_sig = k1_priv.sign(fc::sha256(reinterpret_cast<const char*>(hash.data()), 32));
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("EM link requires an EM signature"),
      createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", k1_sig, link.pub, link.nonce)
   );
   BOOST_REQUIRE(get_link(0).is_null());
} FC_LOG_AND_RETHROW()

// ——— createlink: stale nonce ———

BOOST_FIXTURE_TEST_CASE( createlink_stale_nonce, sysio_authex_tester ) try {
   uint64_t stale_nonce = now_ms() - (20 * 60 * 1000);
   auto link = make_eth_link("alice", stale_nonce);

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Invalid nonce: must be within the last 10 minutes"),
      createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce)
   );
} FC_LOG_AND_RETHROW()

// ——— createlink: successful ethereum link ———

BOOST_FIXTURE_TEST_CASE( createlink_eth_success, sysio_authex_tester ) try {
   deploy_dclaim();
   auto link = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce) );
   produce_blocks();

   // The verified EM key is recorded in the links table only -- it must NOT be added
   // to `active` (or any) permission, and no `ex.*` permission is created.
   auto& auth_mgr = control->get_authorization_manager();
   const auto* active = auth_mgr.find_permission({"alice"_n, "active"_n});
   BOOST_REQUIRE( active != nullptr );
   const auto auth = active->auth.to_authority();
   for (const auto& kw : auth.keys)
      BOOST_CHECK_MESSAGE( kw.key != link.pub, "EM key must not be added to active" );
   BOOST_REQUIRE( auth_mgr.find_permission({"alice"_n, "ex.eth"_n}) == nullptr );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_rolls_back_until_dclaim_is_ready,
                         sysio_authex_tester ) try {
   auto link = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("sysio.dclaim must be deployed and privileged before creating a link"),
      createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   BOOST_REQUIRE(get_link(0).is_null());

   deploy_dclaim();
   BOOST_REQUIRE_EQUAL(success(), createlink(
      "alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   BOOST_REQUIRE(!get_link(0).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_rolls_back_until_dclaim_is_privileged,
                         sysio_authex_tester ) try {
   deploy_dclaim();
   set_dclaim_privileged(false);
   auto link = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("sysio.dclaim must be deployed and privileged before creating a link"),
      createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   BOOST_REQUIRE(get_link(0).is_null());

   set_dclaim_privileged(true);
   BOOST_REQUIRE_EQUAL(success(), createlink(
      "alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   BOOST_REQUIRE(!get_link(0).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_eth_sweeps_prelink_dclaim_rewards, sysio_authex_tester ) try {
   deploy_dclaim();
   auto link = make_eth_link("alice", now_ms());
   const auto address_bytes = fc::crypto::ethereum::address_to_bytes(link.pub);
   const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());

   BOOST_REQUIRE_EQUAL(success(), import_credit(native_address, 5000));
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());

   BOOST_REQUIRE_EQUAL(success(), createlink(
      "alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   produce_blocks();

   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   auto pending = get_dclaim_row("pclaims"_n, "pending_claim", "alice"_n.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 5000);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_preserves_old_prelink_dclaim_rewards,
                         sysio_authex_tester ) try {
   deploy_dclaim();
   auto link = make_eth_link("alice", now_ms());
   const auto address_bytes = fc::crypto::ethereum::address_to_bytes(link.pub);
   const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());
   BOOST_REQUIRE_EQUAL(success(), import_credit(native_address, 5000));
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   constexpr uint32_t years_without_link_sec = 3u * 365u * 24u * 60u * 60u;
   produce_block(fc::seconds(years_without_link_sec));
   // Refresh the link proof's nonce after the wait; the reward itself remains old.
   link.nonce = now_ms();
   const auto message = build_link_message(link.pub, "alice", ChainKind::CHAIN_KIND_EVM, link.nonce);
   const auto message_hash = fc::crypto::keccak256::hash(message);
   link.sig = link.priv.sign(fc::sha256(reinterpret_cast<const char*>(message_hash.data()), 32));

   BOOST_REQUIRE_EQUAL(success(), createlink(
      "alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   produce_blocks();

   BOOST_REQUIRE(!get_link(0).is_null());
   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   const auto pending = get_dclaim_row("pclaims"_n, "pending_claim", "alice"_n.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 5000);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_svm_sweeps_prelink_dclaim_rewards, sysio_authex_tester ) try {
   deploy_dclaim();
   const auto private_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed);
   const auto public_key = private_key.get_public_key();
   const auto raw_key = public_key.get<fc::crypto::ed::public_key_shim>().serialize();
   const std::vector<char> native_address(raw_key.begin(), raw_key.end());
   // Same first 8 bytes, hence the same `bychainad` key: the sweep must match the full address.
   std::vector<char> same_prefix_address = native_address;
   same_prefix_address.back() ^= char{0x01};

   BOOST_REQUIRE_EQUAL(success(), import_credit(
      same_prefix_address, 3000, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE_EQUAL(success(), import_credit(
      native_address, 8000, ChainKind::CHAIN_KIND_SVM));

   const auto nonce = now_ms();
   const auto sig = sign_createlink(private_key, "carol", ChainKind::CHAIN_KIND_SVM, nonce);
   BOOST_REQUIRE_EQUAL(success(), createlink(
      "carol"_n, ChainKind::CHAIN_KIND_SVM, "carol", sig, public_key, nonce));
   produce_blocks();

   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 2).is_null());
   const auto same_prefix = get_dclaim_row("unmapped"_n, "unmapped_token", 1);
   BOOST_REQUIRE(!same_prefix.is_null());
   BOOST_REQUIRE_EQUAL(same_prefix["balance"].as<asset>().get_amount(), 3000);
   const auto pending = get_dclaim_row("pclaims"_n, "pending_claim", "carol"_n.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 8000);
} FC_LOG_AND_RETHROW()

/// recordlink stores a link without a signature by its key, so every EM encoding that is not a
/// secp256k1 point is dropped before insertion and before any DClaim sweep: no prefix, the
/// uncompressed prefix, x = 0 (7 is not a square mod p), x = p, a valid point's x plus p (a
/// non-canonical encoding of a real point), and an x no y completes.
BOOST_FIXTURE_TEST_CASE( recordlink_drops_evm_keys_that_are_not_curve_points, sysio_authex_tester ) try {
   deploy_dclaim();
   const auto valid = em_key_bytes(fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key());
   const auto address = evm_address_of(em_public_key(valid));
   BOOST_REQUIRE_EQUAL(success(), import_credit(address, 7000));

   const std::vector<fc::em::public_key_data> rejected = {
      secp256k1::compressed_key(0x00, zero_coordinate()),
      secp256k1::compressed_key(0x00, x_coordinate(valid)),
      secp256k1::compressed_key(0x04, x_coordinate(valid)),
      secp256k1::compressed_key(0x02, zero_coordinate()),
      secp256k1::compressed_key(0x02, secp256k1::field_prime()),
      secp256k1::compressed_key(0x02, secp256k1::plus_field_prime(secp256k1::smallest_x(true))),
      secp256k1::compressed_key(0x03, secp256k1::smallest_x(false)),
   };
   for (const auto& key_bytes : rejected) {
      BOOST_REQUIRE(!secp256k1::is_point(key_bytes));
      BOOST_REQUIRE_EQUAL(error("missing authority of sysio.authex"), push_raw_action("carol"_n, "recordlink"_n,
         pack_recordlink("carol"_n, ChainKind::CHAIN_KIND_EVM, em_public_key(key_bytes), address)));
      BOOST_REQUIRE_EQUAL(success(), push_raw_action(AUTHEX, "recordlink"_n,
         pack_recordlink("carol"_n, ChainKind::CHAIN_KIND_EVM, em_public_key(key_bytes), address)));
      BOOST_REQUIRE(get_link(0).is_null());
   }
   const auto parked = get_dclaim_row("unmapped"_n, "unmapped_token", 1);
   BOOST_REQUIRE(!parked.is_null());
   BOOST_REQUIRE_EQUAL(parked["balance"].as<asset>().get_amount(), 7000);
   BOOST_REQUIRE(get_dclaim_row("pclaims"_n, "pending_claim", "carol"_n.to_uint64_t()).is_null());

   // A dropped key leaves the account/chain slot free: the real key then links and sweeps.
   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "carol", em_public_key(valid), address));
   BOOST_REQUIRE_EQUAL(get_link(0)["pub_key"].as<fc::crypto::public_key>(), em_public_key(valid));
   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   const auto pending = get_dclaim_row("pclaims"_n, "pending_claim", "carol"_n.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 7000);
} FC_LOG_AND_RETHROW()

/// Both parities of one x-coordinate are curve points (P and -P), so both prefixes are recorded.
BOOST_FIXTURE_TEST_CASE( recordlink_accepts_both_parities_of_a_point, sysio_authex_tester ) try {
   const auto x = x_coordinate(em_key_bytes(fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key()));
   const auto even = secp256k1::compressed_key(0x02, x);
   const auto odd = secp256k1::compressed_key(0x03, x);
   BOOST_REQUIRE(secp256k1::is_point(even));
   BOOST_REQUIRE(secp256k1::is_point(odd));
   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", em_public_key(even), evm_address_of(em_public_key(even))));
   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "carol", em_public_key(odd), evm_address_of(em_public_key(odd))));
   BOOST_REQUIRE_EQUAL(get_link(0)["pub_key"].as<fc::crypto::public_key>(), em_public_key(even));
   BOOST_REQUIRE_EQUAL(get_link(1)["pub_key"].as<fc::crypto::public_key>(), em_public_key(odd));
} FC_LOG_AND_RETHROW()

/// The contract's curve check agrees with libsecp256k1 on arbitrary x-coordinates. About half of all
/// x values have a point, so a fixed-seed sample exercises both verdicts many times over.
BOOST_FIXTURE_TEST_CASE( recordlink_curve_check_matches_libsecp256k1, sysio_authex_tester ) try {
   constexpr uint32_t sample_seed = 722;
   constexpr size_t sample_count = 32;
   std::mt19937 random(sample_seed);
   size_t accepted = 0;
   for (size_t i = 0; i < sample_count; ++i) {
      fc::em::public_key_data key_bytes{};
      key_bytes[0] = static_cast<char>(i % 2 == 0 ? 0x02 : 0x03);
      for (size_t b = 1; b < key_bytes.size(); ++b) key_bytes[b] = static_cast<char>(random());
      const auto account = name("acct" + std::string(1, static_cast<char>('a' + i / 5)) +
                                std::string(1, static_cast<char>('a' + i % 5)));
      BOOST_REQUIRE_EQUAL(success(), push_raw_action(AUTHEX, "recordlink"_n, pack_recordlink(
         account, ChainKind::CHAIN_KIND_EVM, em_public_key(key_bytes), std::vector<char>(evm_address_size, char{0}))));
      const bool linked = !get_link(accepted).is_null();
      BOOST_REQUIRE_EQUAL(linked, secp256k1::is_point(key_bytes));
      if (linked) {
         BOOST_REQUIRE_EQUAL(get_link(accepted)["username"].as<name>(), account);
         ++accepted;
      }
   }
   BOOST_REQUIRE_GT(accepted, 0u);
   BOOST_REQUIRE_LT(accepted, sample_count);
} FC_LOG_AND_RETHROW()

/// recordlink never records an SVM link: every SVM key reaches the OPERATORS roster through
/// createlink's ed25519 verification, so a trusted caller cannot seat a key that cannot sign.
BOOST_FIXTURE_TEST_CASE( recordlink_refuses_svm_links, sysio_authex_tester ) try {
   deploy_dclaim();
   const auto public_key = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::ed).get_public_key();
   const auto raw_key = public_key.get<fc::crypto::ed::public_key_shim>().serialize();
   const std::vector<char> native_address(raw_key.begin(), raw_key.end());
   BOOST_REQUIRE_EQUAL(success(), import_credit(native_address, 8000, ChainKind::CHAIN_KIND_SVM));

   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "carol", public_key, native_address, ChainKind::CHAIN_KIND_SVM));
   const auto zero = all_zero_ed_key();
   BOOST_REQUIRE_EQUAL(success(), push_raw_action(AUTHEX, "recordlink"_n, pack_recordlink(
      "carol"_n, ChainKind::CHAIN_KIND_SVM, ed_public_key(zero), std::vector<char>(zero.begin(), zero.end()))));
   BOOST_REQUIRE(get_link(0).is_null());
   const auto parked = get_dclaim_row("unmapped"_n, "unmapped_token", 1);
   BOOST_REQUIRE(!parked.is_null());
   BOOST_REQUIRE_EQUAL(parked["balance"].as<asset>().get_amount(), 8000);
   BOOST_REQUIRE(get_dclaim_row("pclaims"_n, "pending_claim", "carol"_n.to_uint64_t()).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( recordlink_records_link_when_dclaim_is_missing, sysio_authex_tester ) try {
   const auto public_key = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key();
   const auto address_bytes = fc::crypto::ethereum::address_to_bytes(public_key);
   const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());

   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", public_key, native_address));
   produce_blocks();

   auto link = get_link(0);
   BOOST_REQUIRE(!link.is_null());
   BOOST_REQUIRE_EQUAL(link["username"].as<name>(), "bob"_n);
   BOOST_REQUIRE_EQUAL(link["pub_key"].as<fc::crypto::public_key>(), public_key);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( recordlink_records_link_when_native_address_is_malformed,
                         sysio_authex_tester ) try {
   const auto public_key = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key();
   const std::vector<char> malformed_address(19, char{0x01});

   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", public_key, malformed_address));
   produce_blocks();

   auto link = get_link(0);
   BOOST_REQUIRE(!link.is_null());
   BOOST_REQUIRE_EQUAL(link["username"].as<name>(), "bob"_n);
   BOOST_REQUIRE_EQUAL(link["pub_key"].as<fc::crypto::public_key>(), public_key);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( recordlink_records_link_when_dclaim_is_not_privileged,
                         sysio_authex_tester ) try {
   deploy_dclaim();
   const auto public_key = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key();
   const auto address_bytes = fc::crypto::ethereum::address_to_bytes(public_key);
   const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());

   BOOST_REQUIRE_EQUAL(success(), import_credit(native_address, 6500));
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   set_dclaim_privileged(false);

   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", public_key, native_address));
   produce_blocks();

   auto link = get_link(0);
   BOOST_REQUIRE(!link.is_null());
   BOOST_REQUIRE_EQUAL(link["username"].as<name>(), "bob"_n);
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   BOOST_REQUIRE(get_dclaim_row("pclaims"_n, "pending_claim", "bob"_n.to_uint64_t()).is_null());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( recordlink_eth_sweeps_prelink_dclaim_rewards, sysio_authex_tester ) try {
   deploy_dclaim();
   const auto private_key = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em);
   const auto public_key = private_key.get_public_key();
   const auto address_bytes = fc::crypto::ethereum::address_to_bytes(public_key);
   const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());

   BOOST_REQUIRE_EQUAL(success(), import_credit(native_address, 7000));
   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", public_key, native_address));
   produce_blocks();

   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   auto pending = get_dclaim_row("pclaims"_n, "pending_claim", "bob"_n.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 7000);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( recordlink_identical_retry_resweeps_dclaim_rewards, sysio_authex_tester ) try {
   deploy_dclaim();
   const auto public_key = fc::crypto::private_key::generate(
      fc::crypto::private_key::key_type::em).get_public_key();
   const auto address_bytes = fc::crypto::ethereum::address_to_bytes(public_key);
   const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());

   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", public_key, native_address));
   BOOST_REQUIRE_EQUAL(success(), import_credit(native_address, 9000));
   BOOST_REQUIRE(!get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());

   BOOST_REQUIRE_EQUAL(success(), recordlink(AUTHEX, "bob", public_key, native_address));
   produce_blocks();

   BOOST_REQUIRE(get_dclaim_row("unmapped"_n, "unmapped_token", 1).is_null());
   auto pending = get_dclaim_row("pclaims"_n, "pending_claim", "bob"_n.to_uint64_t());
   BOOST_REQUIRE(!pending.is_null());
   BOOST_REQUIRE_EQUAL(pending["balance"].as<asset>().get_amount(), 9000);
} FC_LOG_AND_RETHROW()

// --- createlink: account bears no RAM cost (link row is sysio-paid) ---

BOOST_FIXTURE_TEST_CASE( createlink_no_account_ram_cost, sysio_authex_tester ) try {
   deploy_dclaim();
   auto& rlm = control->get_resource_limits_manager();
   int64_t q0, net, cpu;
   rlm.get_account_limits("alice"_n, q0, net, cpu);
   int64_t u0 = rlm.get_account_ram_usage("alice"_n);
   auto link = make_eth_link("alice", now_ms());
   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce) );
   produce_blocks();

   int64_t q1;
   rlm.get_account_limits("alice"_n, q1, net, cpu);
   int64_t u1 = rlm.get_account_ram_usage("alice"_n);
   // The link row is billed to sysio and no permission is changed, so the account's
   // own RAM usage and quota are untouched -- linking is free to the account.
   BOOST_CHECK_EQUAL( u1, u0 );
   BOOST_CHECK_EQUAL( q1, q0 );
} FC_LOG_AND_RETHROW()

// ——— createlink: duplicate pubkey ———

BOOST_FIXTURE_TEST_CASE( createlink_duplicate_pubkey, sysio_authex_tester ) try {
   deploy_dclaim();
   auto link1 = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link1.sig, link1.pub, link1.nonce) );
   produce_blocks();

   // Bob tries to link the same pubkey
   uint64_t nonce2 = now_ms();
   auto msg2 = build_link_message(link1.pub, "bob", ChainKind::CHAIN_KIND_EVM, nonce2);
   auto hash2 = fc::crypto::keccak256::hash(msg2);
   auto sig2 = link1.priv.sign(fc::sha256(reinterpret_cast<const char*>(hash2.data()), 32));

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Public key already linked to a different account."),
      createlink("bob"_n, ChainKind::CHAIN_KIND_EVM, "bob", sig2, link1.pub, nonce2)
   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( createlink_opposite_parity_cannot_duplicate_verified_key,
                         sysio_authex_tester ) try {
   deploy_dclaim();
   auto link = make_eth_link("alice", now_ms());
   BOOST_REQUIRE_EQUAL(success(), createlink(
      "alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce));
   produce_blocks();

   auto opposite_bytes = link.pub.get<fc::em::public_key_shim>().serialize();
   opposite_bytes[0] = opposite_bytes[0] == char{0x02} ? char{0x03} : char{0x02};
   const auto opposite_pub = fc::crypto::public_key{
      fc::crypto::public_key::storage_type{fc::em::public_key_shim{opposite_bytes}}};
   const uint64_t nonce = now_ms();
   const auto msg = build_link_message(opposite_pub, "bob", ChainKind::CHAIN_KIND_EVM, nonce);
   const auto hash = fc::crypto::keccak256::hash(msg);
   const auto sig = link.priv.sign(fc::sha256(reinterpret_cast<const char*>(hash.data()), 32));

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Public key already linked to a different account."),
      createlink("bob"_n, ChainKind::CHAIN_KIND_EVM, "bob", sig, opposite_pub, nonce)
   );
   BOOST_REQUIRE(get_link(1).is_null());
} FC_LOG_AND_RETHROW()

// ——— createlink: duplicate chain for same user ———

BOOST_FIXTURE_TEST_CASE( createlink_duplicate_chain_for_user, sysio_authex_tester ) try {
   deploy_dclaim();
   auto link1 = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link1.sig, link1.pub, link1.nonce) );
   produce_blocks();

   auto link2 = make_eth_link("alice", now_ms());

   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("Account already has a link for this chain."),
      createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link2.sig, link2.pub, link2.nonce)
   );
} FC_LOG_AND_RETHROW()

/// Linking records an association only: the linked EM key never lands in the account's permissions.
BOOST_FIXTURE_TEST_CASE( createlink_leaves_account_permissions_untouched, sysio_authex_tester ) try {
   deploy_dclaim();
   auto link = make_eth_link("alice", now_ms());
   BOOST_REQUIRE_EQUAL( success(), createlink("alice"_n, ChainKind::CHAIN_KIND_EVM, "alice", link.sig, link.pub, link.nonce) );
   produce_blocks();

   auto& auth_mgr = control->get_authorization_manager();
   const auto* active = auth_mgr.find_permission({"alice"_n, "active"_n});
   BOOST_REQUIRE( active != nullptr );
   const auto auth = active->auth.to_authority();
   for (const auto& kw : auth.keys)
      BOOST_CHECK_MESSAGE( fc::variant(kw.key).as_string().rfind("PUB_EM_", 0) != 0,
                           "no EM key should be in active" );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
