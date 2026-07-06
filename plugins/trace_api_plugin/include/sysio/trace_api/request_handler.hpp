#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <type_traits>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/trace_api/bloom_sidecar.hpp>
#include <sysio/trace_api/metadata_log.hpp>
#include <sysio/trace_api/data_log.hpp>
#include <sysio/trace_api/common.hpp>

namespace sysio::trace_api {
   using data_handler_function = std::function<std::tuple<fc::variant, std::optional<fc::variant>>( const std::variant<action_trace_v0>& action)>;

   namespace detail {
      class response_formatter {
      public:
         static fc::variant process_block( const data_log_entry& trace, bool irreversible, const data_handler_function& data_handler );
      };
   }

   // Serialise an action's authorization vector to {actor, permission} JSON
   // objects.  Shared by get_block (response_formatter) and the get_actions
   // / get_token_transfers handlers below.
   inline fc::variants serialize_authorizations(const std::vector<authorization_trace_v0>& auths) {
      fc::variants result;
      result.reserve(auths.size());
      for (const auto& a : auths)
         result.emplace_back(fc::mutable_variant_object()
            ("actor",      a.actor.to_string())
            ("permission", a.permission.to_string()));
      return result;
   }

   // ABI decode payload used by the shared variant builder below.  Mirrors
   // abi_data_handler::decode_result fields that end up in the HTTP response.
   struct decoded_action {
      fc::variant                params;
      std::optional<fc::variant> return_data;
      std::string                error_message;
   };

   enum class variant_shape {
      full,  // get_actions / get_block: every field on action_trace_v0
      slim,  // get_token_transfers: drops ordinals, receipt seqs, ram_deltas, resource usage
   };

   // Shared action->variant builder used by get_actions, get_token_transfers,
   // and the legacy process_block path.  Lives in request_handler.cpp so the
   // formatting logic isn't duplicated in every template instantiation.
   fc::mutable_variant_object build_action_variant(const action_trace_v0& a,
                                                   const decoded_action& decoded,
                                                   variant_shape shape);

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
    * Enforced at block boundaries (see get_actions_impl): the scan stops once a fully scanned block
    * brings the running total to this value, so a single response may exceed it by at most one
    * block's worth of matching actions (itself bounded by consensus block limits).  Clients page
    * past the ceiling by resuming the scan at actions_result::last_block_num + 1.
    *
    * Intentionally a hard-coded constant, NOT a nodeop configuration option: there is no setting to
    * raise or disable it, so the bound holds on every node regardless of operator configuration.
    */
   inline constexpr uint32_t max_actions_per_response = 10'000;

   /**
    * Result returned by get_actions.
    */
   struct actions_result {
      fc::variants actions;
      /// Highest block number fully scanned by the request.  Equals the clamped block_num_end on a
      /// complete scan; lower when max_actions_per_response stopped the scan early.  Clients resume
      /// at last_block_num + 1.
      uint32_t     last_block_num = 0;
   };

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
       * Fetch the trace for a given block height and convert it to a fc::variant for conversion to a final format
       * (eg JSON)
       *
       * @param block_height - the height of the block whose trace is requested
       * @return a properly formatted variant representing the trace for the given block height if it exists, an
       * empty variant otherwise.
       * @throws bad_data_exception when there are issues with the underlying data preventing processing.
       */
      fc::variant get_block_trace( uint32_t block_height ) {
         auto data = logfile_provider.get_block(block_height);
         if (!data) {
            _log("No block found at block height " + std::to_string(block_height) );
            return {};
         }

         auto data_handler = [this](const std::variant<action_trace_v0>& action) -> std::tuple<fc::variant, std::optional<fc::variant>> {
            return std::visit([&](const auto& a) {
               return data_handler_provider.serialize_to_variant(a);
            }, action);
         };

         return detail::response_formatter::process_block(std::get<0>(*data), std::get<1>(*data), data_handler);
      }

