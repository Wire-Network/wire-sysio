#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/snapshot_detail.hpp>
#include <sysio/chain/block_handle.hpp>
#include <sysio/chain/genesis_state.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace testing;
using namespace chain;

// Accessor reaches into block_handle's private internal() for tests that need
// the underlying block_state_ptr. block_handle declares friend block_handle_accessor.
namespace sysio::chain {
   struct block_handle_accessor {
      static const block_state_ptr& get_bsp(const block_handle& h) { return h.internal(); }
   };
}

BOOST_AUTO_TEST_SUITE(block_tests)

BOOST_AUTO_TEST_CASE( block_with_invalid_tx_test )
{
   savanna_tester main;

   // First we create a valid block with valid transaction
   main.create_account("newacc"_n, config::system_account_name, false, true, false);
   auto b = main.produce_block();

   // Make a copy of the valid block and corrupt the transaction
   auto copy_b = b->clone();
   auto signed_tx = copy_b->transactions.back().trx.get_signed_transaction();
   auto it = std::ranges::find_if(signed_tx.actions, [](const action& a) { return a.name == "newaccount"_n; });
   BOOST_REQUIRE(it != signed_tx.actions.end());
   auto& act = *it;
   auto act_data = act.template data_as<newaccount>();
   // Make the transaction invalid by having the new account name the same as the creator name
   act_data.name = act_data.creator;
   act.data = fc::raw::pack(act_data);
   // Re-sign the transaction
   signed_tx.signatures.clear();
   signed_tx.sign(main.get_private_key(config::system_account_name, "active"), main.get_chain_id());
   // Replace the valid transaction with the invalid transaction
   auto invalid_packed_tx = packed_transaction(signed_tx);
   copy_b->transactions.back().trx = std::move(invalid_packed_tx);

   // Re-calculate the transaction merkle
   deque<digest_type> trx_digests;
   const auto& trxs = copy_b->transactions;
   for( const auto& a : trxs )
      trx_digests.emplace_back( a.digest() );
   copy_b->transaction_mroot = calculate_merkle( std::move(trx_digests) );

   // Re-sign the block
   copy_b->producer_signatures = {main.get_private_key(config::system_account_name, "active").sign(copy_b->calculate_id())};

   // Push block with invalid transaction to other chain
   savanna_tester validator;
   auto signed_copy_b = signed_block::create_signed_block(std::move(copy_b));
   auto [best_head, obh] = validator.control->accept_block( signed_copy_b->calculate_id(), signed_copy_b );
   BOOST_REQUIRE(obh);
   validator.control->abort_block();
   BOOST_REQUIRE_EXCEPTION(validator.control->apply_blocks( {}, trx_meta_cache_lookup{} ), fc::exception ,
   [] (const fc::exception &e)->bool {
      return e.code() == account_name_exists_exception::code_value ;
   }) ;

}

BOOST_AUTO_TEST_CASE( block_with_invalid_tx_mroot_test )
{
   savanna_tester main;

   // First we create a valid block with valid transaction
   main.create_account("newacc"_n, config::system_account_name, false, true, false);
   auto b = main.produce_block();

   // Make a copy of the valid block and corrupt the transaction
   auto copy_b = b->clone();
   const auto& packed_trx = copy_b->transactions.back().trx;
   auto signed_tx = packed_trx.get_signed_transaction();

   // Change the transaction that will be run
   signed_tx.actions[0].name = "something"_n;
   // Re-sign the transaction
   signed_tx.signatures.clear();
   signed_tx.sign(main.get_private_key(config::system_account_name, "active"), main.get_chain_id());
   // Replace the valid transaction with the invalid transaction
   auto invalid_packed_tx = packed_transaction(std::move(signed_tx), packed_trx.get_compression());
   copy_b->transactions.back().trx = std::move(invalid_packed_tx);

   // Re-sign the block
   copy_b->producer_signatures = {main.get_private_key(config::system_account_name, "active").sign(copy_b->calculate_id())};

   // Push block with invalid transaction to other chain
   savanna_tester validator;
   auto signed_copy_b = signed_block::create_signed_block(std::move(copy_b));
   BOOST_REQUIRE_EXCEPTION(validator.control->accept_block( signed_copy_b->calculate_id(), signed_copy_b ), fc::exception,
                           [] (const fc::exception &e)->bool {
                              return e.code() == block_validate_exception::code_value &&
                                     e.to_detail_string().find("invalid block transaction merkle root") != std::string::npos;
                           }) ;
}

