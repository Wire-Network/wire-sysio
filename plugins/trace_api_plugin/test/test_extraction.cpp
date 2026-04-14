#include <boost/test/unit_test.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/block.hpp>

#include <sysio/trace_api/test_common.hpp>
#include <sysio/trace_api/chain_extraction.hpp>

#include <fc/bitutil.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;
using sysio::chain::name;
using sysio::chain::digest_type;

namespace {
   chain::transaction_trace_ptr make_transaction_trace( const chain::transaction_id_type& id, uint32_t block_number,
         uint32_t slot, chain::transaction_receipt_header::status_enum status, std::vector<chain::action_trace>&& actions ) {
      return std::make_shared<chain::transaction_trace>(chain::transaction_trace{
         id,
         block_number,
         chain::block_timestamp_type(slot),
         {},
         chain::transaction_receipt_header{},
         0,
         fc::microseconds(0),
         0,
         std::move(actions),
         {},
         {},
         {},
         {}
      });
   }

   auto make_transfer_action( chain::name from, chain::name to, chain::asset quantity, std::string memo ) {
      return chain::action( std::vector<chain::permission_level> {{from, chain::config::active_name}},
                            "sysio.token"_n, "transfer"_n, make_transfer_data( from, to, quantity, std::move(memo) ) );
   }

   // sysio::setabi action data: (name account, vector<char> abi).  The extraction path
   // unpacks it via fc::raw in chain_extraction.hpp.
   std::vector<char> make_setabi_data( chain::name target, const std::vector<char>& abi_bytes ) {
      fc::datastream<size_t> ps;
      fc::raw::pack(ps, target, abi_bytes);
      std::vector<char> data( ps.tellp() );
      if (!data.empty()) {
         fc::datastream<char*> ds(data.data(), data.size());
         fc::raw::pack(ds, target, abi_bytes);
      }
      return data;
   }

   auto make_setabi_action( chain::name target, const std::vector<char>& abi_bytes ) {
      return chain::action( {}, chain::config::system_account_name, "setabi"_n,
                            make_setabi_data( target, abi_bytes ) );
   }

   auto make_simple_action( chain::name account, chain::name act_name ) {
      return chain::action( {}, account, act_name, std::vector<char>{} );
   }

   auto make_packed_trx( std::vector<chain::action> actions ) {
      chain::signed_transaction trx;
      trx.actions = std::move( actions );
      return packed_transaction( std::move(trx) );
   }

    auto make_trx_header( const chain::transaction& trx ) {
        chain::transaction_header th;
        th.expiration = trx.expiration;
        th.ref_block_num = trx.ref_block_num;
        th.ref_block_prefix = trx.ref_block_prefix;
        th.max_net_usage_words = trx.max_net_usage_words;
        th.max_cpu_usage_ms = trx.max_cpu_usage_ms;
        th.delay_sec = trx.delay_sec;
        return th;
    }

   chain::action_trace make_action_trace( uint64_t global_sequence, chain::action act, chain::name receiver ) {
      chain::action_trace result;
      // don't think we need any information other than receiver and global sequence
      result.receipt.emplace(chain::action_receipt{
         receiver,
         digest_type::hash(act),
         global_sequence,
         0,
         {},
         0,
         0
      });
      result.receiver = receiver;
      result.act = std::move(act);
      return result;
   }

}

struct extraction_test_fixture {
   /**
    * MOCK implementation of the logfile input API
    */
   struct mock_logfile_provider_type {
      mock_logfile_provider_type(extraction_test_fixture& fixture)
      :fixture(fixture)
      {}

      /**
       * append an entry to the data store
       *
       * @param entry : the entry to append
       */
      template <typename BlockTrace>
      void append( const BlockTrace& entry ) {
         fixture.data_log.emplace_back(entry);
      }

      void append_lib( uint32_t lib ) {
         fixture.max_lib = std::max(fixture.max_lib, lib);
      }

      void append_trx_ids(const block_trxs_entry& tt){
         fixture.id_log[tt.block_num] = tt.ids;
      }