      /**
       * Fetch the trace for a given transaction id and convert it to a fc::variant for conversion to a final format
       * (eg JSON)
       *
       * @param trxid - the transaction id whose trace is requested
       * @param block_height - the height of the block whose trace contains requested transaction trace
       * @return a properly formatted variant representing the trace for the given transaction id if it exists, an
       * empty variant otherwise.
       * @throws bad_data_exception when there are issues with the underlying data preventing processing.
       */
      fc::variant get_transaction_trace(chain::transaction_id_type trxid, uint32_t block_height){
         _log("get_transaction_trace called" );
         // Named local with the exact return type so the compiler can NRVO it
         // directly into the caller's slot.  Scan the raw transaction_trace_v0[]
         // for a matching id (cheap sha256 equality) and build the variant for
         // ONLY the matching trx.  The previous implementation materialised the
         // full block variant (ABI-decoding every action) and then string-matched
         // through the resulting JSON, which cost O(block) work per lookup.
         fc::variant result;

         auto data = logfile_provider.get_block(block_height);
         if (!data) {
            _log("No block found at block height " + std::to_string(block_height));
            return result;
         }

         auto data_handler = [this](const std::variant<action_trace_v0>& action) -> std::tuple<fc::variant, std::optional<fc::variant>> {
            return std::visit([&](const auto& a) {
               return data_handler_provider.serialize_to_variant(a);
            }, action);
         };
         const bool irreversible = std::get<1>(*data);

         std::visit([&](const auto& block_trace) {
            for (const auto& trx : block_trace.transactions) {
               if (trx.id == trxid) {
                  // Build a single-transaction variant by calling the shared
                  // formatter on a synthesized block containing only this trx.
                  // Copy the scalar header fields plus the one matching
                  // transaction - copying the whole block first (and discarding
                  // its transaction vector) would duplicate every action's data
                  // buffers and decode nothing extra for it.
                  std::decay_t<decltype(block_trace)> single;
                  single.id                = block_trace.id;
                  single.number            = block_trace.number;
                  single.previous_id       = block_trace.previous_id;
                  single.timestamp         = block_trace.timestamp;
                  single.producer          = block_trace.producer;
                  single.transaction_mroot = block_trace.transaction_mroot;
                  single.finality_mroot    = block_trace.finality_mroot;
                  single.transactions      = {trx};
                  auto block_var = detail::response_formatter::process_block(
                     data_log_entry{std::move(single)}, irreversible, data_handler);
                  const auto& obj = block_var.get_object();
                  if (obj.contains("transactions")) {
                     const auto& txs = obj["transactions"];
                     if (!txs.is_null() && txs.size() > 0)
                        result = txs[size_t{0}];
                  }
                  return;
               }
            }
            _log("Transaction id " + trxid.str() + " not found in block " + std::to_string(block_height));
         }, std::get<0>(*data));

         return result;
      }

      /**
       * Scan a block range for action traces matching the given filter.
       *
       * Blocks are scanned in ascending order.  Within each block, actions are visited in ascending
       * global_sequence order.  Matching actions in the (caller-clamped) block range are returned up
       * to max_actions_per_response; if that ceiling is reached the scan stops at the next block
       * boundary and actions_result::last_block_num reports the last block fully scanned, so the
       * client can resume at last_block_num + 1.  Capping the block window itself remains the
       * caller's responsibility (trace-max-block-range).
       *
       * @param query - filter parameters
       * @return actions_result with the matching actions and the last block scanned
       */
      actions_result get_actions(const action_query& query) {
         return get_actions_impl(query, variant_shape::full);
      }

      /// Slim response for get_token_transfers: transfer-relevant fields only.
      /// Omits execution-tree ordinals, receipt sequences, ram_deltas, and resource usage.
      /// Subject to the same max_actions_per_response ceiling as get_actions.
      actions_result get_token_transfer_actions(const action_query& query) {
         return get_actions_impl(query, variant_shape::slim);
      }

