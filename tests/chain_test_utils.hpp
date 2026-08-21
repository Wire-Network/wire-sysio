#pragma once

#include <sysio/testing/tester.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/application.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/controller.hpp>

#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>

namespace sysio::test_utils {

using namespace sysio::chain;
using namespace sysio::chain::literals;

struct testit {
   uint64_t id;
   explicit testit(uint64_t id = 0)
      :id(id){}
   static account_name get_account() {
      return chain::config::system_account_name;
   }
   static action_name get_name() {
      return "testit"_n;
   }
};

// Corresponds to the reqactivated action of the bios contract.
// See libraries/testing/contracts/sysio.bios/sysio.bios.hpp
struct reqactivated {
   chain::digest_type feature_digest;

   explicit reqactivated(const chain::digest_type& fd)
      :feature_digest(fd){};

   static account_name get_account() {
      return chain::config::system_account_name;
   }
   static action_name get_name() {
      return "reqactivated"_n;
   }
};

inline private_key_type get_private_key( name keyname, string role ) {
   if (keyname == config::system_account_name)
   {
      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      auto system_keys = sig_plug.query_providers(std::nullopt, std::nullopt, crypto::chain_key_type_wire);
      if (system_keys.empty())
      {
         // register_default_signature_providers is a pre-startup mutator: the manager's provider set is immutable once
         // the node is running, so this lazy fallback is only valid before the tester has started.
         FC_ASSERT(sig_plug.get_state() != appbase::abstract_plugin::started,
                   "no system wire signature provider registered before startup");
         sig_plug.register_default_signature_providers(std::vector{crypto::chain_key_type_wire});
         system_keys = sig_plug.query_providers(std::nullopt, std::nullopt, crypto::chain_key_type_wire);
      }

      FC_ASSERT(!system_keys.empty(), "No system keys registered");
      auto& sys_key = system_keys[0];

      return sys_key->private_key.value();
   }
   return private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(keyname.to_string()+role).to_uint64_array());
}

inline public_key_type  get_public_key( name keyname, string role ){
   return get_private_key( keyname, role ).get_public_key();
}

// Create a read-only trx that works with bios reqactivated action
inline auto make_bios_ro_trx(sysio::chain::controller& control) {
   const auto& pfm = control.get_protocol_feature_manager();
   static auto feature_digest = pfm.get_builtin_digest(builtin_protocol_feature_t::reserved_second_protocol_feature);

   signed_transaction trx;
   trx.expiration = fc::time_point_sec{fc::time_point::now() + fc::seconds(30)};
   vector<permission_level> no_auth{};
   trx.actions.emplace_back( no_auth, reqactivated{*feature_digest} );
   return std::make_shared<packed_transaction>( std::move(trx) );
}

/// A node drops a transaction that trips an objective cpu limit and leaves resubmission to the client. The helpers
/// below only build the chain state a test starts from, so they resubmit rather than report such a drop as a test
/// failure: deploying the bios contract bills a few milliseconds of the chain's 150ms max_transaction_cpu_usage, but a
/// CI host running the whole suite in parallel can deschedule the producing thread mid-action for long enough to blow
/// through that limit. Only the codes a resubmission can plausibly clear are retried; anything else is a real failure
/// and propagates from the first attempt.
/// @param e exception reported for the dropped transaction
/// @return true when the same transaction is worth pushing again
inline bool is_resubmittable_setup_failure( const fc::exception& e ) {
   return e.code() == tx_cpu_usage_exceeded::code_value
       || e.code() == block_cpu_usage_exceeded::code_value
       || e.code() == deadline_exception::code_value;
}

/// Number of attempts a setup transaction is given before its failure is reported to the test.
inline constexpr size_t max_setup_trx_attempts = 4;

/// How long a resubmission waits for the chain to advance past the block its predecessor was dropped from.
inline constexpr std::chrono::seconds head_advance_timeout{5};

/// How long a single head read is given before the app is treated as no longer servicing its queues.
inline constexpr std::chrono::seconds head_read_timeout{5};

/// How often the head-advance wait samples the head block.
inline constexpr std::chrono::milliseconds head_advance_poll_interval{10};

/// What a caller needs from the controller's head block, copied on the thread that owns it.
struct head_snapshot {
   uint32_t                    block_num = 0;
   sysio::chain::block_id_type id;
};

