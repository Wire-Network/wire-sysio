#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/block.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/config.hpp>

#include <fc/time.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <algorithm>
#include <concepts>
#include <unordered_map>
#include <unordered_set>

namespace sysio::chain {

class subjective_billing {
public:
   static constexpr uint32_t subjective_time_interval_ms = 5'000;

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

   static constexpr uint32_t default_expired_accumulator_average_window =
      chain::config::account_cpu_usage_average_window_ms / subjective_time_interval_ms;

   /// The account cache is swept for fully decayed entries this many times per decay window.
   static constexpr uint32_t prunes_per_decay_window = 4;

   bool                                      _disabled = false;
   bool                                      _disabled_payer_billing = false;
   bool                                      _disabled_sysio_authorizer_billing = false;
   fc::microseconds                          _subjective_account_cpu_allowed{config::default_subjective_cpu_us};
   trx_cache_index                           _trx_cache_index;
   account_subjective_bill_cache             _account_subjective_bill_cache;
   std::unordered_set<chain::account_name>    _disabled_accounts;
   /// Accounts exempted by chain state rather than by this node's configuration; see set_chain_exempt_accounts.
   std::unordered_set<chain::account_name>    _chain_exempt_accounts;
   uint32_t                                  _expired_accumulator_average_window = default_expired_accumulator_average_window;
   uint32_t                                  _next_prune_time_ordinal = 0;

private:
   /// True if `a` is never billed or limited subjectively, by this node's configuration or by chain state.
   bool is_exempt( const account_name& a ) const {
      return _disabled_accounts.contains( a ) || _chain_exempt_accounts.contains( a );
   }

