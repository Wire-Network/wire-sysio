#pragma once

#include <sysio/chain/vote_message.hpp>
#include <sysio/chain/block_state.hpp>
#include <sysio/chain/controller.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sysio::chain {

/// Sizing of the vote processor's limits from the block interval, the finalizer count and the vote threads.
namespace vote_limits {
   /// Time a parked vote waits for its block before it is discarded.
   inline constexpr fc::microseconds parked_vote_expiry = fc::seconds(5);
   /// Blocks the newest known block may lag the chain head by.
   inline constexpr block_num_type park_lead_margin_blocks = 2;
   /// park_lead_blocks = parked_vote_expiry / block interval + park_lead_margin_blocks: a vote for a block further
   /// beyond the newest known block expires before its block arrives.
   inline constexpr block_num_type park_lead_blocks =
      block_num_type{parked_vote_expiry.count() / config::block_interval_us} + park_lead_margin_blocks;
   /// Finalizer count the limits are sized for when the policies hold fewer, and before the first block is reported.
   inline constexpr size_t min_finalizers = 21;
   /// Parking windows of votes one connection may hold: one parked and one queued, doubled for forks and bursts.
   inline constexpr size_t connection_windows = 4;
   /// Parking windows of votes the parked set holds; a vote is parked once for each connection that relays it.
   inline constexpr size_t parked_windows = 8;
   /// Time a vote thread takes to verify a vote: BLS signature verification measures about 1.25 ms on one core.
   inline constexpr fc::microseconds vote_verification_time = fc::microseconds(1250);
   /// Verification time of the votes queued across connections.
   inline constexpr fc::microseconds max_queued_verification_time = fc::seconds(2);

   /// window_votes = max(finalizers, min_finalizers) * park_lead_blocks: one vote per finalizer per block.
   constexpr size_t window_votes(size_t finalizers) {
      return std::max(finalizers, min_finalizers) * park_lead_blocks;
   }
   /// max_votes_per_connection = window_votes * connection_windows
   constexpr size_t max_votes_per_connection(size_t finalizers) {
      return window_votes(finalizers) * connection_windows;
   }
   /// max_parked_votes = window_votes * parked_windows
   constexpr size_t max_parked_votes(size_t finalizers) {
      return window_votes(finalizers) * parked_windows;
   }
   /// max_queued_votes = vote_threads * max_queued_verification_time / vote_verification_time
   constexpr size_t max_queued_votes(size_t vote_threads) {
      return vote_threads * size_t(max_queued_verification_time.count() / vote_verification_time.count());
   }
} // namespace vote_limits

/**
 * Process votes in a dedicated thread pool.
 *
 * A vote counts against its connection from the time it is posted to the thread pool until it is aggregated, dropped,
 * or leaves the parked set. A vote for a block not in the fork database is parked under its block id and aggregated
 * when notify_new_block() reports that block. Votes beyond the parking lead, and parked votes that expire or fall at or
 * below LIB, are discarded without a report.
 *
 * A session is the span of a connection id between notify_connection_closed() calls. Once a vote of a session fails
 * with invalid_signature or unknown_public_key, the session's other votes are dropped without verification.
 *
 * Unless constructed with fixed limits, the per-connection and parked limits are sized by vote_limits for the
 * finalizers of the policies of the block last reported to notify_new_block(), and the queued limit for the vote
 * threads.
 */
class vote_processor_t {
public:
   /// Limits on the votes the processor holds; the defaults are sized for vote_limits::min_finalizers and one thread.
   struct limits_t {
      /// Votes from one connection, queued or parked, at which its further votes are dropped and reported max_exceeded.
      size_t           max_votes_per_connection = vote_limits::max_votes_per_connection(vote_limits::min_finalizers);
      /// Votes queued from all connections at which further votes from peers are dropped.
      size_t           max_queued_votes         = vote_limits::max_queued_votes(1);
      /// Parked votes at which further votes for unknown blocks are dropped.
      size_t           max_parked_votes         = vote_limits::max_parked_votes(vote_limits::min_finalizers);
      /// Votes from peers for blocks more than this many blocks beyond the newest known block are dropped.
      block_num_type   park_lead_blocks         = vote_limits::park_lead_blocks;
      /// Time a parked vote waits for its block before it is discarded.
      fc::microseconds parked_vote_expiry       = vote_limits::parked_vote_expiry;
   };

