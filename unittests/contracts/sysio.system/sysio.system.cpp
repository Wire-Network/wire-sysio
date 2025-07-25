#include "sysio.system.hpp"
#include <sysio/dispatcher.hpp>
#include <sysio/crypto.hpp>

#include "producer_pay.cpp"
#include "delegate_bandwidth.cpp"
#include "voting.cpp"
#include "exchange_state.cpp"
#include "rex.cpp"

namespace sysiosystem {

system_contract::system_contract( name s, name code, datastream<const char*> ds )
      :native(s,code,ds),
       _voters(_self, _self.value),
       _producers(_self, _self.value),
       _producers2(_self, _self.value),
       _global(_self, _self.value),
       _global2(_self, _self.value),
       _global3(_self, _self.value),
       _rammarket(_self, _self.value),
       _rexpool(_self, _self.value),
       _rexfunds(_self, _self.value),
       _rexbalance(_self, _self.value),
       _rexorders(_self, _self.value)
{
   //print( "construct system\n" );
   _gstate  = _global.exists() ? _global.get() : get_default_parameters();
   _gstate2 = _global2.exists() ? _global2.get() : sysio_global_state2{};
   _gstate3 = _global3.exists() ? _global3.get() : sysio_global_state3{};
}

sysio_global_state system_contract::get_default_parameters() {
   sysio_global_state dp;
   get_blockchain_parameters(dp);
   return dp;
}

symbol system_contract::core_symbol()const {
   const static auto sym = get_core_symbol( _rammarket );
   return sym;
}

system_contract::~system_contract() {
   _global.set( _gstate, _self );
   _global2.set( _gstate2, _self );
   _global3.set( _gstate3, _self );
}

void system_contract::setram( uint64_t max_ram_size ) {
   require_auth( _self );

   check( _gstate.max_ram_size < max_ram_size, "ram may only be increased" ); /// decreasing ram might result market maker issues
   check( max_ram_size < 1024ll*1024*1024*1024*1024, "ram size is unrealistic" );
   check( max_ram_size > _gstate.total_ram_bytes_reserved, "attempt to set max below reserved" );

   auto delta = int64_t(max_ram_size) - int64_t(_gstate.max_ram_size);
   auto itr = _rammarket.find(ramcore_symbol.raw());

   /**
    *  Increase the amount of ram for sale based upon the change in max ram size.
    */
   _rammarket.modify( itr, same_payer, [&]( auto& m ) {
      m.base.balance.amount += delta;
   });

   _gstate.max_ram_size = max_ram_size;
}

void system_contract::update_ram_supply() {
   auto cbt = current_block_time();

   if( cbt <= _gstate2.last_ram_increase ) return;

   auto itr = _rammarket.find(ramcore_symbol.raw());
   auto new_ram = (cbt.slot - _gstate2.last_ram_increase.slot)*_gstate2.new_ram_per_block;
   _gstate.max_ram_size += new_ram;

   /**
    *  Increase the amount of ram for sale based upon the change in max ram size.
    */
   _rammarket.modify( itr, same_payer, [&]( auto& m ) {
      m.base.balance.amount += new_ram;
   });
   _gstate2.last_ram_increase = cbt;
}

/**
 *  Sets the rate of increase of RAM in bytes per block. It is capped by the uint16_t to
 *  a maximum rate of 3 TB per year.
 *
 *  If update_ram_supply hasn't been called for the most recent block, then new ram will
 *  be allocated at the old rate up to the present block before switching the rate.
 */
void system_contract::setramrate( uint16_t bytes_per_block ) {
   require_auth( _self );

   update_ram_supply();
   _gstate2.new_ram_per_block = bytes_per_block;
}

void system_contract::setparams( const sysio::blockchain_parameters& params ) {
   require_auth( _self );
   (sysio::blockchain_parameters&)(_gstate) = params;
   check( 3 <= _gstate.max_authority_depth, "max_authority_depth should be at least 3" );
   set_blockchain_parameters( params );
}

void system_contract::setpriv( name account, uint8_t ispriv ) {
   require_auth( _self );
   set_privileged( account, ispriv );
}

void system_contract::setalimits( name account, int64_t ram, int64_t net, int64_t cpu ) {
   require_auth( _self );
   user_resources_table userres( _self, account.value );
   auto ritr = userres.find( account.value );
   check( ritr == userres.end(), "only supports unlimited accounts" );
   set_resource_limits( account, ram, net, cpu );
}

void system_contract::rmvproducer( name producer ) {
   require_auth( _self );
   auto prod = _producers.find( producer.value );
   check( prod != _producers.end(), "producer not found" );
   _producers.modify( prod, same_payer, [&](auto& p) {
      p.deactivate();
   });
}

void system_contract::updtrevision( uint8_t revision ) {
   require_auth( _self );
   check( _gstate2.revision < 255, "can not increment revision" ); // prevent wrap around
   check( revision == _gstate2.revision + 1, "can only increment revision by one" );
   check( revision <= 1, // set upper bound to greatest revision supported in the code
          "specified revision is not yet supported by the code" );
   _gstate2.revision = revision;
}

void system_contract::bidname( name bidder, name newname, asset bid ) {
   require_auth( bidder );
   check( newname.suffix() == newname, "you can only bid on top-level suffix" );

   check( (bool)newname, "the empty name is not a valid account name to bid on" );
   check( (newname.value & 0xFull) == 0, "13 character names are not valid account names to bid on" );
   check( (newname.value & 0x1F0ull) == 0, "accounts with 12 character names and no dots can be created without bidding required" );
   check( !is_account( newname ), "account already exists" );
   check( bid.symbol == core_symbol(), "asset must be system token" );
   check( bid.amount > 0, "insufficient bid" );

   INLINE_ACTION_SENDER(sysio::token, transfer)(
         token_account, { {bidder, active_permission}, {bidder, payer_permission} },
         { bidder, names_account, bid, std::string("bid name ")+ newname.to_string() }
   );

   name_bid_table bids(_self, _self.value);
   print( name{bidder}, " bid ", bid, " on ", name{newname}, "\n" );
   auto current = bids.find( newname.value );
   if( current == bids.end() ) {
      bids.emplace( bidder, [&]( auto& b ) {
         b.newname = newname;
         b.high_bidder = bidder;
         b.high_bid = bid.amount;
         b.last_bid_time = current_time_point();
      });
   } else {
      check( current->high_bid > 0, "this auction has already closed" );
      check( bid.amount - current->high_bid > (current->high_bid / 10), "must increase bid by 10%" );
      check( current->high_bidder != bidder, "account is already highest bidder" );

      bid_refund_table refunds_table(_self, newname.value);

      auto it = refunds_table.find( current->high_bidder.value );
      if ( it != refunds_table.end() ) {
         refunds_table.modify( it, same_payer, [&](auto& r) {
            r.amount += asset( current->high_bid, core_symbol() );
         });
      } else {
         refunds_table.emplace( bidder, [&](auto& r) {
            r.bidder = current->high_bidder;
            r.amount = asset( current->high_bid, core_symbol() );
         });
      }

      transaction t;
      t.actions.emplace_back( permission_level{_self, active_permission},
                              _self, "bidrefund"_n,
                              std::make_tuple( current->high_bidder, newname )
      );
      t.delay_sec = 0;
      uint128_t deferred_id = (uint128_t(newname.value) << 64) | current->high_bidder.value;
      cancel_deferred( deferred_id );
      t.send( deferred_id, bidder );

      bids.modify( current, bidder, [&]( auto& b ) {
         b.high_bidder = bidder;
         b.high_bid = bid.amount;
         b.last_bid_time = current_time_point();
      });
   }
}

void system_contract::bidrefund( name bidder, name newname ) {
   bid_refund_table refunds_table(_self, newname.value);
   auto it = refunds_table.find( bidder.value );
   check( it != refunds_table.end(), "refund not found" );
   INLINE_ACTION_SENDER(sysio::token, transfer)(
         token_account, { {names_account, active_permission}, {bidder, active_permission} },
         { names_account, bidder, asset(it->amount), std::string("refund bid on name ")+(name{newname}).to_string() }
   );
   refunds_table.erase( it );
}

/**
 *  Called after a new account is created. This code enforces resource-limits rules
 *  for new accounts as well as new account naming conventions.
 *
 *  Account names containing '.' symbols must have a suffix equal to the name of the creator.
 *  This allows users who buy a premium name (shorter than 12 characters with no dots) to be the only ones
 *  who can create accounts with the creator's name as a suffix.
 *
 */
void native::newaccount( name              creator,
                         name              newact,
                         ignore<authority> owner,
                         ignore<authority> active ) {

   if( creator != _self ) {
      uint64_t tmp = newact.value >> 4;
      bool has_dot = false;

      for( uint32_t i = 0; i < 12; ++i ) {
         has_dot |= !(tmp & 0x1f);
         tmp >>= 5;
      }
      if( has_dot ) { // or is less than 12 characters
         auto suffix = newact.suffix();
         if( suffix == newact ) {
            name_bid_table bids(_self, _self.value);
            auto current = bids.find( newact.value );
            check( current != bids.end(), "no active bid for name" );
            check( current->high_bidder == creator, "only highest bidder can claim" );
            check( current->high_bid < 0, "auction for name is not closed yet" );
            bids.erase( current );
         } else {
            check( creator == suffix, "only suffix may create this account" );
         }
      }
   }

   user_resources_table  userres( _self, newact.value);

   userres.emplace( newact, [&]( auto& res ) {
      res.owner = newact;
      res.net_weight = asset( 0, system_contract::get_core_symbol() );
      res.cpu_weight = asset( 0, system_contract::get_core_symbol() );
   });

   set_resource_limits( newact, 0, 0, 0 );
}

void native::setabi( name acnt, const std::vector<char>& abi ) {
   sysio::multi_index< "abihash"_n, abi_hash >  table(_self, _self.value);
   auto itr = table.find( acnt.value );
   if( itr == table.end() ) {
      table.emplace( acnt, [&]( auto& row ) {
         row.owner= acnt;
         row.hash = sysio::sha256( const_cast<char*>(abi.data()), abi.size());
      });
   } else {
      table.modify( itr, same_payer, [&]( auto& row ) {
         row.hash = sysio::sha256( const_cast<char*>(abi.data()), abi.size() );
      });
   }
}

void system_contract::init( unsigned_int version, symbol core ) {
   require_auth( _self );
   check( version.value == 0, "unsupported version for init action" );

   auto itr = _rammarket.find(ramcore_symbol.raw());
   check( itr == _rammarket.end(), "system contract has already been initialized" );

   auto system_token_supply   = sysio::token::get_supply(token_account, core.code() );
   check( system_token_supply.symbol == core, "specified core symbol does not exist (precision mismatch)" );

   check( system_token_supply.amount > 0, "system token supply must be greater than 0" );
   _rammarket.emplace( _self, [&]( auto& m ) {
      m.supply.amount = 100000000000000ll;
      m.supply.symbol = ramcore_symbol;
      m.base.balance.amount = int64_t(_gstate.free_ram());
      m.base.balance.symbol = ram_symbol;
      m.quote.balance.amount = system_token_supply.amount / 1000;
      m.quote.balance.symbol = core;
   });

   INLINE_ACTION_SENDER(sysio::token, open)( token_account, { _self, active_permission },
                                             { rex_account, core, _self } );
}

} /// sysio.system


SYSIO_DISPATCH( sysiosystem::system_contract,
// native.hpp (newaccount definition is actually in sysio.system.cpp)
(newaccount)(updateauth)(deleteauth)(linkauth)(unlinkauth)(canceldelay)(onerror)(setabi)
// sysio.system.cpp
      (init)(setram)(setramrate)(setparams)(setpriv)(setalimits)(rmvproducer)(updtrevision)(bidname)(bidrefund)
// rex.cpp
      // (deposit)(withdraw)(buyrex)(unstaketorex)(sellrex)(cnclrexorder)(rentcpu)(rentnet)(fundcpuloan)(fundnetloan)
      // (defcpuloan)(defnetloan)(updaterex)(consolidate)(rexexec)(closerex)
// delegate_bandwidth.cpp
       (buyrambytes)(buyram)(sellram)(delegatebw)(undelegatebw)(refund)
// voting.cpp
      (regproducer)(unregprod)(voteproducer)(regproxy)
// producer_pay.cpp
      (onblock)(claimrewards)
)