   void _reset() { // for testing
      _disabled = false;
      _disabled_payer_billing = false;
      _disabled_sysio_authorizer_billing = false;
      _subjective_account_cpu_allowed = fc::microseconds{config::default_subjective_cpu_us};
      _trx_cache_index.clear();
      _account_subjective_bill_cache.clear();
      _disabled_accounts.clear();
      _chain_exempt_accounts.clear();
      _expired_accumulator_average_window = default_expired_accumulator_average_window;
      _next_prune_time_ordinal = 0;
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
            const auto bill = subjective_cpu_bill.count();
            SYS_ASSERT( bill >= 0 && aitr->second.pending_cpu_us >= static_cast<uint64_t>(bill),
                        chain::tx_resource_exhaustion,
                        "Logic error in subjective account billing {}", account );
            aitr->second.pending_cpu_us -= bill;
            if( aitr->second.empty(time_ordinal, _expired_accumulator_average_window) ) _account_subjective_bill_cache.erase( aitr );
         }
      }
   }

   void transition_to_expired( const trx_cache_entry& entry, uint32_t time_ordinal ) {
      for (const auto& [account, subjective_cpu_bill] : entry.account_subjective_cpu_bill) {
         auto aitr = _account_subjective_bill_cache.find( account );
         if( aitr != _account_subjective_bill_cache.end() ) {
            const auto bill = subjective_cpu_bill.count();
            SYS_ASSERT( bill >= 0 && aitr->second.pending_cpu_us >= static_cast<uint64_t>(bill),
                        chain::tx_resource_exhaustion,
                        "Logic error in subjective account billing {}", account );
            aitr->second.pending_cpu_us -= bill;
            aitr->second.expired_accumulator.add(bill, time_ordinal, _expired_accumulator_average_window);
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

   /**
    * Adds `units` charged at `ordinal`, so they decay from that time rather than from when this node learned of them.
    * The accumulator holds one timeline, so a charge older than its last update is first decayed to that update.
    */
   void add_dated_charge( decaying_accumulator& accumulator, uint64_t units, uint32_t ordinal ) const {
      const uint32_t window = _expired_accumulator_average_window;
      if( accumulator.last_ordinal > ordinal ) {
         const uint32_t age = accumulator.last_ordinal - ordinal;
         if( age >= window ) return;
         units = units * ( window - age ) / window;
      }
      accumulator.add( units, ordinal, window );
   }

   /**
    * Charges the billed CPU of each sysio.*-paid action in `block` to its authorizer, where it decays like a
    * failure. Reading it from the block counts every included transaction on every node, including ones this node
    * never executed and ones in blocks it produced. The charge is dated at the block's time, so a node applying a
    * block late, e.g. while syncing, holds the same charge from then on as a node that applied it live, and a block
    * older than the decay window costs one check. A transaction re-included after a fork switch is charged again.
    *
    * A receipt holds one entry per action, then one per contract billed for its own code (see
    * transaction_receipt_header::cpu_usage_us). Only the action entries are read: the others are paid by a contract
    * outside sysio.*, from its own CPU limit.
    * @return the number of actions charged
    */
   uint32_t bill_sysio_paid_authorizers( const chain::signed_block& block, uint32_t time_ordinal ) {
      const uint32_t block_time_ordinal = time_ordinal_for( block.timestamp.to_time_point() );
      const uint32_t block_age = time_ordinal > block_time_ordinal ? time_ordinal - block_time_ordinal : 0;
      if( block_age >= _expired_accumulator_average_window ) return 0;

      uint32_t charged = 0;
      for( const auto& receipt : block.transactions ) {
         const transaction& trx = receipt.trx.get_transaction();
         // Checked against the transaction when the block was applied, which also rejects context-free actions.
         assert( trx.context_free_actions.empty() && receipt.cpu_usage_us.size() >= trx.actions.size() );
         if( !trx.context_free_actions.empty() || receipt.cpu_usage_us.size() < trx.actions.size() ) continue;
         const account_name trx_first_authorizer = trx.first_authorizer();
         size_t action_index = 0;
         for( const auto& act : trx.actions ) {
            const uint64_t cpu_us = receipt.cpu_usage_us[action_index++].value;
            const account_name authorizer = billed_authorizer( act, trx_first_authorizer );
            if( !is_sysio_paid_authorizer( act.payer(), authorizer ) || is_exempt( authorizer ) )
               continue;
            add_dated_charge( _account_subjective_bill_cache[authorizer].expired_accumulator, cpu_us,
                              block_time_ordinal );
            ++charged;
         }
      }
      return charged;
   }

   /// Erases accounts with nothing pending and nothing left to decay, at most once per prune interval.
   void prune_decayed_accounts( uint32_t time_ordinal ) {
      if( time_ordinal < _next_prune_time_ordinal ) return;
      _next_prune_time_ordinal =
         time_ordinal + std::max<uint32_t>( 1, _expired_accumulator_average_window / prunes_per_decay_window );
      std::erase_if( _account_subjective_bill_cache, [&]( const auto& entry ) {
         return entry.second.empty( time_ordinal, _expired_accumulator_average_window );
      } );
   }

public: // public for tests
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
   /**
    * The account an action's CPU is subjectively billed to as its authorizer: the action's first authorizer, or, for
    * an action without authorization, `trx_first_authorizer`, the transaction's first authorizer. Callers look that
    * up once per transaction: transaction::first_authorizer() scans the actions.
    */
   static account_name billed_authorizer( const action& act, const account_name& trx_first_authorizer ) {
      const account_name first_authorizer = act.first_authorizer();
      return first_authorizer.empty() ? trx_first_authorizer : first_authorizer;
   }

   /**
    * True if an action's CPU stays charged to its authorizer after its transaction is in a block: the payer is a
    * sysio.* contract, whose CPU is unlimited, and the authorizer is someone else. An explicit sysio.payer makes the
    * authorizer the payer, which pays from its own objective CPU instead.
    */
   static bool is_sysio_paid_authorizer( const account_name& payer, const account_name& authorizer ) {
      return payer.prefix() == config::system_account_name && authorizer != payer;
   }

   void set_subjective_account_cpu_allowed( fc::microseconds v ) { _subjective_account_cpu_allowed = v; }
   fc::microseconds get_subjective_account_cpu_allowed() const { return _subjective_account_cpu_allowed; }
   void set_disabled(bool disable) { _disabled = disable; }
   bool is_disabled() const { return _disabled; }
   void disable_payer_billing(bool disable) { _disabled_payer_billing = disable; }
   bool is_payer_billing_disabled() const { return _disabled_payer_billing; }
   /// Stops on_block from charging the authorizers of sysio.*-paid actions in a block (see is_sysio_paid_authorizer).
   void disable_sysio_authorizer_billing(bool disable) { _disabled_sysio_authorizer_billing = disable; }
   bool is_sysio_authorizer_billing_disabled() const { return _disabled_sysio_authorizer_billing; }
   void disable_account( chain::account_name a ) { _disabled_accounts.emplace( a ); }
   /**
    * Replaces the accounts that chain state exempts from subjective billing, which are treated like the accounts
    * of disable_account. Call in the write window, as for every other change to this object.
    */
   void set_chain_exempt_accounts( std::unordered_set<chain::account_name> accounts ) {
      _chain_exempt_accounts = std::move( accounts );
   }
   /// The accounts of the last set_chain_exempt_accounts.
   const std::unordered_set<chain::account_name>& get_chain_exempt_accounts() const { return _chain_exempt_accounts; }
   bool is_account_disabled(const account_name& a ) const { return _disabled || is_exempt( a ); }
   bool is_any_account_disabled(const action_payers_t& accounts ) const {
      if ( _disabled ) return true;
      return std::ranges::any_of(accounts, [&](const account_name& a) { return is_exempt( a ); });
   }

   void subjective_bill( const chain::transaction_id_type& id, fc::time_point_sec expire,
                         const accounts_billing_t& accounts_billing, const account_subjective_cpu_bill_t& auth_cpu)
   {
      if (_disabled) return;
      if (_trx_cache_index.contains(id)) return;
      account_subjective_cpu_bill_t account_subjective_cpu_bill;
      if (!_disabled_payer_billing) {
         for (const auto& [a, b] : accounts_billing) {
            if (!is_exempt(a)) {
               account_subjective_cpu_bill[a] += fc::microseconds(b.cpu_usage_us);
               _account_subjective_bill_cache[a].pending_cpu_us += b.cpu_usage_us;
            }
         }
      }
      for (const auto& [a, b] : auth_cpu) {
         assert(b.count() >= 0);
         if (!is_exempt(a)) {
            account_subjective_cpu_bill[a] += b;
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
            if (!is_exempt(a)) {
               _account_subjective_bill_cache[a].expired_accumulator.add(b.cpu_usage_us, time_ordinal, _expired_accumulator_average_window);
            }
         }
      }
      for (const auto& [a, b] : auth_cpu) {
         assert(b.count() >= 0);
         if (!is_exempt(a)) {
            _account_subjective_bill_cache[a].expired_accumulator.add(b.count(), time_ordinal, _expired_accumulator_average_window);
         }
      }
   }

   fc::microseconds get_subjective_bill( const account_name& a, const fc::time_point& now ) const {
      if( _disabled || is_exempt( a ) ) return fc::microseconds{0};
      auto aitr = _account_subjective_bill_cache.find( a );
      if( aitr != _account_subjective_bill_cache.end() ) {
         const subjective_billing_info& sub_bill_info = aitr->second;
         const auto time_ordinal = time_ordinal_for(now);
         int64_t sub_bill = sub_bill_info.pending_cpu_us + sub_bill_info.expired_accumulator.value_at(time_ordinal, _expired_accumulator_average_window );
         return fc::microseconds{sub_bill};
      }
      return fc::microseconds{0};
   }

   /**
    * Applies an accepted block, produced here or received: drops the pending bill of each transaction it includes,
    * then charges the authorizers of its sysio.*-paid actions (bill_sysio_paid_authorizers), so that charge outlasts
    * inclusion. Also prunes accounts whose bill has fully decayed.
    */
   void on_block( fc::logger& log, const chain::signed_block_ptr& block, const fc::time_point& now ) {
      if( block == nullptr || _disabled ) return;
      const auto time_ordinal = time_ordinal_for(now);
      const auto orig_count = _account_subjective_bill_cache.size();
      remove_subjective_billing( block, time_ordinal );
      const auto removed = orig_count - _account_subjective_bill_cache.size();
      const uint32_t charged =
         _disabled_sysio_authorizer_billing ? 0 : bill_sysio_paid_authorizers( *block, time_ordinal );
      if (orig_count > 0 || charged > 0) {
         fc_dlog( log, "Subjective billed accounts {} removed {}, sysio-paid actions charged {}",
                  orig_count, removed, charged );
      }
      prune_decayed_accounts( time_ordinal );
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