template <typename T = savanna_validating_tester>
std::pair<signed_block_ptr, signed_block_ptr> corrupt_trx_in_block(T& main, account_name act_name) {
   // First we create a valid block with valid transaction
   main.create_account(act_name, config::system_account_name, false, true, false);
   signed_block_ptr b = main.produce_block_no_validation();

   // Make a copy of the valid block and corrupt the transaction
   auto copy_b = b->clone();
   const auto& packed_trx = copy_b->transactions.back().trx;
   auto signed_tx = packed_trx.get_signed_transaction();
   // Corrupt one signature
   signed_tx.signatures.clear();
   signed_tx.sign(main.get_private_key(act_name, "active"), main.get_chain_id());

   // Replace the valid transaction with the invalid transaction
   auto invalid_packed_tx = packed_transaction(signed_tx, packed_trx.get_compression());
   copy_b->transactions.back().trx = std::move(invalid_packed_tx);

   // Re-calculate the transaction merkle
   deque<digest_type> trx_digests;
   const auto& trxs = copy_b->transactions;
   for( const auto& a : trxs )
      trx_digests.emplace_back( a.digest() );
   copy_b->transaction_mroot = calculate_merkle( std::move(trx_digests) );

   // Re-sign the block
   copy_b->producer_signatures = {main.get_private_key(b->producer, "active").sign(copy_b->calculate_id())};

   return std::pair<signed_block_ptr, signed_block_ptr>(b, signed_block::create_signed_block(std::move(copy_b)));
}

// verify that a block with a transaction with an incorrect signature, is blindly accepted from a trusted producer
BOOST_AUTO_TEST_CASE( trusted_producer_test )
{
   flat_set<account_name> trusted_producers = { "defproducera"_n, "defproducerc"_n };
   savanna_validating_tester main(trusted_producers);
   // only using validating_tester to keep the 2 chains in sync, not to validate that the validating_node matches the main node,
   // since it won't be
   main.skip_validate = true;

   // First we create a valid block with valid transaction
   std::set<account_name> producers = { "defproducera"_n, "defproducerb"_n, "defproducerc"_n, "defproducerd"_n };
   for (auto prod : producers)
       main.create_account(prod, config::system_account_name, false, true, false);
   auto b = main.produce_block();

   std::vector<account_name> schedule(producers.cbegin(), producers.cend());
   auto trace = main.set_producers(schedule);

   while (b->producer != "defproducera"_n) {
      b = main.produce_block();
   }

   auto blocks = corrupt_trx_in_block(main, "tstproducera"_n);
   main.validate_push_block( blocks.second );
}

// like trusted_producer_test, except verify that any entry in the trusted_producer list is accepted
BOOST_AUTO_TEST_CASE( trusted_producer_verify_2nd_test )
{
   flat_set<account_name> trusted_producers = { "defproducera"_n, "defproducerc"_n };
   savanna_validating_tester main(trusted_producers);
   // only using validating_tester to keep the 2 chains in sync, not to validate that the validating_node matches the main node,
   // since it won't be
   main.skip_validate = true;

   // First we create a valid block with valid transaction
   std::set<account_name> producers = { "defproducera"_n, "defproducerb"_n, "defproducerc"_n, "defproducerd"_n };
   for (auto prod : producers)
       main.create_account(prod, config::system_account_name, false, true, false);
   auto b = main.produce_block();

   std::vector<account_name> schedule(producers.cbegin(), producers.cend());
   auto trace = main.set_producers(schedule);

   while (b->producer != "defproducerc"_n) {
      b = main.produce_block();
   }

   auto blocks = corrupt_trx_in_block(main, "tstproducera"_n);
   main.validate_push_block( blocks.second );
}

