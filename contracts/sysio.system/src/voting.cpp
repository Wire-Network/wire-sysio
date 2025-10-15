#include <sysio/crypto.hpp>
#include <sysio/datastream.hpp>
#include <sysio/sysio.hpp>
#include <sysio/multi_index.hpp>
#include <sysio/permission.hpp>
#include <sysio/privileged.hpp>
#include <sysio/serialize.hpp>
#include <sysio/singleton.hpp>

#include <sysio.system/sysio.system.hpp>
#include <sysio.token/sysio.token.hpp>

#include <type_traits>
#include <limits>
#include <set>
#include <algorithm>
#include <cmath>

namespace sysiosystem {

   using sysio::const_mem_fun;
   using sysio::current_time_point;
   using sysio::indexed_by;
   using sysio::microseconds;
   using sysio::singleton;

   void system_contract::register_producer( const name& producer, const sysio::block_signing_authority& producer_authority, const std::string& url, uint16_t location ) {
      auto prod = _producers.find( producer.value );
      const auto ct = current_time_point();

      sysio::public_key producer_key{};

      std::visit( [&](auto&& auth ) {
         if( auth.keys.size() == 1 ) {
            // if the producer_authority consists of a single key, use that key in the legacy producer_key field
            producer_key = auth.keys[0].key;
         }
      }, producer_authority );

      if ( prod != _producers.end() ) {
         _producers.modify( prod, get_self(), [&]( producer_info& info ){
            info.producer_key       = producer_key;
            info.is_active          = true;
            info.url                = url;
            info.location           = location;
            info.producer_authority = producer_authority;
            if ( info.last_claim_time == time_point() )
               info.last_claim_time = ct;
         });
      } else {
         _producers.emplace( get_self(), [&]( producer_info& info ){
            info.owner              = producer;
            info.producer_key       = producer_key;
            info.is_active          = true;
            info.url                = url;
            info.location           = location;
            info.last_claim_time    = ct;
            info.producer_authority = producer_authority;
         });
      }

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

      const auto& prod = _producers.get( producer.value, "producer not found" );
      _producers.modify( prod, get_self(), [&]( producer_info& info ){
         info.deactivate();
      });
   }

   void system_contract::update_elected_producers( const block_timestamp& block_time ) {
      _gstate.last_producer_schedule_update = block_time;

      // TODO: this doesn't do anything as producer change is undefined at the moment
   }

} /// namespace sysiosystem
