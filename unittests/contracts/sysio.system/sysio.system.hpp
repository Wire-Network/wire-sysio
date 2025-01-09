#pragma once

#include "native.hpp"
#include <sysio/asset.hpp>
#include <sysio/time.hpp>
#include <sysio/privileged.hpp>
#include <sysio/singleton.hpp>
#include "exchange_state.hpp"

#include <string>
#include <deque>

#ifdef CHANNEL_RAM_AND_NAMEBID_FEES_TO_REX
#undef CHANNEL_RAM_AND_NAMEBID_FEES_TO_REX
#endif
// CHANNEL_RAM_AND_NAMEBID_FEES_TO_REX macro determines whether ramfee and namebid proceeds are
// channeled to REX pool. In order to stop these proceeds from being channeled, the macro must
// be set to 0.
#define CHANNEL_RAM_AND_NAMEBID_FEES_TO_REX 1

namespace sysiosystem {

using sysio::name;
using sysio::asset;
using sysio::symbol;
using sysio::symbol_code;
using sysio::indexed_by;
using sysio::const_mem_fun;
using sysio::block_timestamp;
using sysio::time_point;
using sysio::time_point_sec;
using sysio::microseconds;
using sysio::datastream;
using sysio::check;
using sysio::unsigned_int;

struct [[sysio::table, sysio::contract("sysio.system")]] name_bid {
   name            newname;
   name            high_bidder;
   int64_t         high_bid = 0; ///< negative high_bid == closed auction waiting to be claimed
   time_point      last_bid_time;

   uint64_t primary_key()const { return newname.value;                    }
   uint64_t by_high_bid()const { return static_cast<uint64_t>(-high_bid); }
};

struct [[sysio::table, sysio::contract("sysio.system")]] bid_refund {
   name         bidder;
   asset        amount;

   uint64_t primary_key()const { return bidder.value; }
};

typedef sysio::multi_index< "namebids"_n, name_bid,
      indexed_by<"highbid"_n, const_mem_fun<name_bid, uint64_t, &name_bid::by_high_bid>  >
> name_bid_table;

typedef sysio::multi_index< "bidrefunds"_n, bid_refund > bid_refund_table;

struct [[sysio::table("global"), sysio::contract("sysio.system")]] sysio_global_state : sysio::blockchain_parameters {
   uint64_t free_ram()const { return max_ram_size - total_ram_bytes_reserved; }

   uint64_t             max_ram_size = 64ll*1024 * 1024 * 1024;
   uint64_t             total_ram_bytes_reserved = 0;
   int64_t              total_ram_stake = 0;

   block_timestamp      last_producer_schedule_update;
   time_point           last_pervote_bucket_fill;
   int64_t              pervote_bucket = 0;
   int64_t              perblock_bucket = 0;
   uint32_t             total_unpaid_blocks = 0; /// all blocks which have been produced but not paid
   int64_t              total_activated_stake = 0;
   time_point           thresh_activated_stake_time;
   uint16_t             last_producer_schedule_size = 0;
   double               total_producer_vote_weight = 0; /// the sum of all producer votes
   block_timestamp      last_name_close;

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE_DERIVED( sysio_global_state, sysio::blockchain_parameters,
   (max_ram_size)(total_ram_bytes_reserved)(total_ram_stake)
         (last_producer_schedule_update)(last_pervote_bucket_fill)
         (pervote_bucket)(perblock_bucket)(total_unpaid_blocks)(total_activated_stake)(thresh_activated_stake_time)
         (last_producer_schedule_size)(total_producer_vote_weight)(last_name_close) )
};

/**
 * Defines new global state parameters added after version 1.0
 */
struct [[sysio::table("global2"), sysio::contract("sysio.system")]] sysio_global_state2 {
   sysio_global_state2(){}

   uint16_t          new_ram_per_block = 0;
   block_timestamp   last_ram_increase;
   block_timestamp   last_block_num; /* deprecated */
   double            total_producer_votepay_share = 0;
   uint8_t           revision = 0; ///< used to track version updates in the future.

   SYSLIB_SERIALIZE( sysio_global_state2, (new_ram_per_block)(last_ram_increase)(last_block_num)
         (total_producer_votepay_share)(revision) )
};

struct [[sysio::table("global3"), sysio::contract("sysio.system")]] sysio_global_state3 {
   sysio_global_state3() { }
   time_point        last_vpay_state_update;
   double            total_vpay_share_change_rate = 0;

