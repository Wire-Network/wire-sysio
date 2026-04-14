#include <boost/test/unit_test.hpp>

#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

struct get_actions_fixture {
   struct mock_logfile_provider {
      mock_logfile_provider(get_actions_fixture& f) : fixture(f) {}

      get_block_t get_block(uint32_t height) {
         auto it = fixture.blocks.find(height);
         if (it == fixture.blocks.end()) return {};
         return std::make_tuple(data_log_entry{it->second}, true /*irreversible*/);
      }

      get_actions_fixture& fixture;
   };

   struct mock_data_handler_provider {
      mock_data_handler_provider(get_actions_fixture& f) : fixture(f) {}

      std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant(const action_trace_v0& a) {
         return fixture.mock_data_handler(a);
      }

      get_actions_fixture& fixture;
   };

   using impl_type = request_handler<mock_logfile_provider, mock_data_handler_provider>;

   get_actions_fixture()
   : impl(mock_logfile_provider(*this), mock_data_handler_provider(*this),
          [](const std::string& msg){ fc_dlog(fc::logger::default_logger(), "{}", msg); })
   {}

   actions_result get_actions(const action_query& query) {
      return impl.get_actions(query);
   }

   // Default: no ABI decoding — params/return_data absent from result
   std::function<std::tuple<fc::variant, std::optional<fc::variant>>(const action_trace_v0&)>
   mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      return {};
   };

   std::map<uint32_t, block_trace_v0> blocks;
   impl_type impl;
};

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

action_trace_v0 make_action(uint64_t seq, chain::name receiver, chain::name account,
                             chain::name act, chain::bytes data = {}) {
   action_trace_v0 a{};
   a.global_sequence = seq;
   a.receiver        = receiver;
   a.account         = account;
   a.action          = act;
   a.data            = std::move(data);
   return a;
}

transaction_trace_v0 make_trx(chain::transaction_id_type id, uint32_t block_num,
                               std::vector<action_trace_v0> actions) {
   transaction_trace_v0 trx;
   trx.id       = id;
   trx.actions  = std::move(actions);
   trx.block_num  = block_num;
   trx.block_time = chain::block_timestamp_type(0);
   return trx;
}

block_trace_v0 make_block(uint32_t num, std::vector<transaction_trace_v0> trxs) {
   block_trace_v0 blk;
   blk.number       = num;
   blk.producer     = "bp.one"_n;
   blk.transactions = std::move(trxs);
   return blk;
}

// Convenience: one action per block, sequential global_sequences
static const chain::transaction_id_type TRX1 =
   "0000000000000000000000000000000000000000000000000000000000000001"_h;
static const chain::transaction_id_type TRX2 =
   "0000000000000000000000000000000000000000000000000000000000000002"_h;
static const chain::transaction_id_type TRX3 =
   "0000000000000000000000000000000000000000000000000000000000000003"_h;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test suite
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(get_actions_tests)

// No blocks in the queried range -> empty result
BOOST_FIXTURE_TEST_CASE(empty_range, get_actions_fixture)
{
   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 10;

   auto r = get_actions(q);

   BOOST_TEST(r.actions.empty());
}

// Filter that matches nothing returns empty result
BOOST_FIXTURE_TEST_CASE(no_matching_filter, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, { make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n) })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "alice"_n;  // alice is not a receiver in this block

   auto r = get_actions(q);

   BOOST_TEST(r.actions.empty());
}

// receiver filter: keeps only actions where receiver matches
BOOST_FIXTURE_TEST_CASE(filter_by_receiver, get_actions_fixture)
{
   // Two actions: original transfer (receiver=sysio.token) + inline notification (receiver=bob)
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),
         make_action(2, "bob"_n,         "sysio.token"_n, "transfer"_n)
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "sysio.token"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
}

// account filter: keeps only actions where account (code) matches
BOOST_FIXTURE_TEST_CASE(filter_by_account, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "alice"_n, "sysio.token"_n, "transfer"_n),
         make_action(2, "alice"_n, "mycontract"_n,  "foo"_n)
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.account         = "sysio.token"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["account"].as_string() == "sysio.token");
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
}

// action name filter: keeps only the named action
BOOST_FIXTURE_TEST_CASE(filter_by_action_name, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),
         make_action(2, "sysio"_n,       "sysio"_n,       "newaccount"_n)
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.action          = "transfer"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["name"].as_string() == "transfer");
}

// Actions are returned across multiple blocks; missing blocks in the log are skipped
BOOST_FIXTURE_TEST_CASE(multi_block_scan, get_actions_fixture)
{
   blocks[1] = make_block(1, { make_trx(TRX1, 1, { make_action(1, "a"_n, "tok"_n, "transfer"_n) }) });
   blocks[2] = make_block(2, { make_trx(TRX2, 2, { make_action(2, "a"_n, "tok"_n, "transfer"_n) }) });
   // block 3 is missing (gap in trace log)
   blocks[4] = make_block(4, { make_trx(TRX3, 4, { make_action(3, "a"_n, "tok"_n, "transfer"_n) }) });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 5;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 3u);
   BOOST_TEST(r.actions[0].get_object()["block_num"].as<uint32_t>() == 1u);
   BOOST_TEST(r.actions[1].get_object()["block_num"].as<uint32_t>() == 2u);
   BOOST_TEST(r.actions[2].get_object()["block_num"].as<uint32_t>() == 4u);
}