   using emit_vote_signal_func_t = std::function<void(const vote_signal_params&)>;
   using fetch_block_func_t      = std::function<block_state_ptr(const block_id_type&)>;

private:
   /// Connection id of votes from this node's finalizers: never limited, rejected or reported.
   static constexpr uint32_t local_connection_id = 0;

   struct by_block_id;
   struct by_block_num;
   struct by_connection;
   struct by_received;

   /// A vote parked until its block is added to the fork database.
   struct vote {
      uint32_t         connection_id;
      uint64_t         session;
      fc::time_point   received;
      vote_message_ptr msg;

      const block_id_type& id() const { return msg->block_id; }
      block_num_type block_num() const { return block_header::num_from_id(msg->block_id); }
   };

   // Ordered, not hashed: block ids come from peers, which could choose ids whose hashes collide.
   using vote_index_type = boost::multi_index_container< vote,
      indexed_by<
         ordered_non_unique< tag<by_block_id>, const_mem_fun<vote, const block_id_type&, &vote::id> >,
         ordered_non_unique< tag<by_block_num>, const_mem_fun<vote, block_num_type, &vote::block_num> >,
         ordered_non_unique< tag<by_connection>, member<vote, uint32_t, &vote::connection_id> >,
         ordered_non_unique< tag<by_received>, member<vote, fc::time_point, &vote::received> >
      >
   >;

   /// Votes held for a connection id. Erased once the connection has closed and none of its votes are queued.
   struct connection_votes {
      uint32_t                queued = 0;       ///< posted to the thread pool and not yet run
      uint32_t                parked = 0;       ///< in the parked index
      uint64_t                session = 0;      ///< current session, unique for the life of the processor
      std::optional<uint64_t> rejected_session; ///< latest session whose votes are dropped without verification
      bool                    closed = false;   ///< closed with votes still queued, no vote received since
   };
   using connection_map = std::unordered_map<uint32_t, connection_votes>;

   const bool                   size_from_chain;  // false when constructed with fixed limits
   emit_vote_signal_func_t      emit_vote_signal_func;
   fetch_block_func_t           fetch_block_func;

   std::mutex                   mtx;
   limits_t                     limits;           // caps guarded by mtx; park_lead_blocks and parked_vote_expiry are
                                                  // never changed and are read without it
   finalizer_policy_ptr         sized_active_policy;  // policies the caps were last sized for
   finalizer_policy_ptr         sized_pending_policy;
   vote_index_type              index;            // parked votes
   block_state_ptr              last_bsp;
   connection_map               connections;
   uint32_t                     queued_votes = 0; // across all connections
   uint64_t                     last_session = 0;

   std::atomic<block_num_type>  lib{0};
   std::atomic<block_num_type>  newest_block_num{0};
   std::atomic<bool>            stopped{true};
   named_thread_pool<vote>      thread_pool;

private:
   /// Report a vote result to the signal. Called with mtx unlocked.
   void emit(uint32_t connection_id, vote_result_t status, const vote_message_ptr& msg,
             const finalizer_authority_ptr& active_auth, const finalizer_authority_ptr& pending_auth) {
      if (connection_id != local_connection_id) { // this nodes vote was already signaled
         if (status != vote_result_t::duplicate) { // don't bother emitting duplicates
            emit_vote_signal_func(vote_signal_params{connection_id, status,
                                                     std::cref(msg), std::cref(active_auth), std::cref(pending_auth)});
         }
      }
   }

   /// Raise the newest block number known to be in the fork database to at least block_num.
   void raise_newest_block_num(block_num_type block_num) {
      block_num_type newest = newest_block_num.load(std::memory_order_relaxed);
      while (block_num > newest &&
             !newest_block_num.compare_exchange_weak(newest, block_num, std::memory_order_relaxed)) {
      }
   }

   /// True when block_num is more than the parking lead beyond the newest known block.
   bool beyond_park_lead(block_num_type block_num) const {
      const block_num_type newest = newest_block_num.load(std::memory_order_relaxed);
      return block_num > newest && block_num - newest > limits.park_lead_blocks;
   }

   /// Look up a block, caching the last one found. Called with mtx locked.
   block_state_ptr get_block(const block_id_type& id) {
      if (last_bsp && last_bsp->id() == id)
         return last_bsp;
      block_state_ptr bsp = fetch_block_func(id);
      if (bsp) {
         last_bsp = bsp;
         raise_newest_block_num(bsp->block_num());
      }
      return bsp;
   }

