#include <trx_provider.hpp>
#include <trx_generator.hpp>
#include <http_client_async.hpp>
#include <simple_rest_server.hpp>
#include <test_ports.hpp>
#include <sysio/net_plugin/protocol.hpp>

#define BOOST_TEST_MODULE trx_generator_tests
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <future>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sysio;
using namespace sysio::testing;
using namespace std::literals::string_literals;

static const char* api_name = "/v1/chain/test";

namespace {
/** Restore SYSIO_TEST_PORTS after tests that exercise lease parsing. */
class test_ports_env_guard {
 public:
   test_ports_env_guard() {
      if(const char* value = std::getenv(test_ports_env_var))
         _previous = value;
   }

   ~test_ports_env_guard() {
      if(_previous)
         setenv(test_ports_env_var, _previous->c_str(), 1);
      else
         unsetenv(test_ports_env_var);
   }

   /** Set the lease for this scoped test. */
   void set(const std::string& value) const { setenv(test_ports_env_var, value.c_str(), 1); }

   /** Clear the lease for this scoped test. */
   void clear() const { unsetenv(test_ports_env_var); }

 private:
   std::optional<std::string> _previous;
};

/** Each category's expected slots; must match `Utils.getPort` in tests/TestHarness/testUtils.py. */
struct expected_port {
   port_category category;
   uint32_t      slot;
   uint32_t      count;
};

constexpr std::array expected_ports{
    expected_port{port_category::ship, 0, 1},
    expected_port{port_category::state_history, 1, 1},
    expected_port{port_category::bios_http, 2, 1},
    expected_port{port_category::node_http, 3, 88},
    expected_port{port_category::alternate_service, 91, 1},
    expected_port{port_category::plugin_http_peer, 92, 1},
    expected_port{port_category::plugin_http_local, 93, 1},
    expected_port{port_category::bios_p2p, 94, 1},
    expected_port{port_category::alternate_p2p, 95, 46},
    expected_port{port_category::p2p, 141, 23},
    expected_port{port_category::wallet, 164, 5},
    expected_port{port_category::transaction_only, 169, 2},
    expected_port{port_category::ipv6_probe, 171, 4},
    expected_port{port_category::raw_client, 175, 16}};

/** The lease this test was started with; fails the test when it runs without one. */
std::vector<uint16_t> require_lease() {
   const char* lease = std::getenv(test_ports_env_var);
   BOOST_REQUIRE_MESSAGE(lease != nullptr, "run through ctest or `python3 tests/bind_registry.py`");
   return parse_test_ports(lease);
}

/** Join ports into a SYSIO_TEST_PORTS value. */
std::string join_ports(const std::vector<uint32_t>& ports) {
   std::string joined;
   for(const auto port : ports)
      joined += (joined.empty() ? "" : ",") + std::to_string(port);
   return joined;
}

/** Leased P2P slots of the provider tests' local peers (slot 0 is the constructor test's address). */
constexpr uint32_t handshake_test_p2p_index = 1;
constexpr uint32_t write_failure_test_p2p_index = 2;

/** Independent endpoint, URL and CLI spellings expected by native port consumers. */
constexpr std::string_view expected_loopback_http_prefix = "http://127.0.0.1:";
constexpr std::string_view expected_state_history_endpoint_prefix = "127.0.0.1:";
constexpr std::string_view expected_socket_address_option = "--socket-address";

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

/** A mock generator whose send number `late_call` takes `delay` to return. */
struct late_mock_trx_generator {
   std::vector<fc::time_point> _calls;
   size_t _late_call;
   std::chrono::microseconds _delay;

   bool setup() { return true; }
   bool tear_down() { return true; }
   bool stop_on_trx_fail() { return false; }

   bool generate_and_send() {
      _calls.push_back(fc::time_point::now());
      if (_calls.size() - 1 == _late_call)
         std::this_thread::sleep_for(_delay);
      return true;
   }