   SYSLIB_SERIALIZE( sysio_global_state3, (last_vpay_state_update)(total_vpay_share_change_rate) )
};

struct [[sysio::table, sysio::contract("sysio.system")]] producer_info {
   name                  owner;
   double                total_votes = 0;
   sysio::public_key     producer_key; /// a packed public key object
   bool                  is_active = true;
   std::string           url;
   uint32_t              unpaid_blocks = 0;
   time_point            last_claim_time;
   uint16_t              location = 0;

   uint64_t primary_key()const { return owner.value;                             }
   double   by_votes()const    { return is_active ? -total_votes : total_votes;  }
   bool     active()const      { return is_active;                               }
   void     deactivate()       { producer_key = public_key(); is_active = false; }

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( producer_info, (owner)(total_votes)(producer_key)(is_active)(url)
         (unpaid_blocks)(last_claim_time)(location) )
};

struct [[sysio::table, sysio::contract("sysio.system")]] producer_info2 {
   name            owner;
   double          votepay_share = 0;
   time_point      last_votepay_share_update;

   uint64_t primary_key()const { return owner.value; }

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( producer_info2, (owner)(votepay_share)(last_votepay_share_update) )
};

struct [[sysio::table, sysio::contract("sysio.system")]] voter_info {
   name                owner;     /// the voter
   name                proxy;     /// the proxy set by the voter, if any
   std::vector<name>   producers; /// the producers approved by this voter if no proxy set
   int64_t             staked = 0;

   /**
    *  Every time a vote is cast we must first "undo" the last vote weight, before casting the
    *  new vote weight.  Vote weight is calculated as:
    *
    *  stated.amount * 2 ^ ( weeks_since_launch/weeks_per_year)
    */
   double              last_vote_weight = 0; /// the vote weight cast the last time the vote was updated

   /**
    * Total vote weight delegated to this voter.
    */
   double              proxied_vote_weight= 0; /// the total vote weight delegated to this voter as a proxy
   bool                is_proxy = 0; /// whether the voter is a proxy for others


   uint32_t            reserved1 = 0;
   uint32_t            reserved2 = 0;
   sysio::asset        reserved3;

   uint64_t primary_key()const { return owner.value; }

   // explicit serialization macro is not necessary, used here only to improve compilation time
   SYSLIB_SERIALIZE( voter_info, (owner)(proxy)(producers)(staked)(last_vote_weight)(proxied_vote_weight)(is_proxy)(reserved1)(reserved2)(reserved3) )
};

typedef sysio::multi_index< "voters"_n, voter_info >  voters_table;


typedef sysio::multi_index< "producers"_n, producer_info,
      indexed_by<"prototalvote"_n, const_mem_fun<producer_info, double, &producer_info::by_votes>  >
> producers_table;
typedef sysio::multi_index< "producers2"_n, producer_info2 > producers_table2;

typedef sysio::singleton< "global"_n, sysio_global_state >   global_state_singleton;
typedef sysio::singleton< "global2"_n, sysio_global_state2 > global_state2_singleton;
typedef sysio::singleton< "global3"_n, sysio_global_state3 > global_state3_singleton;

static constexpr uint32_t     seconds_per_day = 24 * 3600;

struct [[sysio::table,sysio::contract("sysio.system")]] rex_pool {
   uint8_t    version = 0;
   asset      total_lent; /// total amount of CORE_SYMBOL in open rex_loans
   asset      total_unlent; /// total amount of CORE_SYMBOL available to be lent (connector)
   asset      total_rent; /// fees received in exchange for lent  (connector)
   asset      total_lendable; /// total amount of CORE_SYMBOL that have been lent (total_unlent + total_lent)
   asset      total_rex; /// total number of REX shares allocated to contributors to total_lendable
   asset      namebid_proceeds; /// the amount of CORE_SYMBOL to be transferred from namebids to REX pool
   uint64_t   loan_num = 0; /// increments with each new loan

   uint64_t primary_key()const { return 0; }
};

typedef sysio::multi_index< "rexpool"_n, rex_pool > rex_pool_table;

struct [[sysio::table,sysio::contract("sysio.system")]] rex_fund {
   uint8_t version = 0;
   name    owner;
   asset   balance;

   uint64_t primary_key()const { return owner.value; }
};

typedef sysio::multi_index< "rexfund"_n, rex_fund > rex_fund_table;

struct [[sysio::table,sysio::contract("sysio.system")]] rex_balance {
   uint8_t version = 0;
   name    owner;
   asset   vote_stake; /// the amount of CORE_SYMBOL currently included in owner's vote
   asset   rex_balance; /// the amount of REX owned by owner
   int64_t matured_rex = 0; /// matured REX available for selling
   std::deque<std::pair<time_point_sec, int64_t>> rex_maturities; /// REX daily maturity buckets

