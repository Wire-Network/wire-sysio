#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/testing/tester.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/fork_database.hpp>
#include <sysio/chain/plugin_interface.hpp>

#include <fc/io/json.hpp>

#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "incoming_trx_utils.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::test;

namespace {

/// How long a node may take to start, to deliver a transaction result, or to apply the blocks it is given.
constexpr std::chrono::seconds node_timeout{60};
/// How often a test polls a node's head while waiting for it to apply blocks.
constexpr std::chrono::milliseconds head_poll_interval{50};
/// Signatures on the block transaction a node limited to one signature still applies.
constexpr uint32_t block_trx_signatures = 40;

/// @return @p count distinct K1 keys, the same on every call
std::vector<private_key_type> make_keys(uint32_t count) {
   std::vector<private_key_type> keys;
   keys.reserve(count);
   for (uint32_t i = 0; i < count; ++i) {
      keys.push_back(private_key_type::regenerate<fc::ecc::private_key_shim>(
         fc::sha256::hash("incoming-trx-limit-key-" + std::to_string(i)).to_uint64_array()));
   }
   return keys;
}

/// @return an authority satisfied only by all of @p keys
authority all_keys_authority(const std::vector<private_key_type>& keys) {
   authority auth;
   auth.threshold = keys.size();
   for (const private_key_type& key : keys)
      auth.keys.push_back(key_weight{key.get_public_key(), 1});
   auth.sort_fields();
   return auth;
}

/// @return a transaction updating sysio@active to @p auth
signed_transaction make_updateauth_trx(const authority& auth) {
   signed_transaction trx;
   trx.expiration = fc::time_point_sec{fc::time_point::now() + trx_lifetime};
   trx.actions.emplace_back(vector<permission_level>{{config::system_account_name, config::active_name}},
                            updateauth{.account    = config::system_account_name,
                                       .permission = config::active_name,
                                       .parent     = config::owner_name,
                                       .auth       = auth});
   return trx;
}

/// Signs @p trx with each of @p keys, in order.
packed_transaction_ptr sign(signed_transaction trx, const std::vector<private_key_type>& keys,
                            const chain_id_type& chain_id) {
   for (const private_key_type& key : keys)
      trx.sign(key, chain_id);
   return std::make_shared<packed_transaction>(std::move(trx));
}

/// A node running chain_plugin and producer_plugin on its own thread, shut down on destruction.
class test_node {
public:
   /// Starts the node.
   /// @param args       command line arguments after the data and config directories
   /// @param config_ini contents of the node's config.ini; empty for the default one
   explicit test_node(const std::vector<std::string>& args, const std::string& config_ini = {}) {
      const std::string dir = temp.path().string();
      if (!config_ini.empty())
         std::ofstream(temp.path() / "config.ini") << config_ini;
      std::vector<std::string> all_args = {"test", "--data-dir", dir, "--config-dir", dir};
      all_args.insert(all_args.end(), args.begin(), args.end());

      auto              started     = std::make_shared<std::promise<void>>();
      std::future<void> started_fut = started->get_future();
      app_thread = std::thread([this, all_args, started]() {
         try {
            std::vector<const char*> argv;
            for (const std::string& arg : all_args)
               argv.push_back(arg.c_str());
            if (!app->initialize<chain_plugin, producer_plugin>(argv.size(), const_cast<char**>(argv.data())))
               throw std::runtime_error("node rejected its arguments");
            app->startup();
            // app was constructed on the test thread; make this thread the main thread for producer_plugin's checks
            app->executor().set_main_thread_id();
            chain_plug = app->find_plugin<chain_plugin>();
            prod_plug  = app->find_plugin<producer_plugin>();
         } catch (...) {
            started->set_exception(std::current_exception());
            return;
         }
         started->set_value();
         try {
            app->exec();
         } FC_LOG_AND_DROP()
      });
      try {
         BOOST_REQUIRE(started_fut.wait_for(node_timeout) == std::future_status::ready);
         started_fut.get();
      } catch (...) {
         app->quit();
         app_thread.join();
         throw;
      }
   }

   ~test_node() {
      app->quit();
      app_thread.join();
   }

   test_node(const test_node&)            = delete;
   test_node& operator=(const test_node&) = delete;

   chain_id_type chain_id() const { return chain_plug->chain().get_chain_id(); }

   /// @return the byte limit of the node's incoming transaction queue
   uint64_t max_incoming_transaction_queue_size() const { return prod_plug->get_max_incoming_transaction_queue_size(); }

