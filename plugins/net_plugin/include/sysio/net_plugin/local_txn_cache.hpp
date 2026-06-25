#pragma once

#include <sysio/net_plugin/protocol.hpp>

#include <boost/container/small_vector.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/key.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <list>
#include <vector>

namespace sysio {
   using connection_id_vector = boost::container::small_vector<connection_id_t, 64>;

   namespace local_txn_cache_detail {
      using connection_id_set = boost::unordered_flat_set<connection_id_t>;

      struct by_trx_id;
      struct by_expiry;

      /// Stores one locally tracked transaction ID and its associated peer observations.
      struct node_transaction_state {
         transaction_id_type        id;
         time_point_sec             expires;           ///< time after which this may be purged.
         mutable connection_id_set  connection_ids;    ///< connections where this transaction or notice was observed.
         mutable bool               have_trx = false;  ///< true once the packed transaction was received.
      };

      using node_transaction_index = boost::multi_index_container<
         node_transaction_state,
         boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<
               boost::multi_index::tag<by_trx_id>,
               boost::multi_index::member<node_transaction_state, transaction_id_type, &node_transaction_state::id>>,
            boost::multi_index::ordered_non_unique<
               boost::multi_index::tag<by_expiry>,
               boost::multi_index::member<node_transaction_state, fc::time_point_sec, &node_transaction_state::expires>>>>;

      using notice_lru_list = std::list<transaction_id_type>;

      /// Tracks notice-only transaction IDs for one peer in least-recently-used order.
      struct notice_lru_state {
         notice_lru_list lru;
         boost::unordered_flat_map<transaction_id_type, notice_lru_list::iterator> positions;
      };

      using notice_lru_by_connection = boost::unordered_flat_map<connection_id_t, notice_lru_state>;
   } // namespace local_txn_cache_detail

   /** Tracks transactions known to the node and the peers associated with those IDs. */
   class local_txn_cache {
   public:
      /// Approximate per-transaction cache state size used for connection-level accounting.
      static constexpr std::size_t tracked_entry_size = sizeof(local_txn_cache_detail::node_transaction_state);

      /// Maximum number of notice-only transaction IDs retained per peer.
      static constexpr std::size_t default_notice_only_entry_cap_per_connection = 512;

      /// Short lifetime for notice-only IDs that have not been upgraded by a full transaction.
      static constexpr fc::microseconds notice_only_lifetime = fc::seconds(3);

      /// Describes the per-connection accounting delta produced by a cache mutation.
      enum class entry_delta {
         none,
         connection,
         full
      };

      /// Result returned by cache mutation and lookup operations.
      struct record_result {
         bool        recorded = false;
         bool        already_have_trx = false;
         entry_delta delta = entry_delta::none;
      };

      /// Constructs a cache with a per-connection notice-only cap.
      explicit local_txn_cache(std::size_t notice_only_entry_cap_per_connection = default_notice_only_entry_cap_per_connection)
      : notice_only_entry_cap_per_connection(notice_only_entry_cap_per_connection) {}

      /// Records a packed transaction and associates the peer that sent it.
      record_result add_transaction(const transaction_id_type& id, const time_point_sec& expires, connection_id_t connection_id) {
         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         if(auto tptr = id_idx.find(id); tptr != id_idx.end()) {
            const bool already_have_trx = tptr->have_trx;
            if(!already_have_trx) {
               remove_notice_tracking(*tptr);
            }

            const bool inserted_connection = tptr->connection_ids.insert(connection_id).second;
            const entry_delta delta =
               already_have_trx ? (inserted_connection ? entry_delta::connection : entry_delta::none) : entry_delta::full;
            if(!already_have_trx) {
               id_idx.modify(tptr, [&](auto& v) {
                  v.expires = expires;
                  v.have_trx = true;
               });
            }
            return {.recorded = true, .already_have_trx = already_have_trx, .delta = delta};
         }

         transactions.insert(local_txn_cache_detail::node_transaction_state{
            .id = id,
            .expires = expires,
            .connection_ids = {connection_id},
            .have_trx = true});
         return {.recorded = true, .already_have_trx = false, .delta = entry_delta::full};
      }

      /// Records a transaction notice, bounding notice-only IDs until the full transaction arrives.
      record_result add_transaction_notice(const transaction_id_type& id, const time_point_sec& notice_expires, connection_id_t connection_id) {
         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         if(auto tptr = id_idx.find(id); tptr != id_idx.end()) {
            const bool inserted_connection = tptr->connection_ids.insert(connection_id).second;
            if(tptr->have_trx) {
               const entry_delta delta = inserted_connection ? entry_delta::connection : entry_delta::none;
               return {.recorded = true, .already_have_trx = true, .delta = delta};
            }

            id_idx.modify(tptr, [&](auto& v) {
               v.expires = notice_expires;
            });
            record_notice_observation(id, connection_id);
            return {.recorded = true, .already_have_trx = false, .delta = entry_delta::none};
         }

         transactions.insert(local_txn_cache_detail::node_transaction_state{
            .id = id,
            .expires = notice_expires,
            .connection_ids = {connection_id},
            .have_trx = false});
         record_notice_observation(id, connection_id);
         return {.recorded = true, .already_have_trx = false, .delta = entry_delta::none};
      }