   uint64_t primary_key()const { return owner.value; }
};

typedef sysio::multi_index< "rexbal"_n, rex_balance > rex_balance_table;

struct [[sysio::table,sysio::contract("sysio.system")]] rex_loan {
   uint8_t             version = 0;
   name                from;
   name                receiver;
   asset               payment;
   asset               balance;
   asset               total_staked;
   uint64_t            loan_num;
   sysio::time_point   expiration;

   uint64_t primary_key()const { return loan_num;                   }
   uint64_t by_expr()const     { return expiration.elapsed.count(); }
   uint64_t by_owner()const    { return from.value;                 }
};

typedef sysio::multi_index< "cpuloan"_n, rex_loan,
      indexed_by<"byexpr"_n,  const_mem_fun<rex_loan, uint64_t, &rex_loan::by_expr>>,
indexed_by<"byowner"_n, const_mem_fun<rex_loan, uint64_t, &rex_loan::by_owner>>
> rex_cpu_loan_table;

typedef sysio::multi_index< "netloan"_n, rex_loan,
      indexed_by<"byexpr"_n,  const_mem_fun<rex_loan, uint64_t, &rex_loan::by_expr>>,
indexed_by<"byowner"_n, const_mem_fun<rex_loan, uint64_t, &rex_loan::by_owner>>
> rex_net_loan_table;

struct [[sysio::table,sysio::contract("sysio.system")]] rex_order {
   uint8_t             version = 0;
   name                owner;
   asset               rex_requested;
   asset               proceeds;
   asset               stake_change;
   sysio::time_point   order_time;
   bool                is_open = true;

   void close()                { is_open = false;    }
   uint64_t primary_key()const { return owner.value; }
   uint64_t by_time()const     { return is_open ? order_time.elapsed.count() : std::numeric_limits<uint64_t>::max(); }
};

typedef sysio::multi_index< "rexqueue"_n, rex_order,
      indexed_by<"bytime"_n, const_mem_fun<rex_order, uint64_t, &rex_order::by_time>>> rex_order_table;

struct rex_order_outcome {
   bool success;
   asset proceeds;
   asset stake_change;
};

class [[sysio::contract("sysio.system")]] system_contract : public native {

private:
   voters_table            _voters;
   producers_table         _producers;
   producers_table2        _producers2;
   global_state_singleton  _global;
   global_state2_singleton _global2;
   global_state3_singleton _global3;
   sysio_global_state      _gstate;
   sysio_global_state2     _gstate2;
   sysio_global_state3     _gstate3;
   rammarket               _rammarket;
   rex_pool_table          _rexpool;
   rex_fund_table          _rexfunds;
   rex_balance_table       _rexbalance;
   rex_order_table         _rexorders;

public:
   static constexpr sysio::name active_permission{"active"_n};
   static constexpr sysio::name token_account{"sysio.token"_n};
   static constexpr sysio::name ram_account{"sysio.ram"_n};
   static constexpr sysio::name ramfee_account{"sysio.ramfee"_n};
   static constexpr sysio::name stake_account{"sysio.stake"_n};
   static constexpr sysio::name bpay_account{"sysio.bpay"_n};
   static constexpr sysio::name vpay_account{"sysio.vpay"_n};
   static constexpr sysio::name names_account{"sysio.names"_n};
   static constexpr sysio::name saving_account{"sysio.saving"_n};
   static constexpr sysio::name rex_account{"sysio.rex"_n};
   static constexpr symbol ramcore_symbol = symbol(symbol_code("RAMCORE"), 4);
   static constexpr symbol ram_symbol     = symbol(symbol_code("RAM"), 0);
   static constexpr symbol rex_symbol     = symbol(symbol_code("REX"), 4);

   system_contract( name s, name code, datastream<const char*> ds );
   ~system_contract();

   static symbol get_core_symbol( name system_account = "sysio"_n ) {
      rammarket rm(system_account, system_account.value);
      const static auto sym = get_core_symbol( rm );
      return sym;
   }

   // Actions:
   [[sysio::action]]
   void init( unsigned_int version, symbol core );
   [[sysio::action]]
   void onblock( ignore<block_header> header );

   [[sysio::action]]
   void setalimits( name account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight );
   // functions defined in delegate_bandwidth.cpp

   /**
    *  Stakes SYS from the balance of 'from' for the benfit of 'receiver'.
    *  If transfer == true, then 'receiver' can unstake to their account
    *  Else 'from' can unstake at any time.
    */
   [[sysio::action]]
   void delegatebw( name from, name receiver,
                    asset stake_net_quantity, asset stake_cpu_quantity, bool transfer );