/// Read the controller's head block from the app thread.
/// controller::head() is not thread-safe -- chain_head is a plain block_handle assigned as blocks are applied, and
/// head() copies it -- so sampling it from a test thread races with block production. Posting the read serializes it
/// against that work on the one queue the app thread alone drains, which is the same queue the pushes below go
/// through; the copy then crosses back to the caller through the future.
/// Must be called from a thread other than the app thread, which is the one that answers the post.
/// @param app     running application whose thread owns the controller
/// @param control controller to read from
/// @return the head block number and id, or nullopt if the read did not complete before head_read_timeout
inline std::optional<head_snapshot> read_head( appbase::scoped_app& app, sysio::chain::controller& control ) {
   auto head_promise = std::make_shared<std::promise<head_snapshot>>();
   std::future<head_snapshot> head_future = head_promise->get_future();

   app->executor().post( priority::high, exec_queue::read_write, [&control, head_promise]() {
      auto head = control.head();
      head_promise->set_value( head_snapshot{ .block_num = head.block_num(), .id = head.id() } );
   });

   if( head_future.wait_for( head_read_timeout ) == std::future_status::timeout )
      return std::nullopt;
   return head_future.get();
}

/// Wait for the controller to report a head block past the one observed on entry.
/// The wait is bounded: a chain that has stopped advancing is a failure for the caller to report, not one to spin on.
/// @param app     running application whose thread owns the controller
/// @param control controller to observe
/// @return true if the head advanced before head_advance_timeout elapsed
inline bool wait_for_head_block_advance( appbase::scoped_app& app, sysio::chain::controller& control ) {
   const auto dropped_from = read_head( app, control );
   if( !dropped_from )
      return false;

   const auto deadline = std::chrono::steady_clock::now() + head_advance_timeout;
   while( true ) {
      const auto head = read_head( app, control );
      if( !head )
         return false;
      if( head->block_num != dropped_from->block_num )
         return true;
      if( std::chrono::steady_clock::now() >= deadline )
         return false;
      std::this_thread::sleep_for( head_advance_poll_interval );
   }
}

/// Push an input transaction to controller once and return its trx trace.
/// If account is sysio then signs with the default private key.
/// @param app        running application hosting the chain and producer plugins
/// @param control    controller the transaction is stamped against
/// @param account    authorizing account, also the signer
/// @param trx        transaction to stamp, sign and push; restamped on every call
/// @return trace of the executed transaction
/// @throws fc::exception the node reported for the transaction, std::runtime_error if it did not execute in time
inline transaction_trace_ptr push_input_trx_once( appbase::scoped_app& app, sysio::chain::controller& control,
                                                  account_name account, signed_transaction& trx ) {
   const auto head = read_head( app, control );
   if( !head )
      throw std::runtime_error("failed to read head block to stamp trx for account: " + account.to_string());

   trx.expiration = fc::time_point_sec{fc::time_point::now() + fc::seconds(30)};
   trx.set_reference_block( head->id );
   trx.signatures.clear(); // a resubmission signs the restamped transaction, it does not accumulate signatures
   trx.sign(get_private_key(account, "active"), control.get_chain_id());
   auto ptrx = std::make_shared<packed_transaction>( trx, packed_transaction::compression_type::none );

   auto trx_promise = std::make_shared<std::promise<transaction_trace_ptr>>();
   std::future<transaction_trace_ptr> trx_future = trx_promise->get_future();

   // ptrx is captured by value: on the timeout path below this function returns while the posted work may still be
   // queued, so the lambda cannot reference a local of a frame that is already gone.
   app->executor().post( priority::low, exec_queue::read_write, [ptrx, &app, trx_promise]() {
      app->get_method<plugin_interface::incoming::methods::transaction_async>()(ptrx,
                                                                                false, // api_trx
                                                                                transaction_metadata::trx_type::input, // trx_type
                                                                                true, // return_failure_traces
           [trx_promise](const next_function_variant<transaction_trace_ptr>& result) {
              if( std::holds_alternative<fc::exception_ptr>( result ) ) {
                 try {
                    std::get<fc::exception_ptr>(result)->rethrow();
                 } catch(...) {
                    trx_promise->set_exception(std::current_exception());
                 }
              } else if ( std::get<chain::transaction_trace_ptr>( result )->except ) {
                 try {
                    std::get<chain::transaction_trace_ptr>(result)->except->rethrow();
                 } catch(...) {
                    trx_promise->set_exception(std::current_exception());
                 }
              } else {
                 trx_promise->set_value(std::get<chain::transaction_trace_ptr>(result));
              }
           });
   });

   if (trx_future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
      throw std::runtime_error("failed to execute trx: " + ptrx->get_transaction().actions.at(0).name.to_string() + " to account: " + account.to_string());

   return trx_future.get();
}

/// Run a setup transaction through the resubmission policy: while the node keeps dropping it for a failure a
/// resubmission can clear, wait for the chain to advance and push it again.
/// The policy is kept separate from the push and wait mechanics so it can be exercised without a running node.
/// @param push_once      makes one attempt, throwing whatever the node reported for the transaction
/// @param wait_for_chain waits for the chain to advance before a resubmission, false if it stopped advancing
/// @return trace of the executed transaction
/// @throws whatever push_once threw on the last attempt made
template<typename PushOnce, typename WaitForChain>
transaction_trace_ptr resubmit_setup_trx( PushOnce&& push_once, WaitForChain&& wait_for_chain ) {
   for( size_t attempt = 1; ; ++attempt ) {
      try {
         return push_once();
      } catch( const fc::exception& e ) {
         if( attempt == max_setup_trx_attempts || !is_resubmittable_setup_failure( e ) )
            throw;
         wlog( "setup transaction dropped on attempt {} of {}, resubmitting: {}",
               attempt, max_setup_trx_attempts, e.top_message() );
         // The resubmission has to carry a new reference block to stay distinct from the transaction just dropped, so
         // wait for the chain to actually advance instead of assuming it does within one nominal block interval: the
         // starvation that drops the transaction is just as able to delay the block that follows it. A chain that
         // stops advancing altogether is reported as the drop that got us here rather than as a wait that timed out.
         if( !wait_for_chain() )
            throw;
      }
   }
}

/// Push an input transaction to controller and return its trx trace, resubmitting the transaction while the node
/// keeps dropping it for a transient objective-cpu failure. Every resubmission waits for the chain to advance first.
/// If account is sysio then signs with the default private key.
/// @param app        running application hosting the chain and producer plugins
/// @param control    controller the transaction is stamped against
/// @param account    authorizing account, also the signer
/// @param trx        transaction to stamp, sign and push; restamped on every attempt
/// @return trace of the executed transaction
/// @throws fc::exception the node reported on the last attempt made, whether that is the final attempt or the one
///                       after which the chain stopped advancing; std::runtime_error if it did not execute in time
inline transaction_trace_ptr push_input_trx( appbase::scoped_app& app, sysio::chain::controller& control,
                                             account_name account, signed_transaction& trx ) {
   return resubmit_setup_trx( [&]() { return push_input_trx_once( app, control, account, trx ); },
                              [&]() { return wait_for_head_block_advance( app, control ); } );
}

// Push setcode trx to controller and return trx trace
inline auto set_code(appbase::scoped_app& app, sysio::chain::controller& control, account_name account, const vector<uint8_t>& wasm) {
   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{account, sysio::chain::config::active_name}},
                            chain::setcode{
                               .account = account,
                               .vmtype = 0,
                               .vmversion = 0,
                               .code = bytes(wasm.begin(), wasm.end())
                            });
   return push_input_trx(app, control, account, trx);
}

