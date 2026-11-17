#include <boost/lexical_cast.hpp>
#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/auto_bp_peering.hpp>
#include <sysio/net_plugin/message_rate_limiter.hpp>

#include <fc/crypto/private_key.hpp>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <map>

#include "test_ports.hpp"

using namespace sysio::net_utils;

struct mock_connection {
   enum class bp_connection_type { non_bp, bp_config, bp_gossip, bp_gossip_validating };
   bp_connection_type bp_connection = bp_connection_type::non_bp;

   bool is_open            = false;
   bool handshake_received = false;
   bool closing            = false;
   int32_t peer_score      = sysio::peer_scoring::baseline;
   static inline uint32_t last_connection_id = 0;
   uint32_t connection_id  = ++last_connection_id;
   bool go_away_queued     = false;
   boost::asio::ip::address_v6 address;
   std::string log_p2p_address; ///< as connection::log_p2p_address, the address mark_configured_bp_connection splits
   mock_connection(bool bp_connection, bool open, bool received)
     : bp_connection(bp_connection ? bp_connection_type::bp_config : bp_connection_type::non_bp)
     , is_open(open)
     , handshake_received(received)
   {}

   bool socket_is_open() const { return is_open; }
   bool closed() const { return !is_open || closing; }
   /// as connection::close(), marks the connection closing at once; its socket closes later on its strand
   void close(bool reconnect = true) { closing = true; }
   bool incoming_and_handshake_received() const { return handshake_received; }
   int32_t get_peer_score() const { return peer_score; }
   boost::asio::ip::address_v6 remote_address() const { return address; }
};

using namespace sysio::chain::literals;
using namespace std::literals::string_literals;

struct mock_connections_manager {
   uint32_t                     max_client_count = 0;
   std::vector<std::shared_ptr<mock_connection>> connections;

   std::function<void(std::string, std::string)> resolve_and_connect;

   uint32_t get_max_client_count() const { return max_client_count; }

   template <typename Function>
   void for_each_connection(Function&& func) const {
      for (auto c : connections) {
         if (!func(c))
            return;
      }
   }
};

/// The controller calls bp_connection_manager makes, answered from peer_infos
struct mock_controller {
   std::map<sysio::name, sysio::chain::peer_info_t> peer_infos; ///< on-chain peer keys, by producer

   std::optional<sysio::chain::peer_info_t> get_peer_info(sysio::name producer) const {
      const auto i = peer_infos.find(producer);
      if (i == peer_infos.end())
         return std::nullopt;
      return i->second;
   }
};

/// The chain_plugin calls bp_connection_manager makes
struct mock_chain_plugin {
   mock_controller controller;

   mock_controller& chain() { return controller; }
};

struct mock_net_plugin : sysio::auto_bp_peering::bp_connection_manager<mock_net_plugin, mock_connection> {

   bool                         lib_catchup = true;
   mock_connections_manager     connections;
   mock_chain_plugin            mock_chain;
   mock_chain_plugin*           chain_plug = &mock_chain;
   std::atomic<fc::time_point>  head_block_time{};
   sysio::chain_id_type         chain_id = sysio::chain_id_type::empty_chain_id();
   std::vector<std::string>     p2p_addresses{"0.0.0.0:9876"};
   const std::string&           get_first_p2p_address() const { return *p2p_addresses.begin(); }

   bool is_lib_catchup() { return lib_catchup; }

   void setup_test_peers() {
      set_configured_bp_peers({ "proda,127.0.0.1:8001:blk"s, "prodb,127.0.0.1:8002:trx"s, "prodc,127.0.0.1:8003"s,
                     "prodd,127.0.0.1:8004"s, "prode,127.0.0.1:8005"s, "prodf,127.0.0.1:8006"s, "prodg,127.0.0.1:8007"s,
                     "prodh,127.0.0.1:8008"s, "prodi,127.0.0.1:8009"s, "prodj,127.0.0.1:8010"s,
                     // prodk is intentionally skipped
                     "prodl,127.0.0.1:8012"s, "prodm,127.0.0.1:8013"s, "prodn,127.0.0.1:8014"s, "prodo,127.0.0.1:8015"s,
                     "prodp,127.0.0.1:8016"s, "prodq,127.0.0.1:8017"s, "prodr,127.0.0.1:8018"s, "prods,127.0.0.1:8019"s,
                     "prodt,127.0.0.1:8020"s, "produ,127.0.0.1:8021"s }, {});
   }

   fc::logger get_logger() const { return fc::logger::default_logger(); }
};

const std::vector<std::string> peer_addresses{
   "127.0.0.1:8001"s, "127.0.0.1:8002"s, "127.0.0.1:8003"s,
   "127.0.0.1:8004"s, "127.0.0.1:8005"s, "127.0.0.1:8006"s, "127.0.0.1:8007"s,
   "127.0.0.1:8008"s, "127.0.0.1:8009"s, "127.0.0.1:8010"s,
   // prodk is intentionally skipped
   "127.0.0.1:8012"s, "127.0.0.1:8013"s, "127.0.0.1:8014"s, "127.0.0.1:8015"s,
   "127.0.0.1:8016"s, "127.0.0.1:8017"s, "127.0.0.1:8018"s, "127.0.0.1:8019"s,
   // "prodt,127.0.0.1:8020"s - prodt is not included in the schedules
   "127.0.0.1:8021"s};

BOOST_AUTO_TEST_CASE(test_set_bp_peers) {

   mock_net_plugin plugin;
   BOOST_CHECK_THROW(plugin.set_configured_bp_peers({ "producer17,127.0.0.1:8888"s }, {}), sysio::chain::plugin_config_exception);
   BOOST_CHECK_THROW(plugin.set_configured_bp_peers({ "producer1"s }, {}), sysio::chain::plugin_config_exception);

   plugin.set_configured_bp_peers({
         "producer1,127.0.0.1:8888:blk"s,
         "producer2,127.0.0.1:8889:trx"s,
         "producer3,127.0.0.1:8890"s,
         "producer4,127.0.0.1:8891"s
   }, {});
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_addresses["producer1"_n], endpoint("127.0.0.1", "8888"));
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_addresses["producer2"_n], endpoint("127.0.0.1", "8889"));
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_addresses["producer3"_n], endpoint("127.0.0.1", "8890"));
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_addresses["producer4"_n], endpoint("127.0.0.1", "8891"));

   BOOST_CHECK_EQUAL(plugin.config.auto_bp_accounts[endpoint("127.0.0.1", "8888")], "producer1"_n);
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_accounts[endpoint("127.0.0.1", "8889")], "producer2"_n);
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_accounts[endpoint("127.0.0.1", "8890")], "producer3"_n);
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_accounts[endpoint("127.0.0.1", "8891")], "producer4"_n);
}

