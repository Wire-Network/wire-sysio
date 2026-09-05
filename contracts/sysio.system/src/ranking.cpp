#include <sysio/crypto.hpp>
#include <sysio/datastream.hpp>
#include <sysio/sysio.hpp>
#include <sysio/permission.hpp>
#include <sysio/privileged.hpp>
#include <sysio/serialize.hpp>

#include <sysio.system/sysio.system.hpp>
#include <sysio.system/opreg_status.hpp>
#include <sysio.system/producer_score.hpp>
#include <sysio.token/sysio.token.hpp>

#include <type_traits>
#include <limits>
#include <set>
#include <algorithm>
#include <cmath>

namespace sysiosystem {

   using sysio::const_mem_fun;
   using sysio::current_time_point;
   using sysio::microseconds;

   void system_contract::register_producer( const name& producer, const sysio::block_signing_authority& producer_authority, const std::string& url, uint16_t location ) {
      const auto ct = current_time_point();

      sysio::public_key producer_key{};

      std::visit( [&](auto&& auth ) {
         if( auth.keys.size() == 1 ) {
            // if the producer_authority consists of a single key, use that key in the legacy producer_key field
            producer_key = auth.keys[0].key;
         }
         for (const auto& kw : auth.keys) {
            check( kw.key.index() < 2, "Only K1 & R1 keys allowed" );
         }
      }, producer_authority );

      auto key = producer_key_t{producer.value};
      _producers.upsert( get_self(), key,
         producer_info{
            .owner              = producer,
            .producer_key       = producer_key,
            .is_active          = true,
            .url                = url,
            .last_claim_time    = ct,
            .location           = location,
            .producer_authority = producer_authority,
         },
         [&]( producer_info& info ){
            info.producer_key       = producer_key;
            info.is_active          = true;
            info.url                = url;
            info.location           = location;
            info.producer_authority = producer_authority;
            if ( info.last_claim_time == time_point() )
               info.last_claim_time = ct;
            // regproducer is the door back for a producer the schedule has DROPPED -- a voluntary
            // `unregprod` park, an involuntary demotion, or a participation penalty that pushed it
            // below the active set (the streak clears only by producing, and a producer that is not
            // scheduled cannot produce). `unregprod` erases the signing key, so re-registering has
            // to re-supply it, which makes this a genuine assertion of readiness rather than a
            // no-op. There is deliberately no cooldown and no expiry: a producer that comes back
            // before it is ready is demoted again within max_consecutive_missed_rounds rounds,
            // which is self-correcting. A demoted producer that is still in the active schedule
            // recovers on its own by producing -- see `record_round_participation`.
            // Clears the DEMOTION, not the record. `regproducer` costs nothing but a signature
            // and may be repeated, so clearing the streak here would let an offline operator cron
            // its way back to healthy after every second miss and never produce a block -- the
            // demotion model would stop meaning anything. The streak survives and clears only by
            // PRODUCING, so a producer that returns unready is demoted again on its next missed
            // round; until it produces, the participation factor keeps scoring it accordingly.
            //
            // The miss WINDOW does reset, and that is safe for the same reason: the consecutive
            // gate is what defeats a cron loop, and it is untouched here. Without this reset a
            // producer whose rate gate tripped could never recover -- demoted, it is not
            // scheduled, so it observes no rounds, so its rate can never improve.
            info.is_demoted              = false;
            info.rounds_in_window        = 0;
            info.missed_rounds_in_window = 0;
            info.miss_window_open_ms     = 0;
         });

      // The clear above changes the producer's tier, so its sort key is stale until rescored.
      rescore_producer( producer );
   }

   void system_contract::rescore_producer( const name& producer ) {
      producer_rank::rescore( get_self(), _producers, producer );
   }

   void system_contract::reconcile_and_rescore( const name& producer ) {
      auto key = producer_key_t{producer.value};
      if( !_producers.contains(key) ) return;

      producer_rank::producer_score_config_t weights_tbl( get_self() );
      const auto weights = weights_tbl.get_or_default( producer_rank::producer_score_config{} );
      const auto info    = _producers.get(key);

      // A LOWERED RATE threshold has to reach the windows that already breach it. The rate gate is
      // the one that cannot self-correct: a producer the schedule has dropped observes no rounds,
      // so its recorded rate never improves, and without this a tightened limit would never bind on
      // exactly the producers it was tightened to catch. This runs only on the sweep, which the
      // config change itself opens, so an ordinary rescore never re-derives the flag.
      //
      // The CONSECUTIVE gate is deliberately NOT reconciled here, and that is load-bearing rather
      // than an omission. `regproducer` clears the demotion and the window but preserves the
      // streak, so a pardoned producer sits at (is_demoted == false, streak >= threshold) until it
      // produces. Re-deriving the consecutive gate on that row would re-demote it with no new miss,
      // and -- being unscheduled -- it could never produce the block that is its only other door
      // back, so every subsequent sweep would undo `regproducer` again: a permanent lockout. The
      // gate loses nothing by waiting. It asks "are you offline right now", which only an OBSERVED
      // round can answer, and `record_round_outcome` evaluates the live threshold on every miss --
      // so a lowered limit binds on the producer's very next missed round.
      //
      // A RAISED threshold deliberately does NOT un-demote anyone. Demotion is categorical: it
      // clears by producing a block while still scheduled, or by `regproducer`. Governance
      // widening the tolerance is not a pardon for producers already judged under the old one, and
      // either door back is open to them immediately.
      if( !info.is_demoted
          && producer_rank::exceeds_miss_rate( info.rounds_in_window,
                                               info.missed_rounds_in_window, weights ) ) {
         _producers.modify( same_payer, key, []( auto& p ) { p.set_demoted( true ); });
      }
      rescore_producer( producer );
   }