// verify that a block with a transaction with an incorrect signature, is rejected if it is not from a trusted producer
BOOST_AUTO_TEST_CASE_TEMPLATE( untrusted_producer_test, T, validating_testers )
{
   flat_set<account_name> trusted_producers = { "defproducera"_n, "defproducerc"_n };
   T main(trusted_producers);
   // only using validating_tester to keep the 2 chains in sync, not to validate that the validating_node matches the main node,
   // since it won't be
   main.skip_validate = true;

   // First we create a valid block with valid transaction
   std::set<account_name> producers = { "defproducera"_n, "defproducerb"_n, "defproducerc"_n, "defproducerd"_n };
   for (auto prod : producers)
       main.create_account(prod, config::system_account_name, false, true, false);
   auto b = main.produce_block();

   std::vector<account_name> schedule(producers.cbegin(), producers.cend());
   auto trace = main.set_producers(schedule);

   while (b->producer != "defproducerb"_n) {
      b = main.produce_block();
   }

   auto blocks = corrupt_trx_in_block<T>(main, "tstproducera"_n);
   BOOST_REQUIRE_EXCEPTION(main.validate_push_block( blocks.second ), fc::exception ,
   [] (const fc::exception &e)->bool {
      return e.code() == unsatisfied_authorization::code_value ;
   }) ;
}

/**
 * Ensure that the block broadcasted by producing node and receiving node is identical
 */
BOOST_AUTO_TEST_CASE_TEMPLATE( broadcasted_block_test, T, testers )
{

  T producer_node;
  T receiving_node;

  signed_block_ptr bcasted_blk_by_prod_node;
  signed_block_ptr bcasted_blk_by_recv_node;

  producer_node.control->accepted_block().connect( [&](block_signal_params t) {
    const auto& [ block, id ] = t;
    bcasted_blk_by_prod_node = block;
  });
  receiving_node.control->accepted_block().connect( [&](block_signal_params t) {
    const auto& [ block, id ] = t;
    bcasted_blk_by_recv_node = block;
  });

  auto b = producer_node.produce_block();
  receiving_node.push_block(b);

  bytes bcasted_blk_by_prod_node_packed = fc::raw::pack(*bcasted_blk_by_prod_node);
  bytes bcasted_blk_by_recv_node_packed = fc::raw::pack(*bcasted_blk_by_recv_node);
  BOOST_CHECK(std::equal(bcasted_blk_by_prod_node_packed.begin(), bcasted_blk_by_prod_node_packed.end(), bcasted_blk_by_recv_node_packed.begin()));
}

/**
 * Verify abort block returns applied transactions in block
 */
BOOST_FIXTURE_TEST_CASE( abort_block_transactions, validating_tester) { try {

      produce_block();
      signed_transaction trx;

      account_name a = "newco"_n;
      account_name creator = config::system_account_name;

      // account does not exist before test
      BOOST_REQUIRE_EXCEPTION(control->get_account( a ), fc::exception,
                              [a] (const fc::exception& e)->bool {
                                 return std::string( e.what() ).find( a.to_string() ) != std::string::npos;
                              }) ;

      auto owner_auth = authority( get_public_key( a, "owner" ) );
      trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                newaccount{
                                      .creator  = creator,
                                      .name     = a,
                                      .owner    = owner_auth,
                                      .active   = authority( get_public_key( a, "active" ) )
                                });
      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), get_chain_id()  );
      auto trace = push_transaction( trx );

      get_account( a ); // throws if it does not exist

      deque<transaction_metadata_ptr> unapplied_trxs = control->abort_block();

      // verify transaction returned from abort_block()
      BOOST_REQUIRE_EQUAL( 1u,  unapplied_trxs.size() );
      BOOST_REQUIRE_EQUAL( trx.id(), unapplied_trxs.at(0)->id() );

      // account does not exist block was aborted which had transaction
      BOOST_REQUIRE_EXCEPTION(get_account( a ), fc::exception,
                              [a] (const fc::exception& e)->bool {
                                 return std::string( e.what() ).find( a.to_string() ) != std::string::npos;
                              }) ;

      produce_block();

   } FC_LOG_AND_RETHROW() }

