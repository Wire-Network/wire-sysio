#pragma once

#include <sysio/net_plugin/protocol.hpp>

#include <boost/container/small_vector.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/key.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <cstddef>

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
   } // namespace local_txn_cache_detail

   /** Tracks transactions known to the node and the peers associated with those IDs. */
   class local_txn_cache {
   public:
      /// Approximate per-transaction cache state size used for connection-level accounting.
      static constexpr std::size_t tracked_entry_size = sizeof(local_txn_cache_detail::node_transaction_state);

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

      /// Records a packed transaction and associates the peer that sent it.
      record_result add_transaction(const transaction_id_type& id, const time_point_sec& expires, connection_id_t connection_id) {
         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         if(auto tptr = id_idx.find(id); tptr != id_idx.end()) {
            const bool already_have_trx = tptr->have_trx;
            const entry_delta delta =
               tptr->connection_ids.insert(connection_id).second ? entry_delta::connection : entry_delta::none;
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

      /// Records a transaction notice only when the transaction ID is already known locally.
      record_result add_transaction_notice(const transaction_id_type& id, connection_id_t connection_id) {
         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         if(auto tptr = id_idx.find(id); tptr != id_idx.end()) {
            const entry_delta delta =
               tptr->connection_ids.insert(connection_id).second ? entry_delta::connection : entry_delta::none;
            return {.recorded = true, .already_have_trx = tptr->have_trx, .delta = delta};
         }

         return {};
      }

      /// Returns true when the packed transaction is known and records the peer association.
      record_result have_transaction(const transaction_id_type& id, connection_id_t connection_id) {
         auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         auto  tptr = id_idx.find(id);
         if(tptr != id_idx.end() && tptr->have_trx) {
            const entry_delta delta =
               tptr->connection_ids.insert(connection_id).second ? entry_delta::connection : entry_delta::none;
            return {.recorded = true, .already_have_trx = true, .delta = delta};
         }
         return {};
      }

      /// Returns the peers associated with a locally tracked transaction ID.
      connection_id_vector peer_connections(const transaction_id_type& id) const {
         const auto& id_idx = transactions.get<local_txn_cache_detail::by_trx_id>();
         if(auto tptr = id_idx.find(id); tptr != id_idx.end()) {
            return {tptr->connection_ids.begin(), tptr->connection_ids.end()};
         }
         return {};
      }

      /// Removes entries expiring at or before the provided cutoff and returns the number removed.
      std::size_t expire(const time_point_sec& cutoff) {
         const std::size_t start_size = transactions.size();
         auto&             old = transactions.get<local_txn_cache_detail::by_expiry>();
         auto              ex_lo = old.lower_bound(fc::time_point_sec(0));
         auto              ex_up = old.upper_bound(cutoff);
         old.erase(ex_lo, ex_up);
         return start_size - transactions.size();
      }

      /// Returns the number of tracked transaction IDs.
      std::size_t size() const {
         return transactions.size();
      }

   private:
      local_txn_cache_detail::node_transaction_index transactions;
   };

} // namespace sysio
