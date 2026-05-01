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
