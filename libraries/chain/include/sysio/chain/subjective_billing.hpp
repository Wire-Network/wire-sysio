#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/config.hpp>

#include <fc/time.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <concepts>

namespace sysio::chain {

class subjective_billing {
private:

   struct trx_cache_entry {
      transaction_id_type            trx_id;
      account_subjective_cpu_bill_t  account_subjective_cpu_bill;
      fc::time_point                 expiry;
   };
   struct by_id;
   struct by_expiry;

   using trx_cache_index = bmi::multi_index_container<
         trx_cache_entry,
         indexed_by<
               bmi::hashed_unique<tag<by_id>, BOOST_MULTI_INDEX_MEMBER( trx_cache_entry, chain::transaction_id_type, trx_id ) >,
               ordered_non_unique<tag<by_expiry>, BOOST_MULTI_INDEX_MEMBER( trx_cache_entry, fc::time_point, expiry ) >
         >
   >;

   using decaying_accumulator = chain::resource_limits::impl::exponential_decay_accumulator<>;

   struct subjective_billing_info {
      uint64_t              pending_cpu_us = 0;    // tracked cpu us for transactions that may still succeed in a block
      decaying_accumulator  expired_accumulator;   // accumulator used to account for transactions that have expired

      bool empty(uint32_t time_ordinal, uint32_t expired_accumulator_average_window) const {
         return pending_cpu_us == 0 && expired_accumulator.value_at(time_ordinal, expired_accumulator_average_window) == 0;
      }
   };

   using account_subjective_bill_cache = std::unordered_map<chain::account_name, subjective_billing_info>;

   bool                                      _disabled = false;
   bool                                      _disabled_payer_billing = false;
   fc::microseconds                          _subjective_account_cpu_allowed{config::default_subjective_cpu_us};
   trx_cache_index                           _trx_cache_index;
   account_subjective_bill_cache             _account_subjective_bill_cache;
   std::set<chain::account_name>             _disabled_accounts;
   uint32_t                                  _expired_accumulator_average_window = chain::config::account_cpu_usage_average_window_ms / subjective_time_interval_ms;

private:
   void _reset() { // for testing
      _disabled = false;
      _subjective_account_cpu_allowed = fc::microseconds{config::default_subjective_cpu_us};
      _trx_cache_index.clear();
      _account_subjective_bill_cache.clear();
      _disabled_accounts.clear();
      _expired_accumulator_average_window = chain::config::account_cpu_usage_average_window_ms / subjective_time_interval_ms;
   }

   static uint32_t time_ordinal_for( const fc::time_point& t ) {
      auto ordinal = t.time_since_epoch().count() / (1000U * (uint64_t)subjective_time_interval_ms);
      SYS_ASSERT(ordinal <= std::numeric_limits<uint32_t>::max(), chain::tx_resource_exhaustion, "overflow of quantized time in subjective billing");
      return ordinal;
   }

   void remove_subjective_billing( const trx_cache_entry& entry, uint32_t time_ordinal ) {
      for (const auto& [account, subjective_cpu_bill] : entry.account_subjective_cpu_bill) {
         auto aitr = _account_subjective_bill_cache.find( account );
         if( aitr != _account_subjective_bill_cache.end() ) {
            aitr->second.pending_cpu_us -= subjective_cpu_bill.count();
            SYS_ASSERT( aitr->second.pending_cpu_us >= 0, chain::tx_resource_exhaustion,
                        "Logic error in subjective account billing {}", account );
            if( aitr->second.empty(time_ordinal, _expired_accumulator_average_window) ) _account_subjective_bill_cache.erase( aitr );
         }
      }
   }

   void transition_to_expired( const trx_cache_entry& entry, uint32_t time_ordinal ) {
      for (const auto& [account, subjective_cpu_bill] : entry.account_subjective_cpu_bill) {
         auto aitr = _account_subjective_bill_cache.find( account );
         if( aitr != _account_subjective_bill_cache.end() ) {
            aitr->second.pending_cpu_us -= subjective_cpu_bill.count();
            aitr->second.expired_accumulator.add(subjective_cpu_bill.count(), time_ordinal, _expired_accumulator_average_window);
         }
      }
   }

   void remove_subjective_billing( const chain::signed_block_ptr& block, uint32_t time_ordinal ) {
      if( !_trx_cache_index.empty() ) {
         for( const auto& receipt : block->transactions ) {
            remove_subjective_billing( receipt.trx.id(), time_ordinal );
         }
      }
   }

public: // public for tests
   static constexpr uint32_t subjective_time_interval_ms = 5'000;
   size_t get_account_cache_size() const {return _account_subjective_bill_cache.size();}
   void remove_subjective_billing( const chain::transaction_id_type& trx_id, uint32_t time_ordinal ) {
      auto& idx = _trx_cache_index.get<by_id>();
      auto itr = idx.find( trx_id );
      if( itr != idx.end() ) {
         remove_subjective_billing( *itr, time_ordinal );
         idx.erase( itr );
      }
   }

public:
   void set_subjective_account_cpu_allowed( fc::microseconds v ) { _subjective_account_cpu_allowed = v; }
   fc::microseconds get_subjective_account_cpu_allowed() const { return _subjective_account_cpu_allowed; }
   void set_disabled(bool disable) { _disabled = disable; }
   bool is_disabled() const { return _disabled; }
   void disable_payer_billing(bool disable) { _disabled_payer_billing = disable; }
   bool is_payer_billing_disabled() const { return _disabled_payer_billing; }
   void disable_account( chain::account_name a ) { _disabled_accounts.emplace( a ); }
   bool is_account_disabled(const account_name& a ) const { return _disabled || _disabled_accounts.contains( a ); }
   bool is_any_account_disabled(const action_payers_t& accounts ) const {
      if ( _disabled ) return true;
      return std::ranges::any_of(accounts, [&](const account_name& a) { return _disabled_accounts.contains( a ); });
   }

