#include <trx_provider.hpp>
#include <trx_generator.hpp>
#include <http_client_async.hpp>
#include <simple_rest_server.hpp>
#include <test_port_shard.hpp>
#include <sysio/net_plugin/protocol.hpp>

#define BOOST_TEST_MODULE trx_generator_tests
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstdlib>
#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace sysio;
using namespace sysio::testing;
using namespace std::literals::string_literals;

static const char* api_name = "/v1/chain/test";

namespace {
/** Restore SYSIO_TEST_PORT_OFFSET after tests that exercise environment parsing. */
class test_port_offset_env_guard {
 public:
   test_port_offset_env_guard() {
      if(const char* value = std::getenv(test_port_offset_env_var))
         _previous = value;
   }

   ~test_port_offset_env_guard() {
      if(_previous)
         setenv(test_port_offset_env_var, _previous->c_str(), 1);
      else
         unsetenv(test_port_offset_env_var);
   }

   /** Set the current process test port offset for this scoped test. */
   void set(const char* value) const { setenv(test_port_offset_env_var, value, 1); }

   /** Clear the current process test port offset for this scoped test. */
   void clear() const { unsetenv(test_port_offset_env_var); }

 private:
   std::optional<std::string> _previous;
};

struct expected_port {
   port_category category;
   uint32_t      slot;
   uint32_t      count;
   uint16_t      unsharded_base;
};

constexpr std::array expected_ports{
    expected_port{port_category::ship, compact_ship_slot, 1, 7899},
    expected_port{port_category::state_history, compact_state_history_slot, 1, default_state_history_port},
    expected_port{port_category::bios_http, compact_bios_http_slot, 1, 8788},
    expected_port{port_category::bios_p2p, compact_bios_p2p_slot, 1, compact_bios_p2p_port},
    expected_port{port_category::node_http, compact_http_slot, compact_http_last_port - compact_http_first_port + 1,
                  compact_http_first_port},
    expected_port{port_category::alternate_service, compact_alternate_service_slot, 1, 8976},
    expected_port{port_category::plugin_http_peer, compact_plugin_http_peer_slot, 1, 9009},
    expected_port{port_category::plugin_http_local, compact_plugin_http_local_slot, 1, 9011},
    expected_port{port_category::alternate_p2p, compact_alternate_p2p_slot,
                  compact_alternate_last_port - compact_alternate_first_port + 1, compact_alternate_first_port},
    expected_port{port_category::p2p, compact_p2p_slot, compact_p2p_last_port - compact_p2p_first_port + 1,
                  compact_p2p_first_port},
    expected_port{port_category::wallet, compact_wallet_base_slot, wallet_port_count, compact_wallet_first_port},
    expected_port{port_category::transaction_only, compact_transaction_only_slot, transaction_only_port_count,
                  compact_transaction_only_first_port},
    expected_port{port_category::ipv6_probe, compact_ipv6_probe_slot,
                  compact_ipv6_probe_last_port - compact_ipv6_probe_first_port + 1, compact_ipv6_probe_first_port}};

constexpr int16_t expected_trx_generator_handshake_generation = 1;
constexpr int16_t expected_trx_generator_response_handshake_generation = 2;
constexpr std::string_view expected_trx_generator_chain_id = "999";
constexpr std::string_view expected_trx_generator_p2p_address = "127.0.0.1:0:trx";
constexpr std::string_view expected_trx_generator_lib_id =
      "00000062989f69fd251df3e0b274c3364ffc2f4fce73de3f1c7b5e11a4c92f21";
constexpr std::string_view expected_trx_generator_peer_lib_id =
      "00000063989f69fd251df3e0b274c3364ffc2f4fce73de3f1c7b5e11a4c92f21";
constexpr std::string_view expected_trx_generator_peer_head_id =
      "00000064989f69fd251df3e0b274c3364ffc2f4fce73de3f1c7b5e11a4c92f21";
constexpr std::string_view expected_trx_generator_peer_p2p_address = "127.0.0.1:0";
constexpr std::string_view expected_trx_generator_peer_agent = "nodeop";
constexpr std::string_view expected_trx_generator_peer_node_id_seed = "trx_generator_test_peer";
constexpr int64_t expected_trx_generator_peer_time_xmt = 42;

struct p2p_provider_exchange {
   net_message initial_handshake;
   net_message handshake_response;
   net_message time_response;
};

/** Reads one framed net_message from a test P2P socket. */
static net_message read_test_net_message(boost::asio::ip::tcp::socket& socket) {
   uint32_t payload_size = 0;
   boost::asio::read(socket, boost::asio::buffer(&payload_size, sizeof(payload_size)));

   std::vector<char> payload(payload_size);
   boost::asio::read(socket, boost::asio::buffer(payload));

   fc::datastream<const char*> ds(payload.data(), payload.size());
   net_message msg;
   fc::raw::unpack(ds, msg);
   if (ds.remaining() != 0) {
      throw std::runtime_error("framed net_message has trailing bytes");
   }
   return msg;
}

/** Writes one framed net_message to a test P2P socket. */
template<typename Message>
static void write_test_net_message(boost::asio::ip::tcp::socket& socket, const Message& message) {
   const net_message net_msg{message};
   const uint32_t payload_size = fc::raw::pack_size(net_msg);
   const size_t buffer_size = sizeof(payload_size) + payload_size;
   std::vector<char> buffer(buffer_size);
   fc::datastream<char*> ds(buffer.data(), buffer.size());
   ds.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
   fc::raw::pack(ds, net_msg);
   boost::asio::write(socket, boost::asio::buffer(buffer));
}

/** Builds a peer heartbeat handshake carrying fresh chain head state. */
static handshake_message make_test_peer_handshake(const chain::chain_id_type& chain_id,
                                                  const chain::block_id_type& fork_db_root_id,
                                                  const chain::block_id_type& fork_db_head_id) {
   handshake_message handshake;
   handshake.network_version = wire_protocol_base_version;
   handshake.chain_id = chain_id;
   handshake.node_id = fc::sha256::hash(std::string(expected_trx_generator_peer_node_id_seed));
   handshake.fork_db_root_id = fork_db_root_id;
   handshake.fork_db_head_id = fork_db_head_id;
   handshake.chain_head_id = fork_db_head_id;
   handshake.generation = expected_trx_generator_handshake_generation;
   handshake.p2p_address = std::string(expected_trx_generator_peer_p2p_address);
   handshake.agent = std::string(expected_trx_generator_peer_agent);
   return handshake;
}

/** Builds a minimal signed transaction that can be packed for provider transport tests. */
static chain::signed_transaction make_test_signed_transaction() {
   chain::signed_transaction trx;
   trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(30));
   trx.actions.emplace_back(std::vector<chain::permission_level>{}, chain::name("sysio"), chain::name("noop"),
                            chain::bytes{});
   return trx;
}

} // namespace