   /// Discard the parked votes of a connection. Called with mtx locked.
   void remove_parked(connection_map::iterator it) {
      auto& idx = index.get<by_connection>();
      idx.erase(idx.lower_bound(it->first), idx.upper_bound(it->first));
      it->second.parked = 0;
   }

   /// Remove the parked votes in [first, last) of idx. Called with mtx locked.
   template <typename Index>
   void erase_parked(Index& idx, typename Index::iterator first, typename Index::iterator last) {
      for (auto it = first; it != last; ++it) {
         auto c = connections.find(it->connection_id);
         assert(c != connections.end() && c->second.parked > 0); // not erased while it has parked votes
         --c->second.parked;
      }
      idx.erase(first, last);
   }

   /// Discard the parked votes whose block is at or below lib or that expired. Called with mtx locked.
   void discard_unmatched() {
      if (index.empty())
         return;
      auto& by_num = index.get<by_block_num>();
      erase_parked(by_num, by_num.begin(), by_num.upper_bound(lib.load(std::memory_order_relaxed)));
      auto& by_time = index.get<by_received>();
      const fc::time_point expired = fc::time_point::now() - limits.parked_vote_expiry;
      erase_parked(by_time, by_time.begin(), by_time.upper_bound(expired));
   }

   /// Park or drop a vote taken off the queue, or return its block when the vote is to be aggregated.
   /// Called with mtx locked.
   block_state_ptr dispose(connection_map::iterator it, uint64_t session, const vote_message_ptr& msg) {
      connection_votes& c = it->second;
      if (c.rejected_session == session)
         return {}; // the session sent a vote that failed verification
      if (block_header::num_from_id(msg->block_id) <= lib.load(std::memory_order_relaxed))
         return {}; // ignore votes at or below lib
      // Looked up under mtx so that a block added to the fork database is either found here or drains this vote in
      // notify_new_block().
      block_state_ptr bsp = get_block(msg->block_id);
      if (bsp)
         return bsp;
      if (c.session != session)
         return {}; // votes of a closed session are aggregated when their block is known, never parked
      if (index.size() >= limits.max_parked_votes) {
         ilog("Exceeded max parked votes {}, dropping vote from {}", limits.max_parked_votes, it->first);
         return {};
      }
      index.insert(vote{.connection_id = it->first, .session = session, .received = fc::time_point::now(), .msg = msg});
      ++c.parked;
      return {};
   }

   /// Take an admitted vote off the queue and act on it. Runs on the thread pool, or inline for async_t::no.
   void process_queued_vote(uint32_t connection_id, uint64_t session, const vote_message_ptr& msg) {
      if (stopped)
         return;
      std::unique_lock g(mtx);
      discard_unmatched();
      auto it = connections.find(connection_id);
      assert(it != connections.end()); // not erased while it has queued votes
      --it->second.queued;
      --queued_votes;
      const block_state_ptr bsp = dispose(it, session, msg);
      if (it->second.closed && it->second.queued == 0) {
         assert(it->second.parked == 0); // closing purges parked votes and closed sessions do not park
         connections.erase(it);
      }
      g.unlock();

      if (bsp)
         aggregate(connection_id, session, bsp, msg);
   }

   /// Verify and aggregate a vote, rejecting its session when it fails verification. Called with mtx unlocked.
   void aggregate(uint32_t connection_id, uint64_t session, const block_state_ptr& bsp, const vote_message_ptr& msg) {
      aggregate_vote_result_t r = bsp->aggregate_vote(connection_id, *msg);
      if (r.result == vote_result_t::invalid_signature || r.result == vote_result_t::unknown_public_key)
         reject(connection_id, session);
      emit(connection_id, r.result, msg, r.active_authority, r.pending_authority);
   }

   /// Drop the other votes of a session without verification and discard its parked votes. Called with mtx unlocked.
   void reject(uint32_t connection_id, uint64_t session) {
      if (connection_id == local_connection_id)
         return;
      std::lock_guard g(mtx);
      auto it = connections.find(connection_id);
      if (it == connections.end())
         return; // closed with nothing queued, no vote of the session remains
      connection_votes& c = it->second;
      if (!c.rejected_session || *c.rejected_session < session)
         c.rejected_session = session;
      if (c.session == session)
         remove_parked(it);
   }