bool operator==(const sysio::chain::name_set_t& a, const sysio::chain::name_set_t& b) {
   return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

bool operator==(const std::vector<std::string>& a, const std::vector<std::string>& b) {
   return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

namespace boost::container {
std::ostream& boost_test_print_type(std::ostream& os, const sysio::chain::name_set_t& accounts) {
   os << "{";
   const char* sep = "";
   for (auto e : accounts) {
      os << sep << e.to_string();
      sep = ", ";
   }
   os << "}";
   return os;
}
} // namespace boost::container

namespace std {
std::ostream& boost_test_print_type(std::ostream& os, const std::vector<std::string>& content) {
   os << "{";
   const char* sep = "";
   for (auto e : content) {
      os << sep << e;
      sep = ", ";
   }
   os << "}";
   return os;
}
} // namespace std

const sysio::chain::producer_authority_schedule test_schedule1{
   1,
   { { "proda"_n, {} }, { "prodb"_n, {} }, { "prodc"_n, {} }, { "prodd"_n, {} }, { "prode"_n, {} }, { "prodf"_n, {} },
     { "prodg"_n, {} }, { "prodh"_n, {} }, { "prodi"_n, {} }, { "prodj"_n, {} }, { "prodk"_n, {} }, { "prodl"_n, {} },
     { "prodm"_n, {} }, { "prodn"_n, {} }, { "prodo"_n, {} }, { "prodp"_n, {} }, { "prodq"_n, {} }, { "prodr"_n, {} },
     { "prods"_n, {} },
     // { "prodt"_n, {} }, pick one to skip so not a full schedule
     { "produ"_n, {} } }
};

const sysio::chain::producer_authority_schedule test_schedule2{
   2,
   { { "proda"_n, {} }, { "prode"_n, {} }, { "prodi"_n, {} }, { "prodm"_n, {} }, { "prodp"_n, {} }, { "prods"_n, {} },
     { "prodb"_n, {} }, { "prodf"_n, {} }, { "prodj"_n, {} }, { "prodn"_n, {} }, { "prodq"_n, {} },
     // { "prodt"_n, {} }, pick one to skip so not a full schedule
     { "prodc"_n, {} }, { "prodg"_n, {} }, { "prodk"_n, {} }, { "prodo"_n, {} }, { "prodr"_n, {} }, { "produ"_n, {} },
     { "prodd"_n, {} }, { "prodh"_n, {} }, { "prodl"_n, {} } }
};

const sysio::chain::name_set_t producers_minus_prodkt{
   "proda"_n, "prodb"_n, "prodc"_n, "prodd"_n, "prode"_n, "prodf"_n,
   "prodg"_n, "prodh"_n, "prodi"_n, "prodj"_n,
   // "prodk"_n, not part of the peer addresses
   "prodl"_n,
   "prodm"_n, "prodn"_n, "prodo"_n, "prodp"_n, "prodq"_n, "prodr"_n,
   "prods"_n,
   // "prodt"_n, not part of the schedules, see above
   "produ"_n };

const sysio::chain::producer_authority_schedule reset_schedule1{ 1, {} };

/// address_set_t is unordered; sort into a vector for stable comparisons
template <typename Set>
std::vector<std::string> sorted(const Set& addresses) {
   std::vector<std::string> result(addresses.begin(), addresses.end());
   std::ranges::sort(result);
   return result;
}

BOOST_AUTO_TEST_CASE(test_on_pending_schedule) {

   mock_net_plugin plugin;
   plugin.setup_test_peers();
   plugin.set_pending_bps({ "prodj"_n, "prodm"_n });

   std::vector<std::string> connected_hosts;

   plugin.connections.resolve_and_connect = [&connected_hosts, &plugin](std::string host, std::string p2p_address) {
      // the new pending set must be published before any dial goes out, otherwise a concurrent
      // connection_monitor pass could prune the just-established pending-only connections
      BOOST_TEST(plugin.get_pending_bps() == producers_minus_prodkt);
      connected_hosts.push_back(host);
   };

   // make sure nothing happens when it is not in_sync
   plugin.lib_catchup = true;
   plugin.on_pending_schedule(test_schedule1);

   BOOST_CHECK_EQUAL(connected_hosts, (std::vector<std::string>{}));
   BOOST_TEST(plugin.get_pending_bps() == (sysio::chain::name_set_t{ "prodj"_n, "prodm"_n }));
   BOOST_CHECK_EQUAL(plugin.pending_schedule_version, 0u);

   // when it is in sync and on_pending_schedule is called
   plugin.lib_catchup = false;
   plugin.on_pending_schedule(test_schedule1);

   // the pending are connected to
   BOOST_TEST(plugin.get_pending_bps() == producers_minus_prodkt);

   // all connect to bp peers should be invoked
   std::ranges::sort(connected_hosts);
   BOOST_CHECK_EQUAL(connected_hosts, peer_addresses);

   BOOST_CHECK_EQUAL(plugin.pending_schedule_version, 1u);

   // make sure we don't change the active_schedule_version
   BOOST_CHECK_EQUAL(plugin.active_schedule_version, 0u);

   // Let's call on_pending_schedule() again, and connect shouldn't be called again
   connected_hosts.clear();
   plugin.on_pending_schedule(test_schedule1);
   BOOST_CHECK_EQUAL(connected_hosts, (std::vector<std::string>{}));

   plugin.on_pending_schedule(reset_schedule1);
   BOOST_TEST(plugin.get_pending_bps() == sysio::chain::name_set_t{});
}

BOOST_AUTO_TEST_CASE(test_on_active_schedule1) {

   mock_net_plugin plugin;
   plugin.setup_test_peers();

   plugin.set_active_bps( { "proda"_n, "prodh"_n, "prodn"_n, "prodt"_n } );
   plugin.connections.resolve_and_connect = [](std::string host, std::string p2p_address) {};

   // make sure nothing happens when it is not in_sync
   plugin.lib_catchup = true;
   plugin.on_active_schedule(test_schedule1);

   BOOST_TEST(plugin.get_active_bps() == (sysio::chain::name_set_t{ "proda"_n, "prodh"_n, "prodn"_n, "prodt"_n }));
   BOOST_CHECK_EQUAL(plugin.active_schedule_version, 0u);

   // when it is in sync and on_active_schedule is called
   plugin.lib_catchup = false;
   plugin.on_pending_schedule(test_schedule1);
   plugin.on_active_schedule(test_schedule1);

   BOOST_TEST(plugin.get_active_bps() == producers_minus_prodkt);

   // make sure we change the active_schedule_version
   BOOST_CHECK_EQUAL(plugin.active_schedule_version, 1u);

   // prodt is configured but neither active nor pending: the connection monitor prunes its address
   BOOST_CHECK_EQUAL(sorted(plugin.gossip_bp_addresses_to_drop()), (std::vector<std::string>{ "127.0.0.1:8020"s }));
}

BOOST_AUTO_TEST_CASE(test_on_active_schedule2) {

   mock_net_plugin plugin;
   plugin.setup_test_peers();

   plugin.set_active_bps( { "proda"_n, "prodh"_n, "prodn"_n, "prodt"_n } );
   plugin.connections.resolve_and_connect = [](std::string host, std::string p2p_address) {};

   // when pending and active schedules are changed simultaneously
   plugin.lib_catchup = false;
   plugin.on_pending_schedule(test_schedule2);
   plugin.on_active_schedule(test_schedule1);

   BOOST_TEST(plugin.get_active_bps() == producers_minus_prodkt);

   // make sure we change the active_schedule_version
   BOOST_CHECK_EQUAL(plugin.active_schedule_version, 1u);

   // prodt is configured but neither in the active nor in the pending schedule
   BOOST_CHECK_EQUAL(sorted(plugin.gossip_bp_addresses_to_drop()), (std::vector<std::string>{ "127.0.0.1:8020"s }));
}

BOOST_AUTO_TEST_CASE(test_drop_out_of_gossip_group) {

   mock_net_plugin plugin;
   plugin.setup_test_peers();
   // this node's own bp account (prodt) participates in bp gossip
   plugin.set_bp_producer_peers({ "prodt,127.0.0.1:8020,127.0.0.1"s });
   plugin.connections.resolve_and_connect = [](std::string host, std::string p2p_address) {};
   plugin.lib_catchup = false;

   // prodt was active, and is in neither the new active schedule nor the pending one: drop out
   plugin.set_active_bps({ "proda"_n, "prodt"_n });
   plugin.on_active_schedule(test_schedule1);

   BOOST_CHECK(!plugin.connect_bp_gossip_peers);

   // out of the gossip group every known bp peer address is pruned
   std::vector<std::string> all_configured_addresses = peer_addresses;
   all_configured_addresses.push_back("127.0.0.1:8020"s);
   std::ranges::sort(all_configured_addresses);
   BOOST_CHECK_EQUAL(sorted(plugin.gossip_bp_addresses_to_drop()), all_configured_addresses);

   // a new pending schedule re-enters the gossip group and shrinks the prune set again
   plugin.on_pending_schedule(test_schedule2);
   BOOST_CHECK(plugin.connect_bp_gossip_peers);
   BOOST_CHECK_EQUAL(sorted(plugin.gossip_bp_addresses_to_drop()), (std::vector<std::string>{ "127.0.0.1:8020"s }));
}

BOOST_AUTO_TEST_CASE(test_no_drop_out_while_pending) {

   mock_net_plugin plugin;
   plugin.setup_test_peers();
   plugin.set_bp_producer_peers({ "prodt,127.0.0.1:8020,127.0.0.1"s });
   plugin.connections.resolve_and_connect = [](std::string host, std::string p2p_address) {};
   plugin.lib_catchup = false;

   // prodt leaves the active schedule but is still in the pending one: stay in the gossip group
   plugin.set_active_bps({ "proda"_n, "prodt"_n });
   plugin.set_pending_bps({ "prodt"_n });
   plugin.on_active_schedule(test_schedule1);

   BOOST_CHECK(plugin.connect_bp_gossip_peers);

   // prodt's own address stays connected while prodt is pending
   BOOST_CHECK_EQUAL(sorted(plugin.gossip_bp_addresses_to_drop()), (std::vector<std::string>{}));
}

/// Record conn as admitted, as admit_client() does once it admits a client.
void record_admitted(mock_net_plugin& plugin, const std::shared_ptr<mock_connection>& conn) {
   fc::lock_guard g(plugin.client_admission_mtx);
   plugin.admitted_client_ids.insert(conn->connection_id);
}

/// Whether plugin has recorded conn as admitted.
bool admitted(mock_net_plugin& plugin, const std::shared_ptr<mock_connection>& conn) {
   fc::lock_guard g(plugin.client_admission_mtx);
   return plugin.admitted_client_ids.contains(conn->connection_id);
}

/// The admission helpers below, each under the admission mutex as admit_client() calls them.
std::size_t num_clients(mock_net_plugin& plugin) {
   fc::lock_guard g(plugin.client_admission_mtx);
   return plugin.num_established_clients();
}

bool exceeding(mock_net_plugin& plugin, const std::shared_ptr<mock_connection>& conn) {
   fc::lock_guard g(plugin.client_admission_mtx);
   return plugin.exceeding_connection_limit(conn);
}

std::shared_ptr<mock_connection> victim_for(mock_net_plugin& plugin, const std::shared_ptr<mock_connection>& conn) {
   fc::lock_guard g(plugin.client_admission_mtx);
   return plugin.select_client_to_evict(conn);
}

BOOST_AUTO_TEST_CASE(test_exceeding_connection_limit) {
   mock_net_plugin plugin;
   plugin.setup_test_peers();
   plugin.connections.max_client_count = 1;
   plugin.connections.connections = {
      std::make_shared<mock_connection>( true, true, true ),   // 0
      std::make_shared<mock_connection>( true, true, false ),  // 1
      std::make_shared<mock_connection>( true, false, true ),  // 2
      std::make_shared<mock_connection>( true, false, false ), // 3
      std::make_shared<mock_connection>( false, true, true ),  // 4
      std::make_shared<mock_connection>( false, true, false ), // 5
      std::make_shared<mock_connection>( false, true, true ),  // 6
      std::make_shared<mock_connection>( false, false, false ) // 7
   };
   for (const auto& conn : plugin.connections.connections)
      record_admitted(plugin, conn); // each admitted when it handshook

   BOOST_CHECK_EQUAL(num_clients(plugin), 2u);

   BOOST_CHECK(!exceeding(plugin, plugin.connections.connections[0]));
   BOOST_CHECK(!exceeding(plugin, plugin.connections.connections[1]));
   BOOST_CHECK(!exceeding(plugin, plugin.connections.connections[2]));
   BOOST_CHECK(!exceeding(plugin, plugin.connections.connections[3]));
   BOOST_CHECK(exceeding(plugin, plugin.connections.connections[4]));
   BOOST_CHECK(!exceeding(plugin, plugin.connections.connections[5]));
   BOOST_CHECK(exceeding(plugin, plugin.connections.connections[6]));
   BOOST_CHECK(!exceeding(plugin, plugin.connections.connections[7]));
}

/// The index-th alternate-P2P port of this test's lease, as text, for the endpoints the bp fixtures configure.
std::string leased_port(uint32_t index = 0) {
   return std::to_string(sysio::testing::get_port(sysio::testing::port_category::alternate_p2p, index));
}

/// An open inbound client connection from address that has sent its first handshake, not yet admitted.
std::shared_ptr<mock_connection> inbound_from(const std::string& address) {
   auto conn = std::make_shared<mock_connection>(false, true, true);
   conn->address = sysio::net_utils::parse_mapped_v6(address).value();
   return conn;
}

/// An inbound client connection from address that admit_client admitted.
std::shared_ptr<mock_connection> admitted_from(mock_net_plugin& plugin, const std::string& address) {
   auto conn = inbound_from(address);
   record_admitted(plugin, conn);
   return conn;
}

/// The bp connection type mark_inbound_bp_connection gives an inbound connection from address.
mock_connection::bp_connection_type marked_type(const mock_net_plugin& plugin, const std::string& address) {
   const auto conn = inbound_from(address);
   plugin.mark_inbound_bp_connection(conn.get());
   return conn->bp_connection;
}

/// A gossiped bp peer as validate_gossip_bp_peers_message leaves it, signed by a new key so its signature is unique.
sysio::gossip_bp_peers_message::signed_bp_peer gossip_peer(sysio::name producer, std::string server_endpoint,
                                                           std::string outbound_ip_address) {
   sysio::gossip_bp_peers_message::signed_bp_peer peer{{.producer_name = producer}};
   peer.cached_bp_peer_info.emplace(std::move(server_endpoint), std::move(outbound_ip_address),
                                    sysio::block_timestamp_type{});
   peer.bp_peer_info = fc::raw::pack(*peer.cached_bp_peer_info);
   peer.sig = fc::crypto::private_key::generate().sign(peer.digest(sysio::chain_id_type::empty_chain_id()));
   return peer;
}

BOOST_AUTO_TEST_CASE(test_configured_bp_ip_addresses) {
   mock_net_plugin plugin;
   const std::string port = leased_port(), other_port = leased_port(1);
   plugin.set_configured_bp_peers({ "proda,192.0.2.1:" + port, "prodb,[2001:db8::2]:" + port + ":blk",
                                    "prodc,p2p.example.com:" + port, "prodd,192.0.2.1:" + other_port }, {});

   // host names are not addresses, and two endpoints on one host give one address
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_ip_addresses.size(), 2u);
   BOOST_CHECK(plugin.config.auto_bp_ip_addresses.contains(boost::asio::ip::make_address_v6("::ffff:192.0.2.1")));
   BOOST_CHECK(plugin.config.auto_bp_ip_addresses.contains(boost::asio::ip::make_address_v6("2001:db8::2")));
}

/// A configured bp peer on an IPv6 literal is dialed, and pruned, at an address that splits back into its endpoint.
/// Host names and IPv4 addresses are dialed as configured.
BOOST_AUTO_TEST_CASE(test_configured_bp_ipv6_peer_address) {
   mock_net_plugin plugin;
   const std::string port = leased_port();
   const std::vector<std::string> scheduled = { "[::1]:" + port, "[2001:db8::2]:" + port, "192.0.2.1:" + port,
                                                "p2p.example.com:" + port };
   const std::string unscheduled = "[2001:db8::3]:" + port;
   plugin.set_configured_bp_peers({ "proda," + scheduled[0] + ":blk", "prodb," + scheduled[1], "prodc," + scheduled[2],
                                    "prodd," + scheduled[3], "prodt," + unscheduled }, {});

   BOOST_CHECK_EQUAL(plugin.config.auto_bp_addresses["proda"_n], endpoint("::1", port));
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_addresses["proda"_n].address(), scheduled[0]);
   BOOST_CHECK_EQUAL(boost::lexical_cast<std::string>(plugin.config.auto_bp_addresses["prodb"_n]), scheduled[1]);

   std::vector<std::string> dialed;
   plugin.connections.resolve_and_connect = [&dialed](std::string host, std::string p2p_address) {
      dialed.push_back(host);
   };
   plugin.lib_catchup = false;
   plugin.on_pending_schedule(test_schedule1);

   BOOST_CHECK_EQUAL(sorted(dialed), sorted(scheduled));
   for (const std::string& address : dialed) {
      // the outbound connection constructor marks the connection it dials by the address it was given
      mock_connection conn(false, true, false);
      conn.log_p2p_address = address;
      plugin.mark_configured_bp_connection(&conn);
      BOOST_CHECK_MESSAGE(conn.bp_connection == mock_connection::bp_connection_type::bp_config,
                          address << " is not recognized as a configured bp peer");
   }

   // prodt is configured but not scheduled: the connection monitor prunes the address it was dialed at
   BOOST_CHECK_EQUAL(sorted(plugin.gossip_bp_addresses_to_drop()), (std::vector<std::string>{ unscheduled }));
}