namespace http = boost::beast::http;
struct echo_server_impl : rest::simple_server<echo_server_impl> {

   std::string server_header() const { return "/"; }

   void log_error(char const* what, const std::string& message) {
      elog("{}: {}", what, message);
   }

   bool allow_method(http::verb method) const { return method == http::verb::post; }

   std::optional<http::response<http::string_body>> on_request(http::request<http::string_body>&& req) {
      if (req.target() != api_name)
         return {};
      http::response<http::string_body> res{http::status::ok, req.version()};
      // Respond to POST request
      res.set(http::field::server, server_header());
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      // echo request body back in response body
      res.body() = req.body();
      res.prepare_payload();
      return res;
   }

   sysio::chain::named_thread_pool<struct trxgen> _trx_gen_server_thread_pool;
   boost::asio::io_context::strand                _trx_gen_server_strand;

   echo_server_impl()
       : _trx_gen_server_strand(_trx_gen_server_thread_pool.get_executor()) {}

   void start(boost::asio::ip::tcp::endpoint endpoint) {
      run(_trx_gen_server_thread_pool.get_executor(), endpoint);
      _trx_gen_server_thread_pool.start(
          1, [](const fc::exception& e) { elog("Trx gen http server exception {}", e.to_detail_string()); });
   }

   void shutdown() {
      _trx_gen_server_thread_pool.stop();
      ilog("echo_server_impl shutdown.");
   }
};