   /// Aggregate the votes parked for bsp and discard unmatched parked votes. Called with mtx unlocked.
   void drain_parked(const block_state_ptr& bsp) {
      if (stopped)
         return;
      std::vector<vote> ready;
      {
         std::lock_guard g(mtx);
         if (index.empty())
            return;
         auto& idx = index.get<by_block_id>();
         auto [first, last] = idx.equal_range(bsp->id());
         ready.assign(first, last);
         erase_parked(idx, first, last);
         discard_unmatched();
      }
      for (const vote& v : ready) {
         if (stopped)
            return;
         if (is_rejected(v.connection_id, v.session))
            continue; // the session sent a vote that failed verification after this one was parked
         aggregate(v.connection_id, v.session, bsp, v.msg);
      }
   }

   /// True when the session sent a vote that failed verification. Called with mtx unlocked.
   bool is_rejected(uint32_t connection_id, uint64_t session) {
      std::lock_guard g(mtx);
      auto it = connections.find(connection_id);
      return it != connections.end() && it->second.rejected_session == session;
   }

   /// Number of distinct finalizer keys in the active policy and the pending policy, if any.
   static size_t count_distinct_finalizers(const finalizer_policy_ptr& active, const finalizer_policy_ptr& pending) {
      std::vector<const bls_public_key*> keys;
      for (const finalizer_policy_ptr* policy : {&active, &pending}) {
         if (*policy) {
            for (const finalizer_authority& f : (*policy)->finalizers)
               keys.push_back(&f.public_key);
         }
      }
      auto less  = [](const bls_public_key* a, const bls_public_key* b) { return *a < *b; };
      auto equal = [](const bls_public_key* a, const bls_public_key* b) { return *a == *b; };
      std::ranges::sort(keys, less);
      return keys.size() - std::ranges::unique(keys, equal).size();
   }

   /// Size the per-connection and parked limits for the finalizers of bsp's active and pending policies.
   /// Called with mtx unlocked.
   void size_for_policies(const block_state& bsp) {
      const finalizer_policy_ptr& active  = bsp.active_finalizer_policy;
      const finalizer_policy_ptr  pending = bsp.pending_finalizer_policy ? bsp.pending_finalizer_policy->second
                                                                         : finalizer_policy_ptr{};
      {
         std::lock_guard g(mtx);
         if (active == sized_active_policy && pending == sized_pending_policy)
            return;
      }
      const size_t finalizers = count_distinct_finalizers(active, pending);
      std::lock_guard g(mtx);
      sized_active_policy             = active;
      sized_pending_policy            = pending;
      limits.max_votes_per_connection = vote_limits::max_votes_per_connection(finalizers);
      limits.max_parked_votes         = vote_limits::max_parked_votes(finalizers);
   }

   vote_processor_t(emit_vote_signal_func_t&& emit_vote_signal, fetch_block_func_t&& get_block,
                    const limits_t& initial_limits, bool size_limits_from_chain)
      : size_from_chain(size_limits_from_chain)
      , emit_vote_signal_func(std::move(emit_vote_signal))
      , fetch_block_func(std::move(get_block))
      , limits(initial_limits)
   {
      assert(emit_vote_signal_func);
      assert(fetch_block_func);
   }

public:
   /// Construct with limits sized by vote_limits from the vote threads and the policies of the reported blocks.
   /// @param emit_vote_signal reports the result of each vote received from a connection
   /// @param get_block        looks up a block in the fork database
   explicit vote_processor_t(emit_vote_signal_func_t&& emit_vote_signal, fetch_block_func_t&& get_block)
      : vote_processor_t(std::move(emit_vote_signal), std::move(get_block), limits_t{}, true) {}

   /// Construct with fixed limits.
   explicit vote_processor_t(emit_vote_signal_func_t&& emit_vote_signal, fetch_block_func_t&& get_block,
                             const limits_t& fixed_limits)
      : vote_processor_t(std::move(emit_vote_signal), std::move(get_block), fixed_limits, false) {}

   /// The current limits.
   limits_t current_limits() {
      std::lock_guard g(mtx);
      return limits;
   }

   ~vote_processor_t() {
      stopped = true;
   }

   /// Number of parked votes.
   size_t parked_size() {
      std::lock_guard g(mtx);
      return index.size();
   }