   late_mock_trx_generator(size_t expected_num_calls, size_t late_call, std::chrono::microseconds delay)
       : _late_call(late_call), _delay(delay) {
      _calls.reserve(expected_num_calls);
   }
};

// fc::time_point::now() is the wall clock, so timing checks allow it to step back by this much.
constexpr int64_t wall_clock_tolerance_us = 2000;

BOOST_AUTO_TEST_SUITE(trx_generator_tests)

BOOST_AUTO_TEST_CASE(tps_short_run_low_tps)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 5;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr int64_t interval_us = std::chrono::microseconds(1s).count() / test_tps;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   BOOST_REQUIRE(t1.run());
   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_EQUAL(monitor->_calls.size(), expected_trxs);
   const auto start = monitor->_calls.front().start_time;
   for (uint32_t index = 0; index < expected_trxs; ++index) {
      const auto& stats = monitor->_calls[index];
      // Send k is due at start + k intervals (anchored, so a late send does not shift the rest; see
      // tps_catches_up_after_late_send). Being late is the scheduler's business; being early is a bug.
      BOOST_CHECK_EQUAL((stats.start_time - start).count(), 0);
      BOOST_CHECK_EQUAL(stats.trx_interval.count(), interval_us);
      BOOST_CHECK_EQUAL((stats.next_run - start).count(), (index + 1) * interval_us);
      BOOST_CHECK_EQUAL((stats.expected_end_time - start).count(), test_duration_s * std::chrono::microseconds(1s).count());
      BOOST_CHECK_EQUAL(stats.trxs_sent, index + 1);
      BOOST_CHECK_EQUAL(stats.trxs_left, expected_trxs - index - 1);
      BOOST_CHECK_GE((generator->_calls[index] - start).count(), index * interval_us - wall_clock_tolerance_us);
   }
}

