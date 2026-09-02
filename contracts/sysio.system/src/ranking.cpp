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
            // regproducer is the SINGLE door back -- from a voluntary `unregprod` park and from an
            // involuntary demotion alike. `unregprod` erases the signing key, so re-registering has
            // to re-supply it, which makes this a genuine assertion of readiness rather than a
            // no-op. There is deliberately no cooldown and no expiry: a producer that comes back
            // before it is ready is demoted again within max_consecutive_missed_rounds rounds,
            // which is self-correcting.
            info.is_demoted                = false;
            info.consecutive_missed_rounds = 0;
         });

      // The clear above changes the producer's tier, so its sort key is stale until rescored.
      rescore_producer( producer );
   }

   void system_contract::rescore_producer( const name& producer ) {
      auto key = producer_key_t{producer.value};
      if( !_producers.contains(key) ) return;

      producer_rank::producer_score_config_t weights_tbl( get_self() );
      const auto weights = weights_tbl.get_or_default( producer_rank::producer_score_config{} );

      const auto info = _producers.get(key);
      const auto score = producer_rank::compute(
         producer,
         producer_rank::score_inputs{
            .is_demoted                = info.is_demoted,
            .consecutive_missed_rounds = info.consecutive_missed_rounds,
            .snapshot_attestations     = info.snapshot_attestations
         },
         weights );

      if( score == info.rank_score ) return;   // no index move needed
      _producers.modify( same_payer, key, [&]( auto& p ) { p.rank_score = score; });
   }

   void system_contract::onprocessprod( name account, bool, bool ) {
      // The eligibility flags are not consulted: rescore_producer reads the operator's live status
      // and balances, which is the same information after the flip and cannot go stale between the
      // notification and this handler.
      rescore_producer( account );
   }

   void system_contract::setscorecfg( const producer_rank::producer_score_config& weights ) {
      require_auth( get_self() );

      producer_rank::producer_score_config_t weights_tbl( get_self() );
      weights_tbl.set( weights, get_self() );

      // Every stored rank_score was computed under the OLD weights. Rather than rewrite an
      // unbounded table inline, open a rescore sweep: onblock drains a bounded number of rows per
      // schedule-rebuild tick until the cursor is exhausted.
      _global.modify( get_self(), []( auto& g ) {
         g.rescore_cursor = 0;
         g.rescore_generation++;
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
      });
   }

   void system_contract::update_ranked_producers( const block_timestamp& block_time ) {
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
      for( auto it = idx.cbegin(); it != idx.cend() && top_producers.size() < max_producers; ++it ) {
         if( producer_rank::tier_of( it->rank_score ) == producer_tier::demoted ) break;
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