      /// Returns true when the packed transaction is known and records the peer association.
      record_result have_transaction(const transaction_id_type& id, connection_id_t connection_id) {
         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         auto  tptr = id_idx.find(id);
         if(tptr != id_idx.end() && tptr->have_trx) {
            const bool inserted_connection = tptr->connection_ids.insert(connection_id).second;
            const entry_delta delta = inserted_connection ? entry_delta::connection : entry_delta::none;
            return {.recorded = true, .already_have_trx = true, .delta = delta};
         }
         return {};
      }

      /// Returns the peers associated with a locally tracked transaction ID.
      connection_id_vector peer_connections(const transaction_id_type& id) const {
         const auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         if(auto tptr = id_idx.find(id); tptr != id_idx.end()) {
            connection_id_vector connections{tptr->connection_ids.begin(), tptr->connection_ids.end()};
            std::sort(connections.begin(), connections.end());
            return connections;
         }
         return {};
      }

      /// Removes entries expiring at or before the appropriate cutoff and returns the number removed.
      std::size_t expire(const time_point_sec& transaction_cutoff, const time_point_sec& notice_cutoff) {
         const std::size_t start_size = transactions.size();
         auto&             old = transactions.get<local_txn_cache_detail::by_expiry>();
         const auto        max_cutoff = std::max(transaction_cutoff, notice_cutoff);
         for(auto itr = old.lower_bound(fc::time_point_sec(0)); itr != old.end() && itr->expires <= max_cutoff;) {
            const bool should_expire = itr->have_trx ? itr->expires <= transaction_cutoff : itr->expires <= notice_cutoff;
            if(!should_expire) {
               ++itr;
               continue;
            }

            if(!itr->have_trx) {
               remove_notice_tracking(*itr);
            }
            itr = old.erase(itr);
         }
         return start_size - transactions.size();
      }

      /// Returns the number of tracked transaction IDs.
      std::size_t size() const {
         return transactions.size();
      }

      /// Returns the number of notice-only IDs currently retained for one peer.
      std::size_t notice_only_size(connection_id_t connection_id) const {
         if(auto conn_itr = notice_lru.find(connection_id); conn_itr != notice_lru.end()) {
            return conn_itr->second.lru.size();
         }
         return 0;
      }

   private:
      void record_notice_observation(const transaction_id_type& id, connection_id_t connection_id) {
         auto& state = notice_lru[connection_id];
         if(auto pos_itr = state.positions.find(id); pos_itr != state.positions.end()) {
            state.lru.splice(state.lru.end(), state.lru, pos_itr->second);
            return;
         }

         state.lru.push_back(id);
         state.positions.emplace(id, std::prev(state.lru.end()));
         enforce_notice_cap(connection_id);
      }

      void remove_notice_observation(connection_id_t connection_id, const transaction_id_type& id) {
         auto conn_itr = notice_lru.find(connection_id);
         if(conn_itr == notice_lru.end()) {
            return;
         }

         auto& state = conn_itr->second;
         if(auto pos_itr = state.positions.find(id); pos_itr != state.positions.end()) {
            state.lru.erase(pos_itr->second);
            state.positions.erase(pos_itr);
         }
         if(state.lru.empty()) {
            notice_lru.erase(conn_itr);
         }
      }

      void remove_notice_tracking(const local_txn_cache_detail::node_transaction_state& transaction_state) {
         std::vector<connection_id_t> connection_ids{transaction_state.connection_ids.begin(), transaction_state.connection_ids.end()};
         for(connection_id_t connection_id : connection_ids) {
            remove_notice_observation(connection_id, transaction_state.id);
         }
      }

      void enforce_notice_cap(connection_id_t connection_id) {
         auto conn_itr = notice_lru.find(connection_id);
         if(conn_itr == notice_lru.end()) {
            return;
         }

         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         auto& state = conn_itr->second;
         while(state.lru.size() > notice_only_entry_cap_per_connection) {
            const auto evicted_id = state.lru.front();
            state.positions.erase(evicted_id);
            state.lru.pop_front();

            auto tptr = id_idx.find(evicted_id);
            if(tptr == id_idx.end() || tptr->have_trx) {
               continue;
            }

            id_idx.modify(tptr, [&](auto& v) {
               v.connection_ids.erase(connection_id);
            });
            if(tptr->connection_ids.empty()) {
               id_idx.erase(tptr);
            }
         }
         if(state.lru.empty()) {
            notice_lru.erase(conn_itr);
         }
      }

      local_txn_cache_detail::node_transaction_index transactions;
      local_txn_cache_detail::notice_lru_by_connection notice_lru;
      std::size_t notice_only_entry_cap_per_connection;
   };

} // namespace sysio