BOOST_AUTO_TEST_CASE(tps_catches_up_after_late_send)
{
   constexpr uint32_t test_duration_s = 2;
   constexpr uint32_t test_tps = 5;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr int64_t interval_us = std::chrono::microseconds(1s).count() / test_tps;
   constexpr uint32_t late_call = 1;
   constexpr int64_t late_intervals = 4;

   // Send 1 takes 4 intervals, so sends 2..4 are already overdue when they come up: an anchored schedule sends them
   // without sleeping (negative time to next) instead of pushing every later send back by 4 intervals.
   auto generator = std::make_shared<late_mock_trx_generator>(expected_trxs, late_call,
                                                              std::chrono::microseconds(late_intervals * interval_us));
   auto monitor = std::make_shared<simple_tps_monitor>(expected_trxs);
   trx_tps_tester<late_mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   BOOST_REQUIRE(t1.run());
   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_EQUAL(monitor->_calls.size(), expected_trxs);
   const auto start = monitor->_calls.front().start_time;
   for (uint32_t index = late_call; index < late_call + late_intervals - 1; ++index) {
      BOOST_TEST_CONTEXT("send " << index) {
         BOOST_CHECK_EQUAL((monitor->_calls[index].next_run - start).count(), (index + 1) * interval_us);
         // The monitor sees each send's time to next one send later.
         BOOST_CHECK_LT(monitor->_calls[index + 1].time_to_next_trx_us, 0);
      }
   }
   for (uint32_t index = 0; index < expected_trxs; ++index) {
      BOOST_CHECK_GE((generator->_calls[index] - start).count(), index * interval_us - wall_clock_tolerance_us);
   }
   // The overdue sends go out back to back: together they take less than one interval.
   BOOST_CHECK_LT((generator->_calls[late_call + late_intervals - 1] - generator->_calls[late_call + 1]).count(),
                  interval_us);
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

BOOST_AUTO_TEST_CASE(test_ports_category_slots)
{
   const auto ports = require_lease();
   for(const auto& expected : expected_ports) {
      BOOST_TEST_CONTEXT("slot " << expected.slot) {
         BOOST_REQUIRE_EQUAL(slots_of(expected.category).start, expected.slot);
         BOOST_REQUIRE_EQUAL(slots_of(expected.category).count, expected.count);
         BOOST_REQUIRE_EQUAL(get_port(expected.category), ports.at(expected.slot));
         BOOST_REQUIRE_EQUAL(get_port(expected.category, expected.count - 1),
                             ports.at(expected.slot + expected.count - 1));
      }
   }
}

BOOST_AUTO_TEST_CASE(test_ports_rejects_invalid_indices)
{
   require_lease();
   for(const auto& expected : expected_ports) {
      BOOST_REQUIRE_THROW(get_port(expected.category, expected.count), std::runtime_error);
   }
}

/** Concurrent fixture starts consume distinct category slots and fail at exhaustion. */
BOOST_AUTO_TEST_CASE(test_ports_allocator_concurrent_exhaustion)
{
   const auto ports = require_lease();
   const auto slots = slots_of(port_category::node_http);
   test_port_allocator allocator{port_category::node_http};
   std::vector<std::future<uint16_t>> pending;
   for(uint32_t index = 0; index < slots.count; ++index)
      pending.push_back(std::async(std::launch::async, [&allocator] { return allocator.next(); }));
   std::vector<uint16_t> allocated;
   for(auto& port : pending)
      allocated.push_back(port.get());
   std::sort(allocated.begin(), allocated.end());
   for(uint32_t index = 0; index < slots.count; ++index)
      BOOST_REQUIRE_EQUAL(allocated.at(index), ports.at(slots.start + index));
   BOOST_REQUIRE_THROW(allocator.next(), std::runtime_error);

   test_port_allocator urls{port_category::node_http};
   BOOST_REQUIRE_EQUAL(urls.next_loopback_url(),
                       std::string(expected_loopback_http_prefix) + std::to_string(ports.at(slots.start)));
}

/** SHiP uses the leased default, preserves an explicit endpoint, and reports a missing lease. */
BOOST_AUTO_TEST_CASE(test_ports_state_history_endpoint_resolution)
{
   const auto ports = require_lease();
   std::ostringstream error;
   std::string endpoint;
   BOOST_REQUIRE(resolve_state_history_endpoint(endpoint, error));
   BOOST_REQUIRE_EQUAL(endpoint,
                       std::string(expected_state_history_endpoint_prefix) + std::to_string(ports.at(1)));
   BOOST_REQUIRE(error.str().empty());

   test_ports_env_guard env;
   env.clear();
   const auto explicit_endpoint = endpoint;
   BOOST_REQUIRE(resolve_state_history_endpoint(endpoint, error));
   BOOST_REQUIRE_EQUAL(endpoint, explicit_endpoint);
   endpoint.clear();
   BOOST_REQUIRE(!resolve_state_history_endpoint(endpoint, error));
   BOOST_REQUIRE(endpoint.empty());
   BOOST_REQUIRE(error.str().find(test_ports_env_var) != std::string::npos);
   BOOST_REQUIRE(error.str().find(expected_socket_address_option) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_ports_parses_leases)
{
   BOOST_REQUIRE((parse_test_ports("20000") == std::vector<uint16_t>{20000}));
   BOOST_REQUIRE((parse_test_ports("20000,20001,20002") == std::vector<uint16_t>{20000, 20001, 20002}));
   for(const char* invalid : {"", ",", "20000,", ",20000", "20000,,20001", "0", "65536", "-1", "+1", " 1", "1 ",
                              "abc", "20000,20002", "20001,20000", "20000,20000"}) {
      BOOST_TEST_CONTEXT("lease '" << invalid << "'") {
         BOOST_REQUIRE_THROW(parse_test_ports(invalid), std::runtime_error);
      }
   }
}

BOOST_AUTO_TEST_CASE(test_ports_requires_a_lease)
{
   test_ports_env_guard env;
   env.clear();
   BOOST_REQUIRE_THROW(get_port(port_category::p2p), std::runtime_error);

   // A lease too short for a category's slots is refused rather than read out of range.
   env.set(join_ports({20000, 20001}));
   BOOST_REQUIRE_EQUAL(get_port(port_category::state_history), 20001);
   BOOST_REQUIRE_THROW(get_port(port_category::bios_http), std::runtime_error);
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
   const uint16_t port = get_port(port_category::p2p, handshake_test_p2p_index);
   boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), port});

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
   const uint16_t port = get_port(port_category::p2p, write_failure_test_p2p_index);
   boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), port});

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
