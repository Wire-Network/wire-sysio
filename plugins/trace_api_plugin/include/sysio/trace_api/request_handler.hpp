#pragma once

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/trace_api/bloom_sidecar.hpp>
#include <sysio/trace_api/metadata_log.hpp>
#include <sysio/trace_api/data_log.hpp>
#include <sysio/trace_api/common.hpp>

namespace sysio::trace_api {
   // Emits an action's ABI-decoded "params" and (when applicable) "return_data" key/value pairs directly into the
   // writer.  Decoded bytes flow straight from abi_serializer through binary_to_json_stream into the output buffer
   // with no intermediate fc::variant allocation.
   using stream_data_handler_function = std::function<void(const std::variant<action_trace_v0>& action, fc::json_writer& w)>;

   namespace detail {
      class response_formatter {
      public:
         // Emit a block trace's JSON directly into the caller-provided fc::json_writer.  The HTTP path
         // passes the guarded writer the streaming callback provides, so the memory budget observes the
         // response while it serializes.
         static void process_block_to_stream( const data_log_entry& trace, bool irreversible, const stream_data_handler_function& data_handler, fc::json_writer& w );

         // Buffer-building wrapper over process_block_to_stream (unguarded writer into a local
         // string).  Used by tests and the trace_api_json benchmark.
         static std::string process_block_to_json( const data_log_entry& trace, bool irreversible, const stream_data_handler_function& data_handler );

         // True iff the block trace contains a transaction with the given id.  Lets the HTTP layer
         // resolve its 404 before committing to a 200 emitter -- a streaming response cannot signal
         // "not found" after emission has begun.
         static bool contains_transaction( const data_log_entry& trace, const chain::transaction_id_type& trxid );

         // Streaming counterpart for a single transaction inside a block trace.  Walks the block's
         // transaction list and, for the first entry whose id matches trxid, emits just that
         // transaction's JSON object (no surrounding block wrapper) into the caller-provided writer.
         // Returns false (and emits nothing) if no transaction in the block matches.
         static bool process_transaction_to_stream( const data_log_entry& trace, const chain::transaction_id_type& trxid, const stream_data_handler_function& data_handler, fc::json_writer& w );

         // Buffer-building wrapper over process_transaction_to_stream; returns an empty string on a
         // miss.  Used by tests.
         static std::string process_transaction_to_json( const data_log_entry& trace, const chain::transaction_id_type& trxid, const stream_data_handler_function& data_handler );
      };
   }

   /// The fields an action object carries.
   enum class action_shape {
      full,  // get_actions / get_block: every field on action_trace_v0
      slim,  // get_token_transfers: drops ordinals, receipt seqs, ram_deltas, resource usage
   };

   /**
    * Filter parameters for the get_actions endpoint.
    */
   struct action_query {
      std::optional<chain::name> receiver;      ///< filter by receiver account (any if unset)
      std::optional<chain::name> account;       ///< filter by contract/code account (any if unset)
      std::optional<chain::name> action;        ///< filter by action name (any if unset)
      uint32_t block_num_start = 0;
      uint32_t block_num_end   = std::numeric_limits<uint32_t>::max();
   };

   /**
    * Soft ceiling on the number of action results materialized into a single get_actions /
    * get_token_transfers response.  Complements the per-request block-window cap
    * (trace-max-block-range): the window bounds how many blocks are scanned, this bounds how many
    * matching actions are decoded, hex-serialized, and held in memory from that window, so a broad
    * or unfiltered query over a busy range cannot materialize an unbounded response set.
    *
    * Enforced at block boundaries (see scan_actions): the scan stops once a fully scanned block
    * brings the running total to this value, so a single response may exceed it by at most one
    * block's worth of matching actions (itself bounded by consensus block limits).  Clients page
    * past the ceiling by resuming the scan at the response's block_num_end + 1.
    *
    * Intentionally a hard-coded constant, NOT a nodeop configuration option: there is no setting to
    * raise or disable it, so the bound holds on every node regardless of operator configuration.
    */
   inline constexpr uint32_t max_actions_per_response = 10'000;

