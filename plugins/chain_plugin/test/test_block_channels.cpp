#include <boost/test/unit_test.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>

#include <magic_enum/magic_enum.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace sysio;
using namespace sysio::chain;
namespace channels = sysio::chain::plugin_interface::channels;

/// Command line of the test application.
constexpr auto program_name = "test_block_channels";
constexpr auto data_dir_option = "--data-dir";
constexpr auto config_dir_option = "--config-dir";
constexpr auto genesis_json_option = "--genesis-json";
constexpr auto genesis_file_name = "genesis.json";

/// Longest a test waits for the application thread to deliver what it emitted.
constexpr auto delivery_timeout = std::chrono::seconds(10);
/// Timestamp slots of the synthetic blocks; distinct slots give distinct block ids.
constexpr uint32_t first_block_slot = 1;
constexpr uint32_t second_block_slot = 2;

/// The block channel a delivery arrived on.
enum class block_channel : uint8_t { accepted_block_header, accepted_block, irreversible_block };

std::ostream& operator<<(std::ostream& os, block_channel channel) {
   return os << magic_enum::enum_name(channel);
}

/// One recorded channel delivery.
struct delivery {
   block_channel    channel;
   signed_block_ptr block;
   block_id_type    id;
};

/// A packed block with no transactions whose id is made distinct by @p slot.
signed_block_ptr make_block(uint32_t slot) {
   signed_block_header header;
   header.timestamp = block_timestamp_type{slot};
   return signed_block::create_signed_block(signed_block::create_mutable_block(header));
}

/// An application running only chain_plugin, with a recording subscription on each block channel. No block is
/// produced or received, so every delivery comes from a signal the test emits.
class block_channel_recorder {
public:
   block_channel_recorder() {
      // A node that is not a producer has no default genesis it can start from, so give it the tester's.
      const std::string dir = _dir.path().string();
      const std::string genesis_file = (_dir.path() / genesis_file_name).string();
      fc::json::save_to_file(sysio::testing::base_tester::default_genesis(), genesis_file);

      std::promise<chain_plugin*> started;
      auto started_fut = started.get_future();
      _app_thread = std::thread([this, &started, &dir, &genesis_file]() {
         bool running = false;
         try {
            std::vector<const char*> argv = {program_name,      data_dir_option,     dir.c_str(),
                                             config_dir_option, dir.c_str(),         genesis_json_option,
                                             genesis_file.c_str()};
            _app->initialize<chain_plugin>(argv.size(), (char**)argv.data());
            _app->startup();
            // the app was constructed on the test thread; the executor's main thread is the one running exec()
            _app->executor().set_main_thread_id();
            started.set_value(_app->find_plugin<chain_plugin>());
            running = true;
            _app->exec();
            return;
         }
         FC_LOG_AND_DROP()
         BOOST_CHECK(!"app threw exception see logged error");
         if (!running)
            started.set_value(nullptr);
      });
      _chain_plug = started_fut.get();
      if (!_chain_plug) {
         _app_thread.join();
         BOOST_FAIL("chain_plugin failed to start; see logged error");
      }
      _header_subscription = record<channels::accepted_block_header>(block_channel::accepted_block_header);
      _accepted_subscription = record<channels::accepted_block>(block_channel::accepted_block);
      _irreversible_subscription = record<channels::irreversible_block>(block_channel::irreversible_block);
   }

   ~block_channel_recorder() {
      _app->quit();
      _app_thread.join();
   }

   block_channel_recorder(const block_channel_recorder&) = delete;
   block_channel_recorder& operator=(const block_channel_recorder&) = delete;

   /// Run @p emit on the application thread, where the controller emits its signals. Deliveries it causes run
   /// only after @p emit returns.
   void on_app_thread(std::function<void(controller&)> emit) {
      _app->executor().post(appbase::priority::high, appbase::exec_queue::read_write,
                            [this, emit = std::move(emit)]() { emit(_chain_plug->chain()); });
   }

   /// Wait for one delivery per block channel; false on timeout.
   bool wait_for_one_delivery_per_channel() {
      std::unique_lock lock(_mtx);
      return _delivered.wait_for(lock, delivery_timeout,
                                 [this] { return _deliveries.size() >= magic_enum::enum_count<block_channel>(); });
   }

