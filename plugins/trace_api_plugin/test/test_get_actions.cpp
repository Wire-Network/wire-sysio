#include <boost/test/unit_test.hpp>

#include <limits>
#include <set>
#include <stdexcept>

#include <fc/filesystem.hpp>

#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/trace_api/bloom_sidecar.hpp>
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
         fixture.bump_scan_guard();
         auto it = fixture.blocks.find(height);
         if (it == fixture.blocks.end()) return {};
         // pending_blocks is a per-block override for tests that need to exercise
         // the "pending" branch of block_status emission. Default is irreversible.
         const bool irreversible = fixture.pending_blocks.find(height) == fixture.pending_blocks.end();
         return std::make_tuple(data_log_entry{it->second}, irreversible);
      }

      // Stride/slice mapping is a fixture knob so tests can exercise the per-slice bloom skip path with a small
      // stride rather than the production default of 10,000 blocks.
      uint32_t slice_stride() const noexcept { return fixture.mock_slice_stride; }
      // Not noexcept: bump_scan_guard() may throw to break a non-terminating scan (see scan_guard).
      uint32_t slice_number(uint32_t block_num) const { fixture.bump_scan_guard(); return block_num / fixture.mock_slice_stride; }

      // Default: no sidecar -> invalid bloom_reader -> may_contain_* returns true -> caller scans as before.  Tests
      // that want to exercise skipping install a function that returns a valid reader for specific slices.
      bloom_reader get_bloom(uint32_t slice_number) const {
         return fixture.mock_get_bloom(slice_number);
      }

      get_actions_fixture& fixture;
   };

   struct mock_data_handler_provider {
      mock_data_handler_provider(get_actions_fixture& f) : fixture(f) {}

      std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant(const action_trace_v0& a) {
         return fixture.mock_data_handler(a);
      }

      // Production shared_provider exposes decode(); the request_handler's
      // get_actions path calls decode() directly so a decode_error field can
      // be surfaced on failure.  The mock builds a decode_result from the
      // tuple returned by mock_data_handler -- decode never fails in tests.
      abi_data_handler::decode_result decode(const action_trace_v0& a) {
         auto [params, return_data] = fixture.mock_data_handler(a);
         abi_data_handler::decode_result r;
         r.params      = std::move(params);
         r.return_data = std::move(return_data);
         r.status      = r.params.is_null()
                           ? abi_data_handler::decode_status::not_attempted
                           : abi_data_handler::decode_status::ok;
         return r;
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

   actions_result get_token_transfer_actions(const action_query& query) {
      return impl.get_token_transfer_actions(query);
   }

   // Default: no ABI decoding — params/return_data absent from result
   std::function<std::tuple<fc::variant, std::optional<fc::variant>>(const action_trace_v0&)>
   mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      return {};
   };

   std::map<uint32_t, block_trace_v0> blocks;
   std::set<uint32_t>                 pending_blocks; // blocks that should report "pending" instead of the default "irreversible"
   uint32_t mock_slice_stride = 10;
   std::function<bloom_reader(uint32_t)> mock_get_bloom = [](uint32_t) { return bloom_reader{}; };

   // Optional runaway-scan guard.  When non-zero, the mock throws once get_block + slice_number have
   // together been called more than this many times.  This turns a scan loop that fails to terminate
   // (e.g. a uint32_t block_num wrapping at UINT32_MAX) into a fast, localized failure instead of a hang
   // that would stall the entire test binary.  Disabled (0) by default so the other tests are unaffected.
   uint64_t scan_guard = 0;
   uint64_t scan_calls = 0;
   void bump_scan_guard() {
      if (scan_guard != 0 && ++scan_calls > scan_guard)
         throw std::runtime_error("get_actions_impl exceeded scan guard -- probable non-terminating scan");
   }

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

// Per-trx cpu / net totals are emitted on every action variant in the full
// (get_actions) shape, alongside trx_id / block_num / block_time /
// producer_block_id, so callers can attribute resource usage to the parent
// transaction without a separate lookup.  Deliberately distinct from
// action-level cpu_usage_us / net_usage which are per-action and in different
// units (action net_usage is bytes; trx net_usage_words is ceil/8).
BOOST_FIXTURE_TEST_CASE(emits_trx_resource_totals_in_full_shape, get_actions_fixture)
{
   transaction_trace_v0 trx = make_trx(TRX1, 1, {
      make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),
      make_action(2, "bob"_n,         "sysio.token"_n, "transfer"_n)
   });
   trx.cpu_usage_us    = 1234;
   trx.net_usage_words = fc::unsigned_int{56};
   blocks[1] = make_block(1, { std::move(trx) });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 2u);
   for (const auto& a : r.actions) {
      const auto& obj = a.get_object();
      BOOST_TEST(obj["trx_cpu_usage_us"].as<uint32_t>()    == 1234u);
      BOOST_TEST(obj["trx_net_usage_words"].as<uint32_t>() == 56u);
   }
}

