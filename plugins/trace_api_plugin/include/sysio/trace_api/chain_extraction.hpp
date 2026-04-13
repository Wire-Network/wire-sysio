#pragma once

#include <sysio/trace_api/common.hpp>
#include <sysio/trace_api/extract_util.hpp>
#include <sysio/trace_api/trace.hpp>
#include <sysio/chain/config.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger.hpp>
#include <exception>
#include <functional>
#include <map>
#include <unordered_set>

namespace sysio { namespace trace_api {

using chain::transaction_id_type;
using chain::packed_transaction;

template <typename StoreProvider>
class chain_extraction_impl_type {
public:
   /**
    * Called to fetch the current ABI bytes for an account (lazy init on first encounter).
    * Returns nullopt if the account has no ABI.
    */
   using abi_fetcher_t = std::function<std::optional<std::vector<char>>(chain::name)>;

   /**
    * Chain Extractor for capturing transaction traces, action traces, and block info.
    * @param store provider of append, append_lib, and append_abi
    * @param except_handler called on exceptions, logging if any is left to the user
    * @param abi_fetcher optional callback to lazily fetch the current ABI for an account;
    *                    called on first encounter of each account; receives global_seq 0
    */
   chain_extraction_impl_type( StoreProvider store, exception_handler except_handler,
                                abi_fetcher_t abi_fetcher = {} )
   : store(std::move(store))
   , except_handler(std::move(except_handler))
   , _abi_fetcher(std::move(abi_fetcher))
   {}

   /// connect to chain controller applied_transaction signal
   void signal_applied_transaction( const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& ptrx ) {
      on_applied_transaction( trace, ptrx );
   }

   /// connect to chain controller accepted_block signal
   void signal_accepted_block( const chain::signed_block_ptr& block, const chain::block_id_type& id ) {
      on_accepted_block( block, id );
   }

   /// connect to chain controller irreversible_block signal
   void signal_irreversible_block( uint32_t block_num ) {
      on_irreversible_block( block_num );
   }

   /// connect to chain controller block_start signal
   void signal_block_start( uint32_t block_num ) {
      on_block_start( block_num );
   }

private:

   void on_applied_transaction(const chain::transaction_trace_ptr& trace, const chain::packed_transaction_ptr& t) {
      if( !trace->receipt ) return;
      if( chain::is_onblock( *trace )) {
         onblock_trace.emplace( cache_trace{trace, t} );
      } else {
         cached_traces[trace->id] = {trace, t};
      }

      // ABI capture: scan all action traces (including inlines) in this transaction.
      for (const auto& at : trace->action_traces) {
         if (!at.receipt) continue; // skip context-free or failed actions

         // Lazy ABI fetch: on first encounter of any account, record its current ABI at
         // global_seq 0 ("captured before tracing began; exact version unknown").
         if (_abi_fetcher && _seen_accounts.insert(at.act.account.to_uint64_t()).second) {
            try {
               if (auto abi = _abi_fetcher(at.act.account))
                  store.append_abi(at.act.account, 0, std::move(*abi));
            } catch (...) {}
         }

         // setabi: record the new ABI with its exact global_sequence.
         if (at.act.account == chain::config::system_account_name &&
             at.act.name    == chain::name("setabi")) {
            try {
               chain::name target_account;
               chain::bytes abi_bytes;
               auto ds = fc::datastream<const char*>(at.act.data.data(), at.act.data.size());
               fc::raw::unpack(ds, target_account);
               fc::raw::unpack(ds, abi_bytes);
               store.append_abi(target_account,
                                at.receipt->global_sequence,
                                std::vector<char>(abi_bytes.begin(), abi_bytes.end()));
               // Mark target as seen so lazy fetch doesn't later overwrite with a stale entry.
               _seen_accounts.insert(target_account.to_uint64_t());
            } catch (...) {}
         }
      }
   }

   void on_accepted_block(const chain::signed_block_ptr& block, const chain::block_id_type& id ) {
      store_block_trace( block, id );
   }

   void on_irreversible_block( uint32_t block_num ) {
      store_lib( block_num );
   }

   void on_block_start( uint32_t block_num ) {
      if (!_startup_checked) {
         _startup_checked = true;
         check_continuity(block_num);
      }
      clear_caches();
   }

   void check_continuity(uint32_t block_num) {
      try {
         const auto first = store.first_recorded_block();
         const auto last  = store.last_recorded_block();
         if (!first) {
            ilog("trace_api: no prior trace data found, starting fresh at block {}", block_num);
            return;
         }
         // Overlap or exact continuation: chain head is within or just past existing data.
         // Re-applied blocks will overwrite existing slice entries as they are re-recorded.
         if (block_num >= *first && block_num <= *last + 1)
            return;

         if (block_num < *first) {
            throw std::runtime_error(
               std::string("trace_api: chain head (") + std::to_string(block_num) +
               ") is before the first recorded trace block (" + std::to_string(*first) +
               "). Delete the trace directory and restart to record from the snapshot point.");
         }
         // block_num > last + 1: forward gap
         throw std::runtime_error(
            std::string("trace_api: gap detected in trace data. Last recorded block: ") +
            std::to_string(*last) + ", current block: " + std::to_string(block_num) +
            ". Delete the trace directory and restart to begin fresh, or load a snapshot "
            "that continues from or before block " + std::to_string(*last + 1) + ".");
      } catch (const yield_exception&) {
         throw;
      } catch (...) {
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
      }
   }

   void clear_caches() {
      cached_traces.clear();
      onblock_trace.reset();
   }

   void store_block_trace( const chain::signed_block_ptr& block, const chain::block_id_type& id ) {
      try {
         using transaction_trace_t = transaction_trace_v0;
         auto bt = create_block_trace( block, id );

         std::vector<transaction_trace_t> traces;
         traces.reserve( block->transactions.size() + 1 );
         block_trxs_entry tt;
         tt.ids.reserve(block->transactions.size() + 1);
         if( onblock_trace )
            traces.emplace_back( to_transaction_trace( *onblock_trace ));
         for( const auto& r : block->transactions ) {
            const transaction_id_type& id = r.trx.id();
            const auto it = cached_traces.find( id );
            if( it != cached_traces.end() ) {
               traces.emplace_back( to_transaction_trace( it->second ));
            }
            tt.ids.emplace_back(id);
         }
         bt.transactions = std::move( traces );
         clear_caches();

         // tt entry acts as a placeholder in a trx id slice if this block has no transaction
         tt.block_num = bt.number;
         store.append_trx_ids( std::move(tt) );

         store.append( std::move( bt ) );
      } catch( ... ) {
         except_handler( MAKE_EXCEPTION_WITH_CONTEXT( std::current_exception() ) );
      }
   }

   void store_lib( uint32_t block_num ) {
      try {
         store.append_lib( block_num );
      } catch( ... ) {
         except_handler( MAKE_EXCEPTION_WITH_CONTEXT( std::current_exception() ) );
      }
   }

private:
   StoreProvider                                                store;
   exception_handler                                            except_handler;
   abi_fetcher_t                                                _abi_fetcher;
   std::map<transaction_id_type, cache_trace>                   cached_traces;
   std::optional<cache_trace>                                   onblock_trace;
   // Track seen accounts (by raw uint64 name value) to avoid redundant lazy ABI fetches.
   std::unordered_set<uint64_t>                                 _seen_accounts;
   bool                                                         _startup_checked{false};

};

}}