BOOST_AUTO_TEST_CASE(test_mark_inbound_configured_bp_by_address) {
   using bp_type = mock_connection::bp_connection_type;
   mock_net_plugin plugin;
   const std::string port = leased_port();
   plugin.set_configured_bp_peers(
      { "proda,192.0.2.1:" + port, "prodb,[2001:db8::2]:" + port, "prodc,localhost:" + port }, {});

   // a configured bp connects from any port, and an IPv4 peer may be seen in its mapped spelling
   BOOST_CHECK(marked_type(plugin, "192.0.2.1") == bp_type::bp_config);
   BOOST_CHECK(marked_type(plugin, "::ffff:192.0.2.1") == bp_type::bp_config);
   BOOST_CHECK(marked_type(plugin, "2001:db8::2") == bp_type::bp_config);

   // other addresses are clients; a host name entry is never matched by address, even by one it resolves to
   BOOST_CHECK(marked_type(plugin, "192.0.2.2") == bp_type::non_bp);
   BOOST_CHECK(marked_type(plugin, "2001:db8::3") == bp_type::non_bp);
   BOOST_CHECK(marked_type(plugin, "127.0.0.1") == bp_type::non_bp);

   // an unknown remote endpoint matches nothing, even an unspecified address configured as a bp host
   plugin.config.auto_bp_ip_addresses.insert(boost::asio::ip::address_v6{});
   BOOST_CHECK(marked_type(plugin, "::") == bp_type::non_bp);

   // nor does a link-local one: the connection keeps no scope id, so a peer on another link would look the same
   plugin.config.auto_bp_ip_addresses.insert(boost::asio::ip::make_address_v6("fe80::1"));
   BOOST_CHECK(marked_type(plugin, "fe80::1") == bp_type::non_bp);

   // a connection already recognized is left as it is
   const auto gossip = inbound_from("192.0.2.1");
   gossip->bp_connection = bp_type::bp_gossip;
   plugin.mark_inbound_bp_connection(gossip.get());
   BOOST_CHECK(gossip->bp_connection == bp_type::bp_gossip);
}