/**
 * Verify abort block returns applied transactions in block
 */
BOOST_FIXTURE_TEST_CASE( abort_block_transactions_tester, validating_tester) { try {

      produce_block();
      signed_transaction trx;

      account_name a = "newco"_n;
      account_name creator = config::system_account_name;

      // account does not exist before test
      BOOST_REQUIRE_EXCEPTION(get_account( a ), fc::exception,
                              [a] (const fc::exception& e)->bool {
                                 return std::string( e.what() ).find( a.to_string() ) != std::string::npos;
                              }) ;

      auto owner_auth = authority( get_public_key( a, "owner" ) );
      trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                newaccount{
                                      .creator  = creator,
                                      .name     = a,
                                      .owner    = owner_auth,
                                      .active   = authority( get_public_key( a, "active" ) )
                                });
      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), get_chain_id()  );
      auto trace = push_transaction( trx );

      get_account( a ); // throws if it does not exist

      produce_block( fc::milliseconds(config::block_interval_ms*2) ); // aborts block, tester should reapply trx

      get_account( a ); // throws if it does not exist

      deque<transaction_metadata_ptr> unapplied_trxs = control->abort_block(); // should be empty now

      BOOST_REQUIRE_EQUAL( 0u,  unapplied_trxs.size() );

   } FC_LOG_AND_RETHROW() }

// Verify blocks are produced when onblock fails
BOOST_AUTO_TEST_CASE(no_onblock_test) { try {
   savanna_tester c;

   c.produce_block_ex();
   auto r = c.produce_block_ex();
   BOOST_TEST_REQUIRE(!!r.onblock_trace);
   BOOST_TEST(!!r.onblock_trace->receipt);
   BOOST_TEST(!r.onblock_trace->except);
   BOOST_TEST(!r.onblock_trace->except_ptr);
   BOOST_TEST(!r.block->finality_mroot.empty());

   // Deploy contract that rejects all actions dispatched to it with the following exceptions:
   //   * sysio::setcode to set code on the sysio is allowed (unless the rejectall account exists)
   //   * sysio::newaccount is allowed only if it creates the rejectall account.
   c.set_code( config::system_account_name, test_contracts::reject_all_wasm() );
   c.produce_block();
   r = c.produce_block_ex(); // empty block, no valid onblock since it is rejected
   BOOST_TEST_REQUIRE(!!r.onblock_trace);
   BOOST_TEST(!r.onblock_trace->receipt);
   BOOST_TEST(!!r.onblock_trace->except);
   BOOST_TEST(!!r.onblock_trace->except_ptr);

   // finality_mroot is the root of the Finality Tree
   // associated with the block, i.e. the root of
   // validation_tree(core.latest_qc_claim().block_num).
   BOOST_TEST(!r.block->finality_mroot.empty());
   c.produce_empty_block();

} FC_LOG_AND_RETHROW() }

// Verify a block with invalid QC block number is rejected.
BOOST_FIXTURE_TEST_CASE( invalid_qc_claim_block_num_test, validating_tester ) {
   skip_validate = true;

   // First we create a valid block
   create_account("newacc"_n);
   auto b = produce_block_no_validation();

   // Make a copy of the valid block
   auto copy_b = b->clone();

   // Set QC claim block number to an invalid number (QC claim block number cannot be greater than previous block number)
   copy_b->qc_claim.block_num = copy_b->block_num(); // copy_b->block_num() is 1 greater than previous block number

   // Re-sign the block
   copy_b->producer_signatures = {get_private_key(config::system_account_name, "active").sign(copy_b->calculate_id())};

   // Push the corrupted block. It must be rejected.
   BOOST_REQUIRE_EXCEPTION(validate_push_block(signed_block::create_signed_block(std::move(copy_b))), fc::exception,
                           [] (const fc::exception &e)->bool {
                              return e.code() == invalid_qc_claim::code_value &&
                                     e.to_detail_string().find("that is greater than the previous block number") != std::string::npos;
                           }) ;
}