   /// Key of the action list in a get_actions response.
   inline constexpr std::string_view actions_list_key   = "actions";
   /// Key of the action list in a get_token_transfers response.
   inline constexpr std::string_view transfers_list_key = "transfers";

   /**
    * A transaction holding at least one action a get_actions / get_token_transfers scan matched.  The scan moves
    * the matched actions out of the block trace it read, so building the response never holds a whole block.
    */
   struct matched_transaction {
      chain::transaction_id_type          id;
      uint32_t                            block_num = 0;
      chain::block_timestamp_type         block_time;
      std::optional<chain::block_id_type> producer_block_id;
      uint32_t                            cpu_usage_us = 0;
      fc::unsigned_int                    net_usage_words;
      bool                                irreversible = false; ///< finality of the containing block
      std::vector<action_trace_v0>        actions;              ///< the matches, in global_sequence order
   };

   /**
    * A get_actions / get_token_transfers scan before any action is decoded.
    */
   struct actions_scan {
      std::vector<matched_transaction> transactions;
      size_t                           action_count   = 0;
      /// Highest block number fully scanned, reported as the response's block_num_end.  Equals the clamped
      /// block_num_end on a complete scan; lower when max_actions_per_response stopped the scan early.  Clients
      /// resume at last_block_num + 1.
      uint32_t                         last_block_num = 0;
   };

   /**
    * A scanned get_actions / get_token_transfers response.  emit writes the whole response object from the
    * matched traces it owns, decoding each action straight into the writer.  retained_size is the packed size of
    * those traces, so the HTTP layer can charge them to its memory budget until the response is written.
    */
   struct actions_emitter {
      std::function<void(fc::json_writer&)> emit;
      size_t                                retained_size = 0;
   };

   /// Write the transaction and block fields that end every get_actions / get_token_transfers action object.
   void write_transaction_fields(fc::json_writer& w, const matched_transaction& trx, action_shape shape);

   /**
    * Write the fields of an action object that come from the trace itself: everything before the decoded "params".
    * Shared by the get_block / get_transaction_trace and get_actions / get_token_transfers responses.
    */
   void write_action_fields(fc::json_writer& w, const action_trace_v0& a, action_shape shape);

   /// Packed size of the action traces a scan holds: the estimate actions_emitter::retained_size reports.
   size_t packed_size(const actions_scan& scan);

   /**
    * Fill query.block_num_start / block_num_end from an already-parsed request body object.
    *
    * fc's as<uint32_t>() narrows with a raw static_cast of as_uint64(), so an oversized JSON value
    * (e.g. 2^32) would silently wrap to a small block number instead of failing, and mixed oversized
    * values could bypass the start <= end validation after truncation.  Parse through as_uint64()
    * and range-check explicitly instead; out-of-range values throw, which the HTTP handlers
    * translate into a 400 response.  Free function so the HTTP handlers' behaviour is unit-testable.
    *
    * @throws fc::exception when a present field does not fit in uint32_t.
    */
   inline void parse_query_block_range(const fc::variant_object& obj, action_query& query) {
      auto parse_field = [&obj](const char* field, uint32_t& out) {
         if (!obj.contains(field))
            return;
         const uint64_t value = obj[field].as_uint64();
         FC_ASSERT(value <= std::numeric_limits<uint32_t>::max(), "{} out of range: {}", field, value);
         out = static_cast<uint32_t>(value);
      };
      parse_field("block_num_start", query.block_num_start);
      parse_field("block_num_end",   query.block_num_end);
   }

   /**
    * Canonical-only defaulting for the get_actions HTTP layer: when the caller specifies exactly one
    * of receiver/account and does not opt into notifications, mirror the specified value onto the
    * missing one so only the canonical execution (receiver == account) matches - no notification
    * copies.  Free function so the HTTP handler's behaviour is unit-testable.
    */
   inline void apply_canonical_default(action_query& query, bool include_notifications) {
      if (include_notifications)
         return;
      if (query.account && !query.receiver)
         query.receiver = query.account;
      else if (query.receiver && !query.account)
         query.account = query.receiver;
   }