   /**
    * Deposits core tokens to user REX fund. All proceeds and expenses related to REX are added to
    * or taken out of this fund. Inline token transfer from user balance is executed.
    */
   [[sysio::action]]
   void deposit( const name& owner, const asset& amount );

   /**
    * Withdraws core tokens from user REX fund. Inline token transfer to user balance is
    * executed.
    */
   [[sysio::action]]
   void withdraw( const name& owner, const asset& amount );

   /**
    * Transfers core tokens from user REX fund and converts them to REX stake.
    * A voting requirement must be satisfied before action can be executed.
    * User votes are updated following this action.
    */
   [[sysio::action]]
   void buyrex( const name& from, const asset& amount );

   /**
    * Use staked core tokens to buy REX.
    * A voting requirement must be satisfied before action can be executed.
    * User votes are updated following this action.
    */
   [[sysio::action]]
   void unstaketorex( const name& owner, const name& receiver, const asset& from_net, const asset& from_cpu );

   /**
    * Converts REX stake back into core tokens at current exchange rate. If order cannot be
    * processed, it gets queued until there is enough in REX pool to fill order.
    * If successful, user votes are updated.
    */
   [[sysio::action]]
   void sellrex( const name& from, const asset& rex );

   /**
    * Cancels queued sellrex order. Order cannot be cancelled once it's been filled.
    */
   [[sysio::action]]
   void cnclrexorder( const name& owner );

   /**
    * Use payment to rent as many SYS tokens as possible and stake them for either CPU or NET for the
    * benefit of receiver, after 30 days the rented SYS delegation of CPU or NET will expire unless loan
    * balance is larger than or equal to payment.
    *
    * If loan has enough balance, it gets renewed at current market price, otherwise, it is closed and
    * remaining balance is refunded to loan owner.
    *
    * Owner can fund or defund a loan at any time before its expiration.
    *
    * All loan expenses and refunds come out of or are added to owner's REX fund.
    */
   [[sysio::action]]
   void rentcpu( const name& from, const name& receiver, const asset& loan_payment, const asset& loan_fund );
   [[sysio::action]]
   void rentnet( const name& from, const name& receiver, const asset& loan_payment, const asset& loan_fund );

   /**
    * Loan owner funds a given CPU or NET loan.
    */
   [[sysio::action]]
   void fundcpuloan( const name& from, uint64_t loan_num, const asset& payment );
   [[sysio::action]]
   void fundnetloan( const name& from, uint64_t loan_num, const asset& payment );
   /**
    * Loan owner defunds a given CPU or NET loan.
    */
   [[sysio::action]]
   void defcpuloan( const name& from, uint64_t loan_num, const asset& amount );
   [[sysio::action]]
   void defnetloan( const name& from, uint64_t loan_num, const asset& amount );

   /**
    * Updates REX vote stake of owner to its current value.
    */
   [[sysio::action]]
   void updaterex( const name& owner );

   /**
    * Processes max CPU loans, max NET loans, and max queued sellrex orders.
    * Action does not execute anything related to a specific user.
    */
   [[sysio::action]]
   void rexexec( const name& user, uint16_t max );

   /**
    * Consolidate REX maturity buckets into one that can be sold only 4 days
    * from the end of today.
    */
   [[sysio::action]]
   void consolidate( const name& owner );

   /**
    * Deletes owner records from REX tables and frees used RAM.
    * Owner must not have an outstanding REX balance.
    */
   [[sysio::action]]
   void closerex( const name& owner );

   /**
    *  Decreases the total tokens delegated by from to receiver and/or
    *  frees the memory associated with the delegation if there is nothing
    *  left to delegate.
    *
    *  This will cause an immediate reduction in net/cpu bandwidth of the
    *  receiver.
    *
    *  A transaction is scheduled to send the tokens back to 'from' after
    *  the staking period has passed. If existing transaction is scheduled, it
    *  will be canceled and a new transaction issued that has the combined
    *  undelegated amount.
    *
    *  The 'from' account loses voting power as a result of this call and
    *  all producer tallies are updated.
    */
   [[sysio::action]]
   void undelegatebw( name from, name receiver,
                      asset unstake_net_quantity, asset unstake_cpu_quantity );


   /**
    * Increases receiver's ram quota based upon current price and quantity of
    * tokens provided. An inline transfer from receiver to system contract of
    * tokens will be executed.
    */
   [[sysio::action]]
   void buyram( name payer, name receiver, asset quant );
   [[sysio::action]]
   void buyrambytes( name payer, name receiver, uint32_t bytes );