inline transaction_trace_ptr create_account(appbase::scoped_app& app, sysio::chain::controller& control, account_name a, account_name creator) {
   signed_transaction trx;

   authority owner_auth{ get_public_key( a, "owner" ) };
   authority active_auth{ get_public_key( a, "active" ) };

   trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                             chain::newaccount{
                                .creator  = creator,
                                .name     = a,
                                .owner    = owner_auth,
                                .active   = active_auth,
                             });

   return push_input_trx(app, control, creator, trx);
}

inline void activate_protocol_features_set_bios_contract(appbase::scoped_app& app, chain_plugin* chain_plug) {
   using namespace appbase;

   auto feature_set = std::make_shared<std::atomic<bool>>(false);
   // has to execute when pending block is not null
   for (int tries = 0; tries < 100; ++tries) {
      app->executor().post( priority::high, exec_queue::read_write, [&chain_plug=chain_plug, feature_set](){
         try {
            if (!chain_plug->chain().is_building_block() || *feature_set)
               return;
            const auto& pfm = chain_plug->chain().get_protocol_feature_manager();
            auto preactivate_feature_digest = pfm.get_builtin_digest(builtin_protocol_feature_t::reserved_first_protocol_feature);
            BOOST_CHECK( preactivate_feature_digest );
            chain_plug->chain().preactivate_feature( *preactivate_feature_digest, false );

            vector<digest_type> feature_digests;
            feature_digests.push_back(*pfm.get_builtin_digest(builtin_protocol_feature_t::reserved_second_protocol_feature));

            for (const auto feature_digest : feature_digests) {
               chain_plug->chain().preactivate_feature( feature_digest, false );
            }
            *feature_set = true;
            return;
         } FC_LOG_AND_DROP()
         BOOST_CHECK(!"exception setting protocol features");
      });
      if (*feature_set)
         break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
   }

   // Wait for next block
   std::this_thread::sleep_for( std::chrono::milliseconds(config::block_interval_ms) );

   auto r = set_code(app, chain_plug->chain(), config::system_account_name, testing::contracts::sysio_bios_wasm());
   BOOST_CHECK(!!r->receipt);
}

} // namespace sysio::test_utils

FC_REFLECT( sysio::test_utils::testit, (id) )
FC_REFLECT( sysio::test_utils::reqactivated, (feature_digest) )