   /**
    * Clamp block_num_end so the scan spans at most max_block_range blocks.  Computed in 64-bit:
    * block_num_start + range - 1 can exceed uint32_t near the top of the range, and reducing
    * unconditionally keeps the bound robust regardless of the configured range.
    */
   inline void clamp_query_range(action_query& query, uint32_t max_block_range) {
      const uint64_t max_end = uint64_t{query.block_num_start} + max_block_range - 1;
      query.block_num_end    = static_cast<uint32_t>(std::min<uint64_t>(query.block_num_end, max_end));
   }

   /**
    * Clamp block_num_end to the last block actually recorded by the store.  The response envelope
    * reports the clamped range as "scanned", so without this a request spanning not-yet-produced
    * (or never-recorded) blocks would report them as scanned and a client resuming at
    * block_num_end + 1 would permanently skip them once they exist.  When nothing in the window is
    * recorded yet, the window collapses to just below block_num_start ("nothing scanned") so
    * resume-at-end+1 retries the same spot.
    */
   inline void clamp_query_end_to_recorded(action_query& query, uint32_t last_recorded_block) {
      if (last_recorded_block < query.block_num_start) {
         query.block_num_end = query.block_num_start == 0 ? 0 : query.block_num_start - 1;
      } else {
         query.block_num_end = std::min(query.block_num_end, last_recorded_block);
      }
   }

   template<typename LogfileProvider, typename DataHandlerProvider>
   class request_handler {
   public:
      request_handler(LogfileProvider&& logfile_provider, DataHandlerProvider&& data_handler_provider, log_handler log)
      :logfile_provider(std::move(logfile_provider))
      ,data_handler_provider(std::move(data_handler_provider))
      ,_log(log)
      {
         _log("Constructed request_handler");
      }

      /**
       * Fetch the trace for a given block height, for the HTTP streaming-callback path.
       *
       * Resolves block existence up front so the caller can produce its 404 before
       * committing to a 200 emitter; the returned closure (owning the block's decoded
       * trace data) emits the response body into the caller-provided fc::json_writer.
       * The HTTP layer passes its guarded writer, so the memory budget observes -- and
       * can abort -- the response while it serializes instead of after it is allocated.
       *
       * @param block_height - the height of the block whose trace is requested
       * @return an emitter for the block's trace, or std::nullopt if the block does not exist
       * @throws bad_data_exception when there are issues with the underlying data preventing processing.
       */
      std::optional<std::function<void(fc::json_writer&)>> get_block_trace_emitter( uint32_t block_height ) {
         auto data = logfile_provider.get_block(block_height);
         if (!data) {
            _log("No block found at block height " + std::to_string(block_height) );
            return std::nullopt;
         }

         return [this, data = std::move(*data)](fc::json_writer& w) {
            detail::response_formatter::process_block_to_stream(std::get<0>(data), std::get<1>(data),
                                                                make_stream_data_handler(), w);
         };
      }

      /**
       * Buffer-building wrapper over get_block_trace_emitter: emits into a local string via
       * an unguarded writer.  Returned string is empty if the block does not exist.  Kept
       * for direct (non-HTTP) callers and tests; the HTTP path uses the emitter form so the
       * guarded writer's budget applies.
       */
      std::string get_block_trace_json( uint32_t block_height ) {
         auto emit = get_block_trace_emitter(block_height);
         if (!emit)
            return {};
         std::string out;
         {
            fc::json_writer w(out);
            (*emit)(w);
         }
         return out;
      }