   void system_contract::onprocessprod( name account, bool, bool ) {
      // The eligibility flags are not consulted: rescore_producer reads the operator's live status
      // and balances, which is the same information after the flip and cannot go stale between the
      // notification and this handler.
      rescore_producer( account );
   }

   void system_contract::setscorecfg( const producer_rank::producer_score_config& weights ) {
      require_auth( get_self() );

      // A miss percentage is a percentage.
      check( weights.max_pct_missed_rounds_in_window <= 100,
             "max_pct_missed_rounds_in_window cannot exceed 100" );

      // The rate gate's minimum sample is DERIVED from the consecutive threshold
      // (max_consecutive * 100 / max_pct), so the two settings are not independent: at
      // max_consecutive == 0 the sample floor collapses to zero and the rate gate fires on a
      // sample of ONE, where a single missed round is a 100% miss rate. That inverts the field's
      // meaning -- zero reads as "disable the consecutive gate" and would instead demote every
      // producer on its first missed slot, dropping the schedulable count below
      // `min_schedule_size` chain-wide. Disabling a gate is spelled by zeroing THAT gate's own
      // percentage, so require the pair to be coherent instead.
      check( weights.max_pct_missed_rounds_in_window == 0 || weights.max_consecutive_missed_rounds > 0,
             "max_consecutive_missed_rounds must be positive while the rate gate is armed" );
      check( weights.max_pct_missed_rounds_in_window == 0 || weights.missed_round_window_ms > 0,
             "missed_round_window_ms must be positive while the rate gate is armed" );

      // `rate_gate_minimum_sample` computes `max_consecutive * 100`, so cap the input well below
      // the point that multiply wraps a uint32.
      check( weights.max_consecutive_missed_rounds <= max_consecutive_missed_rounds_limit,
             "max_consecutive_missed_rounds is unreasonably large" );

      // Every factor is multiplied by its weight and summed. `mul_sat` keeps a single term from
      // wrapping, but a configuration whose weights cannot be told apart is still useless, so
      // bound them at the scale the factors are normalised to.
      check( weights.collateral_weight    <= producer_rank::max_factor_weight
             && weights.participation_weight <= producer_rank::max_factor_weight
             && weights.snapshot_weight      <= producer_rank::max_factor_weight
             && weights.relay_weight         <= producer_rank::max_factor_weight
             && weights.api_weight           <= producer_rank::max_factor_weight
             && weights.benchmark_weight     <= producer_rank::max_factor_weight,
             "factor weight exceeds the maximum" );

      // A zero target would make the snapshot factor divide by zero.
      check( weights.snapshot_target_attestations > 0,
             "snapshot_target_attestations must be positive" );

      producer_rank::producer_score_config_t weights_tbl( get_self() );
      weights_tbl.set( weights, get_self() );

      // Every stored rank_score was computed under the OLD weights. Rather than rewrite an
      // unbounded table inline, open a rescore sweep: onblock drains a bounded number of rows per
      // schedule-rebuild tick until the cursor is exhausted.
      open_rescore_sweep();
   }

   void system_contract::onsetconfig() {
      // The collateral minimums moved (sysio.opreg::setconfig notified us), so every stored score's
      // collateral ratio is stale. Same remedy as a weight change.
      open_rescore_sweep();
   }

   void system_contract::open_rescore_sweep() {
      _global.modify( get_self(), []( auto& g ) {
         g.rescore_cursor  = 0;
         g.rescore_pending = true;
      });
   }

   void system_contract::regproducer( const name& producer, const sysio::public_key& producer_key, const std::string& url, uint16_t location ) {
      require_auth( producer );
      check( url.size() < 512, "url too long" );

      register_producer( producer, convert_to_block_signing_authority( producer_key ), url, location );
   }