   /// Number of votes posted to the thread pool and not yet run.
   uint32_t queued_size() {
      std::lock_guard g(mtx);
      return queued_votes;
   }

   /// Number of connection ids with an open session or queued votes.
   size_t connection_count() {
      std::lock_guard g(mtx);
      return connections.size();
   }

   /// Start the thread pool; votes are ignored until started with num_threads > 0.
   void start(size_t num_threads, decltype(thread_pool)::on_except_t&& on_except) {
      if (num_threads == 0)
         return;

      if (size_from_chain) {
         std::lock_guard g(mtx);
         limits.max_queued_votes = vote_limits::max_queued_votes(num_threads);
      }
      stopped = false;
      thread_pool.start( num_threads, std::move(on_except));
   }

   /// Called from the main thread when block_num becomes irreversible. Votes at or below it are ignored.
   void notify_lib(block_num_type block_num) {
      lib = block_num;
      raise_newest_block_num(block_num);
   }

   /// Called from net threads and the controller's thread pool after bsp is added to the fork database.
   /// Aggregates the votes parked for bsp and discards parked votes that will not be matched.
   void notify_new_block(const block_state_ptr& bsp, async_t async) {
      if (stopped)
         return;
      assert(bsp);
      raise_newest_block_num(bsp->block_num());
      if (size_from_chain)
         size_for_policies(*bsp);
      auto drain = [this, bsp] { drain_parked(bsp); };
      if (async == async_t::no)
         drain();
      else
         boost::asio::post(thread_pool.get_executor(), std::move(drain));
   }

   /// Called from net threads when a connection closes; ends its session. Its parked votes are discarded. Its queued
   /// votes are still aggregated when their block is known, unless the session was rejected, and are never parked.
   void notify_connection_closed(uint32_t connection_id) {
      std::lock_guard g(mtx);
      auto it = connections.find(connection_id);
      if (it == connections.end())
         return;
      remove_parked(it);
      if (it->second.queued == 0) {
         connections.erase(it);
      } else {
         it->second.session = ++last_session;
         it->second.closed = true;
      }
   }

   /// Called from net threads and the controller's thread pool. Ignored until start().
   /// A vote from a connection at its limit is dropped and reported max_exceeded, and the connection's parked votes are
   /// discarded; a vote from a peer when the queue is at its global limit, or for a block beyond the parking lead, is
   /// dropped. Votes from this node's finalizers are never dropped.
   void process_vote_message(uint32_t connection_id, const vote_message_ptr& msg, async_t async) {
      if (stopped)
         return;
      assert(msg);
      const block_num_type block_num = block_header::num_from_id(msg->block_id);
      if (block_num <= lib.load(std::memory_order_relaxed))
         return;
      if (connection_id != local_connection_id && beyond_park_lead(block_num))
         return; // not reported: honest peers relay these to a node that is behind

      std::unique_lock g(mtx);
      auto it = connections.find(connection_id);
      if (connection_id != local_connection_id) {
         if (it != connections.end()) {
            connection_votes& c = it->second;
            if (c.rejected_session == c.session)
               return; // the session sent a vote that failed verification
            if (const uint32_t held = c.queued + c.parked; held >= limits.max_votes_per_connection) {
               remove_parked(it);
               g.unlock();
               // drop, too many from this connection to process, consider connection invalid
               ilog("Exceeded max votes per connection {} >= {} for {}",
                    held, limits.max_votes_per_connection, connection_id);
               emit(connection_id, vote_result_t::max_exceeded, msg, {}, {});
               return;
            }
         }
         if (queued_votes >= limits.max_queued_votes) {
            g.unlock();
            ilog("Exceeded max queued votes {}, dropping vote from {}", limits.max_queued_votes, connection_id);
            return;
         }
      }
      if (it == connections.end())
         it = connections.emplace(connection_id, connection_votes{.session = ++last_session}).first;
      connection_votes& c = it->second;
      c.closed = false;
      ++c.queued;
      ++queued_votes;
      const uint64_t session = c.session;
      g.unlock();

      auto process_vote = [this, connection_id, session, msg] { process_queued_vote(connection_id, session, msg); };
      if (async == async_t::no)
         process_vote();
      else
         boost::asio::post(thread_pool.get_executor(), std::move(process_vote));
   }

};

} // namespace sysio::chain