      /**
       * Fetch the trace for a given transaction id, for the HTTP streaming-callback path.
       *
       * Resolves both misses up front -- block absent, or trxid not present in the block
       * (via response_formatter::contains_transaction) -- so the caller can produce its
       * 404 before committing to a 200 emitter; the returned closure emits just the
       * matching transaction's JSON into the caller-provided (guarded) fc::json_writer.
       *
       * @param trxid - the transaction id whose trace is requested
       * @param block_height - the height of the block whose trace contains the transaction
       * @return an emitter for the transaction's trace, or std::nullopt on either miss
       * @throws bad_data_exception when there are issues with the underlying data preventing processing.
       */
      std::optional<std::function<void(fc::json_writer&)>> get_transaction_trace_emitter( const chain::transaction_id_type& trxid, uint32_t block_height ) {
         _log("get_transaction_trace_emitter called");
         auto data = logfile_provider.get_block(block_height);
         if (!data) {
            _log("No block found at block height " + std::to_string(block_height));
            return std::nullopt;
         }
         if (!detail::response_formatter::contains_transaction(std::get<0>(*data), trxid)) {
            _log("No transaction with id " + trxid.str() + " found in block " + std::to_string(block_height));
            return std::nullopt;
         }

         return [this, data = std::move(*data), trxid](fc::json_writer& w) {
            [[maybe_unused]] const bool found = detail::response_formatter::process_transaction_to_stream(
               std::get<0>(data), trxid, make_stream_data_handler(), w);
            assert(found); // contains_transaction gated emitter creation
         };
      }

      /**
       * Buffer-building wrapper over get_transaction_trace_emitter: emits into a local
       * string via an unguarded writer.  Returned string is empty if the block does not
       * exist or the trxid is not present in the block.  Kept for direct (non-HTTP)
       * callers and tests; the HTTP path uses the emitter form so the guarded writer's
       * budget applies.
       */
      std::string get_transaction_trace_json( const chain::transaction_id_type& trxid, uint32_t block_height ) {
         auto emit = get_transaction_trace_emitter(trxid, block_height);
         if (!emit)
            return {};
         std::string out;
         {
            fc::json_writer w(out);
            (*emit)(w);
         }
         return out;
      }

      /**
       * Scan a block range for action traces matching the given filter, for get_actions.
       *
       * Blocks are scanned in ascending order.  Within each block, actions are listed in ascending
       * global_sequence order.  Matching actions in the (caller-clamped) block range are returned up
       * to max_actions_per_response; if that ceiling is reached the scan stops at the next block
       * boundary and the response's block_num_end reports the last block fully scanned, so the
       * client can resume at block_num_end + 1.  Capping the block window itself remains the
       * caller's responsibility (trace-max-block-range).
       *
       * Scans now and returns an emitter for the whole response, {"block_num_start", "block_num_end",
       * "actions": [...]}, that decodes each matched action straight into the caller's writer.  The emitter
       * binds `this`, so it must not outlive the handler.
       *
       * @param query - filter parameters; block_num_start is echoed in the response
       */
      actions_emitter get_actions_emitter(const action_query& query) {
         return make_actions_emitter(query, action_shape::full, actions_list_key);
      }

      /// get_token_transfers: as get_actions_emitter, with the slim shape (transfer-relevant fields only: no
      /// execution-tree ordinals, receipt sequences, ram_deltas or resource usage) listed under "transfers".
      actions_emitter get_token_transfer_actions_emitter(const action_query& query) {
         return make_actions_emitter(query, action_shape::slim, transfers_list_key);
      }

      /**
       * Buffer-building wrapper over get_actions_emitter: emits the response into a local string through an
       * unguarded writer.  Kept for tests; the HTTP path uses the emitter so the guarded writer's budget applies.
       */
      std::string get_actions_json(const action_query& query) {
         return emit_to_json(get_actions_emitter(query));
      }

      /// Buffer-building wrapper over get_token_transfer_actions_emitter, as get_actions_json.
      std::string get_token_transfer_actions_json(const action_query& query) {
         return emit_to_json(get_token_transfer_actions_emitter(query));
      }

   private:
      static std::string emit_to_json(const actions_emitter& emitter) {
         std::string out;
         {
            fc::json_writer w(out);
            emitter.emit(w);
         }
         return out;
      }