// Verify that a block with an invalid finality mroot is rejected
BOOST_FIXTURE_TEST_CASE( invalid_finality_mroot_test, tester )
{
   produce_blocks(5);

   // Create a block with transaction
   create_account("newacc"_n);
   auto b = produce_block();

   // Make a copy of the block and corrupt its finality mroot
   auto copy_b = b->clone();
   copy_b->finality_mroot = digest_type::hash("corrupted");

   // Re-sign the block
   copy_b->producer_signatures = {get_private_key(config::system_account_name, "active").sign(copy_b->calculate_id())};

   // Push the block containing corrupted finality mroot. It should fail
   BOOST_REQUIRE_EXCEPTION(push_block(signed_block::create_signed_block(std::move(copy_b))),
                           fc::exception,
                           [] (const fc::exception &e)->bool {
                              return e.code() == block_validate_exception::code_value &&
                                     e.to_detail_string().find("computed finality mroot") != std::string::npos &&
                                     e.to_detail_string().find("does not match supplied finality mroot") != std::string::npos;
                           });
}

// Verify that a block with block_extensions is rejected
BOOST_FIXTURE_TEST_CASE( no_block_extensions_allowed_test, validating_tester ) {
   skip_validate = true;

   // First we create a valid block
   create_account("newacc"_n);
   auto b = produce_block_no_validation();

   // Make a copy and inject a dummy block extension
   auto copy_b = b->clone();
   copy_b->block_extensions.emplace_back(0xFFFF, std::vector<char>{0x00});

   // Re-sign the block
   copy_b->producer_signatures = {get_private_key(config::system_account_name, "active").sign(copy_b->calculate_id())};

   // Push the block with extensions. It must be rejected.
   BOOST_REQUIRE_EXCEPTION(validate_push_block(signed_block::create_signed_block(std::move(copy_b))), fc::exception,
                           [] (const fc::exception& e)->bool {
                              return e.code() == invalid_block_extension::code_value &&
                                     e.to_detail_string().find("No block extensions currently supported") != std::string::npos;
                           });
}

BOOST_AUTO_TEST_CASE( transaction_receipt_digest_format_test )
{
   // Golden-value test locking in the transaction_receipt digest format:
   //   digest = sha256( pack(cpu_usage_us) || pack(trx.digest()) )
   // A reorder, added field, or removed field in transaction_receipt::digest() will
   // change the hash and fail this test.

   transaction_receipt r;
   r.cpu_usage_us = { fc::unsigned_int(100), fc::unsigned_int(200) };

   // Reconstruct the expected digest from first principles.
   auto expected = [&]() {
      digest_type::encoder enc;
      fc::raw::pack( enc, r.cpu_usage_us );
      fc::raw::pack( enc, r.trx.digest() );
      return enc.result();
   };

   BOOST_CHECK_EQUAL( r.digest(), expected() );

   // Stability across calls.
   BOOST_CHECK_EQUAL( r.digest(), r.digest() );

   // Changing any CPU value changes the digest.
   auto orig = r.digest();
   r.cpu_usage_us[0] = fc::unsigned_int(101);
   BOOST_CHECK_NE( r.digest(), orig );
   BOOST_CHECK_EQUAL( r.digest(), expected() );

   // Appending a CPU value changes the digest.
   auto after_mod = r.digest();
   r.cpu_usage_us.push_back( fc::unsigned_int(50) );
   BOOST_CHECK_NE( r.digest(), after_mod );
   BOOST_CHECK_EQUAL( r.digest(), expected() );

   // Empty cpu_usage_us is also valid and stable.
   r.cpu_usage_us.clear();
   BOOST_CHECK_EQUAL( r.digest(), expected() );
}

