#pragma once
/**
 * @file synd_crank.hpp
 * @brief Bounded syndication queue observation and retry spacing, independent of the bond request scan window.
 */

#include "underwriter.hpp"

namespace sysio::batch_operator_detail::underwriter {

namespace synd {
   constexpr auto table_ledger = "ledger";
   constexpr auto table_envelopes = "envelopes";
   constexpr auto table_syndconfig = "syndconfig";
   constexpr auto table_buckets = "buckets";
   constexpr auto table_items = "items";
   /// Bound both table reads and retained retry state per queue observation pass.
   constexpr uint32_t pairs_per_poll = 8;
   constexpr uint32_t envelopes_per_pair = 16;
   constexpr uint32_t items_per_envelope = 16;
   constexpr uint32_t point_row_limit = 1;
   namespace field {
      constexpr auto queue_epoch = "queue_epoch";
      constexpr auto epoch_index = "epoch_index";
      constexpr auto state = "state";
      constexpr auto request_id = "request_id";
      constexpr auto released = "released";
      constexpr auto synd_total = "synd_total";
      constexpr auto yield_total = "yield_total";
      constexpr auto synd_burst = "synd_burst";
      constexpr auto synd_refill = "synd_refill";
      constexpr auto direction = "direction";
      constexpr auto level = "level";
      constexpr auto last_epoch = "last_epoch";
      constexpr auto outcome = "outcome";
      constexpr auto burned = "burned";
      constexpr auto id = "id";
      constexpr auto kind = "kind";
      constexpr auto remaining = "remaining";
   }
}

/// Contract enum spellings, decoded with the same checked conversion as bond request states.
enum class envelope_state : uint8_t { OPEN, WAITING, REQUESTED, RELEASABLE, HELD, INVALID, DONE };
/// The bucket direction in the contract's composite key.
enum class bucket_direction : uint8_t { SYNDICATION, DESYNDICATION };
/// Terminal outcomes persisted independently of the bond request row.
enum class request_outcome : uint8_t { PENDING, APPROVED, VALID, INVALID };
/// Items with yield still pending bypass the principal bucket.
enum class item_kind : uint8_t { SYNDICATION, YIELD };
/// A registered outpost/token pair, matching the prefix of all four queue tables.
using synd_pair = std::pair<fc::slug_name, fc::slug_name>;

/// Release and invalidation progress for one envelope, preserving contract lifecycle types.
struct synd_queue_position {
   uint32_t queue_epoch;
   uint32_t epoch_index;
   envelope_state state;
   request_outcome outcome;
   uint64_t released;
   uint64_t burned;
   bool operator==(const synd_queue_position&) const = default;
};

/// Lazy bucket eligibility, including governance changes and the current depot epoch.
struct synd_bucket_position {
   uint32_t epoch;
   uint64_t burst;
   uint64_t refill;
   uint64_t level;
   uint32_t last_epoch;
   bool operator==(const synd_bucket_position&) const = default;
};

/// Item progress in an envelope's bounded prefix, including yield deletions that change no released/burned amount.
struct synd_item_position {
   uint64_t id;
   item_kind kind;
   uint64_t remaining;
   bool operator==(const synd_item_position&) const = default;
};

/// A bounded observation changes when the queue, bucket, configuration or refill epoch progresses.
struct synd_work {
   bool ready = false;
   std::vector<synd_queue_position> positions;
   std::optional<synd_bucket_position> bucket;
   std::vector<synd_item_position> items;
   std::vector<std::pair<request_id_t, request_state>> outcomes;
   bool operator==(const synd_work&) const = default;
};

/// Terminal request outcomes need synchronization even after the release cursor has passed their envelopes.
inline synd_work synd_sync_work(const std::vector<request>& requests) {
   synd_work work;
   for (const request& row : requests) {
      if (!is_envelope_request(row) || !is_terminal(row.state) || row.outcome_acknowledged) continue;
      work.ready = true;
      work.outcomes.emplace_back(row.id, row.state);
   }
   return work;
}

/// The ABI key shared by ledger, envelope, configuration and bucket rows.
inline fc::mutable_variant_object synd_pair_key(const synd_pair& pair) {
   return fc::mutable_variant_object()(synd::field::chain_code, pair.first.value)
                                      (synd::field::token_code, pair.second.value);
}

/// Whether a returned envelope belongs to the requested pair; a bounded lower-bound read can cross its end.
inline bool synd_pair_matches(const fc::variant_object& row, const synd_pair& pair) {
   return row[synd::field::chain_code].as<fc::slug_name>() == pair.first &&
          row[synd::field::token_code].as<fc::slug_name>() == pair.second;
}

/// Read at most one crank's item prefix; the item ids reveal partial invalidation even when every item is yield.
template <typename Read>
std::optional<std::vector<synd_item_position>> read_synd_items(const synd_pair& pair, uint32_t envelope_epoch,
                                                               Read&& read) {
   const std::optional<fc::variants> items = read(synd::table_items,
      synd_pair_key(pair)(synd::field::epoch_index, envelope_epoch)(synd::field::id, 0), false,
      synd::items_per_envelope);
   if (!items) return std::nullopt;
   std::vector<synd_item_position> positions;
   for (const auto& value : *items) {
      const auto& item = value.get_object();
      if (!synd_pair_matches(item, pair) || item[synd::field::epoch_index].as<uint32_t>() != envelope_epoch) break;
      const auto kind = magic_enum::enum_cast<item_kind>(item[synd::field::kind].as_string());
      FC_ASSERT(kind, "unrecognized sysio.synd item kind");
      positions.push_back({item[synd::field::id].as_uint64(), *kind, item[synd::field::remaining].as_uint64()});
   }
   return positions;
}

/**
 * Read one pair from its persisted release cursor, never from retained history or request ids. `read` takes a table,
 * key, exact-lookup flag and row limit and returns optional values-only rows. Any failed or malformed read aborts
 * observation; callers must withhold the crank rather than infer an empty queue. All arithmetic mirrors the lazy
 * bucket's eligibility without multiplying refill amounts. The contract remains responsible for FIFO and limits.
 */
template <typename Read>
std::optional<synd_work> read_synd_work(const synd_pair& pair, uint32_t epoch,
                                        const std::vector<request>& requests, Read&& read) {
   synd_work work;
   const auto key = synd_pair_key(pair);
   const std::optional<fc::variants> ledger = read(synd::table_ledger, key, true, synd::point_row_limit);
   if (!ledger) return std::nullopt;
   if (ledger->empty()) return work;
   const auto queue_epoch = ledger->front().get_object()[synd::field::queue_epoch].as<uint32_t>();
   const std::optional<fc::variants> envelopes = read(synd::table_envelopes,
                               fc::mutable_variant_object(key)(synd::field::epoch_index, queue_epoch), false,
                               synd::envelopes_per_pair);
   if (!envelopes) return std::nullopt;
   std::optional<bool> capacity;
   uint32_t passable = 0;
   for (const auto& value : *envelopes) {
      const auto& row = value.get_object();
      if (!synd_pair_matches(row, pair)) break;
      const auto state = magic_enum::enum_cast<envelope_state>(row[synd::field::state].as_string());
      FC_ASSERT(state, "unrecognized sysio.synd envelope state");
      const auto envelope_epoch = row[synd::field::epoch_index].as<uint32_t>();
      const uint64_t released = row[synd::field::released].as_uint64();
      const uint64_t principal = row[synd::field::synd_total].as_uint64();
      const uint64_t yield = row[synd::field::yield_total].as_uint64();
      const auto outcome = magic_enum::enum_cast<request_outcome>(row[synd::field::outcome].as_string());
      FC_ASSERT(outcome, "unrecognized sysio.synd request outcome");
      const uint64_t burned = row[synd::field::burned].as_uint64();
      work.positions.push_back({queue_epoch, envelope_epoch, *state, *outcome, released, burned});
      // Match contract refresh(): persisted terminal outcomes precede any lookup of the possibly pruned request.
      if (*outcome == request_outcome::INVALID && *state != envelope_state::INVALID) {
         const auto items = read_synd_items(pair, envelope_epoch, read);
         if (!items) return std::nullopt;
         work.items.insert(work.items.end(), items->begin(), items->end());
         work.ready = true;
         break; // partial invalidation gates later FIFO release
      }
      if (*state == envelope_state::HELD && *outcome != request_outcome::PENDING) work.ready = true;
      if (*state == envelope_state::DONE || *state == envelope_state::INVALID || *state == envelope_state::HELD)
         ++passable;
      if (*state == envelope_state::OPEN) break;
      if (*state == envelope_state::WAITING) {
         work.ready = true; // issuing a request is independent of release capacity
         break;
      }
      if (*state == envelope_state::REQUESTED) {
         if (*outcome == request_outcome::PENDING) {
            const auto id = row[synd::field::request_id].as_uint64();
            const auto found = std::ranges::find(requests, id, &request::id);
            if (found == requests.end() || found->state == request_state::OPEN ||
                found->state == request_state::HELD)
               break; // an unbonded prefix gates later envelopes
         }
         work.ready = true; // refresh a bonded or ruled request before releasing
      }
      if (*state != envelope_state::RELEASABLE) continue;
      const bool pending = released < principal || released - principal < yield;
      if (!pending) continue;
      if (!capacity) {
         const std::optional<fc::variants> configs = read(synd::table_syndconfig, key, true, synd::point_row_limit);
         if (!configs) return std::nullopt;
         capacity = false;
         if (!configs->empty()) {
            const auto& config = configs->front().get_object();
            const uint64_t burst = config[synd::field::synd_burst].as_uint64();
            const uint64_t refill = config[synd::field::synd_refill].as_uint64();
            const std::optional<fc::variants> buckets = read(synd::table_buckets,
               fc::mutable_variant_object(key)(synd::field::direction,
                                                magic_enum::enum_integer(bucket_direction::SYNDICATION)), true,
                                                synd::point_row_limit);
            if (!buckets) return std::nullopt;
            uint64_t level = burst;
            uint32_t last_epoch = epoch;
            if (!buckets->empty()) {
               const auto& bucket = buckets->front().get_object();
               level = bucket[synd::field::level].as_uint64();
               last_epoch = bucket[synd::field::last_epoch].as<uint32_t>();
            }
            capacity = burst > 0 && (level > 0 || (epoch > last_epoch && refill > 0));
            work.bucket = synd_bucket_position{epoch, burst, refill, level, last_epoch};
         }
      }
      work.ready = work.ready || *capacity;
      if (!*capacity && yield > 0) {
         const auto items = read_synd_items(pair, envelope_epoch, read);
         if (!items) return std::nullopt;
         work.items.insert(work.items.end(), items->begin(), items->end());
         work.ready = work.ready || std::ranges::any_of(*items, [](const auto& item) {
            return item.kind == item_kind::YIELD && item.remaining > 0;
         });
      }
   }
   // A full finished/held prefix needs a crank to advance the persisted cursor to work beyond this page.
   work.ready = work.ready || passable == synd::envelopes_per_pair;
   return work;
}

/// Retry an unchanged eligible observation conservatively; actual queue progress immediately permits another pass.
struct synd_crank_retry {
   static constexpr auto initial_wait = fc::seconds(30);
   static constexpr auto maximum_wait = fc::minutes(5);
   std::optional<synd_work> attempted;
   std::optional<synd_work> observed; ///< last bounded-page observation, independent of transaction attempts
   fc::time_point retry_at;
   fc::microseconds wait = initial_wait;

