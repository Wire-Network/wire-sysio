#pragma once

#include <sysio/chain/trace.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/block.hpp>
#include <boost/container/flat_map.hpp>
#include <utility>

namespace sysio { namespace trace_api {

   struct authorization_trace_v0 {
      chain::name actor;
      chain::name permission;
   };

   struct account_delta_v0 {
      chain::name account;
      int64_t     delta = 0;
   };

   struct action_trace_v0 {
      fc::unsigned_int                               action_ordinal = {};
      fc::unsigned_int                               creator_action_ordinal = {};
      fc::unsigned_int                               closest_unnotified_ancestor_action_ordinal = {};
      uint64_t                                       global_sequence = {};
      uint64_t                                       recv_sequence = {};
      boost::container::flat_map<chain::name, uint64_t> auth_sequence = {};
      fc::unsigned_int                               code_sequence = {};
      fc::unsigned_int                               abi_sequence = {};
      chain::name                                    receiver = {};
      chain::name                                    account = {};
      chain::name                                    action = {};
      std::vector<authorization_trace_v0>            authorization = {};
      chain::bytes                                   data = {};
      chain::bytes                                   return_value = {};
      std::vector<account_delta_v0>                  account_ram_deltas = {};
      std::optional<fc::unsigned_int>                cpu_usage_us = {};
      std::optional<fc::unsigned_int>                net_usage = {};
   };

  struct transaction_trace_v0 {
     chain::transaction_id_type                 id = {};
     std::vector<action_trace_v0>               actions = {};
     uint32_t                                   cpu_usage_us = 0;
     fc::unsigned_int                           net_usage_words;
     std::vector<chain::signature_type>         signatures = {};
     chain::transaction_header                  trx_header = {};
     uint32_t                                   block_num = {};
     chain::block_timestamp_type                block_time = chain::block_timestamp_type(0);
     std::optional<chain::block_id_type>        producer_block_id = {};
  };

  struct block_trace_v0 {
     chain::block_id_type               id = {};
     uint32_t                           number = {};
     chain::block_id_type               previous_id = {};
     chain::block_timestamp_type        timestamp = chain::block_timestamp_type(0);
     chain::name                        producer = {};
     chain::checksum256_type            transaction_mroot = {};
     chain::checksum256_type            finality_mroot = {};
     std::vector<transaction_trace_v0>  transactions = {};
  };

  struct cache_trace {
      chain::transaction_trace_ptr        trace;
      chain::packed_transaction_ptr       trx;
  };

  struct block_trxs_entry {
      std::vector<chain::transaction_id_type> ids;
      uint32_t                                block_num = 0;
  };

} }

FC_REFLECT(sysio::trace_api::authorization_trace_v0, (actor)(permission))
FC_REFLECT(sysio::trace_api::account_delta_v0, (account)(delta))
FC_REFLECT(sysio::trace_api::action_trace_v0,
   (action_ordinal)(creator_action_ordinal)(closest_unnotified_ancestor_action_ordinal)
   (global_sequence)(recv_sequence)(auth_sequence)(code_sequence)(abi_sequence)
   (receiver)(account)(action)(authorization)(data)(return_value)
   (account_ram_deltas)(cpu_usage_us)(net_usage))
FC_REFLECT(sysio::trace_api::transaction_trace_v0, (id)(actions)(cpu_usage_us)(net_usage_words)(signatures)(trx_header)(block_num)(block_time)(producer_block_id))
FC_REFLECT(sysio::trace_api::block_trace_v0, (id)(number)(previous_id)(timestamp)(producer)(transaction_mroot)(finality_mroot)(transactions))
FC_REFLECT(sysio::trace_api::cache_trace, (trace)(trx))
FC_REFLECT(sysio::trace_api::block_trxs_entry, (ids)(block_num))
