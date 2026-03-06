#include <boost/test/unit_test.hpp>

#include <sysio/net_plugin/peer_auth.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::peer_auth;

namespace {

fc::sha256 make_node_id(uint8_t byte) {
   fc::sha256 id;
   memset(id.data(), byte, id.data_size());
   return id;
}

chain_id_type make_chain_id(uint8_t byte) {
   char buf[32];
   memset(buf, byte, sizeof(buf));
   return chain_id_type{buf, sizeof(buf)};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Suite 1: compute_auth_digest — pure function tests
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_SUITE(peer_auth_digest)

BOOST_AUTO_TEST_CASE(deterministic) {
   auto a  = make_node_id(0x01);
   auto b  = make_node_id(0x02);
   auto ch = make_chain_id(0xAA);
   BOOST_CHECK_EQUAL(compute_auth_digest(a, b, ch), compute_auth_digest(a, b, ch));
}

BOOST_AUTO_TEST_CASE(asymmetric) {
   auto a  = make_node_id(0x01);
   auto b  = make_node_id(0x02);
   auto ch = make_chain_id(0xAA);
   // digest(A,B,C) != digest(B,A,C) — anti-replay property
   BOOST_CHECK_NE(compute_auth_digest(a, b, ch), compute_auth_digest(b, a, ch));
}

BOOST_AUTO_TEST_CASE(varies_with_chain_id) {
   auto a   = make_node_id(0x01);
   auto b   = make_node_id(0x02);
   auto ch1 = make_chain_id(0xAA);
   auto ch2 = make_chain_id(0xBB);
   BOOST_CHECK_NE(compute_auth_digest(a, b, ch1), compute_auth_digest(a, b, ch2));
}

BOOST_AUTO_TEST_CASE(varies_with_node_ids) {
   auto a  = make_node_id(0x01);
   auto b  = make_node_id(0x02);
   auto c  = make_node_id(0x03);
   auto ch = make_chain_id(0xAA);
   BOOST_CHECK_NE(compute_auth_digest(a, b, ch), compute_auth_digest(a, c, ch));
   BOOST_CHECK_NE(compute_auth_digest(a, b, ch), compute_auth_digest(c, b, ch));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Suite 2: needs_auth
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_SUITE(peer_auth_needs_auth)

BOOST_AUTO_TEST_CASE(none_false) {
   peer_auth_config cfg;
   cfg.allowed_connections = None;
   BOOST_CHECK(!cfg.needs_auth());
}

BOOST_AUTO_TEST_CASE(any_false) {
   peer_auth_config cfg;
   cfg.allowed_connections = Any;
   BOOST_CHECK(!cfg.needs_auth());
}

BOOST_AUTO_TEST_CASE(producers_true) {
   peer_auth_config cfg;
   cfg.allowed_connections = Producers;
   BOOST_CHECK(cfg.needs_auth());
}

BOOST_AUTO_TEST_CASE(specified_true) {
   peer_auth_config cfg;
   cfg.allowed_connections = Specified;
   BOOST_CHECK(cfg.needs_auth());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Suite 3: is_key_authorized
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_SUITE(peer_auth_key_authorization)

BOOST_AUTO_TEST_CASE(none_rejects_all) {
   peer_auth_config cfg;
   cfg.allowed_connections = None;
   auto key = fc::crypto::private_key::generate().get_public_key();
   BOOST_CHECK(!cfg.is_key_authorized(key));
}

BOOST_AUTO_TEST_CASE(any_accepts_all) {
   peer_auth_config cfg;
   cfg.allowed_connections = Any;
   auto key = fc::crypto::private_key::generate().get_public_key();
   BOOST_CHECK(cfg.is_key_authorized(key));
}

BOOST_AUTO_TEST_CASE(specified_allowed_peer) {
   auto priv = fc::crypto::private_key::generate();
   auto pub = priv.get_public_key();

   peer_auth_config cfg;
   cfg.allowed_connections = Specified;
   cfg.allowed_peers.push_back(pub);

   BOOST_CHECK(cfg.is_key_authorized(pub));

   auto unknown = fc::crypto::private_key::generate().get_public_key();
   BOOST_CHECK(!cfg.is_key_authorized(unknown));
}

BOOST_AUTO_TEST_CASE(specified_private_key) {
   auto priv = fc::crypto::private_key::generate();
   auto pub = priv.get_public_key();

   peer_auth_config cfg;
   cfg.allowed_connections = Specified;
   cfg.private_keys[pub] = priv;

   BOOST_CHECK(cfg.is_key_authorized(pub));

   auto unknown = fc::crypto::private_key::generate().get_public_key();
   BOOST_CHECK(!cfg.is_key_authorized(unknown));
}

BOOST_AUTO_TEST_CASE(producers_callback) {
   auto priv = fc::crypto::private_key::generate();
   auto pub = priv.get_public_key();

   peer_auth_config cfg;
   cfg.allowed_connections = Producers;
   cfg.is_producer_key_func = [&](const public_key_type& k) { return k == pub; };

   BOOST_CHECK(cfg.is_key_authorized(pub));

   auto unknown = fc::crypto::private_key::generate().get_public_key();
   BOOST_CHECK(!cfg.is_key_authorized(unknown));
}

BOOST_AUTO_TEST_CASE(producers_null_callback) {
   peer_auth_config cfg;
   cfg.allowed_connections = Producers;
   // No callback set, no keys configured
   auto key = fc::crypto::private_key::generate().get_public_key();
   BOOST_CHECK(!cfg.is_key_authorized(key));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Suite 4: sign / verify round-trip
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_SUITE(peer_auth_sign_verify)

BOOST_AUTO_TEST_CASE(sign_recover_roundtrip) {
   auto priv = fc::crypto::private_key::generate();
   auto pub  = priv.get_public_key();

   peer_auth_config cfg;
   cfg.private_keys[pub] = priv;

   auto digest = fc::sha256::hash("test_digest");
   auto sig = cfg.sign_compact(pub, digest);
   BOOST_CHECK(sig != signature_type());

   auto recovered = fc::crypto::public_key::recover(sig, digest);
   BOOST_CHECK_EQUAL(recovered, pub);
}

BOOST_AUTO_TEST_CASE(wrong_digest_no_match) {
   auto priv = fc::crypto::private_key::generate();
   auto pub  = priv.get_public_key();

   peer_auth_config cfg;
   cfg.private_keys[pub] = priv;

   auto digest1 = fc::sha256::hash("digest_one");
   auto digest2 = fc::sha256::hash("digest_two");
   auto sig = cfg.sign_compact(pub, digest1);

   auto recovered = fc::crypto::public_key::recover(sig, digest2);
   BOOST_CHECK_NE(recovered, pub);
}

BOOST_AUTO_TEST_CASE(bidirectional_auth) {
   // Simulate a full A <-> B authentication exchange
   auto priv_a = fc::crypto::private_key::generate();
   auto pub_a  = priv_a.get_public_key();
   auto priv_b = fc::crypto::private_key::generate();
   auto pub_b  = priv_b.get_public_key();

   auto node_a = make_node_id(0x0A);
   auto node_b = make_node_id(0x0B);
   auto chain  = make_chain_id(0xCC);

   // A signs: digest(remote=B, my=A, chain) — what A sends to B
   auto digest_a_sends = compute_auth_digest(node_b, node_a, chain);
   auto sig_a = priv_a.sign(digest_a_sends);

   // B verifies A's auth: digest(my=B, remote=A, chain) — note B flips the perspective
   // But wait: B recomputes as digest(remote_for_B=A's_node, my_for_B=B's_node)
   // Actually the protocol: when receiving, verifier computes digest(my_node_id, conn_node_id, chain)
   // So B computes: digest(B_node, A_node, chain)
   // But A signed: digest(B_node, A_node, chain) — same thing!
   auto digest_b_verifies = compute_auth_digest(node_b, node_a, chain);
   auto recovered_a = fc::crypto::public_key::recover(sig_a, digest_b_verifies);
   BOOST_CHECK_EQUAL(recovered_a, pub_a);

   // B signs: digest(remote=A, my=B, chain) — what B sends to A
   auto digest_b_sends = compute_auth_digest(node_a, node_b, chain);
   auto sig_b = priv_b.sign(digest_b_sends);

   // A verifies B's auth
   auto digest_a_verifies = compute_auth_digest(node_a, node_b, chain);
   auto recovered_b = fc::crypto::public_key::recover(sig_b, digest_a_verifies);
   BOOST_CHECK_EQUAL(recovered_b, pub_b);

   // Cross-replay: A's signature should NOT verify under B's digest
   auto cross_recovered = fc::crypto::public_key::recover(sig_a, digest_b_sends);
   BOOST_CHECK_NE(cross_recovered, pub_a);
}

BOOST_AUTO_TEST_CASE(get_auth_key_empty) {
   peer_auth_config cfg;
   BOOST_CHECK(cfg.get_authentication_key() == public_key_type());
}

BOOST_AUTO_TEST_CASE(sign_compact_known) {
   auto priv = fc::crypto::private_key::generate();
   auto pub  = priv.get_public_key();

   peer_auth_config cfg;
   cfg.private_keys[pub] = priv;

   auto digest = fc::sha256::hash("known_key_test");
   auto sig = cfg.sign_compact(pub, digest);
   BOOST_CHECK(sig != signature_type());
}

BOOST_AUTO_TEST_CASE(sign_compact_unknown) {
   peer_auth_config cfg;
   auto unknown_key = fc::crypto::private_key::generate().get_public_key();
   auto digest = fc::sha256::hash("unknown_key_test");
   auto sig = cfg.sign_compact(unknown_key, digest);
   BOOST_CHECK(sig == signature_type());
}

BOOST_AUTO_TEST_SUITE_END()