   private:
      actions_result get_actions_impl(const action_query& query, variant_shape shape) {
         actions_result result;

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
         std::vector<const action_trace_v0*> matches;

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
         result.last_block_num = query.block_num_end;

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
            const bool        irreversible_block = std::get<1>(*data);
            const char* const block_status_str   = irreversible_block ? "irreversible" : "pending";

            std::visit([&](const auto& bt) {
               for (const auto& trx : bt.transactions) {
                  // Filter first, sort after.  trx.actions is stored in schedule order (how apply_context scheduled
                  // action slots), which is NOT global_sequence order when a parent action queues both inline actions
                  // and require_recipient notifications: notifications run before inlines, so the inline's
                  // global_sequence is higher than later-scheduled notifications'.  Sort the matches by
                  // global_sequence so clients see execution order, matching chain_plugin's push_transaction response.
                  // global_sequence is unique per action, so sort stability is not required.  Sorting only after
                  // filtering avoids the cost for transactions whose actions are all rejected by the filter - the
                  // common case when scanning for a specific receiver/account/action across a wide block range.
                  matches.clear();
                  for (const auto& a : trx.actions) {
                     if (has_receiver && a.receiver != receiver_name) continue;
                     if (has_account  && a.account  != account_name)  continue;
                     if (has_action   && a.action   != action_name)   continue;
                     matches.push_back(&a);
                  }
                  if (matches.empty()) continue;
                  std::ranges::sort(matches, {}, &action_trace_v0::global_sequence);

                  // Hoist per-trx variant fields so a multi-match trx doesn't repeat the checksum->hex conversion or
                  // re-read the same block-level members for each emitted action.  trx_cpu_usage_us /
                  // trx_net_usage_words are full-shape only - they are the parent transaction's resource totals
                  // (action-level cpu_usage_us / net_usage are per-action and in different units: action net_usage
                  // is bytes, trx net_usage_words is ceil(net_usage / 8)).  Slim (get_token_transfers) omits all
                  // resource fields, so we don't emit the trx-level totals there either.
                  const std::string trx_id_str = trx.id.str();
                  const uint32_t    trx_block  = trx.block_num;
                  const auto&       trx_time   = trx.block_time;
                  const auto&       trx_pbid   = trx.producer_block_id;
                  const bool        full_shape = (shape == variant_shape::full);

                  for (const action_trace_v0* ap : matches) {
                     const auto& a = *ap;
                     // Decode via the provider; build the variant via the shared helper so get_actions /
                     // get_token_transfers / get_block all agree on field shapes.
                     auto dec = data_handler_provider.decode(a);
                     decoded_action da{std::move(dec.params), std::move(dec.return_data), std::move(dec.error_message)};
                     fc::mutable_variant_object av = build_action_variant(a, da, shape);
                     av("trx_id",            trx_id_str)
                       ("block_num",         trx_block)
                       ("block_time",        trx_time)
                       ("producer_block_id", trx_pbid)
                       ("block_status",      block_status_str);
                     if (full_shape) {
                        av("trx_cpu_usage_us",    trx.cpu_usage_us)
                          ("trx_net_usage_words", trx.net_usage_words);
                     }
                     result.actions.emplace_back(std::move(av));
                  }
               }
            }, std::get<0>(*data));

            // Stop at this block boundary once the response reaches the ceiling.  The current block
            // was scanned in full above, so no partial block is ever returned and a client resuming
            // at last_block_num + 1 neither skips nor duplicates actions.  Worst-case overshoot is
            // one block's matching actions.
            if (result.actions.size() >= max_actions_per_response) {
               result.last_block_num = block_num;
               break;
            }
         }

         return result;
      }

      LogfileProvider logfile_provider;
      DataHandlerProvider data_handler_provider;
      log_handler _log;
   };


}
