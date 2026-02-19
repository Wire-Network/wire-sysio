#pragma once

#include <sysio/trace_api/trace.hpp>

namespace sysio { namespace trace_api {

inline action_trace_v0 to_action_trace( const chain::action_trace& at ) {
   action_trace_v0 r;
   r.receiver = at.receiver;
   r.account = at.act.account;
   r.action = at.act.name;
   r.data = at.act.data;
   r.return_value = at.return_value;
   if( at.receipt ) {
      r.global_sequence = at.receipt->global_sequence;
   }
   r.authorization.reserve( at.act.authorization.size());
   for( const auto& auth : at.act.authorization ) {
      r.authorization.emplace_back( authorization_trace_v0{auth.actor, auth.permission} );
   }
   return r;
}

inline transaction_trace_v0 to_transaction_trace( const cache_trace& t ) {
   transaction_trace_v0 r;
   r.id = t.trace->id;
   if (t.trace->receipt) {
      r.status = chain::transaction_receipt_header::status_enum::executed;
      r.cpu_usage_us = t.trace->total_cpu_usage_us;
      // Round up net_usage and convert to words
      r.net_usage_words = (t.trace->net_usage + 7)/8;
   }
   r.signatures = t.trx->get_signatures();
   r.trx_header = static_cast<const chain::transaction_header&>( t.trx->get_transaction() );
   r.block_num = t.trace->block_num;
   r.block_time = t.trace->block_time;
   r.producer_block_id = t.trace->producer_block_id;

   r.actions.reserve( t.trace->action_traces.size());
   for( const auto& at : t.trace->action_traces ) {
      if( !at.context_free ) { // not including CFA at this time
         r.actions.emplace_back( to_action_trace(at) );
      }
   }
   return r;
}

inline block_trace_v0 create_block_trace( const chain::signed_block_ptr& block, const chain::block_id_type& id ) {
   block_trace_v0 r;
   r.id = id;
   r.number = block->block_num();
   r.previous_id = block->previous;
   r.timestamp = block->timestamp;
   r.producer = block->producer;
   r.transaction_mroot = block->transaction_mroot;
   r.finality_mroot = block->finality_mroot;
   return r;
}

} }
