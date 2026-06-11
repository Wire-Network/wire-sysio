#pragma once

#include <sysio/trace_api/common.hpp>
#include <sysio/trace_api/extract_util.hpp>
#include <sysio/trace_api/logging.hpp>
#include <sysio/trace_api/trace.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/name.hpp>
#include <fc/io/raw.hpp>
#include <fmt/format.h>
#include <exception>
#include <functional>
#include <map>
#include <unordered_set>

namespace sysio::trace_api {

// Compile-time constant for setabi detection so we don't pay a chain::name
// construction cost on every action.  string_to_name instead of the ""_n
// literal keeps using-directives out of this widely-included header.
inline constexpr chain::name setabi_action_name = chain::string_to_name("setabi");

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
   , abi_fetcher(std::move(abi_fetcher))
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
      collect_abi_ops( *trace );
   }

   /**
    * ABI capture, phase 1 of 2 (collect).  Nothing is written to the abi log here: at
    * execution time it is unknowable whether this transaction's global_sequences become
    * canonical.  A speculative relay execution may never land on-chain, and even a producer's
    * own production round can be aborted and its transactions re-executed later with different
    * sequences.  Gating on trace.producer_block_id is not the answer either: a producer never
    * re-applies its own block, so such a gate permanently loses every ABI deployed in blocks
    * the local node itself produced.  Instead this mirrors the long-standing trace pattern -
    * collect per-transaction candidates here, commit them in store_block_trace only for
    * transactions that appear in the accepted block.  on_block_start clears leftovers, so
    * candidates from aborted or never-included executions are discarded, and a validating
    * node's canonical re-execution of a relayed transaction overwrites any earlier speculative
    * collection for the same transaction id (so a lazy fetch that read speculative state can
    * never be committed - the execution that commits is the one whose state was canonical).
    */
   void collect_abi_ops(const chain::transaction_trace& trace) {
      // First pass: collect setabi targets in this trx so the second pass can
      // skip the lazy fetch for any account whose ABI is being replaced here.
      // on_applied_transaction runs AFTER all actions in the trx have applied,
      // so the chain DB already reflects the new ABI; doing a lazy fetch and
      // recording it as target@0 would poison lookups for actions on target
      // that executed earlier in this same trx (they need the OLD ABI, which
      // is no longer reachable from post-apply state).
      std::unordered_set<chain::name> setabi_targets_this_trx;
      for (const auto& at : trace.action_traces) {
         if (!at.receipt) continue;
         if (at.act.account == chain::config::system_account_name &&
             at.act.name    == setabi_action_name) {
            try {
               chain::name target;
               chain::bytes abi_bytes;
               auto ds = fc::datastream<const char*>(at.act.data.data(), at.act.data.size());
               fc::raw::unpack(ds, target);
               fc::raw::unpack(ds, abi_bytes);
               setabi_targets_this_trx.insert(target);
            } catch (const std::exception& e) {
               fc_wlog(_log, "trace_api: failed to unpack setabi data (collecting targets) at global_seq {}: {}",
                       at.receipt->global_sequence, e.what());
            }
         }
      }

      // Second pass: collect lazy-fetch + setabi candidates, skipping the lazy
      // fetch for any account whose ABI is being replaced in this trx.
      std::vector<pending_abi_op>     ops;
      std::unordered_set<chain::name> lazy_collected_this_trx;
      for (const auto& at : trace.action_traces) {
         if (!at.receipt) continue; // skip context-free or failed actions

         const chain::name account = at.act.account;

         // Lazy ABI fetch: on first encounter of an account (that isn't having
         // its ABI replaced in this same trx), capture its current ABI for a
         // global_seq 0 record so pre-plugin-start actions still decode.
         // "First encounter" is decided by the abi_log itself: if it has no
         // record for this account, we trigger the fetch.  Once any record
         // exists (lazy or setabi), we never re-fetch.  Using the log as
         // source-of-truth avoids holding a per-node-lifetime set of all
         // accounts ever observed.  The fetch happens NOW (state right after
         // this trx applied), but the record is only committed if this trx
         // lands in an accepted block; commit re-checks has_abi_entry since
         // another transaction may record the account first.
         if (abi_fetcher
             && setabi_targets_this_trx.count(account) == 0
             && lazy_collected_this_trx.count(account) == 0
             && !store.has_abi_entry(account))
         {
            try {
               if (auto abi = abi_fetcher(account)) {
                  ops.push_back({abi_op_kind::lazy_fetch, account, 0, std::move(*abi)});
                  lazy_collected_this_trx.insert(account);
               }
            } catch (const std::exception& e) {
               fc_dlog(_log, "trace_api: lazy ABI fetch for {} failed: {}", account, e.what());
            }
         }

         // setabi: capture the new ABI with its exact global_sequence.
         if (at.act.account == chain::config::system_account_name &&
             at.act.name    == setabi_action_name) {
            try {
               chain::name target_account;
               chain::bytes abi_bytes;
               auto ds = fc::datastream<const char*>(at.act.data.data(), at.act.data.size());
               fc::raw::unpack(ds, target_account);
               fc::raw::unpack(ds, abi_bytes);
               ops.push_back({abi_op_kind::setabi,
                              target_account,
                              at.receipt->global_sequence,
                              std::vector<char>(abi_bytes.begin(), abi_bytes.end())});
            } catch (const std::exception& e) {
               fc_wlog(_log, "trace_api: failed to record setabi at global_seq {}: {}",
                       at.receipt->global_sequence, e.what());
            }
         }
      }

      // Plain assignment (not insert) so a canonical re-execution supersedes any earlier
      // speculative collection for the same transaction id; erase when empty for the same
      // reason, and to keep the per-block map minimal.
      if (ops.empty())
         pending_abi_ops.erase(trace.id);
      else
         pending_abi_ops[trace.id] = std::move(ops);
   }

   /**
    * ABI capture, phase 2 of 2 (commit): write the candidates collected for a transaction
    * that made it into an accepted block.  Called from store_block_trace before the block's
    * traces are appended, so a trace is never readable before the ABI that decodes it.
    * has_abi_entry is re-checked for lazy fetches: an earlier transaction in this block, or
    * a block accepted between collection and now, may have recorded the account already.
    */
   void commit_abi_ops(const chain::transaction_id_type& trx_id) {
      auto it = pending_abi_ops.find(trx_id);
      if (it == pending_abi_ops.end()) return;
      for (auto& op : it->second) {
         switch (op.kind) {
         case abi_op_kind::lazy_fetch:
            if (!store.has_abi_entry(op.account))
               store.append_abi(op.account, 0, std::move(op.abi_bytes));
            break;
         case abi_op_kind::setabi:
            store.append_abi(op.account, op.global_sequence, std::move(op.abi_bytes));
            break;
         }
      }
      pending_abi_ops.erase(it);
   }

   void on_accepted_block(const chain::signed_block_ptr& block, const chain::block_id_type& id ) {
      store_block_trace( block, id );
   }

   void on_irreversible_block( uint32_t block_num ) {
      store_lib( block_num );
   }

   void on_block_start( uint32_t block_num ) {
      if (!startup_checked) {
         startup_checked = true;
         check_continuity(block_num);
      }
      clear_caches();
   }

   void check_continuity(uint32_t block_num) {
      try {
         const auto recorded = store.first_and_last_recorded_blocks();
         if (!recorded) {
            fc_ilog(_log, "trace_api: no prior trace data found, starting fresh at block {}", block_num);
            return;
         }
         const uint32_t first = recorded->first;
         const uint32_t last  = recorded->second;

         // The first/last range check below cannot see a hole in the MIDDLE of the recorded range
         // (e.g. an operator deleted or partially copied middle slices); queries inside such a hole
         // would quietly 404.  Cheap filename-level contiguity check, same recovery guidance.
         if (const auto gap = store.find_index_slice_gap()) {
            throw std::runtime_error(fmt::format(
               "trace_api: trace data is missing slices covering blocks {}..{} (recorded range is [{}, {}]). "
               "To recover: copy the trace files covering blocks {}..{} from another node, "
               "or delete the trace directory to start fresh (loses historical traces).",
               gap->first, gap->second, first, last, gap->first, gap->second));
         }

         // Overlap or exact continuation: chain head is within or just past existing data.
         // Re-applied blocks will overwrite existing slice entries as they are re-recorded.
         if (block_num >= first && block_num <= last + 1)
            return;

         if (block_num < first) {
            throw std::runtime_error(fmt::format(
               "trace_api: chain head ({}) is before the first recorded trace block ({}). "
               "To recover: load a snapshot whose chain head is within [{}, {}], "
               "or copy the trace files covering blocks {}..{} from another node, "
               "or delete the trace directory to start fresh (loses historical traces).",
               block_num, first, first, last + 1, block_num, first - 1));
         }
         // block_num > last + 1: forward gap
         throw std::runtime_error(fmt::format(
            "trace_api: gap detected in trace data. Last recorded block: {}, current block: {}. "
            "To recover: load a snapshot covering block {} (or earlier within the recorded range), "
            "or copy the trace files covering blocks {}..{} from another node, "
            "or delete the trace directory to start fresh (loses historical traces).",
            last, block_num, last + 1, last + 1, block_num - 1));
      } catch (const yield_exception&) {
         // Order matters: yield_exception propagates (it's the signal that the
         // plugin's own except_handler uses to unwind the controller), while
         // other exceptions from store.* calls or the throws above go through
         // except_handler so the operator sees a properly formatted message.
         throw;
      } catch (...) {
         except_handler(MAKE_EXCEPTION_WITH_CONTEXT(std::current_exception()));
      }
   }

   void clear_caches() {
      cached_traces.clear();
      onblock_trace.reset();
      pending_abi_ops.clear();
   }

   void store_block_trace( const chain::signed_block_ptr& block, const chain::block_id_type& id ) {
      try {
         using transaction_trace_t = transaction_trace_v0;
         auto bt = create_block_trace( block, id );

         std::vector<transaction_trace_t> traces;
         traces.reserve( block->transactions.size() + 1 );
         block_trxs_entry tt;
         tt.ids.reserve(block->transactions.size() + 1);
         if( onblock_trace ) {
            traces.emplace_back( to_transaction_trace( *onblock_trace ));
            commit_abi_ops( onblock_trace->trace->id );
         }
         for( const auto& r : block->transactions ) {
            const chain::transaction_id_type& id = r.trx.id();
            const auto it = cached_traces.find( id );
            if( it != cached_traces.end() ) {
               traces.emplace_back( to_transaction_trace( it->second ));
            }
            commit_abi_ops( id );
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
   /// Kind of a collected ABI record candidate (see collect_abi_ops / commit_abi_ops).
   enum class abi_op_kind {
      lazy_fetch,  ///< current ABI of a first-encountered account, recorded at global_seq 0
      setabi       ///< ABI bytes carried by a setabi action, recorded at its global_sequence
   };

   /// One ABI record candidate, collected at execution time and committed only if its
   /// transaction lands in an accepted block.
   struct pending_abi_op {
      abi_op_kind       kind;
      chain::name       account;
      uint64_t          global_sequence = 0;  ///< setabi: the receipt's sequence; lazy_fetch: unused (0)
      std::vector<char> abi_bytes;
   };

   StoreProvider                                                store;
   exception_handler                                            except_handler;
   abi_fetcher_t                                                abi_fetcher;
   std::map<chain::transaction_id_type, cache_trace>            cached_traces;
   std::optional<cache_trace>                                   onblock_trace;
   std::map<chain::transaction_id_type, std::vector<pending_abi_op>> pending_abi_ops;
   bool                                                         startup_checked{false};

};

} // namespace sysio::trace_api