struct simple_tps_monitor {
   std::vector<tps_test_stats> _calls;
   bool monitor_test(const tps_test_stats& stats) {
      _calls.push_back(stats);
      return true;
   }

   simple_tps_monitor(size_t expected_num_calls) { _calls.reserve(expected_num_calls); }
};

struct mock_trx_generator {
   std::vector<fc::time_point> _calls;
   std::chrono::microseconds _delay;

   bool setup() {return true;}
   bool tear_down() {return true;}

   bool generate_and_send() {
      _calls.push_back(fc::time_point::now());
      if (_delay.count() > 0) {
         std::this_thread::sleep_for(_delay);
      }
      return true;
   }

   bool stop_on_trx_fail() {
      return false;
   }

   mock_trx_generator(size_t expected_num_calls, uint32_t delay=0) :_calls(), _delay(delay) {
      _calls.reserve(expected_num_calls);
   }
};

BOOST_AUTO_TEST_SUITE(trx_generator_tests)

BOOST_AUTO_TEST_CASE(tps_short_run_low_tps)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 5;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);
   BOOST_REQUIRE_LT(runtime_us.count(), maximum_runtime_us);
}

BOOST_AUTO_TEST_CASE(tps_short_run_high_tps)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 50000;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran {}us vs expected max {}us",
           runtime_us.count(), maximum_runtime_us);
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_short_run_med_tps_med_delay)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 10000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran {}us vs expected max {}us",
           runtime_us.count(), maximum_runtime_us);
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_med_run_med_tps_med_delay)
{
   constexpr uint32_t test_duration_s = 30;
   constexpr uint32_t test_tps = 10000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran {}us vs expected max {}us",
           runtime_us.count(), maximum_runtime_us);
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_cant_keep_up)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 100000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran {}us vs expected max {}us",
           runtime_us.count(), maximum_runtime_us);
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_med_run_med_tps_30us_delay)
{
   constexpr uint32_t test_duration_s = 15;
   constexpr uint32_t test_tps = 3000;
   constexpr uint32_t trx_delay_us = 30;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran {}us vs expected max {}us",
           runtime_us.count(), maximum_runtime_us);
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_performance_monitor_during_spin_up)
{
   tps_test_stats stats;
   tps_performance_monitor monitor{std::chrono::microseconds(5s).count()};
   stats.total_trxs = 1000;
   stats.start_time = fc::time_point{fc::microseconds{0}};
   stats.expected_sent = 100;
   stats.trxs_sent = 90;

   // behind, but still within spin up window
   stats.last_run = fc::time_point{fc::microseconds{100000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // violation, but still within spin up window
   stats.last_run = fc::time_point{fc::microseconds{1100000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));
}

BOOST_AUTO_TEST_CASE(tps_performance_monitor_outside_spin_up)
{
   tps_test_stats stats;
   tps_performance_monitor monitor{std::chrono::microseconds(5s).count()};
   stats.total_trxs = 1000;
   stats.start_time = fc::time_point{fc::microseconds{0}};
   stats.expected_sent = 100;
   stats.trxs_sent = 90;

   // behind, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{5500000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // violation, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{6600000}};
   BOOST_REQUIRE(!monitor.monitor_test(stats));
}

BOOST_AUTO_TEST_CASE(tps_performance_monitor_outside_spin_up_within_limit)
{
   tps_test_stats stats;
   tps_performance_monitor monitor{std::chrono::microseconds(5s).count()};
   stats.total_trxs = 1000;
   stats.start_time = fc::time_point{fc::microseconds{0}};
   stats.expected_sent = 100;
   stats.trxs_sent = 90;

   // outside of limit,  out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{5500000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // outside of limit, less than max violation duration
   stats.last_run = fc::time_point{fc::microseconds{6000000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   stats.trxs_sent = 98;
   // behind, but within limit, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{6600000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   stats.expected_sent = 150;
   // outside of limit again, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{7000000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // outside of limit for too long
   stats.last_run = fc::time_point{fc::microseconds{8100000}};
   BOOST_REQUIRE(!monitor.monitor_test(stats));
}

BOOST_AUTO_TEST_CASE(tps_cant_keep_up_monitored)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 100000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr int64_t expected_runtime_us = test_duration_s * 1000000;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<tps_performance_monitor> monitor = std::make_shared<tps_performance_monitor>();

   trx_tps_tester<mock_trx_generator, tps_performance_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_LT(runtime_us.count(), expected_runtime_us);
   BOOST_REQUIRE_LT(generator->_calls.size(), expected_trxs);
}

BOOST_AUTO_TEST_CASE(test_port_shard_unsharded_defaults)
{
   test_port_offset_env_guard env;
   env.clear();

   for(const auto& expected : expected_ports) {
      BOOST_REQUIRE_EQUAL(get_port(expected.category), expected.unsharded_base);
      BOOST_REQUIRE_EQUAL(get_port(expected.category, expected.count - 1),
                          expected.unsharded_base + expected.count - 1);
   }
}

BOOST_AUTO_TEST_CASE(test_port_shard_sharded_offsets)
{
   test_port_offset_env_guard env;
   env.set("100");

   constexpr uint32_t offset = 100;
   for(const auto& expected : expected_ports) {
      BOOST_REQUIRE_EQUAL(get_port(expected.category), compact_shard_anchor_port + offset + expected.slot);
      BOOST_REQUIRE_EQUAL(get_port(expected.category, expected.count - 1),
                          compact_shard_anchor_port + offset + expected.slot + expected.count - 1);
   }
}

BOOST_AUTO_TEST_CASE(test_port_shard_rejects_invalid_indices)
{
   test_port_offset_env_guard env;
   env.set("100");

   for(const auto& expected : expected_ports) {
      BOOST_REQUIRE_THROW(get_port(expected.category, expected.count), std::runtime_error);
   }
}

BOOST_AUTO_TEST_CASE(test_port_shard_rejects_invalid_offsets)
{
   test_port_offset_env_guard env;

   for(const char* value : {"-1", "+1", " 1", "1 ", "1_000"}) {
      env.set(value);
      BOOST_REQUIRE_THROW(test_port_offset(), std::runtime_error);
   }

   env.set("60000");
   BOOST_REQUIRE_THROW(get_port(port_category::ipv6_probe, 3), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(trx_generator_constructor)
{
   trx_generator_base_config tg_config{1, chain::chain_id_type("999"), chain::name("sysio"), fc::seconds(3600),
                                       fc::variant("00000062989f69fd251df3e0b274c3364ffc2f4fce73de3f1c7b5e11a4c92f21").as<chain::block_id_type>(), ".", true};
   provider_base_config p_config{"p2p", "127.0.0.1", get_port(port_category::p2p)};
   const std::string abi_file = "../../contracts/sysio.token/sysio.token.abi";
   const std::string actions_data = "[{\"actionAuthAcct\": \"testacct1\",\"actionName\": \"transfer\",\"authorization\": {\"actor\": \"testacct1\",\"permission\": \"active\"},"
                                    "\"actionData\": {\"from\": \"testacct1\",\"to\": \"testacct2\",\"quantity\": \"0.0001 CUR\",\"memo\": \"transaction specified\"}}]";
   const std::string action_auths = "{\"testacct1\":\"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3\",\"testacct2\":\"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3\","
                                    "\"sysio\":\"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3\"}";
   user_specified_trx_config trx_config{abi_file, actions_data, action_auths};

   auto generator = std::make_shared<trx_generator>(tg_config, p_config, trx_config);
}

/// Verifies the P2P provider sends a valid session handshake and answers peer heartbeat messages.
BOOST_AUTO_TEST_CASE(p2p_provider_sends_handshake_on_setup)
{
   boost::asio::io_context io;
   boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
   const uint16_t port = acceptor.local_endpoint().port();

   const chain::chain_id_type chain_id{std::string(expected_trx_generator_chain_id)};
   const auto lib_id = fc::variant(std::string(expected_trx_generator_lib_id)).as<chain::block_id_type>();
   const auto peer_lib_id = fc::variant(std::string(expected_trx_generator_peer_lib_id)).as<chain::block_id_type>();
   const auto peer_head_id = fc::variant(std::string(expected_trx_generator_peer_head_id)).as<chain::block_id_type>();

   std::promise<p2p_provider_exchange> exchange_promise;
   auto exchange_future = exchange_promise.get_future();
   std::thread server([&]() {
      boost::asio::ip::tcp::socket socket(io);
      acceptor.accept(socket);

      auto initial_handshake = read_test_net_message(socket);

      write_test_net_message(socket, make_test_peer_handshake(chain_id, peer_lib_id, peer_head_id));
      auto handshake_response = read_test_net_message(socket);

      time_message ping;
      ping.xmt = expected_trx_generator_peer_time_xmt;
      write_test_net_message(socket, ping);
      auto time_response = read_test_net_message(socket);

      exchange_promise.set_value(
            {std::move(initial_handshake), std::move(handshake_response), std::move(time_response)});
   });

   provider_base_config p_config{"p2p", "127.0.0.1", port};
   trx_provider provider{p_config};

   provider.setup(chain_id, lib_id);
   auto exchange = exchange_future.get();
   BOOST_CHECK(provider.reference_block_id(lib_id) == peer_lib_id);
   provider.teardown();
   server.join();

   const auto* handshake = std::get_if<handshake_message>(&exchange.initial_handshake);
   BOOST_REQUIRE(handshake != nullptr);
   BOOST_CHECK_EQUAL(handshake->network_version, wire_protocol_base_version);
   BOOST_CHECK(handshake->chain_id == chain_id);
   BOOST_CHECK(handshake->fork_db_root_id == lib_id);
   BOOST_CHECK(handshake->fork_db_head_id == lib_id);
   BOOST_CHECK(handshake->chain_head_id == lib_id);
   BOOST_CHECK_EQUAL(handshake->generation, expected_trx_generator_handshake_generation);
   BOOST_CHECK_EQUAL(handshake->p2p_address, std::string(expected_trx_generator_p2p_address));
   BOOST_CHECK_EQUAL(handshake->agent, std::string(trx_generator_agent));
   BOOST_CHECK(!handshake->node_id.empty());

   const auto* response_handshake = std::get_if<handshake_message>(&exchange.handshake_response);
   BOOST_REQUIRE(response_handshake != nullptr);
   BOOST_CHECK_EQUAL(response_handshake->network_version, wire_protocol_base_version);
   BOOST_CHECK(response_handshake->chain_id == chain_id);
   BOOST_CHECK(response_handshake->node_id == handshake->node_id);
   BOOST_CHECK(response_handshake->fork_db_root_id == peer_lib_id);
   BOOST_CHECK(response_handshake->fork_db_head_id == peer_head_id);
   BOOST_CHECK(response_handshake->chain_head_id == peer_head_id);
   BOOST_CHECK_EQUAL(response_handshake->generation, expected_trx_generator_response_handshake_generation);
   BOOST_CHECK_EQUAL(response_handshake->p2p_address, std::string(expected_trx_generator_p2p_address));
   BOOST_CHECK_EQUAL(response_handshake->agent, std::string(trx_generator_agent));

   const auto* time_response = std::get_if<time_message>(&exchange.time_response);
   BOOST_REQUIRE(time_response != nullptr);
   BOOST_CHECK_EQUAL(time_response->org, expected_trx_generator_peer_time_xmt);
   BOOST_CHECK_GT(time_response->rec, 0);
   BOOST_CHECK_GT(time_response->xmt, 0);
   BOOST_CHECK_EQUAL(time_response->dst, trx_generator_empty_time_message_timestamp);
}

/// Verifies a failed P2P transaction write is not counted as a successful acknowledgement.
BOOST_AUTO_TEST_CASE(p2p_provider_write_failure_does_not_ack_transaction)
{
   boost::asio::io_context io;
   boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
   const uint16_t port = acceptor.local_endpoint().port();

   const chain::chain_id_type chain_id{std::string(expected_trx_generator_chain_id)};
   const auto lib_id = fc::variant(std::string(expected_trx_generator_lib_id)).as<chain::block_id_type>();

   std::promise<void> handshake_received_promise;
   auto handshake_received = handshake_received_promise.get_future();
   std::thread server([&]() {
      try {
         boost::asio::ip::tcp::socket socket(io);
         acceptor.accept(socket);
         (void)read_test_net_message(socket);
         handshake_received_promise.set_value();
      } catch (...) {
         handshake_received_promise.set_exception(std::current_exception());
      }
   });

   provider_base_config p_config{"p2p", "127.0.0.1", port};
   p2p_connection connection{p_config};
   connection.init_and_connect(chain_id, lib_id);
   std::exception_ptr unexpected_server_exception;
   try {
      handshake_received.get();
   } catch (...) {
      unexpected_server_exception = std::current_exception();
   }

   if (unexpected_server_exception) {
      connection.cleanup_and_disconnect();
      server.join();
      std::rethrow_exception(unexpected_server_exception);
   }

   boost::asio::post(connection._strand, [&connection]() {
      boost::system::error_code ec;
      connection._p2p_socket.close(ec);
   });

   auto trx = make_test_signed_transaction();
   const auto trx_id = trx.id();
   const chain::packed_transaction packed_trx{trx};
   std::exception_ptr unexpected_send_exception;
   try {
      connection.send_transaction(packed_trx);
   } catch (...) {
      unexpected_send_exception = std::current_exception();
   }
   connection.cleanup_and_disconnect();
   server.join();

   if (unexpected_send_exception) {
      std::rethrow_exception(unexpected_send_exception);
   }

   BOOST_CHECK_EQUAL(connection._sent.load(), 1);
   BOOST_CHECK_EQUAL(connection._sent_callback_num.load(), 0);
   BOOST_CHECK_EQUAL(connection._errors.load(), 1);

   {
      std::lock_guard<std::mutex> lock(connection._trx_ack_map_lock);
      BOOST_CHECK(connection._trxs_ack_time_map.find(trx_id) == connection._trxs_ack_time_map.end());
   }
   BOOST_CHECK_THROW(connection.throw_if_unhealthy(), std::runtime_error);

   auto next_trx = make_test_signed_transaction();
   const chain::packed_transaction next_packed_trx{next_trx};
   BOOST_CHECK_THROW(connection.send_transaction(next_packed_trx), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(account_name_generator_tests)
{
   auto acct_gen = account_name_generator();
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "111111111111");

   //Test account name prefixes for differentiating between transaction generator instances
   acct_gen.setPrefix(1);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "121111111111");
   acct_gen.setPrefix(30);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "1z1111111111");
   acct_gen.setPrefix(31);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "211111111111");
   acct_gen.setPrefix(960);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "zz1111111111");

   //Test account name generation
   std::vector<std::string> expected = {
         "zz1111111111",
         "zz1111111112",
         "zz1111111113",
         "zz1111111114",
         "zz1111111115",
         "zz111111111a",
         "zz111111111b",
         "zz111111111c",
         "zz111111111d",
         "zz111111111e",
         "zz111111111f",
         "zz111111111g",
         "zz111111111h",
         "zz111111111i",
         "zz111111111j",
         "zz111111111k",
         "zz111111111l",
         "zz111111111m",
         "zz111111111n",
         "zz111111111o",
         "zz111111111p",
         "zz111111111q",
         "zz111111111r",
         "zz111111111s",
         "zz111111111t",
         "zz111111111u",
         "zz111111111v",
         "zz111111111w",
         "zz111111111x",
         "zz111111111y",
         "zz111111111z",
         "zz1111111121",
         "zz1111111122"};
   for(size_t i = 0; i < expected.size(); ++i) {
      BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), expected.at(i));
      acct_gen.increment();
   }


   //Test account name generation starting at 31 ^ 5 - 1 = 28629150
   std::vector<std::string> expected2 = {
         "1211111zzzzz",
         "121111211111",
         "121111211112",
         "121111211113",
         "121111211114",
         "121111211115",
         "12111121111a",
         "12111121111b",
         "12111121111c",
         "12111121111d",
         "12111121111e",
         "12111121111f",
         "12111121111g",
         "12111121111h",
         "12111121111i",
         "12111121111j",
         "12111121111k",
         "12111121111l",
         "12111121111m",
         "12111121111n",
         "12111121111o",
         "12111121111p",
         "12111121111q",
         "12111121111r",
         "12111121111s",
         "12111121111t",
         "12111121111u",
         "12111121111v",
         "12111121111w",
         "12111121111x",
         "12111121111y",
         "12111121111z",
         "121111211121",
         "121111211122"};
   auto acct_gen2 = account_name_generator();
   acct_gen2.setPrefix(1);
   int initialVal = 28629150;
   for(int i = 0; i < initialVal; ++i) {
      acct_gen2.increment();
   }
   for(size_t i = 0; i < expected2.size(); ++i) {
      BOOST_REQUIRE_EQUAL(acct_gen2.calc_name(), expected2.at(i));
      acct_gen2.increment();
   }
}

