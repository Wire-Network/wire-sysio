#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/auto_bp_peering.hpp>

using namespace sysio::net_utils;

struct mock_connection {
   enum class bp_connection_type { non_bp, bp_config, bp_gossip, bp_gossip_validating };
   bp_connection_type bp_connection = bp_connection_type::non_bp;

   bool is_open            = false;
   bool handshake_received = false;
   bool closing            = false;
   int32_t peer_score      = sysio::peer_scoring::baseline;
   uint32_t connection_id  = 0;
   boost::asio::ip::address_v6 address;
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

struct mock_net_plugin : sysio::auto_bp_peering::bp_connection_manager<mock_net_plugin, mock_connection> {

   bool                         lib_catchup = true;
   mock_connections_manager     connections;
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

   BOOST_CHECK_EQUAL(plugin.num_established_clients(), 2u);

   BOOST_CHECK(!plugin.exceeding_connection_limit(plugin.connections.connections[0]));
   BOOST_CHECK(!plugin.exceeding_connection_limit(plugin.connections.connections[1]));
   BOOST_CHECK(!plugin.exceeding_connection_limit(plugin.connections.connections[2]));
   BOOST_CHECK(!plugin.exceeding_connection_limit(plugin.connections.connections[3]));
   BOOST_CHECK(plugin.exceeding_connection_limit(plugin.connections.connections[4]));
   BOOST_CHECK(!plugin.exceeding_connection_limit(plugin.connections.connections[5]));
   BOOST_CHECK(plugin.exceeding_connection_limit(plugin.connections.connections[6]));
   BOOST_CHECK(!plugin.exceeding_connection_limit(plugin.connections.connections[7]));
}

/// An open inbound client connection from address that has sent its first handshake.
std::shared_ptr<mock_connection> inbound_from(const std::string& address) {
   auto conn = std::make_shared<mock_connection>(false, true, true);
   conn->address = sysio::net_utils::parse_mapped_v6(address).value();
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
   plugin.set_configured_bp_peers({ "proda,192.0.2.1:9876"s, "prodb,[2001:db8::2]:9876:blk"s,
                                    "prodc,p2p.example.com:9876"s, "prodd,192.0.2.1:9877"s }, {});

   // host names are not addresses, and two endpoints on one host give one address
   BOOST_CHECK_EQUAL(plugin.config.auto_bp_ip_addresses.size(), 2u);
   BOOST_CHECK(plugin.config.auto_bp_ip_addresses.contains(boost::asio::ip::make_address_v6("::ffff:192.0.2.1")));
   BOOST_CHECK(plugin.config.auto_bp_ip_addresses.contains(boost::asio::ip::make_address_v6("2001:db8::2")));
}

BOOST_AUTO_TEST_CASE(test_mark_inbound_configured_bp_by_address) {
   using bp_type = mock_connection::bp_connection_type;
   mock_net_plugin plugin;
   plugin.set_configured_bp_peers({ "proda,192.0.2.1:9876"s, "prodb,[2001:db8::2]:9876"s, "prodc,localhost:9876"s },
                                  {});

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

   // a connection already recognized is left as it is
   const auto gossip = inbound_from("192.0.2.1");
   gossip->bp_connection = bp_type::bp_gossip;
   plugin.mark_inbound_bp_connection(gossip.get());
   BOOST_CHECK(gossip->bp_connection == bp_type::bp_gossip);
}

BOOST_AUTO_TEST_CASE(test_mark_inbound_gossip_bp_by_address) {
   using bp_type = mock_connection::bp_connection_type;
   const auto add_gossip_peers = [](mock_net_plugin& plugin) {
      fc::lock_guard g(plugin.gossip_bps.mtx);
      plugin.gossip_bps.index.insert(gossip_peer("proda"_n, "p2p.proda.example.com:9876", "198.51.100.1"));
      plugin.gossip_bps.index.insert(gossip_peer("prodb"_n, "p2p.prodb.example.com:9876", "2001:db8::b"));
      plugin.gossip_bps.index.insert(gossip_peer("prodc"_n, "p2p.prodc.example.com:9876", "not-an-ip-address"));
   };

   mock_net_plugin gossiping;
   gossiping.set_bp_producer_peers({ "prodt,127.0.0.1:8020,127.0.0.1"s });
   add_gossip_peers(gossiping);
   BOOST_CHECK(marked_type(gossiping, "198.51.100.1") == bp_type::bp_gossip_validating);
   BOOST_CHECK(marked_type(gossiping, "::ffff:198.51.100.1") == bp_type::bp_gossip_validating);
   BOOST_CHECK(marked_type(gossiping, "2001:db8::b") == bp_type::bp_gossip_validating);
   BOOST_CHECK(marked_type(gossiping, "198.51.100.2") == bp_type::non_bp);
   BOOST_CHECK(marked_type(gossiping, "2001:db8::c") == bp_type::non_bp);

   // without bp gossip of its own, a node does not trust gossiped addresses, even with configured bp peers
   mock_net_plugin configured_only;
   configured_only.set_configured_bp_peers({ "proda,192.0.2.1:9876"s }, {});
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
   const auto client = [](int32_t score) {
      auto conn = std::make_shared<mock_connection>(false, true, true);
      conn->peer_score = score;
      return conn;
   };
   const auto poor         = client(eviction_threshold - 1);
   const auto poorest      = client(eviction_threshold - 40);
   const auto bp           = std::make_shared<mock_connection>(true, true, true);
   const auto no_handshake = std::make_shared<mock_connection>(false, true, false);
   const auto closed       = std::make_shared<mock_connection>(false, false, true);
   const auto closing      = client(0);
   const auto newcomer     = client(0);
   bp->peer_score = no_handshake->peer_score = closed->peer_score = 0;
   closing->closing = true;
   plugin.connections.connections = { client(sysio::peer_scoring::baseline), poor, bp, no_handshake, closed, closing,
                                      poorest, newcomer };

   // never the newcomer, a bp, a connection that is not an established client, or one already closing
   BOOST_CHECK(plugin.select_client_to_evict(newcomer) == poorest);

   // a client back at the threshold keeps its slot
   poorest->peer_score = eviction_threshold;
   BOOST_CHECK(plugin.select_client_to_evict(newcomer) == poor);
   poor->peer_score = eviction_threshold;
   BOOST_CHECK(!plugin.select_client_to_evict(newcomer));
}

BOOST_AUTO_TEST_CASE(test_configured_bp_address_never_counted_or_evicted) {
   mock_net_plugin plugin;
   plugin.set_configured_bp_peers({ "proda,192.0.2.1:9876"s }, {});
   plugin.connections.max_client_count = 1;

   const auto client = inbound_from("192.0.2.9");
   const auto bp     = inbound_from("192.0.2.1"); // its handshake p2p_address need not name the configured endpoint
   bp->peer_score = sysio::peer_scoring::min_score;
   plugin.connections.connections = { client, bp };
   plugin.mark_inbound_bp_connection(bp.get());
   BOOST_CHECK(!plugin.exceeding_connection_limit(bp));
   BOOST_CHECK_EQUAL(plugin.num_established_clients(), 1u);

   // a further client exceeds the limit, and the misbehaving bp is still not evicted for it
   const auto newcomer = inbound_from("192.0.2.10");
   plugin.connections.connections.push_back(newcomer);
   BOOST_CHECK(plugin.exceeding_connection_limit(newcomer));
   BOOST_CHECK(!plugin.select_client_to_evict(newcomer));
}

BOOST_AUTO_TEST_CASE(test_closing_clients_not_counted) {
   mock_net_plugin plugin;
   const auto closing = inbound_from("192.0.2.10");
   closing->closing = true; // evicted or refused, its socket not yet closed
   plugin.connections.connections = { inbound_from("192.0.2.9"), closing };
   BOOST_CHECK_EQUAL(plugin.num_established_clients(), 1u);
}

BOOST_AUTO_TEST_CASE(test_admit_client) {
   mock_net_plugin plugin;
   plugin.setup_test_peers(); // auto bp peering, which enforces max_client_count at the handshake
   plugin.connections.max_client_count = 3;
   const auto keeper = inbound_from("192.0.2.9");

   // within the limit
   const auto client = inbound_from("192.0.2.10");
   plugin.connections.connections = { keeper, client };
   BOOST_CHECK(plugin.admit_client(client));
   BOOST_CHECK(!keeper->closing && !client->closing);

   // two newcomers handshake together one over the limit: the first takes the misbehaving client's slot, and the
   // second then fits, as the evicted client stops counting at once
   const auto victim = inbound_from("192.0.2.11");
   victim->peer_score = sysio::peer_scoring::eviction_threshold - 1;
   const auto first  = inbound_from("192.0.2.12");
   const auto second = inbound_from("192.0.2.13");
   plugin.connections.connections = { keeper, victim, first, second };
   BOOST_CHECK(plugin.admit_client(first));
   BOOST_CHECK(victim->closing);
   BOOST_CHECK(plugin.admit_client(second));
   BOOST_CHECK(!keeper->closing && !first->closing && !second->closing);

   // the limit full of clients in good standing: a newcomer is refused
   const auto refused = inbound_from("192.0.2.14");
   plugin.connections.connections = { keeper, first, second, refused };
   BOOST_CHECK(!plugin.admit_client(refused));
   BOOST_CHECK(refused->closing);

   // one slot free and three newcomers handshake together: refused newcomers stop counting at once, so the last fits
   const std::vector<std::shared_ptr<mock_connection>> burst{ inbound_from("192.0.2.15"), inbound_from("192.0.2.16"),
                                                              inbound_from("192.0.2.17") };
   plugin.connections.connections = { keeper, first };
   plugin.connections.connections.insert(plugin.connections.connections.end(), burst.begin(), burst.end());
   BOOST_CHECK(!plugin.admit_client(burst[0]));
   BOOST_CHECK(!plugin.admit_client(burst[1]));
   BOOST_CHECK(plugin.admit_client(burst[2]));
   BOOST_CHECK_EQUAL(plugin.num_established_clients(), 3u);

   // without auto bp peering, create_session() enforces the limit and the handshake admits every client
   mock_net_plugin disabled;
   disabled.connections.max_client_count = 1;
   const auto over = inbound_from("192.0.2.18");
   disabled.connections.connections = { inbound_from("192.0.2.19"), over };
   BOOST_CHECK(disabled.admit_client(over));
   BOOST_CHECK(!over->closing);
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
