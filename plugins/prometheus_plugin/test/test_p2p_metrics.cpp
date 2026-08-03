/**
 * @file test_p2p_metrics.cpp
 * @brief Unit tests for the P2P per-connection metrics in `sysio::metrics::catalog_type` (suite
 *        `prometheus_p2p_metrics`).
 *
 * These pin down the SEC-69 hardening:
 * - per-connection series are keyed by the locally-observed {remote_ip, connection_id}, never the
 *   peer-supplied handshake node id / advertised p2p_address (no label injection);
 * - series for connections that leave the net_plugin snapshot are removed, so P2P churn cannot accumulate
 *   stale series in the registry.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../src/metrics.hpp"

using namespace sysio;
using sysio::metrics::catalog_type;

namespace {

/// Canonical IPv6 bytes for a synthetic peer address.
boost::asio::ip::address_v6::bytes_type ip_bytes(const char* s) {
   return boost::asio::ip::make_address_v6(s).to_bytes();
}

/// Build a P2P snapshot from a list of {connection_id, ipv6-literal} pairs. Every connection also carries
/// peer-controlled identifiers (node id / p2p_address) that must never reach the exported labels.
net_plugin::p2p_connections_metrics
make_snapshot(const std::vector<std::pair<uint32_t, const char*>>& conns) {
   net_plugin::p2p_per_connection_metrics stats(conns.size());
   for (const auto& [cid, ip] : conns) {
      net_plugin::p2p_per_connection_metrics::connection_metric cm{};
      cm.connection_id       = cid;
      cm.address             = ip_bytes(ip);
      cm.port                = 9876;
      cm.latency             = 42;
      cm.unique_conn_node_id = "attacker-controlled-node-id";
      cm.p2p_address         = "attacker-controlled-p2p-addr";
      stats.peers.push_back(std::move(cm));
   }
   return net_plugin::p2p_connections_metrics(conns.size(), 0, std::move(stats));
}

} // namespace

BOOST_AUTO_TEST_SUITE(prometheus_p2p_metrics)

// Series are keyed by the locally-observed {remote_ip, connection_id}; the peer-supplied node id and
// advertised p2p_address never appear in the exported labels.
BOOST_AUTO_TEST_CASE(p2p_series_keyed_by_stable_local_identity) {
   catalog_type cat;
   cat.update(make_snapshot({{1, "::1"}}));
   const std::string out = cat.report();

   BOOST_CHECK(out.find("remote_ip=\"::1\"") != std::string::npos);
   BOOST_CHECK(out.find("connection_id=\"1\"") != std::string::npos);
   // Peer-controlled identifiers must not be exported, and the old peer-controlled label must be gone.
   BOOST_CHECK(out.find("attacker-controlled-node-id") == std::string::npos);
   BOOST_CHECK(out.find("attacker-controlled-p2p-addr") == std::string::npos);
   BOOST_CHECK(out.find("connid=") == std::string::npos);
}

// A connection that drops out of the snapshot has its per-connection series removed, so churn does not
// accumulate stale series.
BOOST_AUTO_TEST_CASE(stale_series_removed_on_churn) {
   catalog_type cat;

   cat.update(make_snapshot({{1, "2001:db8::1"}, {2, "2001:db8::2"}}));
   {
      const std::string out = cat.report();
      BOOST_CHECK(out.find("2001:db8::1") != std::string::npos);
      BOOST_CHECK(out.find("2001:db8::2") != std::string::npos);
   }

   // Second connection leaves the snapshot -> its series must be pruned; the first remains.
   cat.update(make_snapshot({{1, "2001:db8::1"}}));
   {
      const std::string out = cat.report();
      BOOST_CHECK(out.find("2001:db8::1") != std::string::npos);
      BOOST_CHECK(out.find("2001:db8::2") == std::string::npos);
   }

   // All connections gone -> no per-connection series remain.
   cat.update(make_snapshot({}));
   {
      const std::string out = cat.report();
      BOOST_CHECK(out.find("2001:db8::1") == std::string::npos);
   }
}

/// Outbound transport failures expose only the closed enum category set.
BOOST_AUTO_TEST_CASE(outbound_http_failure_labels_have_fixed_cardinality) {
   catalog_type cat;
   const std::string out = cat.report();

   for (const auto failure : magic_enum::enum_values<fc::http::failure_kind>()) {
      const auto expected =
         "nodeop_outbound_http_failures_total{category=\"" +
         std::string(fc::http::failure_kind_name(failure)) + "\"}";
      BOOST_CHECK(out.find(expected) != std::string::npos);
   }
}

BOOST_AUTO_TEST_SUITE_END()