   /// Delivers @p trx the way net_plugin delivers a p2p transaction.
   /// @return the result the node reported for it
   next_function_variant<transaction_trace_ptr> push(const packed_transaction_ptr& trx) {
      auto result = std::make_shared<std::promise<next_function_variant<transaction_trace_ptr>>>();
      std::future<next_function_variant<transaction_trace_ptr>> result_fut = result->get_future();
      app->executor().post(priority::low, exec_queue::read_write, [this, trx, result]() {
         app->get_method<plugin_interface::incoming::methods::transaction_async>()(
            trx, false /*api_trx*/, transaction_metadata::trx_type::input, true /*return_failure_traces*/,
            [result](const next_function_variant<transaction_trace_ptr>& r) { result->set_value(r); });
      });
      BOOST_REQUIRE(result_fut.wait_for(node_timeout) == std::future_status::ready);
      return result_fut.get();
   }

   /// Hands @p block to the node the way net_plugin hands it a received block.
   void receive_block(const signed_block_ptr& block) {
      controller& chain = chain_plug->chain();
      const controller::accepted_block_result result =
         chain.accept_block(block->calculate_id(), block, fc::time_point::now());
      BOOST_REQUIRE(result.add_result != fork_db_add_t::failure);
      prod_plug->process_blocks();
   }

   /// @return true once the node's head is @p id, false if it is not within node_timeout
   bool wait_for_head(const block_id_type& id) {
      const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + node_timeout;
      while (std::chrono::steady_clock::now() < deadline) {
         auto                       head     = std::make_shared<std::promise<block_id_type>>();
         std::future<block_id_type> head_fut = head->get_future();
         app->executor().post(priority::high, exec_queue::read_write,
                              [this, head]() { head->set_value(chain_plug->chain().head().id()); });
         if (head_fut.wait_for(node_timeout) != std::future_status::ready)
            return false;
         if (head_fut.get() == id)
            return true;
         std::this_thread::sleep_for(head_poll_interval);
      }
      return false;
   }

private:
   fc::temp_directory  temp;
   appbase::scoped_app app;
   std::thread         app_thread;
   chain_plugin*       chain_plug = nullptr;
   producer_plugin*    prod_plug  = nullptr;
};

/// Requires that @p result is the rejection of a transaction carrying @p count signatures over a limit of @p limit.
void require_too_many_sigs(const next_function_variant<transaction_trace_ptr>& result, uint32_t count,
                           uint32_t limit) {
   BOOST_REQUIRE(std::holds_alternative<fc::exception_ptr>(result));
   const fc::exception_ptr& e = std::get<fc::exception_ptr>(result);
   BOOST_TEST_INFO("exception: " << e->to_detail_string());
   BOOST_REQUIRE_EQUAL(e->code(), tx_too_many_sigs::code_value);
   const std::string expected =
      "transaction has " + std::to_string(count) + " signatures, exceeding the limit of " + std::to_string(limit);
   BOOST_CHECK_MESSAGE(e->to_detail_string().find(expected) != std::string::npos, e->to_detail_string());
}

/// Requires that @p result is a transaction that executed.
void require_executed(const next_function_variant<transaction_trace_ptr>& result) {
   if (std::holds_alternative<fc::exception_ptr>(result))
      BOOST_FAIL("rejected: " << std::get<fc::exception_ptr>(result)->to_detail_string());
   const transaction_trace_ptr& trace = std::get<transaction_trace_ptr>(result);
   if (trace->except)
      BOOST_FAIL("failed: " << trace->except->to_detail_string());
   BOOST_REQUIRE(trace->receipt);
}

} // namespace

BOOST_AUTO_TEST_SUITE(incoming_trx_limits_tests)

// A transaction carrying one signature more than the default limit is rejected before its keys are recovered: its
// signatures are all the same, which recovery would have reported as a duplicate signature instead.
BOOST_AUTO_TEST_CASE(default_limit_rejects_before_recovery) {
   test_node node(producer_args());
   constexpr uint32_t over_limit = chain::config::default_incoming_trx_max_signatures + 1;

   const std::vector<private_key_type> same_key(over_limit, sysio_key());
   require_too_many_sigs(node.push(sign(make_newaccount_trx(), same_key, node.chain_id())), over_limit,
                         chain::config::default_incoming_trx_max_signatures);

   require_executed(node.push(sign(make_newaccount_trx(), {sysio_key()}, node.chain_id())));
}