// ABI-decoded params are included in the result when the data handler returns them
BOOST_FIXTURE_TEST_CASE(abi_decoded_params_included, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n, {0x01, 0x02})
      })
   });

   mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      return { fc::mutable_variant_object()("amount", 100), std::nullopt };
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   const auto& obj = r.actions[0].get_object();
   BOOST_REQUIRE(obj.contains("params"));
   BOOST_TEST(obj["params"].get_object()["amount"].as<int>() == 100);
   BOOST_TEST(!obj.contains("return_data"));
}

// When the data handler returns null, no params field is emitted
BOOST_FIXTURE_TEST_CASE(no_params_when_handler_returns_null, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, { make_action(1, "alice"_n, "contract"_n, "foo"_n, {static_cast<char>(0xAB)}) })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   const auto& obj = r.actions[0].get_object();
   BOOST_TEST(!obj.contains("params"));
   BOOST_TEST(obj["data"].as_string() == "ab");  // raw hex always present
}

// Verifies the receiver+account+action filter used by get_token_transfers:
// exactly one result per transfer (the original; notification copy is excluded)
BOOST_FIXTURE_TEST_CASE(token_transfer_filter_excludes_notifications, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),  // original
         make_action(2, "bob"_n,         "sysio.token"_n, "transfer"_n)   // inline notification
      })
   });

   // This is the filter preset used by POST /v1/trace_api/get_token_transfers
   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "sysio.token"_n;
   q.account         = "sysio.token"_n;
   q.action          = "transfer"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
   BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
}

// Actions within a transaction are returned sorted by global_sequence (execution order),
// not the schedule order in which the chain stored them.  The divergence matters when
// an action queues both an inline AND a require_recipient notification — notifications
// execute before inlines, so the inline's global_sequence is higher than later-scheduled
// notifications'.  Sorting gives clients a consistent execution view matching what
// chain_plugin's push_transaction does and what the legacy get_block response returned.
BOOST_FIXTURE_TEST_CASE(actions_sorted_by_global_sequence, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(5, "a"_n, "tok"_n, "transfer"_n),  // schedule order: 5
         make_action(1, "a"_n, "tok"_n, "transfer"_n),  // schedule order: 1
         make_action(3, "a"_n, "tok"_n, "transfer"_n)   // schedule order: 3
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 3u);
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
   BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 3u);
   BOOST_TEST(r.actions[2].get_object()["global_sequence"].as_uint64() == 5u);
}

// Realistic scenario exercising top-level actions, notifications (receiver != account),
// inline actions triggered by a notification handler, and the inline's own notifications.
//
// Scenario: alice transfers 1 SYS to bob.contract via sysio.token::transfer.
//   bob.contract has a notif handler that fires an inline logger::log action.
//   logger::log has an audit account notification handler.
//
// Expected global_sequence order (chain-assigned, monotonic):
//   100: sysio.token::transfer     (receiver=sysio.token, account=sysio.token)  <- original
//   101: sysio.token::transfer     (receiver=alice,       account=sysio.token)  <- notif to sender
//   102: sysio.token::transfer     (receiver=bob.contract,account=sysio.token)  <- notif to recipient
//   103: logger::log               (receiver=logger,      account=logger)       <- inline from 102
//   104: logger::log               (receiver=audit,       account=logger)       <- notif from 103
BOOST_FIXTURE_TEST_CASE(complex_inline_and_notification_ordering, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(100, "sysio.token"_n,  "sysio.token"_n, "transfer"_n),
         make_action(101, "alice"_n,        "sysio.token"_n, "transfer"_n),
         make_action(102, "bob.contract"_n, "sysio.token"_n, "transfer"_n),
         make_action(103, "logger"_n,       "logger"_n,      "log"_n),
         make_action(104, "audit"_n,        "logger"_n,      "log"_n)
      })
   });

   // No filter: all 5 actions in global_sequence order.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 5u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 100u);
      BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 101u);
      BOOST_TEST(r.actions[2].get_object()["global_sequence"].as_uint64() == 102u);
      BOOST_TEST(r.actions[3].get_object()["global_sequence"].as_uint64() == 103u);
      BOOST_TEST(r.actions[4].get_object()["global_sequence"].as_uint64() == 104u);
   }

   // receiver=sysio.token: only the original execution; notifications to alice/bob excluded.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.receiver        = "sysio.token"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 100u);
      BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
   }

   // account=sysio.token: original + both notifications (3 rows), ordered by global_seq.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.account         = "sysio.token"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 3u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 100u);
      BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 101u);
      BOOST_TEST(r.actions[2].get_object()["global_sequence"].as_uint64() == 102u);
      BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
      BOOST_TEST(r.actions[1].get_object()["receiver"].as_string() == "alice");
      BOOST_TEST(r.actions[2].get_object()["receiver"].as_string() == "bob.contract");
   }

   // account=logger: the inline from bob.contract's notif handler + its notification to audit.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.account         = "logger"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 2u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 103u);
      BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 104u);
      BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "logger");
      BOOST_TEST(r.actions[1].get_object()["receiver"].as_string() == "audit");
   }

   // receiver=bob.contract + action=transfer: exactly the one notification to the recipient.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.receiver        = "bob.contract"_n;
      q.action          = "transfer"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 102u);
   }
}

BOOST_AUTO_TEST_SUITE_END()