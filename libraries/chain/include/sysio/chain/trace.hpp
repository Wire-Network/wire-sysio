#pragma once

#include <sysio/chain/action.hpp>
#include <sysio/chain/action_receipt.hpp>
#include <sysio/chain/block.hpp>

namespace sysio { namespace chain {

   struct account_delta {
      account_delta( const account_name& n, int64_t d):account(n),delta(d){}
      account_delta(){}

      account_name account;
      int64_t delta = 0;

      friend bool operator<( const account_delta& lhs, const account_delta& rhs ) { return lhs.account < rhs.account; }
   };

   struct transaction_trace;
   using transaction_trace_ptr = std::shared_ptr<transaction_trace>;

   struct action_trace {
      action_trace(  const transaction_trace& trace, const action& act, account_name receiver, bool context_free,
                     uint32_t action_ordinal, uint32_t creator_action_ordinal,
                     uint32_t closest_unnotified_ancestor_action_ordinal );
      action_trace(  const transaction_trace& trace, action&& act, account_name receiver, bool context_free,
                     uint32_t action_ordinal, uint32_t creator_action_ordinal,
                     uint32_t closest_unnotified_ancestor_action_ordinal );
      action_trace(){}

      fc::unsigned_int                action_ordinal;
      fc::unsigned_int                creator_action_ordinal;
      fc::unsigned_int                closest_unnotified_ancestor_action_ordinal;
      std::optional<action_receipt>   receipt;
      action_name                     receiver;
      action                          act;
      bool                            context_free = false;
      fc::microseconds                elapsed;
      fc::unsigned_int                cpu_usage_us = 0; // only populated for input action
      fc::unsigned_int                net_usage = 0;    // only populated for input action
      string                          console;
      transaction_id_type             trx_id; ///< the transaction that generated this action
      uint32_t                        block_num = 0;
      block_timestamp_type            block_time;
      std::optional<block_id_type>    producer_block_id;
      flat_set<account_delta>         account_ram_deltas;
      std::optional<fc::exception>    except;
      std::optional<uint64_t>         error_code;
      std::vector<char>               return_value;
   };

   struct transaction_trace {
      transaction_id_type                        id;
      uint32_t                                   block_num = 0;
      block_timestamp_type                       block_time;
      std::optional<block_id_type>               producer_block_id;
      std::optional<transaction_receipt_header>  receipt;
      uint32_t                                   total_cpu_usage_us = 0;
      fc::microseconds                           elapsed;
      uint64_t                                   net_usage = 0;
      vector<action_trace>                       action_traces;
      std::optional<account_delta>               account_ram_delta;

      std::optional<fc::exception>               except;
      std::optional<uint64_t>                    error_code;
      std::exception_ptr                         except_ptr;
   };

   /**
    * Deduce if transaction_trace is the trace of an onblock system transaction
    */
   inline bool is_onblock( const transaction_trace& tt ) {
      if (tt.action_traces.empty())
         return false;
      const auto& act = tt.action_traces[0].act;
      if (act.account != sysio::chain::config::system_account_name || act.name != "onblock"_n ||
          act.authorization.size() != 1)
         return false;
      const auto& auth = act.authorization[0];
      return auth.actor == sysio::chain::config::system_account_name &&
             auth.permission == sysio::chain::config::active_name;
   }

} }  /// namespace sysio::chain

FC_REFLECT( sysio::chain::account_delta,
            (account)(delta) )

FC_REFLECT( sysio::chain::action_trace,
               (action_ordinal)(creator_action_ordinal)(closest_unnotified_ancestor_action_ordinal)(receipt)
               (receiver)(act)(context_free)(elapsed)(console)(trx_id)(block_num)(block_time)
               (producer_block_id)(account_ram_deltas)(except)(error_code)(return_value) )

// @ignore except_ptr
FC_REFLECT( sysio::chain::transaction_trace, (id)(block_num)(block_time)(producer_block_id)
                                             (receipt)(elapsed)(net_usage)
                                             (action_traces)(account_ram_delta)(except)(error_code) )