      std::optional<std::pair<uint32_t,uint32_t>> first_and_last_recorded_blocks() const {
         return std::nullopt; // no prior data in unit tests
      }

      void append_abi(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
         fixture.abi_calls.push_back({account, global_seq, std::move(abi_bytes)});
      }

      bool has_abi_entry(chain::name account) const {
         for (const auto& c : fixture.abi_calls)
            if (c.account == account) return true;
         return false;
      }

      extraction_test_fixture& fixture;
   };

   struct abi_call {
      chain::name       account;
      uint64_t          global_seq = 0;
      std::vector<char> abi_bytes;
   };

   extraction_test_fixture()
   : extraction_impl(mock_logfile_provider_type(*this), exception_handler{} )
   {
   }

   void signal_block_start( uint32_t block_num ) {
      extraction_impl.signal_block_start(block_num);
   }

   void signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx ) {
      extraction_impl.signal_applied_transaction(trace, ptrx);
   }

   void signal_accepted_block( const chain::signed_block_ptr& bp ) {
      extraction_impl.signal_accepted_block(bp, bp->calculate_id());
   }

   // fixture data and methods
   uint32_t max_lib = 0;
   std::vector<data_log_entry> data_log = {};
   std::unordered_map<uint32_t, std::vector<chain::transaction_id_type>> id_log;
   std::vector<abi_call> abi_calls;

   chain_extraction_impl_type<mock_logfile_provider_type> extraction_impl;
};