BOOST_AUTO_TEST_CASE(test_mark_inbound_gossip_bp_by_address) {
   using bp_type = mock_connection::bp_connection_type;
   const std::string port = leased_port();
   const auto add_gossip_peers = [&port](mock_net_plugin& plugin) {
      fc::lock_guard g(plugin.gossip_bps.mtx);
      plugin.gossip_bps.index.insert(gossip_peer("proda"_n, "p2p.proda.example.com:" + port, "198.51.100.1"));
      plugin.gossip_bps.index.insert(gossip_peer("prodb"_n, "p2p.prodb.example.com:" + port, "2001:db8::b"));
      plugin.gossip_bps.index.insert(gossip_peer("prodc"_n, "p2p.prodc.example.com:" + port, "not-an-ip-address"));
      plugin.gossip_bps.index.insert(gossip_peer("prodd"_n, "p2p.prodd.example.com:" + port, "fe80::d"));
   };

   mock_net_plugin gossiping;
   gossiping.set_bp_producer_peers({ "prodt,127.0.0.1:" + leased_port(1) + ",127.0.0.1" });
   add_gossip_peers(gossiping);
   BOOST_CHECK(marked_type(gossiping, "198.51.100.1") == bp_type::bp_gossip_validating);
   BOOST_CHECK(marked_type(gossiping, "::ffff:198.51.100.1") == bp_type::bp_gossip_validating);
   BOOST_CHECK(marked_type(gossiping, "2001:db8::b") == bp_type::bp_gossip_validating);
   BOOST_CHECK(marked_type(gossiping, "198.51.100.2") == bp_type::non_bp);
   BOOST_CHECK(marked_type(gossiping, "2001:db8::c") == bp_type::non_bp);
   BOOST_CHECK(marked_type(gossiping, "fe80::d") == bp_type::non_bp); // link-local is never matched

   // without bp gossip of its own, a node does not trust gossiped addresses, even with configured bp peers
   mock_net_plugin configured_only;
   configured_only.set_configured_bp_peers({ "proda,192.0.2.1:" + port }, {});
   add_gossip_peers(configured_only);
   BOOST_CHECK(marked_type(configured_only, "198.51.100.1") == bp_type::non_bp);

   // without auto bp peering, nothing is recognized
   mock_net_plugin disabled;
   add_gossip_peers(disabled);
   BOOST_CHECK(marked_type(disabled, "198.51.100.1") == bp_type::non_bp);
}

BOOST_AUTO_TEST_CASE(test_select_client_to_evict) {
   using sysio::peer_scoring::eviction_threshold;
   mock_net_plugin plugin;
   plugin.setup_test_peers();
   const auto connection = [&plugin](bool bp, bool open, bool received, int32_t score, bool admit = true) {
      auto conn = std::make_shared<mock_connection>(bp, open, received);
      conn->peer_score = score;
      if (admit)
         record_admitted(plugin, conn);
      return conn;
   };
   const auto poor         = connection(false, true, true, eviction_threshold - 1);
   const auto poorest      = connection(false, true, true, eviction_threshold - 40);
   const auto bp           = connection(true, true, true, 0);
   const auto no_handshake = connection(false, true, false, 0);
   const auto closed       = connection(false, false, true, 0);
   const auto closing      = connection(false, true, true, 0);
   closing->closing = true;
   const auto turned_away  = connection(false, true, true, 0);
   turned_away->go_away_queued = true;
   const auto unadmitted   = connection(false, true, true, 0, false); // a newcomer handshaking alongside
   const auto newcomer     = connection(false, true, true, 0, false);
   plugin.connections.connections = { connection(false, true, true, sysio::peer_scoring::baseline), poor, bp,
                                      no_handshake, closed, closing, turned_away, unadmitted, poorest, newcomer };

   // never the newcomer, a bp, a connection not yet admitted, or one closing or being turned away
   BOOST_CHECK(victim_for(plugin, newcomer) == poorest);

   // a client back at the threshold is not evicted
   poorest->peer_score = eviction_threshold;
   BOOST_CHECK(victim_for(plugin, newcomer) == poor);
   poor->peer_score = eviction_threshold;
   BOOST_CHECK(!victim_for(plugin, newcomer));
}