BOOST_AUTO_TEST_CASE( explicit_cpu_usage_uint32_overflow_test )
{
   // A malicious/buggy block with explicit per-action cpu_usage_us values whose
   // sum exceeds uint32_t must be rejected. Without the overflow guard, the sum
   // would silently narrow when assigned to transaction_trace::total_cpu_usage_us
   // (fc::unsigned_int = uint32_t), potentially defeating the subsequent
   // validate_trx_billed_cpu check and under-billing the accounts.

   tester chain( setup_policy::full );
   account_name acc = "acc"_n;
   chain.create_accounts( {acc} );
   chain.produce_block();

   // Build a 2-action trx. nonce + nonce so that total_actions() == 2 and
   // signatures are trivially satisfied at the system-account level.
   signed_transaction trx;
   trx.actions.emplace_back(
      vector<permission_level>{{config::system_account_name, config::active_name}},
      config::system_account_name, "nonce"_n, fc::raw::pack(std::string("a")) );
   trx.actions.emplace_back(
      vector<permission_level>{{config::system_account_name, config::active_name}},
      config::system_account_name, "nonce"_n, fc::raw::pack(std::string("b")) );
   chain.set_transaction_headers( trx, 10 );
   trx.sign( tester::get_private_key( config::system_account_name, "active" ), chain.control->get_chain_id() );

   auto ptrx = std::make_shared<packed_transaction>(trx);
   auto fut = transaction_metadata::start_recover_keys( ptrx, chain.control->get_thread_pool(),
                                                       chain.control->get_chain_id(), fc::microseconds::maximum(),
                                                       transaction_metadata::trx_type::input );
   auto mtrx = fut.get();

   // Two values each just over 2^31 so their sum exceeds uint32_t max.
   cpu_usage_t overflow_cpu{ fc::unsigned_int(0x80000000u), fc::unsigned_int(0x80000001u) };

   BOOST_CHECK_EXCEPTION(
      chain.control->test_push_transaction( mtrx, fc::time_point::maximum(),
                                            fc::microseconds::maximum(), overflow_cpu,
                                            true /*explicit_billed_cpu_time*/ ),
      transaction_exception,
      fc_exception_message_contains("overflows uint32") );
}

// A non-transient trx that is rejected during apply (here, by failing the auth
// check after net_usage was already populated by init_for_input_trx) must NOT
// contribute its net/cpu/elapsed to the producer's _block_report.  The block
// itself contains zero such trxs; receivers replaying it see net=0, and the
// producer's log_applied / produced_block_metrics must agree.
BOOST_AUTO_TEST_CASE( failed_trx_excluded_from_block_report )
{
   // log_applied skips the metrics callback when the block timestamp is more than
   // 5 minutes from wall-clock now (sync-mode throttle), so build a chain whose
   // genesis is current time and the produced blocks land within the window.
   fc::temp_directory tempdir;
   auto def_conf = base_tester::default_config(tempdir);
   auto genesis = def_conf.second;
   genesis.initial_timestamp = fc::time_point::now() - fc::seconds(1);
   savanna_tester chain( def_conf.first, genesis );
   chain.execute_setup_policy( setup_policy::full );
   chain.create_account( "alice"_n );
   chain.produce_block(); // commit create_account before installing the callback

   std::optional<produced_block_metrics> captured;
   chain.control->register_update_produced_block_metrics(
      [&]( produced_block_metrics m ) { captured = m; } );

   // Baseline: produce a block with no user trxs (just onblock).
   chain.produce_block();
   BOOST_REQUIRE( captured );
   const auto baseline = *captured;
   BOOST_TEST( baseline.trxs_produced_total == 0u );
   BOOST_TEST( baseline.net_usage_us == 0u ); // onblock is implicit; no wire bytes
   captured.reset();

   // Action authorized by alice@active, but signed with sysio's key -- the
   // auth check throws unsatisfied_authorization AFTER init_for_input_trx
   // has populated trace->net_usage from the action's billable size.
   signed_transaction stx;
   stx.actions.emplace_back(
      vector<permission_level>{{"alice"_n, config::active_name}},
      config::system_account_name, "nonce"_n, fc::raw::pack(std::string("x")) );
   chain.set_transaction_headers( stx, 10 );
   stx.sign( chain.get_private_key( config::system_account_name, "active" ),
             chain.control->get_chain_id() );

   auto trace = chain.push_transaction( stx, fc::time_point::maximum(),
                                        base_tester::DEFAULT_BILLED_CPU_TIME_US,
                                        true /*no_throw*/ );
   BOOST_REQUIRE( trace->except );
   BOOST_REQUIRE_EQUAL( trace->except->code(), unsatisfied_authorization::code_value );
   BOOST_REQUIRE_GT( trace->net_usage, 0u ); // populated before the throw

   // Failed-trx block: same shape as baseline (just onblock makes it into the block).
   chain.produce_block();
   BOOST_REQUIRE( captured );

   // Block content matches baseline: zero user trxs.
   BOOST_TEST( captured->trxs_produced_total == 0u );
   // Net is the cleanest signal. onblock contributes zero, so any non-zero net_usage_us would mean the rejected
   // trx leaked its net into the report. Pre-fix: == trace->net_usage (>0).
   BOOST_TEST( captured->net_usage_us == 0u );
   // Cpu must match the onblock-only baseline. Pre-fix it would be baseline + trace->total_cpu_usage_us.
   // net/cpu/elapsed all aggregate at the same controller site under one condition, so they leak together or
   // not at all. net + cpu are sufficient signal. A wall-clock assertion on total_elapsed_time_us would only
   // add CI jitter noise without strengthening the test.
   BOOST_TEST( captured->cpu_usage_us == baseline.cpu_usage_us );
}