   void subjective_bill( const chain::transaction_id_type& id, fc::time_point_sec expire,
                         const accounts_billing_t& accounts_billing, const account_subjective_cpu_bill_t& auth_cpu)
   {
      if (_disabled) return;
      if (_trx_cache_index.contains(id)) return;
      account_subjective_cpu_bill_t account_subjective_cpu_bill;
      if (!_disabled_payer_billing) {
         for (const auto& [a, b] : accounts_billing) {
            if (!_disabled_accounts.contains(a)) {
               account_subjective_cpu_bill[a] = fc::microseconds(b.cpu_usage_us);
               _account_subjective_bill_cache[a].pending_cpu_us += b.cpu_usage_us;
            }
         }
      }
      for (const auto& [a, b] : auth_cpu) {
         if (!_disabled_accounts.contains(a)) {
            account_subjective_cpu_bill[a] = b;
            _account_subjective_bill_cache[a].pending_cpu_us += b.count();
         }
      }
      if (!account_subjective_cpu_bill.empty()) {
         _trx_cache_index.emplace(
            trx_cache_entry{id,
                            std::move(account_subjective_cpu_bill),
                            expire.to_time_point()});
      }
   }

   void subjective_bill_failure( const accounts_billing_t& accounts_billing, const account_subjective_cpu_bill_t& auth_cpu, const fc::time_point& now ) {
      if (_disabled) return;
      const auto time_ordinal = time_ordinal_for(now);
      if (!_disabled_payer_billing) {
         for (const auto& [a, b] : accounts_billing) {
            if (!_disabled_accounts.contains(a)) {
               _account_subjective_bill_cache[a].expired_accumulator.add(b.cpu_usage_us, time_ordinal, _expired_accumulator_average_window);
            }
         }
      }
      for (const auto& [a, b] : auth_cpu) {
         if (!_disabled_accounts.contains(a)) {
            _account_subjective_bill_cache[a].expired_accumulator.add(b.count(), time_ordinal, _expired_accumulator_average_window);
         }
      }
   }

   fc::microseconds get_subjective_bill( const account_name& a, const fc::time_point& now ) const {
      if( _disabled || _disabled_accounts.contains( a ) ) return fc::microseconds{0};
      auto aitr = _account_subjective_bill_cache.find( a );
      if( aitr != _account_subjective_bill_cache.end() ) {
         const subjective_billing_info& sub_bill_info = aitr->second;
         const auto time_ordinal = time_ordinal_for(now);
         int64_t sub_bill = sub_bill_info.pending_cpu_us + sub_bill_info.expired_accumulator.value_at(time_ordinal, _expired_accumulator_average_window );
         return fc::microseconds{sub_bill};
      }
      return fc::microseconds{0};
   }

   account_subjective_cpu_bill_t get_subjective_bill( const action_payers_t& payers, const fc::time_point& now ) const {
      account_subjective_cpu_bill_t result;
      if (_disabled ) return result;
      const auto time_ordinal = time_ordinal_for(now);
      for (const auto& payer : payers) {
         auto aitr = _account_subjective_bill_cache.find( payer );
         if( aitr != _account_subjective_bill_cache.end() ) {
            const subjective_billing_info& sub_bill_info = aitr->second;
            int64_t sub_bill = sub_bill_info.pending_cpu_us + sub_bill_info.expired_accumulator.value_at(time_ordinal, _expired_accumulator_average_window );
            result.insert({payer, fc::microseconds{sub_bill}});
         }
      }
      return result;
   }

   void on_block( fc::logger& log, const chain::signed_block_ptr& block, const fc::time_point& now ) {
      if( block == nullptr || _disabled ) return;
      const auto time_ordinal = time_ordinal_for(now);
      const auto orig_count = _account_subjective_bill_cache.size();
      remove_subjective_billing( block, time_ordinal );
      if (orig_count > 0) {
         fc_dlog( log, "Subjective billed accounts {} removed {}",
                  orig_count, orig_count - _account_subjective_bill_cache.size() );
      }
   }

   template <std::predicate<> Yield>
   std::pair<bool,uint32_t> remove_expired( fc::logger& log, const fc::time_point& pending_block_time, const fc::time_point& now, Yield&& yield ) {
      bool exhausted = false;
      uint32_t num_expired = 0;
      auto& idx = _trx_cache_index.get<by_expiry>();
      if( !idx.empty() ) {
         const auto time_ordinal = time_ordinal_for(now);
         const auto orig_count = _trx_cache_index.size();

         while( !idx.empty() ) {
            if( yield() ) {
               exhausted = true;
               break;
            }
            auto b = idx.begin();
            if( b->expiry > pending_block_time ) break;
            transition_to_expired( *b, time_ordinal );
            idx.erase( b );
            num_expired++;
         }

         fc_dlog( log, "Processed {} subjective billed transactions, Expired {}",
                  orig_count, num_expired );
      }
      return std::make_pair(!exhausted, num_expired);
   }

   uint32_t get_expired_accumulator_average_window() const {
      return _expired_accumulator_average_window;
   }

   void set_expired_accumulator_average_window( fc::microseconds subjective_account_decay_time ) {
      _expired_accumulator_average_window =
        subjective_account_decay_time.count() / 1000 / subjective_time_interval_ms;
   }

   void reset() { // for testing
      _reset();
   }
};

} //sysio::chain