BOOST_AUTO_TEST_CASE(test_configured_bp_address_never_counted_or_evicted) {
   mock_net_plugin plugin;
   plugin.set_configured_bp_peers({ "proda,192.0.2.1:" + leased_port() }, {});
   plugin.connections.max_client_count = 1;

   const auto client = admitted_from(plugin, "192.0.2.9");
   const auto bp     = inbound_from("192.0.2.1"); // its handshake p2p_address need not name the configured endpoint
   bp->peer_score = sysio::peer_scoring::min_score;
   plugin.connections.connections = { client, bp };
   plugin.mark_inbound_bp_connection(bp.get());
   BOOST_CHECK(!exceeding(plugin, bp));
   BOOST_CHECK(plugin.admit_client(bp));
   BOOST_CHECK(!admitted(plugin, bp)); // only clients are recorded
   BOOST_CHECK_EQUAL(num_clients(plugin), 1u);

   // a further client exceeds the limit, and the misbehaving bp is still not evicted for it
   const auto newcomer = inbound_from("192.0.2.10");
   plugin.connections.connections.push_back(newcomer);
   BOOST_CHECK(exceeding(plugin, newcomer));
   BOOST_CHECK(!victim_for(plugin, newcomer));
}

BOOST_AUTO_TEST_CASE(test_departing_clients_not_counted) {
   mock_net_plugin plugin;
   const auto evicted = admitted_from(plugin, "192.0.2.10");
   evicted->closing = true; // its socket not yet closed
   const auto turned_away = admitted_from(plugin, "192.0.2.11");
   turned_away->go_away_queued = true; // its go-away not yet sent
   const auto rejected = inbound_from("192.0.2.12");
   rejected->go_away_queued = true; // rejected at its handshake, never admitted
   const auto unadmitted = inbound_from("192.0.2.13"); // its handshake still being checked
   plugin.connections.connections = { admitted_from(plugin, "192.0.2.9"), evicted, turned_away, rejected, unadmitted };
   BOOST_CHECK_EQUAL(num_clients(plugin), 1u);

   // so a newcomer arriving with them fits in the slot left of a limit of two, and none of them is chosen to evict
   plugin.setup_test_peers();
   plugin.connections.max_client_count = 2;
   evicted->peer_score = turned_away->peer_score = rejected->peer_score = unadmitted->peer_score =
      sysio::peer_scoring::min_score;
   const auto newcomer = inbound_from("192.0.2.14");
   plugin.connections.connections.push_back(newcomer);
   BOOST_CHECK(!exceeding(plugin, newcomer));
   BOOST_CHECK(!victim_for(plugin, newcomer));
}

BOOST_AUTO_TEST_CASE(test_admit_client) {
   mock_net_plugin plugin;
   plugin.setup_test_peers(); // auto bp peering, which enforces max_client_count at the handshake
   plugin.connections.max_client_count = 3;
   const auto keeper = admitted_from(plugin, "192.0.2.9");

   // within the limit, a newcomer is admitted
   const auto client = inbound_from("192.0.2.10");
   plugin.connections.connections = { keeper, client };
   BOOST_CHECK(plugin.admit_client(client));
   BOOST_CHECK(admitted(plugin, client));
   BOOST_CHECK(!keeper->closing && !client->closing);

   // the limit full, a newcomer takes the misbehaving client's place
   const auto victim = admitted_from(plugin, "192.0.2.11");
   victim->peer_score = sysio::peer_scoring::eviction_threshold - 1;
   const auto newcomer = inbound_from("192.0.2.12");
   plugin.connections.connections = { keeper, client, victim, newcomer };
   BOOST_CHECK(plugin.admit_client(newcomer));
   BOOST_CHECK(victim->closing);
   BOOST_CHECK(!newcomer->closing);
   BOOST_CHECK_EQUAL(num_clients(plugin), 3u);

   // the limit full of clients in good standing, a newcomer is refused
   const auto refused = inbound_from("192.0.2.13");
   plugin.connections.connections = { keeper, client, newcomer, refused };
   BOOST_CHECK(!plugin.admit_client(refused));
   BOOST_CHECK(refused->closing);
   BOOST_CHECK(!admitted(plugin, refused));
   BOOST_CHECK(!keeper->closing && !client->closing && !newcomer->closing);

   // the limit full with one misbehaving client: newcomers that handshake together do not share its place, as the
   // first admitted counts and the victim is closing before the next is decided
   const auto shared_victim = admitted_from(plugin, "192.0.2.20");
   shared_victim->peer_score = sysio::peer_scoring::eviction_threshold - 1;
   const auto first = inbound_from("192.0.2.21");
   const auto second = inbound_from("192.0.2.22");
   plugin.connections.connections = { keeper, client, shared_victim, first, second };
   BOOST_CHECK(plugin.admit_client(first));
   BOOST_CHECK(shared_victim->closing);
   BOOST_CHECK(!plugin.admit_client(second));
   BOOST_CHECK_EQUAL(num_clients(plugin), 3u);

   // one place free: of newcomers that handshake together, beside one rejected at its handshake, the first admitted
   // takes it, as admissions count one another and the rejected one never counts; the rest are refused
   const auto rejected = inbound_from("192.0.2.14");
   rejected->go_away_queued = true;
   const std::vector<std::shared_ptr<mock_connection>> burst{ inbound_from("192.0.2.15"), inbound_from("192.0.2.16"),
                                                              inbound_from("192.0.2.17") };
   plugin.connections.connections = { keeper, client, rejected };
   plugin.connections.connections.insert(plugin.connections.connections.end(), burst.begin(), burst.end());
   BOOST_CHECK(plugin.admit_client(burst[0]));
   BOOST_CHECK(!plugin.admit_client(burst[1]));
   BOOST_CHECK(!plugin.admit_client(burst[2]));
   BOOST_CHECK_EQUAL(num_clients(plugin), 3u);
   BOOST_CHECK(!keeper->closing && !client->closing);

   // without auto bp peering, create_session() enforces the limit and the handshake admits every client
   mock_net_plugin disabled;
   disabled.connections.max_client_count = 1;
   const auto over = inbound_from("192.0.2.18");
   disabled.connections.connections = { admitted_from(disabled, "192.0.2.19"), over };
   BOOST_CHECK(disabled.admit_client(over));
   BOOST_CHECK(!over->closing);
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_not_counted_or_evicted) {
   using bp_type = mock_connection::bp_connection_type;
   mock_net_plugin plugin;
   plugin.setup_test_peers();
   plugin.connections.max_client_count = 1;
   const auto client = admitted_from(plugin, "192.0.2.9");
   client->peer_score = sysio::peer_scoring::min_score;
   const auto newcomer = inbound_from("192.0.2.10");
   plugin.connections.connections = { client, newcomer };
   BOOST_CHECK(victim_for(plugin, newcomer) == client);

   // once its gossip shows it is a bp, it is neither counted nor evicted
   plugin.set_bp_connection(client.get(), bp_type::bp_gossip);
   BOOST_CHECK(client->bp_connection == bp_type::bp_gossip);
   BOOST_CHECK_EQUAL(num_clients(plugin), 0u);
   BOOST_CHECK(!victim_for(plugin, newcomer));
   BOOST_CHECK(plugin.admit_client(newcomer));
   BOOST_CHECK(!client->closing);
}

