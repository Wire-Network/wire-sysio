#include <sysio/crypto.hpp>
#include <sysio/datastream.hpp>
#include <sysio/sysio.hpp>
#include <sysio/permission.hpp>
#include <sysio/privileged.hpp>
#include <sysio/serialize.hpp>

#include <sysio.system/sysio.system.hpp>
#include <sysio.system/opreg_status.hpp>
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
      _gstate.last_producer_schedule_update = block_time;

      auto idx = _producers.get_index<"prodrank"_n>();

      using value_type = std::pair<sysio::producer_authority, uint16_t>;
      std::vector< value_type > top_producers;
      std::vector< finalizer_auth_info > proposed_finalizers;
      top_producers.reserve(max_producers);
      proposed_finalizers.reserve(max_producers);

      // Standbys (rank above max_producers, up to standby_end_rank) may backfill
      // active slots vacated by ineligible producers, so the schedule stays at
      // max_producers whenever replacements exist. standby_end_rank is
      // governance-tunable on the emitcfg singleton (>= 22, capped by
      // setemitcfg); before emissions config is installed there are no standbys,
      // so fall back to max_producers.
      uint32_t schedule_rank_limit = max_producers;
      emissions::emitcfg_t emitcfg( get_self() );
      if( emitcfg.exists() ) {
         schedule_rank_limit = emitcfg.get().standby_end_rank;
      }

      for( auto it = idx.cbegin(); it != idx.cend() && top_producers.size() < max_producers; ++it ) {
         if( it->rank > schedule_rank_limit ) break;   // past the last standby
         if( !it->active() ) continue;

         // A producer must be a live, collateral-backed producer operator in
         // sysio.opreg. A producer that withdrew collateral (status UNKNOWN),
         // was slashed, or was terminated is no longer OPERATOR_STATUS_ACTIVE
         // and must not be scheduled. Requiring OPERATOR_TYPE_PRODUCER prevents
         // an account that is ACTIVE only as a different operator type (e.g. a
         // batch operator, backed by different collateral) from being scheduled.
         if( !is_op_active( it->owner, sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER ) ) {
            continue;
         }

         // Require active finalizer key for all scheduled producers
         auto fin_key = finalizer_key_t{it->owner.value};
         if( !_finalizers.contains(fin_key) ) {
            continue;
         }
         auto finalizer = _finalizers.get(fin_key);
         if( finalizer.active_key_binary.empty() ) {
            continue;
         }

         proposed_finalizers.emplace_back(finalizer);
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
         _gstate.last_producer_schedule_size = static_cast<decltype(_gstate.last_producer_schedule_size)>( producers.size() );
      }

      set_proposed_finalizers( std::move(proposed_finalizers) );
   }

} /// namespace sysiosystem