BOOST_AUTO_TEST_SUITE(block_extraction)

   BOOST_FIXTURE_TEST_CASE(basic_single_transaction_block, extraction_test_fixture)
   {
      auto act1 = make_transfer_action( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" );
      auto act2 = make_transfer_action( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" );
      auto act3 = make_transfer_action( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" );
      auto actt1 = make_action_trace( 0, act1, "sysio.token"_n );
      auto actt2 = make_action_trace( 1, act2, "alice"_n );
      auto actt3 = make_action_trace( 2, act3, "bob"_n );
      auto ptrx1 = make_packed_trx( { act1, act2, act3 } );

      // apply a basic transfer
      signal_applied_transaction(
            make_transaction_trace( ptrx1.id(), 1, 1, chain::transaction_receipt_header::executed,
                  { actt1, actt2, actt3 } ),
            std::make_shared<packed_transaction>(ptrx1) );

      // accept the block with one transaction
      auto bp1 = make_block( chain::block_id_type(), 1, 1, "bp.one"_n,
            { chain::packed_transaction(ptrx1) } );
      signal_accepted_block( bp1 );

      action_trace_v0 eat1{};
      eat1.global_sequence = 0;
      eat1.receiver        = "sysio.token"_n;
      eat1.account         = "sysio.token"_n;
      eat1.action          = "transfer"_n;
      eat1.authorization   = {{"alice"_n, "active"_n}};
      eat1.data            = make_transfer_data("alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!");
      eat1.cpu_usage_us    = fc::unsigned_int{0};
      eat1.net_usage       = fc::unsigned_int{0};

      action_trace_v0 eat2{};
      eat2.global_sequence = 1;
      eat2.receiver        = "alice"_n;
      eat2.account         = "sysio.token"_n;
      eat2.action          = "transfer"_n;
      eat2.authorization   = {{"alice"_n, "active"_n}};
      eat2.data            = make_transfer_data("alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!");
      eat2.cpu_usage_us    = fc::unsigned_int{0};
      eat2.net_usage       = fc::unsigned_int{0};

      action_trace_v0 eat3{};
      eat3.global_sequence = 2;
      eat3.receiver        = "bob"_n;
      eat3.account         = "sysio.token"_n;
      eat3.action          = "transfer"_n;
      eat3.authorization   = {{"alice"_n, "active"_n}};
      eat3.data            = make_transfer_data("alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!");
      eat3.cpu_usage_us    = fc::unsigned_int{0};
      eat3.net_usage       = fc::unsigned_int{0};

      const std::vector<action_trace_v0> expected_action_traces { eat1, eat2, eat3 };

      const transaction_trace_v0 expected_transaction_trace {
         ptrx1.id(),
         expected_action_traces,
         0,
         0,
         ptrx1.get_signatures(),
         make_trx_header(ptrx1.get_transaction()),
         1,                              // block_num
         chain::block_timestamp_type(1), // block_time
         {}                              // producer_block_id
      };

      const block_trace_v0 expected_block_trace {
         bp1->calculate_id(),
         1,
         bp1->previous,
         chain::block_timestamp_type(1),
         "bp.one"_n,
         bp1->transaction_mroot,
         bp1->finality_mroot,
         std::vector<transaction_trace_v0> {
            expected_transaction_trace
         }
      };

      BOOST_REQUIRE_EQUAL(max_lib, 0u);
      BOOST_REQUIRE(data_log.size() == 1u);
      BOOST_REQUIRE(std::holds_alternative<block_trace_v0>(data_log.at(0)));
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(data_log.at(0)), expected_block_trace);
      BOOST_REQUIRE_EQUAL(id_log.at(bp1->block_num()).size(),  bp1->transactions.size());
   }

   BOOST_FIXTURE_TEST_CASE(basic_multi_transaction_block, extraction_test_fixture) {
      auto act1 = make_transfer_action( "alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" );
      auto act2 = make_transfer_action( "bob"_n, "alice"_n, "0.0001 SYS"_t, "Memo!" );
      auto act3 = make_transfer_action( "fred"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" );
      auto actt1 = make_action_trace( 0, act1, "sysio.token"_n );
      auto actt2 = make_action_trace( 1, act2, "bob"_n );
      auto actt3 = make_action_trace( 2, act3, "fred"_n );
      auto ptrx1 = make_packed_trx( { act1 } );
      auto ptrx2 = make_packed_trx( { act2 } );
      auto ptrx3 = make_packed_trx( { act3 } );

      signal_applied_transaction(
            make_transaction_trace( ptrx1.id(), 1, 1, chain::transaction_receipt_header::executed,
                  { actt1 } ),
            std::make_shared<packed_transaction>( ptrx1 ) );
      signal_applied_transaction(
            make_transaction_trace( ptrx2.id(), 1, 1, chain::transaction_receipt_header::executed,
                  { actt2 } ),
            std::make_shared<packed_transaction>( ptrx2 ) );
      signal_applied_transaction(
            make_transaction_trace( ptrx3.id(), 1, 1, chain::transaction_receipt_header::executed,
                  { actt3 } ),
            std::make_shared<packed_transaction>( ptrx3 ) );

      // accept the block with three transaction
      auto bp1 = make_block( chain::block_id_type(), 1, 1, "bp.one"_n,
            { chain::packed_transaction(ptrx1), chain::packed_transaction(ptrx2), chain::packed_transaction(ptrx3) } );
      signal_accepted_block( bp1 );

      action_trace_v0 eat1{};
      eat1.global_sequence = 0;
      eat1.receiver        = "sysio.token"_n;
      eat1.account         = "sysio.token"_n;
      eat1.action          = "transfer"_n;
      eat1.authorization   = {{"alice"_n, "active"_n}};
      eat1.data            = make_transfer_data("alice"_n, "bob"_n, "0.0001 SYS"_t, "Memo!");
      eat1.cpu_usage_us    = fc::unsigned_int{0};
      eat1.net_usage       = fc::unsigned_int{0};

      action_trace_v0 eat2{};
      eat2.global_sequence = 1;
      eat2.receiver        = "bob"_n;
      eat2.account         = "sysio.token"_n;
      eat2.action          = "transfer"_n;
      eat2.authorization   = {{ "bob"_n, "active"_n }};
      eat2.data            = make_transfer_data( "bob"_n, "alice"_n, "0.0001 SYS"_t, "Memo!" );
      eat2.cpu_usage_us    = fc::unsigned_int{0};
      eat2.net_usage       = fc::unsigned_int{0};

      action_trace_v0 eat3{};
      eat3.global_sequence = 2;
      eat3.receiver        = "fred"_n;
      eat3.account         = "sysio.token"_n;
      eat3.action          = "transfer"_n;
      eat3.authorization   = {{ "fred"_n, "active"_n }};
      eat3.data            = make_transfer_data( "fred"_n, "bob"_n, "0.0001 SYS"_t, "Memo!" );
      eat3.cpu_usage_us    = fc::unsigned_int{0};
      eat3.net_usage       = fc::unsigned_int{0};

      const std::vector<action_trace_v0> expected_action_trace1 { eat1 };
      const std::vector<action_trace_v0> expected_action_trace2 { eat2 };
      const std::vector<action_trace_v0> expected_action_trace3 { eat3 };

      const std::vector<transaction_trace_v0> expected_transaction_traces {
         {
            ptrx1.id(),
            expected_action_trace1,
            0,
            0,
            ptrx1.get_signatures(),
            make_trx_header(ptrx1.get_transaction()),
            1,                              // block_num
            chain::block_timestamp_type(1), // block_time
            {}                              // producer_block_id
         },
         {
            ptrx2.id(),
            expected_action_trace2,
            0,
            0,
            ptrx2.get_signatures(),
            make_trx_header(ptrx2.get_transaction()),
            1,                              // block_num
            chain::block_timestamp_type(1), // block_time
            {}                              // producer_block_id
         },
         {
            ptrx3.id(),
            expected_action_trace3,
            0,
            0,
            ptrx3.get_signatures(),
            make_trx_header(ptrx3.get_transaction()),
            1,                              // block_num
            chain::block_timestamp_type(1), // block_time
            {}                              // producer_block_id
         }
      };

      const block_trace_v0 expected_block_trace {
         bp1->calculate_id(),
         1,
         bp1->previous,
         chain::block_timestamp_type(1),
         "bp.one"_n,
         bp1->transaction_mroot,
         bp1->finality_mroot,
         expected_transaction_traces
      };

      BOOST_REQUIRE_EQUAL(max_lib, 0u);
      BOOST_REQUIRE(data_log.size() == 1u);
      BOOST_REQUIRE(std::holds_alternative<block_trace_v0>(data_log.at(0)));
      BOOST_REQUIRE_EQUAL(std::get<block_trace_v0>(data_log.at(0)), expected_block_trace);
   }

BOOST_AUTO_TEST_SUITE_END()


// ---------------------------------------------------------------------------
// ABI capture: lazy fetch + setabi interaction
// ---------------------------------------------------------------------------

struct abi_capture_fixture {
   struct mock_store {
      explicit mock_store(abi_capture_fixture& f) : fixture(f) {}

      template <typename BT> void append(const BT&) {}
      void append_lib(uint32_t) {}
      void append_trx_ids(const block_trxs_entry&) {}
      std::optional<std::pair<uint32_t,uint32_t>> first_and_last_recorded_blocks() const { return std::nullopt; }

      void append_abi(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
         fixture.abi_calls.push_back({account, global_seq, std::move(abi_bytes)});
      }

      bool has_abi_entry(chain::name account) const {
         for (const auto& c : fixture.abi_calls)
            if (c.account == account) return true;
         return false;
      }

      abi_capture_fixture& fixture;
   };

   struct abi_call {
      chain::name       account;
      uint64_t          global_seq = 0;
      std::vector<char> abi_bytes;
   };

   using extraction_t = chain_extraction_impl_type<mock_store>;

   // Fetcher returns whatever is keyed in fetcher_state (mimics reading the
   // chain DB's account_metadata_object post-apply).
   std::map<chain::name, std::vector<char>> fetcher_state;

   extraction_t::abi_fetcher_t make_fetcher() {
      return [this](chain::name account) -> std::optional<std::vector<char>> {
         auto it = fetcher_state.find(account);
         if (it == fetcher_state.end()) return std::nullopt;
         return it->second;
      };
   }

   std::vector<abi_call> abi_calls;

   // Feed a pre-built transaction through the extraction impl.  Returns the extraction
   // so additional trxs can be fed through the same instance (mimicking multi-trx flows).
   std::unique_ptr<extraction_t> extraction = std::make_unique<extraction_t>(
      mock_store{*this}, exception_handler{}, make_fetcher());

   void signal(const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx) {
      extraction->signal_applied_transaction(trace, ptrx);
   }
};


BOOST_AUTO_TEST_SUITE(abi_capture_tests)

// Trx = [X.foo, sysio.setabi(X, newAbi), X.bar].  X has NEVER been observed before.
// Without the fix: lazy fetch on iter 1 would read post-apply state (newAbi) and
// record X@0=newAbi, which would be served for X.foo's pre-setabi global_seq and
// decode with the wrong schema.  Fix: scan for setabi targets first and skip
// lazy fetch for them.  X.foo remains undecodable (there's no pre-setabi ABI
// anywhere reachable), but returning raw hex is strictly safer than wrong data.
BOOST_FIXTURE_TEST_CASE(lazy_fetch_skipped_for_same_trx_setabi_target, abi_capture_fixture)
{
   auto new_abi = std::vector<char>{'n', 'e', 'w'};
   fetcher_state["x"_n] = new_abi; // what a post-apply lazy fetch would return

   auto x_foo_action  = make_simple_action("x"_n, "foo"_n);
   auto setabi_action = make_setabi_action("x"_n, new_abi);
   auto x_bar_action  = make_simple_action("x"_n, "bar"_n);

   auto x_foo  = make_action_trace(100, x_foo_action,  "x"_n);
   auto setabi = make_action_trace(101, setabi_action, chain::config::system_account_name);
   auto x_bar  = make_action_trace(102, x_bar_action,  "x"_n);

   auto ptrx  = make_packed_trx({ x_foo_action, setabi_action, x_bar_action });
   auto trace = make_transaction_trace(
      ptrx.id(), 1, 1, chain::transaction_receipt_header::executed,
      { x_foo, setabi, x_bar });

   signal(trace, std::make_shared<packed_transaction>(ptrx));

   // Exactly one append: the setabi at its own global_sequence.
   // No X@0 (the poisoning case), and NO X@100/102 (those are not setabis).
   BOOST_REQUIRE_EQUAL(abi_calls.size(), 1u);
   BOOST_TEST(abi_calls[0].account    == "x"_n);
   BOOST_TEST(abi_calls[0].global_seq == 101u);
   BOOST_TEST(abi_calls[0].abi_bytes  == new_abi);
}

// Common case: X already has a prior setabi record (from an earlier trx), so
// _seen_accounts contains X.  A later trx does [X.foo, setabi(X, newAbi), X.bar].
// The lazy fetch is a no-op (X already seen); the setabi record is appended.
// After the trx, the in-memory log should contain BOTH records, so a lookup
// for X.foo's global_sequence (< setabi_seq) resolves to the old ABI via
// upper_bound step-back in abi_log.  This is the fix's key property: it does
// not clobber the prior entry that lets pre-setabi actions decode correctly.
BOOST_FIXTURE_TEST_CASE(prior_setabi_survives_later_setabi_in_same_trx, abi_capture_fixture)
{
   auto old_abi = std::vector<char>{'o', 'l', 'd'};
   auto new_abi = std::vector<char>{'n', 'e', 'w'};

   // Trx 1: the original setabi that registered X@50=old_abi.
   {
      auto setabi_old = make_setabi_action("x"_n, old_abi);
      auto setabi_old_trace = make_action_trace(50, setabi_old, chain::config::system_account_name);
      auto ptrx = make_packed_trx({ setabi_old });
      auto trace = make_transaction_trace(
         ptrx.id(), 1, 1, chain::transaction_receipt_header::executed,
         { setabi_old_trace });
      signal(trace, std::make_shared<packed_transaction>(ptrx));
   }

   BOOST_REQUIRE_EQUAL(abi_calls.size(), 1u);
   BOOST_TEST(abi_calls[0].account    == "x"_n);
   BOOST_TEST(abi_calls[0].global_seq == 50u);
   BOOST_TEST(abi_calls[0].abi_bytes  == old_abi);

   // Fetcher now returns newAbi since in reality the chain DB has been updated.
   fetcher_state["x"_n] = new_abi;

   // Trx 2: X.foo (ran under oldAbi), setabi(X, newAbi), X.bar (ran under newAbi).
   {
      auto x_foo_action  = make_simple_action("x"_n, "foo"_n);
      auto setabi_action = make_setabi_action("x"_n, new_abi);
      auto x_bar_action  = make_simple_action("x"_n, "bar"_n);

      auto x_foo  = make_action_trace(200, x_foo_action,  "x"_n);
      auto setabi = make_action_trace(201, setabi_action, chain::config::system_account_name);
      auto x_bar  = make_action_trace(202, x_bar_action,  "x"_n);

      auto ptrx = make_packed_trx({ x_foo_action, setabi_action, x_bar_action });
      auto trace = make_transaction_trace(
         ptrx.id(), 1, 1, chain::transaction_receipt_header::executed,
         { x_foo, setabi, x_bar });
      signal(trace, std::make_shared<packed_transaction>(ptrx));
   }

   // Expect exactly two appends total: the prior setabi plus the new one.
   // No spurious X@0 lazy-fetch that would poison X.foo's lookup.
   BOOST_REQUIRE_EQUAL(abi_calls.size(), 2u);
   BOOST_TEST(abi_calls[1].account    == "x"_n);
   BOOST_TEST(abi_calls[1].global_seq == 201u);
   BOOST_TEST(abi_calls[1].abi_bytes  == new_abi);

   // Hand-check of the lookup contract (which abi_log tests cover end-to-end):
   //   lookup(X, 200) -> upper_bound finds X@201, step back to X@50 = OLD ABI.  Correct.
   //   lookup(X, 201) -> upper_bound finds > X@201 (none), step back to X@201 = NEW.  Correct.
   //   lookup(X, 202) -> same as above, NEW.  Correct.
}

// An unrelated account in the same trx as a setabi should still get a lazy
// fetch — the skip is narrowly scoped to the setabi target.
BOOST_FIXTURE_TEST_CASE(lazy_fetch_fires_for_non_setabi_target_in_same_trx, abi_capture_fixture)
{
   auto y_abi   = std::vector<char>{'y'};
   auto x_abi   = std::vector<char>{'x'};
   fetcher_state["y"_n] = y_abi;

   auto y_foo_action  = make_simple_action("y"_n, "foo"_n);
   auto setabi_action = make_setabi_action("x"_n, x_abi);

   auto y_foo  = make_action_trace(300, y_foo_action,  "y"_n);
   auto setabi = make_action_trace(301, setabi_action, chain::config::system_account_name);

   auto ptrx = make_packed_trx({ y_foo_action, setabi_action });
   auto trace = make_transaction_trace(
      ptrx.id(), 1, 1, chain::transaction_receipt_header::executed,
      { y_foo, setabi });
   signal(trace, std::make_shared<packed_transaction>(ptrx));

   // Two appends: Y@0 lazy fetch (Y isn't a setabi target) and X@301 setabi.
   BOOST_REQUIRE_EQUAL(abi_calls.size(), 2u);
   BOOST_TEST(abi_calls[0].account    == "y"_n);
   BOOST_TEST(abi_calls[0].global_seq == 0u);
   BOOST_TEST(abi_calls[0].abi_bytes  == y_abi);
   BOOST_TEST(abi_calls[1].account    == "x"_n);
   BOOST_TEST(abi_calls[1].global_seq == 301u);
   BOOST_TEST(abi_calls[1].abi_bytes  == x_abi);
}

BOOST_AUTO_TEST_SUITE_END()