using signed_bp_peer = sysio::gossip_bp_peers_message::signed_bp_peer;
using key_type       = fc::crypto::private_key::key_type;

/// Header byte of a K1 compact signature by a compressed key less that of one by an uncompressed key
constexpr unsigned char k1_compressed_header_offset = 4;
/// Offset of s in a K1 compact signature: the header byte, then r
constexpr size_t k1_s_offset = 33;

/// secp256k1 context for the signature forms the tests make beside the one fc signs
const secp256k1_context* test_secp256k1_context() {
   static secp256k1_context* const ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
   return ctx;
}

/// @return compact as a signature
fc::crypto::signature to_signature(const fc::ecc::compact_signature& compact) {
   return fc::crypto::signature(fc::crypto::signature::storage_type(fc::ecc::signature_shim(compact)));
}

/// @return the high-S form of the canonical K1 signature sig, which recovers the same key: s negated modulo the curve
///         order, which flips the parity of the recovery id
fc::crypto::signature high_s_form(const fc::crypto::signature& sig) {
   fc::ecc::compact_signature compact = sig.get<fc::ecc::signature_shim>()._data;
   // s is a scalar below the curve order, which secret key negation takes to the curve order less s
   BOOST_REQUIRE(secp256k1_ec_seckey_negate(test_secp256k1_context(), compact.data() + k1_s_offset));
   compact[0] = fc::ecc::compact_signature_compressed_header +
                 ((compact[0] - fc::ecc::compact_signature_compressed_header) ^ 1);
   return to_signature(compact);
}

/// @return the canonical K1 signature sig with the header byte for an uncompressed key, which recovers the same key
fc::crypto::signature uncompressed_header_form(const fc::crypto::signature& sig) {
   fc::ecc::compact_signature compact = sig.get<fc::ecc::signature_shim>()._data;
   compact[0] -= k1_compressed_header_offset;
   return to_signature(compact);
}

/// @return a canonical K1 signature of digest by key other than key.sign(digest): libsecp256k1 signs with extra nonce
///         data, as a signer that draws a random nonce does
fc::crypto::signature signed_with_other_nonce(const fc::crypto::private_key& key, const fc::sha256& digest) {
   const fc::ecc::private_key_secret& secret = key.get<fc::ecc::private_key_shim>()._data;
   const std::array<unsigned char, 32> nonce_data{1};
   secp256k1_ecdsa_recoverable_signature recoverable{};
   BOOST_REQUIRE(secp256k1_ecdsa_sign_recoverable(test_secp256k1_context(), &recoverable,
                                                  reinterpret_cast<const unsigned char*>(digest.data()),
                                                  reinterpret_cast<const unsigned char*>(secret.data()),
                                                  secp256k1_nonce_function_rfc6979, nonce_data.data()));
   fc::ecc::compact_signature compact{};
   int recovery_id = 0;
   BOOST_REQUIRE(secp256k1_ecdsa_recoverable_signature_serialize_compact(test_secp256k1_context(), compact.data() + 1,
                                                                         &recovery_id, &recoverable));
   compact[0] = fc::ecc::compact_signature_compressed_header + recovery_id;
   return to_signature(compact);
}

/// An entry of gossip_bps as the tests compare it: producer, server endpoint, outbound ip address, expiration slot and
/// signature
using held_entry = std::tuple<std::string, std::string, std::string, uint32_t, std::string>;

/// @return the entries plugin holds, in key order
std::vector<held_entry> held_entries(mock_net_plugin& plugin) {
   std::vector<held_entry> entries;
   fc::lock_guard g(plugin.gossip_bps.mtx);
   for (const signed_bp_peer& peer : plugin.gossip_bps.index)
      entries.emplace_back(peer.producer_name.to_string(), peer.server_endpoint(), peer.outbound_ip_address(),
                           peer.expiration().slot, peer.sig.to_string({}));
   return entries;
}

/// @return a gossip message of peers as it arrives: unpacked, its peer info not yet read
sysio::gossip_bp_peers_message received(std::vector<signed_bp_peer> peers) {
   const sysio::gossip_bp_peers_message msg{std::move(peers)};
   return fc::raw::unpack<sysio::gossip_bp_peers_message>(fc::raw::pack(msg));
}

/// A node gossiping as prodt, beside the on-chain peer key of each producer the tests sign an entry for
struct gossip_fixture {
   /// Port of the gossiped server endpoints; nothing binds or dials it
   static constexpr std::string_view port = "9876";
   /// Head block slot the fixture starts at
   static constexpr uint32_t start_slot = 1'000'000;

   mock_net_plugin                                plugin;
   std::map<sysio::name, fc::crypto::private_key> peer_keys;
   sysio::block_timestamp_type                    head{start_slot};

   gossip_fixture() {
      plugin.set_bp_producer_peers({ "prodt,127.0.0.1:" + std::string(port) + ",127.0.0.1" });
      set_head(head);
   }

   void set_head(sysio::block_timestamp_type t) {
      head = t;
      plugin.head_block_time = t.to_time_point();
   }

   /// @return the head block time plus d
   sysio::block_timestamp_type after(fc::microseconds d) const {
      return sysio::block_timestamp_type(head.to_time_point() + d);
   }

   /// @return the private key of producer's on-chain peer key, registering a new key of type on first use
   const fc::crypto::private_key& peer_key(sysio::name producer, key_type type = key_type::k1) {
      if (!peer_keys.contains(producer)) {
         const fc::crypto::private_key& key = peer_keys.emplace(producer, fc::crypto::private_key::generate(type))
                                                 .first->second;
         plugin.mock_chain.controller.peer_infos[producer] = { .rank = 1, .key = key.get_public_key() };
      }
      return peer_keys.at(producer);
   }

   /// @return producer's entry for its server endpoint on host and outbound ip address, expiring at expiration and
   ///         signed with its peer key
   signed_bp_peer entry(sysio::name producer, const std::string& host, const std::string& outbound_ip_address,
                        sysio::block_timestamp_type expiration) {
      signed_bp_peer peer{{ .producer_name = producer }};
      peer.cached_bp_peer_info.emplace(host + ":" + std::string(port), outbound_ip_address, expiration);
      peer.bp_peer_info = fc::raw::pack(*peer.cached_bp_peer_info);
      peer.sig = peer_key(producer).sign(peer.digest(plugin.chain_id));
      return peer;
   }

   /// @return the signed initial entry of producer, which has no peer info
   signed_bp_peer initial_entry(sysio::name producer) {
      signed_bp_peer peer{{ .producer_name = producer }};
      peer.sig = peer_key(producer).sign(peer.digest(plugin.chain_id));
      return peer;
   }

   /// @return true when plugin accepts a message of peers as valid
   bool valid(std::vector<signed_bp_peer> peers) {
      sysio::gossip_bp_peers_message msg = received(std::move(peers));
      return plugin.validate_gossip_bp_peers_message(msg);
   }