// Slim (get_token_transfers) omits all resource fields - both action-level
// cpu_usage_us / net_usage and the trx-level totals.  Per-trx context
// (trx_id, block_num, etc.) still appears so transfers can be located in the
// chain.
BOOST_FIXTURE_TEST_CASE(slim_shape_omits_trx_resource_totals, get_actions_fixture)
{
   transaction_trace_v0 trx = make_trx(TRX1, 1, {
      make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n)
   });
   trx.cpu_usage_us    = 1234;
   trx.net_usage_words = fc::unsigned_int{56};
   blocks[1] = make_block(1, { std::move(trx) });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "sysio.token"_n;
   q.account         = "sysio.token"_n;
   q.action          = "transfer"_n;

   auto r = get_token_transfer_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   const auto& obj = r.actions[0].get_object();
   BOOST_TEST(obj.contains("trx_id"));
   BOOST_TEST(!obj.contains("trx_cpu_usage_us"));
   BOOST_TEST(!obj.contains("trx_net_usage_words"));
   BOOST_TEST(!obj.contains("cpu_usage_us"));
   BOOST_TEST(!obj.contains("net_usage"));
}

// Every action carries a block_status mirroring get_block's "status" field, sourced from the same
// data log tuple so trace_api remains the single source of truth for "is this action's block final."
// Both shapes (full / slim) emit it: an exchange consuming get_token_transfers needs finality just
// as much as a general consumer of get_actions. Operators that want only-irreversible responses can
// run nodeop in irreversible mode -- every block returned will then carry "irreversible".
BOOST_FIXTURE_TEST_CASE(emits_block_status_per_action, get_actions_fixture)
{
   blocks[1] = make_block(1, { make_trx(TRX1, 1, { make_action(1, "a"_n, "tok"_n, "transfer"_n) }) });
   blocks[2] = make_block(2, { make_trx(TRX2, 2, { make_action(2, "a"_n, "tok"_n, "transfer"_n) }) });
   pending_blocks.insert(2); // block 1 irreversible, block 2 still pending

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 2;

   auto r_full = get_actions(q);
   BOOST_REQUIRE_EQUAL(r_full.actions.size(), 2u);
   BOOST_TEST(r_full.actions[0].get_object()["block_status"].as_string() == "irreversible");
   BOOST_TEST(r_full.actions[1].get_object()["block_status"].as_string() == "pending");

   q.receiver = "a"_n;
   q.account  = "tok"_n;
   q.action   = "transfer"_n;
   auto r_slim = get_token_transfer_actions(q);
   BOOST_REQUIRE_EQUAL(r_slim.actions.size(), 2u);
   BOOST_TEST(r_slim.actions[0].get_object()["block_status"].as_string() == "irreversible");
   BOOST_TEST(r_slim.actions[1].get_object()["block_status"].as_string() == "pending");
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

// Per-slice bloom skip: a valid bloom that does not contain the queried receiver causes get_actions_impl to advance
// past the entire slice without scanning any of its blocks.  The fixture observes "no scan" by having get_block
// return a single well-known action in every block; if the scan ran, the result would include that action.
BOOST_FIXTURE_TEST_CASE(bloom_skips_entire_slice_when_receiver_absent, get_actions_fixture) {
   fc::temp_directory tempdir;

   // Three slices of 10 blocks each; populate every block so a non-skipped scan would always find the single action.
   mock_slice_stride = 10;
   for (uint32_t n = 1; n < 30; ++n) {
      blocks[n] = make_block(n, { make_trx(TRX1, n, { make_action(n, "alice"_n, "alice"_n, "transfer"_n) }) });
   }

   // Build bloom sidecars for slices 0, 1, 2.  Slice 1 is the only one that contains alice; slices 0 and 2 have no
   // receivers at all (empty blooms -> every probe misses).
   auto bloom_for = [&tempdir](std::size_t idx, bool with_alice) {
      bloom_builder b;
      if (with_alice) {
         action_trace_v0 a{};
         a.receiver = "alice"_n;
         a.account  = "alice"_n;
         a.action   = "transfer"_n;
         b.add_action(a);
      }
      const auto path = tempdir.path() / ("bloom_slice_" + std::to_string(idx) + ".log");
      b.finalize_and_write(path);
      return path;
   };
   const auto slice0_path = bloom_for(0, /*with_alice=*/false);
   const auto slice1_path = bloom_for(1, /*with_alice=*/true);
   const auto slice2_path = bloom_for(2, /*with_alice=*/false);

   mock_get_bloom = [slice0_path, slice1_path, slice2_path](uint32_t slice) -> bloom_reader {
      switch (slice) {
         case 0: return bloom_reader{slice0_path};
         case 1: return bloom_reader{slice1_path};
         case 2: return bloom_reader{slice2_path};
         default: return bloom_reader{};
      }
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 29;
   q.receiver        = "alice"_n;

   auto r = get_actions(q);

   // All hits come from slice 1 (blocks 10..19).  Slices 0 and 2 were bloom-skipped without any get_block call.
   BOOST_REQUIRE_EQUAL(r.actions.size(), 10u);
   for (const auto& a : r.actions) {
      const auto block_num = a.get_object()["block_num"].as_uint64();
      BOOST_TEST(block_num >= 10u);
      BOOST_TEST(block_num <= 19u);
   }
}

// Sanity check that a query with no filter cannot bloom-skip: even if the mock would return an empty bloom for
// every slice, we still scan because there's nothing to probe against.  Without this behaviour callers would see
// empty results on unfiltered queries once sidecars exist.
BOOST_FIXTURE_TEST_CASE(bloom_not_consulted_when_no_filter, get_actions_fixture) {
   mock_slice_stride = 10;
   for (uint32_t n = 1; n < 15; ++n) {
      blocks[n] = make_block(n, { make_trx(TRX1, n, { make_action(n, "alice"_n, "alice"_n, "transfer"_n) }) });
   }
   // If the handler ever calls get_bloom under this configuration, fail loudly.
   mock_get_bloom = [](uint32_t) -> bloom_reader {
      BOOST_FAIL("get_bloom should not be called when no filter is set");
      return bloom_reader{};
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 14;
   auto r = get_actions(q);
   BOOST_CHECK_EQUAL(r.actions.size(), 14u);
}

// Regression: block_num_end defaults to (and the HTTP layer accepts) UINT32_MAX, which is unvalidated
// client input.  The scan loop drove a uint32_t counter, so once block_num reached UINT32_MAX the
// ++block_num wrapped back to 0 and the loop ran forever, re-scanning the whole range on every wrap.  A
// single request with a high block_num_start and the default end was enough to hang the API thread.  A
// 64-bit loop counter terminates correctly.  scan_guard turns a re-introduced wrap into a fast failure.
BOOST_FIXTURE_TEST_CASE(scan_terminates_at_uint32_max_end, get_actions_fixture)
{
   constexpr uint32_t max_block = std::numeric_limits<uint32_t>::max();
   blocks[max_block] = make_block(max_block, {
      make_trx(TRX1, max_block, { make_action(1, "a"_n, "tok"_n, "transfer"_n) })
   });

   scan_guard = 1000; // far above the 3-block range below; trips fast if the counter wraps.

   action_query q;
   q.block_num_start = max_block - 2; // only blocks [max-2, max] are in range
   q.block_num_end   = max_block;     // the value that used to wrap the uint32_t counter

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["block_num"].as<uint32_t>() == max_block);
}

// Regression for the per-slice bloom skip-jump near the top of the range.  slice_last was computed as
// (slice+1)*stride - 1 in uint32_t; for block_num near UINT32_MAX, (slice+1)*stride overflows and wraps
// to a small value, so std::min(slice_last, end) drove block_num *backwards* and the scan looped forever.
// Computing the jump in 64-bit keeps it monotonic.  An empty-but-valid bloom makes every slice
// skip-eligible, so the very first iteration exercises the jump from the top of the range.
BOOST_FIXTURE_TEST_CASE(bloom_skip_jump_terminates_near_uint32_max, get_actions_fixture)
{
   fc::temp_directory tempdir;
   mock_slice_stride = 10;

   // Empty bloom: valid() is true, every receiver probe misses -> every slice is skip-eligible.
   bloom_builder b;
   const auto bloom_path = tempdir.path() / "empty_bloom.log";
   b.finalize_and_write(bloom_path);
   mock_get_bloom = [bloom_path](uint32_t) -> bloom_reader { return bloom_reader{bloom_path}; };

   scan_guard = 1000; // trips fast if the slice jump sends block_num backwards and re-scans.

   action_query q;
   q.block_num_start = std::numeric_limits<uint32_t>::max() - 5;
   q.block_num_end   = std::numeric_limits<uint32_t>::max();
   q.receiver        = "alice"_n; // skip_eligible == true

   // The bloom misses every slice, so nothing is scanned; the scan must still terminate.
   auto r = get_actions(q);
   BOOST_TEST(r.actions.empty());
}

// The composite (receiver, action) bloom can skip a slice even when the receiver alone is present:
// alice exists in every slice, but only slice 1 ever pairs her with "transfer".  A (receiver, action)
// query must therefore skip slices 0 and 2 on the composite probe.
BOOST_FIXTURE_TEST_CASE(bloom_skips_slice_on_recv_action_composite, get_actions_fixture) {
   fc::temp_directory tempdir;

   mock_slice_stride = 10;
   for (uint32_t n = 1; n < 30; ++n) {
      const chain::name act = (n >= 10 && n < 20) ? "transfer"_n : "other"_n;
      blocks[n] = make_block(n, { make_trx(TRX1, n, { make_action(n, "alice"_n, "alice"_n, act) }) });
   }

   auto bloom_for = [&tempdir](std::size_t idx, chain::name action) {
      bloom_builder b;
      action_trace_v0 a{};
      a.receiver = "alice"_n;
      a.account  = "alice"_n;
      a.action   = action;
      b.add_action(a);
      const auto path = tempdir.path() / ("bloom_ra_slice_" + std::to_string(idx) + ".log");
      b.finalize_and_write(path);
      return path;
   };
   const auto slice0_path = bloom_for(0, "other"_n);
   const auto slice1_path = bloom_for(1, "transfer"_n);
   const auto slice2_path = bloom_for(2, "other"_n);

   mock_get_bloom = [slice0_path, slice1_path, slice2_path](uint32_t slice) -> bloom_reader {
      switch (slice) {
         case 0: return bloom_reader{slice0_path};
         case 1: return bloom_reader{slice1_path};
         case 2: return bloom_reader{slice2_path};
         default: return bloom_reader{};
      }
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 29;
   q.receiver        = "alice"_n;
   q.action          = "transfer"_n;

   auto r = get_actions(q);

   // The receiver probe passes everywhere (alice is in every slice's bloom); only the composite
   // probe can rule slices 0 and 2 out.  All hits must come from slice 1 (blocks 10..19).
   BOOST_REQUIRE_EQUAL(r.actions.size(), 10u);
   for (const auto& a : r.actions) {
      const auto block_num = a.get_object()["block_num"].as_uint64();
      BOOST_TEST(block_num >= 10u);
      BOOST_TEST(block_num <= 19u);
   }
}

// ---------------------------------------------------------------------------
// HTTP-layer helpers (free functions shared with trace_api_plugin.cpp handlers)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(canonical_default_mirrors_single_filter) {
   // receiver only -> account mirrored
   {
      action_query q;
      q.receiver = "alice"_n;
      apply_canonical_default(q, /*include_notifications=*/false);
      BOOST_REQUIRE(q.account);
      BOOST_TEST(*q.account == "alice"_n);
   }
   // account only -> receiver mirrored
   {
      action_query q;
      q.account = "dex"_n;
      apply_canonical_default(q, /*include_notifications=*/false);
      BOOST_REQUIRE(q.receiver);
      BOOST_TEST(*q.receiver == "dex"_n);
   }
   // both set -> untouched
   {
      action_query q;
      q.receiver = "alice"_n;
      q.account  = "dex"_n;
      apply_canonical_default(q, /*include_notifications=*/false);
      BOOST_TEST(*q.receiver == "alice"_n);
      BOOST_TEST(*q.account  == "dex"_n);
   }
   // neither set -> untouched
   {
      action_query q;
      apply_canonical_default(q, /*include_notifications=*/false);
      BOOST_CHECK(!q.receiver);
      BOOST_CHECK(!q.account);
   }
   // include_notifications opts out of mirroring entirely
   {
      action_query q;
      q.receiver = "alice"_n;
      apply_canonical_default(q, /*include_notifications=*/true);
      BOOST_CHECK(!q.account);
   }
}

BOOST_AUTO_TEST_CASE(clamp_query_range_bounds_span) {
   // Within range: untouched.
   {
      action_query q;
      q.block_num_start = 100;
      q.block_num_end   = 150;
      clamp_query_range(q, 1000);
      BOOST_TEST(q.block_num_end == 150u);
   }
   // Over range: clamped to start + range - 1.
   {
      action_query q;
      q.block_num_start = 100;
      q.block_num_end   = std::numeric_limits<uint32_t>::max();
      clamp_query_range(q, 1000);
      BOOST_TEST(q.block_num_end == 1099u);
   }
   // Near the top of the uint32 range: 64-bit math keeps end at the requested value instead of wrapping.
   {
      action_query q;
      q.block_num_start = std::numeric_limits<uint32_t>::max() - 2;
      q.block_num_end   = std::numeric_limits<uint32_t>::max();
      clamp_query_range(q, 1000);
      BOOST_TEST(q.block_num_end == std::numeric_limits<uint32_t>::max());
   }
}

BOOST_AUTO_TEST_CASE(clamp_query_end_to_recorded_bounds_scan) {
   // Recorded watermark inside the window: end pulled back to it.
   {
      action_query q;
      q.block_num_start = 100;
      q.block_num_end   = 1099;
      clamp_query_end_to_recorded(q, /*last_recorded=*/500);
      BOOST_TEST(q.block_num_end == 500u);
   }
   // Watermark beyond the window: untouched.
   {
      action_query q;
      q.block_num_start = 100;
      q.block_num_end   = 1099;
      clamp_query_end_to_recorded(q, /*last_recorded=*/5000);
      BOOST_TEST(q.block_num_end == 1099u);
   }
   // Nothing recorded in the window yet: collapses to just below start so resume-at-end+1 retries.
   {
      action_query q;
      q.block_num_start = 100;
      q.block_num_end   = 1099;
      clamp_query_end_to_recorded(q, /*last_recorded=*/50);
      BOOST_TEST(q.block_num_end == 99u);
   }
   // Degenerate start == 0 with an empty store: end pinned at 0 (no uint32 underflow).
   {
      action_query q;
      q.block_num_start = 0;
      q.block_num_end   = 999;
      clamp_query_end_to_recorded(q, /*last_recorded=*/0);
      BOOST_TEST(q.block_num_end == 0u);
   }
}

BOOST_AUTO_TEST_SUITE_END()