      actions_emitter make_actions_emitter(const action_query& query, action_shape shape, std::string_view list_key) {
         actions_scan scan     = scan_actions(query);
         const size_t retained = packed_size(scan);
         return {[this, scan = std::move(scan), block_num_start = query.block_num_start, shape,
                  list_key](fc::json_writer& w) {
                    w.begin_object();
                    w.set("block_num_start", block_num_start).set("block_num_end", scan.last_block_num);
                    w.key(list_key);
                    w.begin_array();
                    for (const auto& trx : scan.transactions) {
                       for (const auto& a : trx.actions) {
                          w.begin_object();
                          write_action_fields(w, a, shape);
                          data_handler_provider.decode_to_json_stream(a, w);
                          write_transaction_fields(w, trx, shape);
                          w.end_object();
                       }
                    }
                    w.end_array();
                    w.end_object();
                 },
                 retained};
      }

      /// Adapter from the provider's per-action streaming serializer to the
      /// stream_data_handler_function shape the response_formatter consumes.  Shared by
      /// both emitter factories; binds `this`, so emitters must not outlive the handler.
      stream_data_handler_function make_stream_data_handler() {
         return [this](const std::variant<action_trace_v0>& action, fc::json_writer& w) {
            std::visit([&](const auto& a) {
               data_handler_provider.serialize_to_json_stream(a, w);
            }, action);
         };
      }