   /**
    *  Reduces quota my bytes and then performs an inline transfer of tokens
    *  to receiver based upon the average purchase price of the original quota.
    */
   [[sysio::action]]
   void sellram( name account, int64_t bytes );

   /**
    *  This action is called after the delegation-period to claim all pending
    *  unstaked tokens belonging to owner
    */
   [[sysio::action]]
   void refund( name owner );

   // functions defined in voting.cpp

   [[sysio::action]]
   void regproducer( const name producer, const public_key& producer_key, const std::string& url, uint16_t location );

   [[sysio::action]]
   void unregprod( const name producer );

   [[sysio::action]]
   void setram( uint64_t max_ram_size );
   [[sysio::action]]
   void setramrate( uint16_t bytes_per_block );

   [[sysio::action]]
   void voteproducer( const name voter, const name proxy, const std::vector<name>& producers );

   [[sysio::action]]
   void regproxy( const name proxy, bool isproxy );

   [[sysio::action]]
   void setparams( const sysio::blockchain_parameters& params );

   // functions defined in producer_pay.cpp
   [[sysio::action]]
   void claimrewards( const name owner );

   [[sysio::action]]
   void setpriv( name account, uint8_t is_priv );

   [[sysio::action]]
   void rmvproducer( name producer );

   [[sysio::action]]
   void updtrevision( uint8_t revision );

   [[sysio::action]]
   void bidname( name bidder, name newname, asset bid );

   [[sysio::action]]
   void bidrefund( name bidder, name newname );

private:

   // Implementation details:

   static symbol get_core_symbol( const rammarket& rm ) {
      auto itr = rm.find(ramcore_symbol.raw());
      check(itr != rm.end(), "system contract must first be initialized");
      return itr->quote.balance.symbol;
   }

   //defined in sysio.system.cpp
   static sysio_global_state get_default_parameters();
   symbol core_symbol()const;
   void update_ram_supply();

   // defined in rex.cpp
   void runrex( uint16_t max );
   void update_resource_limits( const name& from, const name& receiver, int64_t delta_net, int64_t delta_cpu );
   void check_voting_requirement( const name& owner,
                                  const char* error_msg = "must vote for at least 21 producers or for a proxy before buying REX" )const;
   rex_order_outcome fill_rex_order( const rex_balance_table::const_iterator& bitr, const asset& rex );
   asset update_rex_account( const name& owner, const asset& proceeds, const asset& unstake_quant, bool force_vote_update = false );
   void channel_to_rex( const name& from, const asset& amount );
   void channel_namebid_to_rex( const int64_t highest_bid );
   template <typename T>
   int64_t rent_rex( T& table, const name& from, const name& receiver, const asset& loan_payment, const asset& loan_fund );
   template <typename T>
   void fund_rex_loan( T& table, const name& from, uint64_t loan_num, const asset& payment );
   template <typename T>
   void defund_rex_loan( T& table, const name& from, uint64_t loan_num, const asset& amount );
   void transfer_from_fund( const name& owner, const asset& amount );
   void transfer_to_fund( const name& owner, const asset& amount );
   bool rex_loans_available()const { return _rexorders.begin() == _rexorders.end() && rex_available(); }
   bool rex_system_initialized()const { return _rexpool.begin() != _rexpool.end(); }
   bool rex_available()const { return rex_system_initialized() && _rexpool.begin()->total_rex.amount > 0; }
   static time_point_sec get_rex_maturity();
   asset add_to_rex_balance( const name& owner, const asset& payment, const asset& rex_received );
   asset add_to_rex_pool( const asset& payment );
   void process_rex_maturities( const rex_balance_table::const_iterator& bitr );
   void consolidate_rex_balance( const rex_balance_table::const_iterator& bitr,
                                 const asset& rex_in_sell_order );

   // defined in delegate_bandwidth.cpp
   void changebw( name from, name receiver,
                  asset stake_net_quantity, asset stake_cpu_quantity, bool transfer );
   void update_voting_power( const name& voter, const asset& total_update );

   // defined in voting.hpp
   void update_elected_producers( block_timestamp timestamp );
   void update_votes( const name voter, const name proxy, const std::vector<name>& producers, bool voting );
   void propagate_weight_change( const voter_info& voter );
   double update_producer_votepay_share( const producers_table2::const_iterator& prod_itr,
                                         time_point ct,
                                         double shares_rate, bool reset_to_zero = false );
   double update_total_votepay_share( time_point ct,
                                      double additional_shares_delta = 0.0, double shares_rate_delta = 0.0 );
};

} /// sysiosystem