   /// Every delivery so far, in arrival order.
   std::vector<delivery> deliveries() const {
      std::lock_guard lock(_mtx);
      return _deliveries;
   }

private:
   template <typename Channel>
   typename Channel::channel_type::handle record(block_channel channel) {
      return _app->get_channel<Channel>().subscribe([this, channel](const channels::block_params& payload) {
         const auto& [block, id] = payload;
         {
            std::lock_guard lock(_mtx);
            _deliveries.push_back({channel, block, id});
         }
         _delivered.notify_all();
      });
   }

   fc::temp_directory      _dir;
   appbase::scoped_app     _app;
   std::thread             _app_thread;
   chain_plugin*           _chain_plug = nullptr;
   mutable std::mutex      _mtx;
   std::condition_variable _delivered;
   std::vector<delivery>   _deliveries; ///< guarded by _mtx

   channels::accepted_block_header::channel_type::handle _header_subscription;
   channels::accepted_block::channel_type::handle        _accepted_subscription;
   channels::irreversible_block::channel_type::handle    _irreversible_subscription;
};

} // namespace

BOOST_AUTO_TEST_SUITE(block_channel_tests)

// A delivery runs after the controller's signal returns, when whatever the signal referenced may already be gone:
// fork database pruning frees block states within a single apply batch. Emitting through one binding and repointing
// it before the emitting task returns makes a payload that only references its source arrive as the replacement.
BOOST_AUTO_TEST_CASE(deliveries_carry_the_emitted_block) {
   block_channel_recorder recorder;
   const signed_block_ptr emitted = make_block(first_block_slot);
   const block_id_type    emitted_id = emitted->calculate_id();
   const signed_block_ptr replacement = make_block(second_block_slot);

   signed_block_ptr binding_block;
   block_id_type    binding_id;
   recorder.on_app_thread([&](controller& chain) {
      const auto emit = [&](auto& signal) {
         binding_block = emitted;
         binding_id = emitted_id;
         signal(std::tie(binding_block, binding_id));
      };
      emit(chain.accepted_block_header());
      emit(chain.accepted_block());
      emit(chain.irreversible_block());
      binding_block = replacement;
      binding_id = replacement->calculate_id();
   });

   BOOST_REQUIRE(recorder.wait_for_one_delivery_per_channel());
   const auto deliveries = recorder.deliveries();
   BOOST_REQUIRE_EQUAL(deliveries.size(), magic_enum::enum_count<block_channel>());
   for (const auto& d : deliveries) {
      BOOST_TEST_CONTEXT(d.channel) {
         BOOST_CHECK(d.block == emitted);
         BOOST_CHECK_EQUAL(d.id.str(), emitted_id.str());
      }
   }
}

// The controller emits accepted_block_header, then accepted_block, then irreversible_block for newly final blocks.
// Deliveries share one priority, so a subscriber of several channels receives them in that order.
BOOST_AUTO_TEST_CASE(deliveries_keep_emission_order) {
   block_channel_recorder recorder;
   const signed_block_ptr head = make_block(second_block_slot);
   const block_id_type    head_id = head->calculate_id();
   const signed_block_ptr final_block = make_block(first_block_slot);
   const block_id_type    final_id = final_block->calculate_id();

   recorder.on_app_thread([&](controller& chain) {
      chain.accepted_block_header()(std::tie(head, head_id));
      chain.accepted_block()(std::tie(head, head_id));
      chain.irreversible_block()(std::tie(final_block, final_id));
   });

   BOOST_REQUIRE(recorder.wait_for_one_delivery_per_channel());
   const auto deliveries = recorder.deliveries();
   const std::vector<std::tuple<block_channel, std::string>> expected{
      {block_channel::accepted_block_header, head_id.str()},
      {block_channel::accepted_block, head_id.str()},
      {block_channel::irreversible_block, final_id.str()},
   };
   BOOST_REQUIRE_EQUAL(deliveries.size(), expected.size());
   for (size_t i = 0; i < expected.size(); ++i) {
      BOOST_TEST_CONTEXT("delivery " << i) {
         BOOST_CHECK_EQUAL(deliveries[i].channel, std::get<block_channel>(expected[i]));
         BOOST_CHECK_EQUAL(deliveries[i].id.str(), std::get<std::string>(expected[i]));
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