      /**
       * Collect the action traces in the query's block range that match its filter, without decoding them.  Each
       * match is moved out of the block trace it was read from, along with its transaction's fields, so a block is
       * held only while it is scanned.  Stops at the block boundary where the match count reaches
       * max_actions_per_response (see get_actions_emitter).
       */
      actions_scan scan_actions(const action_query& query) {
         actions_scan scan;

         // Hoist filter state out of the hot loop: avoids re-loading the optional's discriminator and value on every
         // action comparison in the inner scan.
         const bool        has_receiver  = query.receiver.has_value();
         const bool        has_account   = query.account.has_value();
         const bool        has_action    = query.action.has_value();
         const chain::name receiver_name = has_receiver ? *query.receiver : chain::name{};
         const chain::name account_name  = has_account  ? *query.account  : chain::name{};
         const chain::name action_name   = has_action   ? *query.action   : chain::name{};

         // Reused across all transactions in all blocks: clear() keeps the vector's capacity so repeated scans of
         // trxs with similar action counts avoid per-trx allocations.
         std::vector<action_trace_v0*> matches;

         // Per-slice bloom skip state.  When the caller supplies a receiver (or a non-include_notifications
         // request whose receiver is auto-mirrored onto account upstream), probe the slice's receiver bloom and
         // advance block_num past the slice on a negative probe.  The bloom is opened once per slice (lazy; only
         // if skipping is useful for this query) and held for the life of the scan through that slice.  If the
         // sidecar is missing or CRC-corrupt the bloom_reader is invalid and may_contain_* returns true, which
         // preserves the existing scan behaviour.
         const uint32_t stride = logfile_provider.slice_stride();
         // Both bloom probes below are gated on has_receiver (the receiver bloom is the only one we can hit with a
         // single-filter probe, and the (receiver, action) composite still needs the receiver term).  A query with
         // only account and/or action set can't benefit from the bloom, so don't even open the sidecar.
         const bool skip_eligible = has_receiver;
         std::optional<uint32_t> current_slice;
         bool skip_current_slice = false;

         // Drive the scan with a 64-bit counter.  block_num_end can be UINT32_MAX (its default, and
         // unvalidated client input on the HTTP path), so a uint32_t counter would wrap from UINT32_MAX
         // back to 0 at ++block_num and spin forever.  bn stays in [start, end+1] and never wraps; the
         // HTTP-side clamp is then only a range bound, not the sole thing preventing an infinite loop.

         // Default to reporting a complete scan; lowered below only if the action ceiling stops us
         // early.  On natural completion this stays at the clamped end, so trailing blocks with no
         // recorded data are still counted as scanned and a client resumes past them, not before.
         scan.last_block_num = query.block_num_end;

         const uint64_t end = query.block_num_end;
         for (uint64_t bn = query.block_num_start; bn <= end; ++bn) {
            // bn <= end <= UINT32_MAX throughout the loop body, so this narrowing is value-preserving.
            const uint32_t block_num = static_cast<uint32_t>(bn);
            if (skip_eligible) {
               const uint32_t slice = logfile_provider.slice_number(block_num);
               if (!current_slice || *current_slice != slice) {
                  current_slice = slice;
                  skip_current_slice = false;
                  bloom_reader r = logfile_provider.get_bloom(slice);
                  if (r.valid()) {
                     if (!r.may_contain_receiver(receiver_name)) {
                        skip_current_slice = true;
                     } else if (has_action
                                && !r.may_contain_recv_action(receiver_name, action_name)) {
                        skip_current_slice = true;
                     }
                  }
               }
               if (skip_current_slice) {
                  // Jump bn to the last block of this slice so the for-loop's ++bn takes us to the first block
                  // of the next slice.  Compute in 64-bit: (slice+1)*stride overflows uint32_t near the top of
                  // the range and would wrap slice_last to a small value, driving bn *backwards* into an
                  // infinite loop.  Clamp to the query's end so the last slice in the range doesn't overshoot.
                  const uint64_t slice_last = (uint64_t{slice} + 1) * stride - 1;
                  bn = std::min(slice_last, end);
                  continue;
               }
            }

            auto data = logfile_provider.get_block(block_num);
            if (!data) continue;

            // Block-finality marker mirrors get_block's "status" field. Sourced from the same data log
            // tuple so callers can trust trace_api as a single source of truth for "did this action's
            // block reach finality."  Promotion (pending -> irreversible) happens out-of-band as LIB
            // advances; consumers that gate on finality must re-poll, same as get_block today.
            const bool irreversible_block = std::get<1>(*data);

            std::visit([&](auto& bt) {
               for (auto& trx : bt.transactions) {
                  // Filter first, sort after.  trx.actions is stored in schedule order (how apply_context scheduled
                  // action slots), which is NOT global_sequence order when a parent action queues both inline actions
                  // and require_recipient notifications: notifications run before inlines, so the inline's
                  // global_sequence is higher than later-scheduled notifications'.  Sort the matches by
                  // global_sequence so clients see execution order, matching chain_plugin's push_transaction response.
                  // global_sequence is unique per action, so sort stability is not required.  Sorting only after
                  // filtering avoids the cost for transactions whose actions are all rejected by the filter - the
                  // common case when scanning for a specific receiver/account/action across a wide block range.
                  matches.clear();
                  for (auto& a : trx.actions) {
                     if (has_receiver && a.receiver != receiver_name) continue;
                     if (has_account  && a.account  != account_name)  continue;
                     if (has_action   && a.action   != action_name)   continue;
                     matches.push_back(&a);
                  }
                  if (matches.empty()) continue;
                  std::ranges::sort(matches, {}, &action_trace_v0::global_sequence);

                  // The block is discarded after this scan, so its matched actions move into the result.
                  matched_transaction& matched = scan.transactions.emplace_back();
                  matched.id                   = trx.id;
                  matched.block_num            = trx.block_num;
                  matched.block_time           = trx.block_time;
                  matched.producer_block_id    = trx.producer_block_id;
                  matched.cpu_usage_us         = trx.cpu_usage_us;
                  matched.net_usage_words      = trx.net_usage_words;
                  matched.irreversible         = irreversible_block;
                  matched.actions.reserve(matches.size());
                  for (action_trace_v0* ap : matches)
                     matched.actions.push_back(std::move(*ap));
                  scan.action_count += matches.size();
               }
            }, std::get<0>(*data));

            // Stop at this block boundary once the response reaches the ceiling.  The current block
            // was scanned in full above, so no partial block is ever returned and a client resuming
            // at last_block_num + 1 neither skips nor duplicates actions.  Worst-case overshoot is
            // one block's matching actions.
            if (scan.action_count >= max_actions_per_response) {
               scan.last_block_num = block_num;
               break;
            }
         }

         return scan;
      }

      LogfileProvider logfile_provider;
      DataHandlerProvider data_handler_provider;
      log_handler _log;
   };


}