// Regression test for the snapshot hardening invariant added in block_state(snapshot).
// For any non-genesis block_state, core.latest_qc_claim() must equal header.qc_claim
// (finality_core::next sets latest_qc_claim from the header's claim). A snapshot with
// the two disagreeing would break verify_basic_proper_block_invariants' assumption
// that the core is the authoritative QC-claim reference. Snapshot loading must reject.
BOOST_AUTO_TEST_CASE(snapshot_core_header_qc_claim_mismatch_rejected) try {
   savanna_tester chain;
   chain.produce_blocks(4); // past genesis

   const auto& live_bsp = block_handle_accessor::get_bsp(chain.control->head());
   BOOST_REQUIRE(!live_bsp->core.is_genesis_core());
   BOOST_REQUIRE(live_bsp->core.latest_qc_claim() == live_bsp->header.qc_claim); // sanity

   // Build a snapshot_block_state_v1 from the live one, then tamper header.qc_claim
   // to something provably different (block_num far in the past, weak). Must NOT
   // match core.latest_qc_claim(), which for a running chain reflects a recent block.
   snapshot_detail::snapshot_block_state_v1 sbs{*live_bsp};
   sbs.header.qc_claim = qc_claim_t{0, false};
   BOOST_REQUIRE(sbs.core.latest_qc_claim() != sbs.header.qc_claim); // confirm setup

   BOOST_CHECK_EXCEPTION(block_state{std::move(sbs)}, snapshot_exception,
      fc_exception_message_contains("core.latest_qc_claim"));
} FC_LOG_AND_RETHROW()

// Genesis is intentionally exempt: core is constructed with latest_qc_claim={1,false}
// while header.qc_claim={0,false} (see create_genesis_block). Confirm a genesis-core
// snapshot loads cleanly despite the qc_claim disagreement.
BOOST_AUTO_TEST_CASE(snapshot_genesis_core_header_mismatch_allowed) try {
   genesis_state gs;
   auto genesis_bsp = block_state::create_genesis_block(gs);
   BOOST_REQUIRE(genesis_bsp->core.is_genesis_core());
   BOOST_REQUIRE(genesis_bsp->core.latest_qc_claim() != genesis_bsp->header.qc_claim); // confirmed design

   snapshot_detail::snapshot_block_state_v1 sbs{*genesis_bsp};
   BOOST_CHECK_NO_THROW(block_state{std::move(sbs)});
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