   /// Validate a message of peers and merge what validation keeps, as handle_message does with a message that is not
   /// an initial one
   /// @return whether the merge changed the held entries, or nothing when the message is invalid
   std::optional<bool> deliver(std::vector<signed_bp_peer> peers) {
      sysio::gossip_bp_peers_message msg = received(std::move(peers));
      if (!plugin.validate_gossip_bp_peers_message(msg))
         return std::nullopt;
      if (msg.peers.empty())
         return false;
      return plugin.update_gossip_bps(msg);
   }
};

BOOST_AUTO_TEST_CASE(test_gossip_bp_resigned_entry_not_a_change) {
   gossip_fixture f;
   const signed_bp_peer held = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)));
   BOOST_CHECK(f.deliver({ held }) == true);
   const std::vector<held_entry> before = held_entries(f.plugin);

   // the entry signed again, as a signer drawing a random nonce does: another canonical signature, valid, no change
   signed_bp_peer resigned = held;
   resigned.sig = signed_with_other_nonce(f.peer_key("proda"_n), held.digest(f.plugin.chain_id));
   BOOST_REQUIRE(resigned.sig != held.sig);
   sysio::gossip_bp_peers_message msg = received({ resigned });
   BOOST_CHECK(f.plugin.validate_gossip_bp_peers_message(msg));
   BOOST_CHECK_EQUAL(msg.peers.size(), 1u);
   BOOST_CHECK(!f.plugin.update_gossip_bps(msg));
   BOOST_CHECK(held_entries(f.plugin) == before);

   // an entry of the same key expiring earlier changes nothing either
   BOOST_CHECK(f.deliver({ f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::minutes(30))) }) ==
               false);
   BOOST_CHECK(held_entries(f.plugin) == before);

   // one expiring later replaces the held entry
   const signed_bp_peer later =
      f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)).next());
   BOOST_CHECK(f.deliver({ later }) == true);
   const std::vector<held_entry> after = held_entries(f.plugin);
   BOOST_REQUIRE_EQUAL(after.size(), 1u);
   BOOST_CHECK_EQUAL(std::get<3>(after[0]), later.cached_bp_peer_info->expiration.slot);
   BOOST_CHECK_EQUAL(std::get<4>(after[0]), later.sig.to_string({}));
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_non_canonical_signature_rejected) {
   gossip_fixture f;
   const signed_bp_peer peer = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)));
   const fc::crypto::public_key key = f.peer_key("proda"_n).get_public_key();
   BOOST_CHECK(f.valid({ peer }));

   // each other form of the signature recovers the producer's key, and is refused
   for (const fc::crypto::signature& form : { high_s_form(peer.sig), uncompressed_header_form(peer.sig) }) {
      BOOST_REQUIRE(fc::crypto::public_key::recover(form, peer.digest(f.plugin.chain_id)) == key);
      signed_bp_peer forged = peer;
      forged.sig = form;
      BOOST_CHECK(!f.valid({ forged }));
   }

   // the initial message too
   const signed_bp_peer initial = f.initial_entry("proda"_n);
   BOOST_CHECK(f.valid({ initial }));
   signed_bp_peer forged_initial = initial;
   forged_initial.sig = high_s_form(initial.sig);
   BOOST_CHECK(!f.valid({ forged_initial }));

   // a signature that is not K1 is refused, though it is by the producer's on-chain peer key
   f.peer_key("prodb"_n, key_type::r1);
   BOOST_CHECK(!f.valid({ f.entry("prodb"_n, "p2p.prodb.example.com", "198.51.100.2", f.after(fc::hours(1))) }));
   BOOST_CHECK(held_entries(f.plugin).empty());
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_held_signature_not_reused) {
   gossip_fixture f;
   const signed_bp_peer a = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)));
   const signed_bp_peer b = f.entry("prodb"_n, "p2p.prodb.example.com", "198.51.100.2", f.after(fc::hours(1)));
   BOOST_CHECK(f.deliver({ a, b }) == true);
   const std::vector<held_entry> before = held_entries(f.plugin);

   // later entries of proda and of prodb, each carrying the signature of proda's held entry
   signed_bp_peer forged_a = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)).next());
   signed_bp_peer forged_b = f.entry("prodb"_n, "p2p.prodb.example.com", "198.51.100.2", f.after(fc::hours(1)).next());
   forged_a.sig = a.sig;
   forged_b.sig = a.sig;
   for (const signed_bp_peer& forged : { forged_a, forged_b }) {
      sysio::gossip_bp_peers_message msg = received({ forged });
      BOOST_CHECK(f.plugin.validate_gossip_bp_peers_message(msg));
      BOOST_CHECK(msg.peers.empty()); // the signature recovers another key than the producer's
      BOOST_CHECK(!f.plugin.update_gossip_bps(msg));
      BOOST_CHECK(held_entries(f.plugin) == before);
   }
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_change_only_when_stored) {
   const size_t max_per_producer = mock_net_plugin::max_bp_gossip_peers_per_producer;
   gossip_fixture f;
   const signed_bp_peer a = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)));
   BOOST_CHECK(f.deliver({ a }) == true);

   // a merge reports a change exactly when the held entries change, even for an entry under a new key that carries a
   // held signature, which validation removes
   signed_bp_peer copied_sig = f.entry("prodb"_n, "p2p.prodb.example.com", "198.51.100.2", f.after(fc::hours(1)));
   copied_sig.sig = a.sig;
   const std::vector<held_entry> before = held_entries(f.plugin);
   const bool changed = f.plugin.update_gossip_bps(sysio::gossip_bp_peers_message{{ copied_sig }});
   BOOST_CHECK_EQUAL(changed, held_entries(f.plugin) != before);

   // a producer holding its most entries: a new key expiring no later than every held one is not stored
   gossip_fixture full;
   std::vector<signed_bp_peer> entries;
   for (size_t n = 0; n < max_per_producer; ++n)
      entries.push_back(full.entry("proda"_n, "p2p" + std::to_string(n) + ".proda.example.com", "198.51.100.1",
                                   full.after(fc::hours(1))));
   BOOST_CHECK(full.deliver(entries) == true);
   const std::vector<held_entry> held = held_entries(full.plugin);
   const std::string extra_host = "p2p" + std::to_string(max_per_producer) + ".proda.example.com";
   BOOST_CHECK(full.deliver({ full.entry("proda"_n, extra_host, "198.51.100.1", full.after(fc::hours(1))) }) == false);
   BOOST_CHECK(held_entries(full.plugin) == held);

   // one expiring later takes the place of an earliest-expiring entry
   BOOST_CHECK(full.deliver({ full.entry("proda"_n, extra_host, "198.51.100.1", full.after(fc::hours(1)).next()) }) ==
               true);
   const std::vector<held_entry> replaced = held_entries(full.plugin);
   BOOST_CHECK_EQUAL(replaced.size(), max_per_producer);
   BOOST_CHECK(std::ranges::any_of(replaced, [&extra_host](const held_entry& e) {
      return std::get<1>(e).starts_with(extra_host);
   }));
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_expired_entries_rejected_and_evicted) {
   const fc::microseconds latest_expiration = mock_net_plugin::bp_gossip_peer_expiration_variance;
   gossip_fixture f;

   // validation removes the entries outside their expiration window: expired, or expiring too late
   sysio::gossip_bp_peers_message msg = received({
      f.entry("proda"_n, "p2p0.proda.example.com", "198.51.100.1", sysio::block_timestamp_type(f.head.slot - 1)),
      f.entry("proda"_n, "p2p1.proda.example.com", "198.51.100.1", f.head),
      f.entry("proda"_n, "p2p2.proda.example.com", "198.51.100.1", f.after(fc::hours(1))),
      f.entry("proda"_n, "p2p3.proda.example.com", "198.51.100.1", f.after(latest_expiration)),
   });
   BOOST_CHECK(f.plugin.validate_gossip_bp_peers_message(msg));
   BOOST_REQUIRE_EQUAL(msg.peers.size(), 1u);
   BOOST_CHECK_EQUAL(msg.peers[0].server_endpoint(), "p2p2.proda.example.com:" + std::string(gossip_fixture::port));

   // a held entry is checked again when it arrives after it has expired
   const signed_bp_peer expiring =
      f.entry("prodb"_n, "p2p.prodb.example.com", "198.51.100.2", f.after(fc::minutes(10)));
   BOOST_CHECK(f.deliver({ expiring }) == true);
   f.set_head(f.after(fc::minutes(10)));
   msg = received({ expiring });
   BOOST_CHECK(f.plugin.validate_gossip_bp_peers_message(msg));
   BOOST_CHECK(msg.peers.empty());

   // a merge evicts the held entries that have expired
   BOOST_CHECK(f.deliver({ f.entry("prodc"_n, "p2p.prodc.example.com", "198.51.100.3", f.after(fc::hours(1))) }) ==
               true);
   const std::vector<held_entry> held = held_entries(f.plugin);
   BOOST_REQUIRE_EQUAL(held.size(), 1u);
   BOOST_CHECK_EQUAL(std::get<0>(held[0]), "prodc");

   // and so does the expire timer
   f.set_head(f.after(fc::hours(1)));
   f.plugin.expire_gossip_bp_peers();
   BOOST_CHECK(held_entries(f.plugin).empty());
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_message_order) {
   const size_t max_per_producer = mock_net_plugin::max_bp_gossip_peers_per_producer;
   gossip_fixture f;
   const sysio::block_timestamp_type expiration = f.after(fc::hours(1));

   // one server endpoint with two outbound ip addresses, as p2p-bp-gossip-endpoint allows, in key order
   const signed_bp_peer first  = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", expiration);
   const signed_bp_peer second = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.2", expiration);
   BOOST_CHECK(f.valid({ first, second }));
   BOOST_CHECK(!f.valid({ second, first }));
   BOOST_CHECK(!f.valid({ first, first }));

   // at most max_bp_gossip_peers_per_producer entries of a producer
   std::vector<signed_bp_peer> entries;
   for (size_t n = 0; n < max_per_producer; ++n)
      entries.push_back(
         f.entry("prodb"_n, "p2p" + std::to_string(n) + ".prodb.example.com", "198.51.100.2", expiration));
   BOOST_CHECK(f.valid(entries));
   entries.push_back(
      f.entry("prodb"_n, "p2p" + std::to_string(max_per_producer) + ".prodb.example.com", "198.51.100.2", expiration));
   BOOST_CHECK(!f.valid(entries));
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_store_collision_keeps_entries) {
   gossip_fixture f;
   const signed_bp_peer a = f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)));
   const signed_bp_peer b = f.entry("prodb"_n, "p2p.prodb.example.com", "198.51.100.2", f.after(fc::hours(1)));
   BOOST_CHECK(f.deliver({ a, b }) == true);
   const std::vector<held_entry> before = held_entries(f.plugin);

   const signed_bp_peer later_a =
      f.entry("proda"_n, "p2p.proda.example.com", "198.51.100.1", f.after(fc::hours(1)).next());
   {
      fc::lock_guard g(f.plugin.gossip_bps.mtx);
      auto& idx = f.plugin.gossip_bps.index.get<sysio::by_producer>();
      // an insert under a key already held
      BOOST_CHECK(!f.plugin.store_gossip_bp(idx.end(), later_a));
      // a replacement of prodb's entry by one under proda's key
      BOOST_CHECK(!f.plugin.store_gossip_bp(idx.find(sysio::gossip_bp_key(b)), later_a));
   }
   BOOST_CHECK(held_entries(f.plugin) == before);
}