   void system_contract::regproducer2( const name& producer, const sysio::block_signing_authority& producer_authority, const std::string& url, uint16_t location ) {
      require_auth( producer );
      check( url.size() < 512, "url too long" );

      std::visit( [&](auto&& auth ) {
         check( auth.is_valid(), "invalid producer authority" );
      }, producer_authority );

      register_producer( producer, producer_authority, url, location );
   }

   void system_contract::unregprod( const name& producer ) {
      require_auth( producer );

      auto key = producer_key_t{producer.value};
      _producers.get( key, "producer not found" );
      _producers.modify( get_self(), key, [&]( producer_info& info ){
         info.deactivate();
         // A park leaves the pay walk exactly as a demotion does, so it consumes the period's
         // snapshot credit for the same reason -- see `record_round_outcome`.
         info.snapshot_attestations = 0;
      });

      // A parked row scores into the demoted tier, so the sort key is stale until rescored. This
      // is what keeps the tier a statement about LIVE standing: every rank walk stops at the first
      // demoted entry, and a parked producer left in its old tier would be visited (and skipped)
      // by every one of them until some unrelated event happened to rescore it.
      rescore_producer( producer );
   }

   void system_contract::update_ranked_producers( const block_timestamp& block_time ) {
      // A config sweep does NOT hold the schedule back. While it drains, the index carries scores
      // from two configurations at once, so a rebuild can order a producer by a score the current
      // weights would not give it -- and that is accepted: ranking is allowed to converge rather
      // than switch atomically, and it self-corrects within a few ticks as the cursor advances.
      //
      // Deferring instead is what is NOT safe. The sweep's length scales with the table, the table
      // is unbounded, and `regproducer` is permissionless with its RAM billed to this contract --
      // so waiting for the sweep would let anyone hold BOTH the producer schedule and the finalizer
      // policy frozen for as long as they kept registering, during which a slashed, terminated or
      // demoted producer would keep its slot and its finality weight. A briefly mixed ordering is a
      // far smaller harm than a schedule that cannot be rebuilt at all.
      _global.modify( get_self(), [&]( auto& g ) { g.last_producer_schedule_update = block_time; });

      auto idx = _producers.get_index<"prodrank"_n>();

      using value_type = std::pair<sysio::producer_authority, uint16_t>;
      std::vector< value_type > top_producers;
      std::vector< finalizer_auth_info > proposed_finalizers;
      top_producers.reserve(max_producers);
      proposed_finalizers.reserve(max_producers);

      // `rank` is POSITION in this index among schedulable producers, so the first max_producers
      // matches ARE ranks 1..max_producers -- the active schedule. Standbys are the positions past
      // it and never enter the schedule, which is why the old schedule_rank_limit branch is gone:
      // a slot vacated by an ineligible producer is filled by the next schedulable entry for free,
      // with no explicit backfill.
      //
      // The walk is bounded by the demoted tier. `regproducer` is permissionless, so the table is
      // unbounded -- but producer_rank::compute sinks every non-ACTIVE producer operator into the
      // demoted tier, which sorts last, so the scan stops before the spam tail.
      uint32_t examined = 0;
      for( auto it = idx.cbegin(); it != idx.cend() && top_producers.size() < max_producers; ++it ) {
         if( producer_rank::tier_of( it->rank_score ) == producer_tier::demoted ) break;
         if( ++examined > max_rank_walk_rows ) break;
         if( !producer_rank::is_schedulable( *it, _finalizers ) ) continue;

         proposed_finalizers.emplace_back( _finalizers.get( finalizer_key_t{it->owner.value} ) );
         top_producers.emplace_back(
            sysio::producer_authority{
               .producer_name = it->owner,
               .authority     = it->get_producer_authority()
            },
            it->location
         );
      }

      // Never publish a schedule (and its lock-step finalizer policy) smaller
      // than the BFT safety floor. If fewer producers are collateral-eligible,
      // retain the last good schedule and finalizer policy rather than
      // concentrate block production and finality onto too few nodes. This
      // early return precedes both set_proposed_producers and
      // set_proposed_finalizers, so below the floor neither is changed and the
      // two stay in lock-step. min_schedule_size >= 1 subsumes the empty check.
      if( top_producers.size() < min_schedule_size ) {
         return;
      }

      // Sort by producer name for deterministic ordering
      std::sort( top_producers.begin(), top_producers.end(), []( const value_type& lhs, const value_type& rhs ) {
         return lhs.first.producer_name < rhs.first.producer_name;
      } );

      std::vector<sysio::producer_authority> producers;
      producers.reserve(top_producers.size());
      for( auto& item : top_producers )
         producers.push_back( std::move(item.first) );

      if( set_proposed_producers( producers ) >= 0 ) {
         _global.modify( get_self(), [&]( auto& g ) {
            g.last_producer_schedule_size = static_cast<decltype(g.last_producer_schedule_size)>( producers.size() );
         });
      }

      set_proposed_finalizers( std::move(proposed_finalizers) );
   }

} /// namespace sysiosystem
