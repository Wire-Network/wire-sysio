#include <sysio/net_plugin/net_utils.hpp>

#include <boost/test/unit_test.hpp>

namespace {

/// Normal listener from the regression configuration whose port sorts after the transaction listener.
const std::string normal_listen_endpoint = "localhost:18115";
/// Transaction-only listener from the regression configuration whose port sorts before the normal listener.
const std::string trx_listen_endpoint = "localhost:17891:trx";
/// Plain peer address used to verify that handshake data does not widen a configured transaction listener.
const std::string plain_peer_endpoint = "localhost:18114";
/// Conflicting block-only peer address used to verify configured listener types stay authoritative.
const std::string blk_peer_endpoint = "localhost:17892:blk";

} // namespace

/// Verifies duplicate listen endpoint removal preserves CLI order so p2p-server-address entries remain paired.
BOOST_AUTO_TEST_CASE(listen_endpoint_dedupe_preserves_server_address_pairing) {
   using namespace sysio::net_utils;

   const std::vector<std::string> listen = {normal_listen_endpoint, trx_listen_endpoint, normal_listen_endpoint};
   std::vector<std::string>       server = {normal_listen_endpoint, trx_listen_endpoint};

   const std::vector<std::string> deduped = dedupe_preserve_order(listen);
   BOOST_REQUIRE_EQUAL(deduped.size(), 2u);
   BOOST_CHECK_EQUAL(deduped[0], normal_listen_endpoint);
   BOOST_CHECK_EQUAL(deduped[1], trx_listen_endpoint);

   server.resize(deduped.size());
   for (size_t i = 0; i < deduped.size(); ++i) {
      BOOST_CHECK_EQUAL(std::get<2>(split_host_port_type(deduped[i])), std::get<2>(split_host_port_type(server[i])));
   }
}

/// Verifies peer-advertised addresses may narrow only connections that still accept both traffic classes.
BOOST_AUTO_TEST_CASE(incoming_trx_listener_type_cannot_be_widened) {
   using sysio::net_utils::connection_type;
   using sysio::net_utils::narrow_connection_type;
   using sysio::net_utils::type_from_address;

   connection_type type = type_from_address(trx_listen_endpoint);
   BOOST_CHECK(type == connection_type::transactions_only);

   type = narrow_connection_type(type, plain_peer_endpoint);
   BOOST_CHECK(type == connection_type::transactions_only);
   BOOST_CHECK(narrow_connection_type(type_from_address(normal_listen_endpoint), trx_listen_endpoint) ==
               connection_type::transactions_only);
   BOOST_CHECK(narrow_connection_type(connection_type::transactions_only, blk_peer_endpoint) ==
               connection_type::transactions_only);
}