// incoming-trx-max-signatures overrides the default: a transaction at the configured limit executes, and one with a
// signature more is rejected although it is far within the default.
BOOST_AUTO_TEST_CASE(configured_limit_overrides_default) {
   constexpr uint32_t limit = 3;
   static_assert(limit + 1 < chain::config::default_incoming_trx_max_signatures);
   test_node node(producer_args({"--incoming-trx-max-signatures", std::to_string(limit)}));
   const std::vector<private_key_type> keys = make_keys(limit + 1);
   const std::vector<private_key_type> signers(keys.begin(), keys.begin() + limit);

   require_executed(node.push(sign(make_updateauth_trx(all_keys_authority(signers)), {sysio_key()},
                                   node.chain_id())));
   require_executed(node.push(sign(make_newaccount_trx(), signers, node.chain_id())));

   require_too_many_sigs(node.push(sign(make_newaccount_trx(), keys, node.chain_id())), limit + 1, limit);
}

// The limit does not apply to a transaction in a received block: a node limited to one signature applies a block
// whose transaction carries many more.
BOOST_AUTO_TEST_CASE(blocks_apply_regardless_of_incoming_limit) {
   testing::tester chain(testing::setup_policy::none);
   const std::vector<private_key_type> keys = make_keys(block_trx_signatures);
   chain.set_authority(config::system_account_name, config::active_name, all_keys_authority(keys), config::owner_name,
                       {{config::system_account_name, config::active_name}}, {sysio_key()});
   chain.produce_block();
   signed_transaction trx = make_newaccount_trx();
   chain.set_transaction_headers(trx);
   for (const private_key_type& key : keys)
      trx.sign(key, chain.get_chain_id());
   chain.push_transaction(trx);
   const signed_block_ptr trx_block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(trx_block->transactions.size(), 1u);
   chain.produce_block();

   fc::temp_directory genesis_dir;
   const std::string  genesis_file = (genesis_dir.path() / "genesis.json").string();
   fc::json::save_to_file(testing::base_tester::default_genesis(), genesis_file);
   test_node node({"--genesis-json", genesis_file, "--incoming-trx-max-signatures", "1"});

   for (uint32_t block_num = 2; block_num <= chain.head().block_num(); ++block_num)
      node.receive_block(chain.fetch_block_by_number(block_num));
   BOOST_REQUIRE(node.wait_for_head(chain.head().id()));
}

// incoming-transaction-queue-size-mb sets the queue's byte limit across the option's range, including sizes whose byte
// count overflows an int: 2048 MiB is 2^31 bytes, and 4097 MiB is 2^32 + 2^20 bytes, which wraps to 1 MiB in 32 bits.
BOOST_AUTO_TEST_CASE(queue_size_option_sets_byte_limit) {
   constexpr uint64_t default_queue_size_mb = 1024;
   {
      test_node node(producer_args());
      BOOST_CHECK_EQUAL(node.max_incoming_transaction_queue_size(), default_queue_size_mb * 1024 * 1024);
   }
   for (const uint16_t size_mb : {uint16_t{2048}, uint16_t{4097}, std::numeric_limits<uint16_t>::max()}) {
      BOOST_TEST_CONTEXT("incoming-transaction-queue-size-mb " << size_mb) {
         test_node node(producer_args({"--incoming-transaction-queue-size-mb", std::to_string(size_mb)}));
         BOOST_CHECK_EQUAL(node.max_incoming_transaction_queue_size(), uint64_t{size_mb} * 1024 * 1024);
      }
   }
}

// An incoming-transaction-queue-size-mb outside 1 to 65535, from the command line or config.ini, fails initialization
// with the configured value and the range in the error. A negative value is not read as its unsigned wraparound.
BOOST_AUTO_TEST_CASE(out_of_range_queue_size_rejected) {
   for (const std::string size_mb : {"0", "-1", "-65535", "65536"}) {
      const std::string expected = "incoming-transaction-queue-size-mb " + size_mb + " must be between 1 and 65535 MiB";
      BOOST_TEST_CONTEXT("command line " << size_mb) {
         BOOST_CHECK_EXCEPTION(test_node(producer_args({"--incoming-transaction-queue-size-mb=" + size_mb})),
                               plugin_config_exception, testing::fc_exception_message_contains(expected));
      }
      BOOST_TEST_CONTEXT("config.ini " << size_mb) {
         BOOST_CHECK_EXCEPTION(test_node(producer_args(), "incoming-transaction-queue-size-mb = " + size_mb + "\n"),
                               plugin_config_exception, testing::fc_exception_message_contains(expected));
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