BOOST_AUTO_TEST_CASE(simple_http_client_async_test) {

   const std::string host     = "127.0.0.1"s;
   const unsigned short port = get_port(port_category::node_http);

   // Start Server
   echo_server_impl               server = echo_server_impl();
   auto                           addr   = boost::asio::ip::make_address(host);
   boost::asio::ip::tcp::endpoint endpoint(addr, port);

   server.start(endpoint);

   // Start Client

   // The io_context is required for all I/O
   boost::asio::io_context ioc;
   const std::string       target        = "/v1/chain/test"s;
   const int               version       = 11;
   const std::string       content_type  = "text/plain"s;
   const std::string       content_type2 = "application/json"s;

   http_client_async::http_request_params params{ioc, host, port, target, version, content_type};

   std::string test_body = "test request body"s;
   std::string test_body_copy = test_body;
   std::string test_body2 =
       "{\"return_failure_trace\":true,\"retry_trx\":false,\"transaction\":{\"signatures\":[\"SIG_K1_"
       "JyzLqbvpdybyujtiN1YdY2FWcBBi8dWWiFgZ515qyyqgKJJ6892i4rXTHdw5KGYut6EBuXPR3ExRwPSioSZ2bZ1RjNUXVj\"],"
       "\"compression\":\"none\",\"packed_context_free_data\":\"\",\"packed_trx\":"
       "\"848a34641800f994a24e00000000030000000000ea305500409e9a2264b89a0160ae423ad15b974a00000000a8ed32326660ae423ad15"
       "b974a1042088a4dd35057010000000100038d26b3d5ce8c7d76ef00d3d586a3d7bbc76c42f0b0719cc6f7b0cce1790622c3010000000100"
       "00000100028dc3921705c71d30b0b26674536fff934f8e43890c980aa1d2c168f00f406539010000000000000000ea3055000000004873b"
       "d3e0160ae423ad15b974a00000000a8ed32322060ae423ad15b974a1042088a4dd350570094357700000000045359530000000000000000"
       "00ea305500003f2a1ba6a24a0160ae423ad15b974a00000000a8ed32323160ae423ad15b974a1042088a4dd3505740420f0000000000045"
       "359530000000040420f000000000004535953000000000000\"}}"s;
   std::string test_body2_copy = test_body2;

   int callbackCalledCnt = 0;

   // Launch the asynchronous operation
   http_client_async::async_http_request(
       params, std::move(test_body),
       [&test_body_copy, &callbackCalledCnt](boost::beast::error_code ec, http::response<http::string_body> response) {
          BOOST_REQUIRE(!ec);
          BOOST_REQUIRE_EQUAL(test_body_copy, response.body());
          callbackCalledCnt++;
       });

   http_client_async::http_request_params params2{ioc, host, port, target, version, content_type2};
   http_client_async::async_http_request(
       params2, std::move(test_body2),
       [&test_body2_copy, &callbackCalledCnt](boost::beast::error_code ec, http::response<http::string_body> response) {
          BOOST_REQUIRE(!ec);
          BOOST_REQUIRE_EQUAL(test_body2_copy, response.body());
          callbackCalledCnt++;
       });

   // Run the I/O service. The call will return when
   // the get operation is complete.
   ioc.run();

   BOOST_REQUIRE_EQUAL(callbackCalledCnt, 2);

   server.shutdown();
}

BOOST_AUTO_TEST_SUITE_END()