BOOST_AUTO_TEST_CASE(test_gossip_bp_rate_limit) {
   using clock_type = sysio::message_rate_limiter::clock;
   const uint32_t              burst    = mock_net_plugin::bp_gossip_msg_burst;
   const clock_type::duration  interval = mock_net_plugin::bp_gossip_msg_refill_interval;
   const clock_type::time_point now     = clock_type::time_point{} + std::chrono::hours(1);
   sysio::message_rate_limiter limiter(burst, interval);

   for (uint32_t n = 0; n < burst; ++n)
      BOOST_REQUIRE(limiter.try_consume(now));
   BOOST_CHECK(!limiter.try_consume(now));
   BOOST_CHECK(limiter.try_consume(now + interval));
   BOOST_CHECK(!limiter.try_consume(now + interval));
}

struct bp_peer_info_v2 : sysio::gossip_bp_peers_message::bp_peer_info_v1 {
   std::string extra;
};

FC_REFLECT_DERIVED(bp_peer_info_v2, (sysio::gossip_bp_peers_message::bp_peer_info_v1), (extra))

BOOST_AUTO_TEST_CASE(test_bp_peer_info_v2) {

   const sysio::chain_id_type chain_id = sysio::chain_id_type::empty_chain_id();
   fc::crypto::private_key pk = fc::crypto::private_key::generate();
   auto public_key = pk.get_public_key();

   bp_peer_info_v2 v2{{"hostname.com", "127.0.0.1", sysio::block_timestamp_type{7}}, "extra"};

   std::vector<char> packed_msg;
   {
      sysio::gossip_bp_peers_message msg;
      sysio::gossip_bp_peers_message::signed_bp_peer peer{{.version = 2, .producer_name = sysio::name("producer")}};
      peer.bp_peer_info = fc::raw::pack(v2);
      peer.sig = pk.sign(peer.digest(chain_id));
      msg.peers.emplace_back(peer);
      packed_msg = fc::raw::pack(msg);
   }

   auto msg = fc::raw::unpack<sysio::gossip_bp_peers_message>(packed_msg);

   auto& peer = msg.peers[0];

   // verify v1 can process data
   fc::crypto::public_key v1k = fc::crypto::public_key::recover(peer.sig, peer.digest(chain_id));
   BOOST_TEST(v1k == public_key);
   BOOST_TEST(peer.version.value == 2u);
   BOOST_TEST(peer.producer_name == sysio::name("producer"));

   // verify can unpack v1
   sysio::gossip_bp_peers_message::bp_peer_info_v1 v1 = fc::raw::unpack<sysio::gossip_bp_peers_message::bp_peer_info_v1>(peer.bp_peer_info);
   BOOST_TEST(v1.server_endpoint == "hostname.com");
   BOOST_TEST(v1.outbound_ip_address == "127.0.0.1");
   BOOST_TEST(v1.expiration == sysio::block_timestamp_type{7});

   // verify v2 can process data
   fc::crypto::public_key v2k = fc::crypto::public_key::recover(peer.sig, peer.digest(chain_id));
   BOOST_TEST(v2k == public_key);

   bp_peer_info_v2 v2b = fc::raw::unpack<bp_peer_info_v2>(peer.bp_peer_info);
   BOOST_TEST(v2b.server_endpoint == "hostname.com");
   BOOST_TEST(v2b.outbound_ip_address == "127.0.0.1");
   BOOST_TEST(v2b.expiration == sysio::block_timestamp_type{7});
   BOOST_TEST(v2b.expiration == sysio::block_timestamp_type{7});
   BOOST_TEST(v2b.extra == "extra");
}