   /// Empty or refill-waiting queues submit nothing; an unchanged blocked queue cannot spin every worker poll.
   bool due(const synd_work& work, fc::time_point now) const {
      return work.ready && (!attempted || *attempted != work || now >= retry_at);
   }

   /// Mark even a refused or timed-out submission, which may still reach the chain.
   void note(const synd_work& work, fc::time_point now) {
      wait = attempted && *attempted == work ? std::min(wait * 2, maximum_wait) : initial_wait;
      attempted = work;
      retry_at = now + wait;
   }
};

/**
 * A crank's budget is shared across pairs. Progress in any observed pair wakes the ready set, including when the
 * progressing pair just drained and an unchanged pair was never reached. Remember changes across bounded pages
 * until an actual global attempt clears `progress`. Keep the last observation separately from the last attempt so
 * an empty pair on a different page contributes its drainage once, without repeatedly waking blocked work.
 */
inline bool synd_crank_due(const std::map<synd_pair, synd_work>& work,
                            std::map<synd_pair, synd_crank_retry>& retries, bool& progress, fc::time_point now) {
   bool ready = false;
   bool retry_due = false;
   for (const auto& [pair, observed] : work) {
      ready = ready || observed.ready;
      auto& retry = retries[pair];
      progress = progress || !retry.observed || *retry.observed != observed;
      retry.observed = observed;
      retry_due = retry_due || retry.due(observed, now);
   }
   return retry_due || (ready && progress);
}

} // namespace sysio::batch_operator_detail::underwriter
