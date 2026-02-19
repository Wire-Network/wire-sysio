#pragma once

#include <sysio/chain/trace.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/block.hpp>
#include <utility>

namespace sysio { namespace trace_api {

   struct authorization_trace_v0 {
      chain::name account;
      chain::name permission;
   };

   struct action_trace_v0 {
      uint64_t                               global_sequence = {};
      chain::name                            receiver = {};
      chain::name                            account = {};
      chain::name                            action = {};
      std::vector<authorization_trace_v0>    authorization = {};
      chain::bytes                           data = {};
      chain::bytes                           return_value = {};
   };

  struct transaction_trace_v0 {
     using status_type = chain::transaction_receipt_header::status_enum;
     chain::transaction_id_type                 id = {};
     std::vector<action_trace_v0>               actions = {};
     fc::enum_type<uint8_t,status_type>         status = {};
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

FC_REFLECT(sysio::trace_api::authorization_trace_v0, (account)(permission))
FC_REFLECT(sysio::trace_api::action_trace_v0, (global_sequence)(receiver)(account)(action)(authorization)(data)(return_value))
FC_REFLECT(sysio::trace_api::transaction_trace_v0, (id)(actions)(status)(cpu_usage_us)(net_usage_words)(signatures)(trx_header)(block_num)(block_time)(producer_block_id))
FC_REFLECT(sysio::trace_api::block_trace_v0, (id)(number)(previous_id)(timestamp)(producer)(transaction_mroot)(finality_mroot)(transactions))
FC_REFLECT(sysio::trace_api::cache_trace, (trace)(trx))
FC_REFLECT(sysio::trace_api::block_trxs_entry, (ids)(block_num))
