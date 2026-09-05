#include <sysio.system/sysio.system.hpp>
#include <sysio.system/producer_score.hpp>
#include <sysio.token/sysio.token.hpp>

#include <algorithm>
#include <vector>

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

      if( _global.get().last_pervote_bucket_fill == time_point() )  /// start the presses
         _global.modify( get_self(), []( auto& g ) { g.last_pervote_bucket_fill = current_time_point(); });


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      // Pay is per block: count it. payepoch credits every counted block at the period's rate and
      // zeroes the count; a block this producer's slot did not deliver is simply never counted.
      auto key = producer_key_t{producer.value};
      if ( _producers.contains(key) ) {
         _producers.modify( same_payer, key, []( auto& p ) { p.unpaid_blocks++; });
      }

      // Attribute the rounds nobody produced. This must happen on every block: the count above
      // records PRESENCE only -- a producer that produces nothing is never visited by onblock at
      // all, so absence leaves no trace unless the schedule is walked explicitly.
      record_round_participation( producer );

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _global.get().last_producer_schedule_update.slot > 120 ) {
         // Drain any pending rescore BEFORE rebuilding, so the rebuild sees the freshest scores it
         // can. A sweep spans several ticks; the schedule is proposed from a partially-rescored
         // index in the meantime, which is safe because the tiers -- not the composite -- decide
         // membership, and a tier only changes on demotion or recovery.
         drain_rescore_cursor();
         update_ranked_producers( timestamp );
      }
   }

   void system_contract::record_round_participation( const name& current_producer ) {
      const auto& state = _global.get();

      // Mid-round: the same producer made the previous block, so no slot was skipped and its miss
      // counter was already cleared on the first block of this round. This is 11 of every 12
      // blocks, and returning here keeps the schedule read and the snapshot compare off the hot
      // path for all of them. The same test also fires when EVERY other producer missed and the
      // round-robin came back to this one; that case is indistinguishable from mid-round here and
      // is deliberately left uncharged -- with every other producer absent the chain has no
      // finality left to activate a replacement schedule anyway.
      if( state.last_producer == current_producer ) return;

      const auto active_schedule = sysio::get_active_producers();

      producer_rank::observed_schedule_t observed_tbl( get_self() );
      const auto observed = observed_tbl.get_or_default( producer_rank::observed_schedule{} );

      // A schedule change invalidates the cursor. The span between the previous producer and this
      // one is only a list of MISSES while the schedule is the same set in the same order: after a
      // change, a newly-added producer sitting in that span never had a slot to miss, and charging
      // it a miss would count toward a demotion it did not earn. There is no schedule-version
      // intrinsic, so the comparison is against the stored snapshot.
      const bool schedule_unchanged = observed.producers == active_schedule;

      if( !schedule_unchanged ) {
         observed_tbl.set( producer_rank::observed_schedule{ .producers = active_schedule }, get_self() );
      }

      if( schedule_unchanged && state.last_producer.value != 0 ) {
         const auto previous = std::find( active_schedule.begin(), active_schedule.end(),
                                          state.last_producer );
         const auto current  = std::find( active_schedule.begin(), active_schedule.end(),
                                          current_producer );
         if( previous != active_schedule.end() && current != active_schedule.end() ) {
            producer_rank::producer_score_config_t weights_tbl( get_self() );
            const auto weights = weights_tbl.get_or_default( producer_rank::producer_score_config{} );

            // Walk forward from the slot AFTER the previous producer to the current one, wrapping
            // at the end of the round-robin. Every name in between held a slot and produced
            // nothing. Bounded by the schedule size (max_producers); normally zero iterations,
            // since the next producer follows the previous one directly.
            auto slot = previous + 1;
            for( size_t stepped = 0; stepped < active_schedule.size(); ++stepped ) {
               if( slot == active_schedule.end() ) slot = active_schedule.begin();
               if( slot == current ) break;
               record_round_outcome( *slot, /*missed*/ true, weights );
               ++slot;
            }
         }
      }

      // The producer of this block is, by construction, not missing its round -- and a block is
      // the strongest liveness proof there is, so it clears a DEMOTION as well as the streak.
      //
      // That matters because demotion and rescheduling are not simultaneous. A producer is demoted
      // the moment its third miss lands, but it leaves the schedule only at the next rebuild, and
      // when demotions drop the schedulable count below `min_schedule_size` the rebuild retains the
      // last good schedule instead of publishing a short one -- so the demoted producers come back
      // and keep producing under it. Without this, they would produce indefinitely while `payepoch`
      // skipped them (the walk stops at the demoted tier), earning nothing until every operator
      // pushed `regproducer` by hand. A mass outage is exactly the case that produces it.
      //
      // A demoted producer that is NOT in the active schedule never reaches this path, so recovery
      // still requires `regproducer` for anyone the schedule has actually dropped.
      {
         producer_rank::producer_score_config_t weights_tbl( get_self() );
         const auto weights = weights_tbl.get_or_default( producer_rank::producer_score_config{} );
         record_round_outcome( current_producer, /*missed*/ false, weights );
      }

      _global.modify( get_self(), [&]( auto& g ) { g.last_producer = current_producer; });
   }

   void system_contract::record_round_outcome( const name& producer, bool missed,
                                               const producer_rank::producer_score_config& weights ) {
      auto key = producer_key_t{producer.value};
      if( !_producers.contains(key) ) return;

      // On-chain time in ms, matching `missed_round_window_ms`. Microsecond precision from the
      // intrinsic, divided down -- the window is measured in hours, so the units are what matter.
      const uint64_t now_ms = static_cast<uint64_t>(
         sysio::current_time_point().time_since_epoch().count() / 1000 );

      _producers.modify( same_payer, key, [&]( auto& p ) {
         // Roll the window before recording into it. A producer that was off the schedule observed
         // no rounds, so nothing accrued while it was away -- and if it stayed away longer than the
         // window, its old counts lapse rather than greeting it on return. That is the WNS-47
         // shape (stale per-period state resurrecting on re-entry) designed out at the source.
         if( p.miss_window_open_ms == 0
             || now_ms - p.miss_window_open_ms >= weights.missed_round_window_ms ) {
            p.miss_window_open_ms     = now_ms;
            p.rounds_in_window        = 0;
            p.missed_rounds_in_window = 0;
         }
         p.rounds_in_window++;

         if( !missed ) {
            // A block is the strongest liveness proof there is: it clears the streak and, when the
            // producer is still holding a schedule slot, the demotion with it. Demotion and
            // rescheduling are not simultaneous, and the `min_schedule_size` floor can hold that
            // gap open indefinitely -- without this those producers would produce for nothing,
            // since `payepoch` stops at the demoted tier. The window counts are NOT cleared: one
            // good round does not erase a day's record.
            p.consecutive_missed_rounds = 0;
            // A block clears the STREAK outright, and with it any demotion the consecutive gate
            // caused -- that gate asks "are you offline right now", and this is the answer. It
            // does NOT pardon the rate gate: one good round cannot erase a bad record, or a
            // producer missing half its rounds would clear its demotion every time it managed
            // one. So the flag is RE-DERIVED from the record rather than forced false. A producer
            // still holding a slot climbs out by producing until its rate falls back under the
            // limit; one the schedule has dropped uses `regproducer`, which opens a fresh window.
            p.is_demoted = producer_rank::warrants_demotion( 0, p.rounds_in_window,
                                                             p.missed_rounds_in_window, weights );
            return;
         }

         p.missed_rounds_in_window++;
         p.consecutive_missed_rounds++;
         // Demotion is CATEGORICAL: it moves the producer into a tier no score can climb out of.
         // There is no cooldown and no expiry, and the only two ways back are `regproducer` and
         // producing a block while still in the active schedule -- the latter covering the window
         // between a demotion and the rebuild that acts on it, which the schedule-size floor can
         // hold open indefinitely.
         if( !p.is_demoted
             && producer_rank::warrants_demotion( p.consecutive_missed_rounds, p.rounds_in_window,
                                                  p.missed_rounds_in_window, weights ) ) {
            p.is_demoted = true;
            // Leaving the pay walk consumes the period's snapshot credit. `payepoch` zeroes that
            // counter only for rows it VISITS, and it stops at the demoted tier -- so a credit
            // carried out of the walk would ride back in on return and outrank producers who
            // actually attested that period. It is a per-period SERVICE RATING, unlike the block
            // count, which is an earned debt and is deliberately kept.
            p.snapshot_attestations = 0;
         }
      });

      // The round moved the participation factor, and a demotion or its clearing moved the tier;
      // either way the stored sort key is stale. A demoted producer keeps its block count: it is
      // paid for those blocks at the first payepoch where it is payable again.
      rescore_producer( producer );
   }

   void system_contract::drain_rescore_cursor() {
      const auto& state = _global.get();
      if( !state.rescore_pending ) return;

      // Bounded per tick, mirroring opreg's MAX_WTDW_FLUSH_PER_EPOCH: the producers table is
      // unbounded, so a weight change can never rewrite it inline.
      constexpr uint32_t max_rescore_per_tick = 32;

      uint64_t cursor = state.rescore_cursor;
      bool     done   = true;

      // COLLECT first, rescore after. rescore_producer writes the row, which moves its entry in the
      // secondary index; mutating the table while an iterator into it is live is not safe to rely
      // on. The batch is bounded by max_rescore_per_tick, so the vector is small and fixed.
      std::vector<name> batch;
      batch.reserve( max_rescore_per_tick );
      for( auto it = _producers.lower_bound( producer_key_t{cursor} ); it != _producers.end(); ++it ) {
         if( batch.size() >= max_rescore_per_tick ) {
            cursor = it->owner.value;   // resume here next tick
            done   = false;
            break;
         }
         batch.push_back( it->owner );
      }
      for( const auto& producer : batch ) {
         reconcile_and_rescore( producer );
      }

      _global.modify( get_self(), [&]( auto& g ) {
         g.rescore_cursor  = done ? 0 : cursor;
         g.rescore_pending = !done;
      });
   }

} //namespace sysiosystem
