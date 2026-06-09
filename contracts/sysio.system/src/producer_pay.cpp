#include <sysio.system/sysio.system.hpp>
#include <sysio.token/sysio.token.hpp>

namespace sysiosystem {

   using sysio::current_time_point;
   using sysio::microseconds;
   using sysio::token;

   void system_contract::onblock( ignore<block_header> ) {
      using namespace sysio;

      require_auth(get_self());

      // Deserialize needed fields from block header.
      block_timestamp timestamp;
      name            producer;
      checksum256     previous_block_id;

      _ds >> timestamp >> producer >> previous_block_id;

      // Add latest block information to blockinfo table.
      add_to_blockinfo_table(previous_block_id, timestamp);

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto key = producer_key_t{producer.value};
      if ( _producers.contains(key) ) {
         // Round-boundary detection uses _gstate.total_unpaid_blocks as a
         // per-producer "sequence stamp" -- NOT a monotonic block height.
         // The counter is decremented by processepoch when it resets producer
         // unpaid_blocks, so its absolute value is not stable across epochs.
         // The gap check (stamp != last_stamp + 1) only remains correct because
         // processepoch ALSO resets each producer's last_block_num to the
         // no_prev_block sentinel, forcing the check to skip on the first
         // onblock after a reset. Invariant: if a producer's last_block_num is
         // non-sentinel, some counter (unpaid_blocks / eligible_rounds /
         // current_round_blocks) is non-zero, so processepoch will reset it.
         uint32_t prod_counter_stamp = _gstate.total_unpaid_blocks; // capture BEFORE increment
         _gstate.total_unpaid_blocks++;
         _producers.modify( same_payer, key, [&](auto& p) {
            p.unpaid_blocks++;

            // Round boundary detection: gap in sequence = new round started
            if (p.last_block_num != no_prev_block && prod_counter_stamp != p.last_block_num + 1) {
               // Previous round ended - check threshold
               if (p.current_round_blocks >= min_blocks_per_round_for_pay) {
                  p.eligible_rounds++;
               }
               p.current_round_blocks = 0;
            }

            p.current_round_blocks++;
            p.last_block_num = prod_counter_stamp;

            // Full round always eligible
            if (p.current_round_blocks >= blocks_per_round) {
               p.eligible_rounds++;
               p.current_round_blocks = 0;
            }
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         update_ranked_producers( timestamp );
      }
   }

} //namespace sysiosystem
