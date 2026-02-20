#include <sysio.system/sysio.system.hpp>
#include <sysio.token/sysio.token.hpp>

namespace sysiosystem {

   constexpr uint32_t blocks_per_round = 12; // set by consensus sysio::chain::config::producer_repetitions
   constexpr uint32_t minimum_blocks_per_round_for_pay = 6;

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

      /** until activation, no new rewards are paid */
      if( _gstate.thresh_activated_stake_time == time_point() )
         return;

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end() ) {
         uint32_t block_seq = _gstate.total_unpaid_blocks; // capture BEFORE increment
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, same_payer, [&](auto& p) {
            p.unpaid_blocks++;

            // Round boundary detection: gap in sequence = new round started
            static constexpr uint32_t NO_PREV = std::numeric_limits<uint32_t>::max();
            if (p.last_block_num != NO_PREV && block_seq != p.last_block_num + 1) {
               // Previous round ended — check threshold
               if (p.current_round_blocks >= minimum_blocks_per_round_for_pay) {
                  p.eligible_rounds++;
               }
               p.current_round_blocks = 0;
            }

            p.current_round_blocks++;
            p.last_block_num = block_seq;

            // Full 12-block round always eligible
            if (p.current_round_blocks >= blocks_per_round) {
               p.eligible_rounds++;
               p.current_round_blocks = 0;
            }
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         update_elected_producers( timestamp );
      }
   }

} //namespace sysiosystem
