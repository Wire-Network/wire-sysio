#include <atomic>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <latch>
#include <memory>
#include <set>
#include <string>
#include <sysio/chain/exceptions.hpp>
#include <sysio/external_debugging_plugin/debug_envelope_event_sink.hpp>
#include <sysio/external_debugging_plugin/external_debugging_plugin.hpp>
#include <thread>
#include <vector>

namespace {

/** Parse external-debugging configuration arguments through the plugin's public option surface. */
boost::program_options::variables_map parse_options(sysio::external_debugging_plugin& plugin,
                                                    const std::vector<std::string>& arguments) {
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   boost::program_options::variables_map options;
   boost::program_options::store(boost::program_options::command_line_parser(arguments).options(cfg).run(), options);
   boost::program_options::notify(options);
   return options;
}

} // namespace

BOOST_AUTO_TEST_SUITE(external_debugging_plugin_tests)

BOOST_AUTO_TEST_CASE(backpressure_options_are_registered) {
   sysio::external_debugging_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   std::set<std::string> option_names;
   for (const auto& option : cfg.options()) {
      option_names.insert(option->long_name());
   }

   BOOST_TEST(option_names.contains("ext-debugging-server"));
   BOOST_TEST(option_names.contains("ext-debugging-max-pending-envelopes"));
   BOOST_TEST(option_names.contains("ext-debugging-request-timeout-ms"));
}

BOOST_AUTO_TEST_CASE(backpressure_defaults_are_bounded) {
   sysio::external_debugging_plugin plugin;
   const auto options = parse_options(plugin, {});

   BOOST_TEST(options["ext-debugging-max-pending-envelopes"].as<uint32_t>() == 16u);
   BOOST_TEST(options["ext-debugging-request-timeout-ms"].as<uint32_t>() == 5'000u);
   BOOST_CHECK_NO_THROW(plugin.plugin_initialize(options));
}

BOOST_AUTO_TEST_CASE(zero_pending_capacity_is_rejected) {
   sysio::external_debugging_plugin plugin;
   const auto options = parse_options(plugin, {"--ext-debugging-max-pending-envelopes=0"});

   BOOST_CHECK_THROW(plugin.plugin_initialize(options), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(zero_request_timeout_is_rejected) {
   sysio::external_debugging_plugin plugin;
   const auto options = parse_options(plugin, {"--ext-debugging-request-timeout-ms=0"});

   BOOST_CHECK_THROW(plugin.plugin_initialize(options), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(debugging_server_hostname_remains_accepted) {
   sysio::external_debugging_plugin plugin;
   const auto options = parse_options(plugin, {"--ext-debugging-server=http://localhost:9876"});

   BOOST_CHECK_NO_THROW(plugin.plugin_initialize(options));
}

BOOST_AUTO_TEST_CASE(event_sink_bounds_pending_envelopes_and_counts_drops) {
   constexpr std::size_t pending_capacity = 2;
   std::atomic<int> processed{0};
   std::latch callback_started(1);
   std::latch release_callback(1);

   auto sink = std::make_shared<sysio::external_debugging::debug_envelope_event_sink>(
      pending_capacity, [&](sysio::opp::debugging::DebugEnvelopeEvent&) {
         callback_started.count_down();
         release_callback.wait();
         ++processed;
      });
   const sysio::opp::debugging::DebugEnvelopeEvent event{};

   sink->enqueue(event);
   callback_started.wait();
   sink->enqueue(event);
   sink->enqueue(event);
   sink->enqueue(event);

   BOOST_TEST(sink->pending_envelopes() == pending_capacity);
   BOOST_TEST(sink->dropped_envelopes() == 1u);

   std::thread stopper([&] { sink->stop(); });
   while (sink->running())
      std::this_thread::yield();
   release_callback.count_down();
   stopper.join();

   BOOST_TEST(processed.load() == 1);
   BOOST_TEST(sink->pending_envelopes() == 0u);
   BOOST_TEST(sink->dropped_envelopes() == 3u);
}

/// The production slot owns the sink independently of the plugin's member so
/// an in-flight emission remains safe after shutdown releases its owner.
BOOST_AUTO_TEST_CASE(event_slot_retains_stopped_sink_through_owner_release) {
   std::atomic<int> processed{0};
   auto sink = std::make_shared<sysio::external_debugging::debug_envelope_event_sink>(
      1, [&](sysio::opp::debugging::DebugEnvelopeEvent&) { ++processed; });
   std::weak_ptr<sysio::external_debugging::debug_envelope_event_sink> weak_sink = sink;
   auto slot = sysio::external_debugging::make_debug_envelope_slot(sink);

   sink->stop();
   sink.reset();
   BOOST_TEST(!weak_sink.expired());

   slot(sysio::opp::debugging::DebugEnvelopeEvent{});
   BOOST_TEST(processed.load() == 0);
   auto retained_sink = weak_sink.lock();
   BOOST_REQUIRE(retained_sink);
   BOOST_TEST(retained_sink->dropped_envelopes() == 1u);
   retained_sink.reset();
   slot = {};
   BOOST_TEST(weak_sink.expired());
}

BOOST_AUTO_TEST_SUITE_END()
