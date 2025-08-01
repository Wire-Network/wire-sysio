#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/generated_transaction_object.hpp>
#include <sysio/chain/transaction_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/deep_mind.hpp>

#include <chrono>

namespace sysio { namespace chain {

   transaction_checktime_timer::transaction_checktime_timer(platform_timer& timer)
         : expired(timer.expired), _timer(timer) {
      expired = 0;
   }

   void transaction_checktime_timer::start(fc::time_point tp) {
      _timer.start(tp);
   }

   void transaction_checktime_timer::stop() {
      _timer.stop();
   }

   void transaction_checktime_timer::set_expiration_callback(void(*func)(void*), void* user) {
      _timer.set_expiration_callback(func, user);
   }

   transaction_checktime_timer::~transaction_checktime_timer() {
      stop();
      _timer.set_expiration_callback(nullptr, nullptr);
   }

   transaction_context::transaction_context( controller& c,
                                             const packed_transaction& t,
                                             const transaction_id_type& trx_id,
                                             transaction_checktime_timer&& tmr,
                                             fc::time_point s,
                                             transaction_metadata::trx_type type)
   :control(c)
   ,packed_trx(t)
   ,id(trx_id)
   ,undo_session()
   ,trace(std::make_shared<transaction_trace>())
   ,start(s)
   ,transaction_timer(std::move(tmr))
   ,trx_type(type)
   ,net_usage(trace->net_usage)
   ,pseudo_start(s)
   {
      if (!c.skip_db_sessions() && !is_read_only()) {
         undo_session.emplace(c.mutable_db().start_undo_session(true));
      }
      trace->id = id;
      trace->block_num = c.head_block_num() + 1;
      trace->block_time = c.pending_block_time();
      trace->producer_block_id = c.pending_producer_block_id();

      if(auto dm_logger = c.get_deep_mind_logger(is_transient()))
      {
         dm_logger->on_start_transaction();
      }
   }

   transaction_context::~transaction_context()
   {
      if(auto dm_logger = control.get_deep_mind_logger(is_transient()))
      {
         dm_logger->on_end_transaction();
      }
   }

   void transaction_context::disallow_transaction_extensions( const char* error_msg )const {
      if( control.is_speculative_block() ) {
         SYS_THROW( subjective_block_production_exception, error_msg );
      } else {
         SYS_THROW( disallowed_transaction_extensions_bad_block_exception, error_msg );
      }
   }

   void transaction_context::init(uint64_t initial_net_usage)
   {
      SYS_ASSERT( !is_initialized, transaction_exception, "cannot initialize twice" );

      // set maximum to a semi-valid deadline to allow for pause math and conversion to dates for logging
      if( block_deadline == fc::time_point::maximum() )
         block_deadline = start + fc::hours(24*7*52);

      const auto& cfg = control.get_global_properties().configuration;
      auto& rl = control.get_mutable_resource_limits_manager();

      net_limit = rl.get_block_net_limit();

      objective_duration_limit = fc::microseconds( rl.get_block_cpu_limit() );
      _deadline = start + objective_duration_limit;

      // Possibly lower net_limit to the maximum net usage a transaction is allowed to be billed
      if( cfg.max_transaction_net_usage <= net_limit && !is_read_only() ) {
         net_limit = cfg.max_transaction_net_usage;
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to the maximum cpu usage a transaction is allowed to be billed
      if( cfg.max_transaction_cpu_usage <= objective_duration_limit.count() && !is_read_only() ) {
         objective_duration_limit = fc::microseconds(cfg.max_transaction_cpu_usage);
         billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
         tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::on_chain_consensus_max_transaction_cpu_usage;
         _deadline = start + objective_duration_limit;
      }

      const transaction& trx = packed_trx.get_transaction();

      // Possibly lower net_limit to optional limit set in the transaction header
      uint64_t trx_specified_net_usage_limit = static_cast<uint64_t>(trx.max_net_usage_words.value) * 8;
      if( trx_specified_net_usage_limit > 0 && trx_specified_net_usage_limit <= net_limit ) {
         net_limit = trx_specified_net_usage_limit;
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to optional limit set in transaction header
      if( trx.max_cpu_usage_ms > 0 ) {
         auto trx_specified_cpu_usage_limit = fc::milliseconds(trx.max_cpu_usage_ms);
         if( trx_specified_cpu_usage_limit <= objective_duration_limit ) {
            objective_duration_limit = trx_specified_cpu_usage_limit;
            billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
            tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::user_specified_trx_max_cpu_usage_ms;
            _deadline = start + objective_duration_limit;
         }
      }

      initial_objective_duration_limit = objective_duration_limit;
      int64_t account_net_limit = 0;
      int64_t account_cpu_limit = 0;

      if ( !is_read_only() ) {
         if( explicit_billed_cpu_time )
            validate_cpu_usage_to_bill( billed_cpu_time_us, std::numeric_limits<int64_t>::max(), false, subjective_cpu_bill_us); // Fail early if the amount to be billed is too high

         // For each action, add either the explicit payer (if present) or the contract (if no payer)
         for ( const auto &act : trx.actions ) {
            bill_to_accounts.insert(act.explicit_payer());
         }

         // ---------------------- NEW ADDITION FOR SYSIO.ROA BILLING ----------------------
         // Identify the contract account from the first action if possible
         account_name contract_account = trx.actions.empty() ? name() : trx.actions.front().account;

         // Only add contract_account if it's a valid (non-empty) name
         if (contract_account.good()) {
            bill_to_accounts.insert(contract_account);
         }

         validate_ram_usage.reserve(bill_to_accounts.size());
         // -------------------------------------------------------------------------

         // Update usage windows for all candidate accounts (user + contract)
         rl.update_account_usage( bill_to_accounts, block_timestamp_type(control.pending_block_time()).slot );

         // Calculate the highest network usage and CPU time that all of the billed accounts can afford to be billed
         bool greylisted_net = false, greylisted_cpu = false;
         std::tie( account_net_limit, account_cpu_limit, greylisted_net, greylisted_cpu) = max_bandwidth_billed_accounts_can_pay();
         net_limit_due_to_greylist |= greylisted_net;
         cpu_limit_due_to_greylist |= greylisted_cpu;
      }

      eager_net_limit = net_limit;

      if ( !is_read_only() ) {
         // Possibly lower eager_net_limit to what the billed accounts can pay plus some (objective) leeway
         auto new_eager_net_limit = std::min( eager_net_limit, static_cast<uint64_t>(account_net_limit + cfg.net_usage_leeway) );
         if( new_eager_net_limit < eager_net_limit ) {
            eager_net_limit = new_eager_net_limit;
            net_limit_due_to_block = false;
         }

         // Possibly limit deadline if the duration accounts can be billed for (+ a subjective leeway) does not exceed current delta
         if( (fc::microseconds(account_cpu_limit) + leeway) <= (_deadline - start) ) {
            _deadline = start + fc::microseconds(account_cpu_limit) + leeway;
            billing_timer_exception_code = leeway_deadline_exception::code_value;
         }
      }

      // Possibly limit deadline to subjective max_transaction_time
      if( max_transaction_time_subjective != fc::microseconds::maximum() && (start + max_transaction_time_subjective) <= _deadline ) {
         _deadline = start + max_transaction_time_subjective;
         tx_cpu_usage_reason = billed_cpu_time_us > 0 ?
            tx_cpu_usage_exceeded_reason::speculative_executed_adjusted_max_transaction_time : tx_cpu_usage_exceeded_reason::node_configured_max_transaction_time;
         billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
      }

      // Possibly limit deadline to caller provided wall clock block deadline
      if( block_deadline < _deadline ) {
         _deadline = block_deadline;
         billing_timer_exception_code = deadline_exception::code_value;
      }

      if ( !is_read_only() ) {
         if( !explicit_billed_cpu_time ) {
            int64_t validate_account_cpu_limit = account_cpu_limit - subjective_cpu_bill_us + leeway.count(); // Add leeway to allow powerup
            // Possibly limit deadline to account subjective cpu left
            if( subjective_cpu_bill_us > 0 && (start + fc::microseconds(validate_account_cpu_limit) < _deadline) ) {
               _deadline = start + fc::microseconds(validate_account_cpu_limit);
               billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
               tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
            }

            // Fail early if amount of the previous speculative execution is within 10% of remaining account cpu available
            if( validate_account_cpu_limit > 0 )
               validate_account_cpu_limit -= SYS_PERCENT( validate_account_cpu_limit, 10 * config::percent_1 );
            if( validate_account_cpu_limit < 0 ) validate_account_cpu_limit = 0;
            validate_account_cpu_usage_estimate( billed_cpu_time_us, validate_account_cpu_limit, subjective_cpu_bill_us );
         }
      }

      // Explicit billed_cpu_time_us used
      if( explicit_billed_cpu_time ) {
         _deadline = block_deadline;
         deadline_exception_code = deadline_exception::code_value;
      } else {
         deadline_exception_code = billing_timer_exception_code;
      }

      eager_net_limit = (eager_net_limit/8)*8; // Round down to nearest multiple of word size (8 bytes) so check_net_usage can be efficient

      if( initial_net_usage > 0 )
         add_net_usage( initial_net_usage );  // Fail early if current net usage exceeds limit

      if(control.skip_trx_checks()) {
         transaction_timer.start( fc::time_point::maximum() );
      } else {
         transaction_timer.start( _deadline );
         checktime(); // Fail early if deadline already exceeded
      }

      is_initialized = true;
   }

   void transaction_context::init_for_implicit_trx( uint64_t initial_net_usage  )
   {
      published = control.pending_block_time();
      init( initial_net_usage);
   }

   void transaction_context::init_for_input_trx( uint64_t packed_trx_unprunable_size,
                                                 uint64_t packed_trx_prunable_size )
   {
      const transaction& trx = packed_trx.get_transaction();
      // read-only and dry-run transactions are not allowed to be delayed at any time
      SYS_ASSERT( trx.delay_sec.value == 0, transaction_exception, "transaction cannot be delayed" );

      const auto& cfg = control.get_global_properties().configuration;

      uint64_t discounted_size_for_pruned_data = packed_trx_prunable_size;
      if( cfg.context_free_discount_net_usage_den > 0
          && cfg.context_free_discount_net_usage_num < cfg.context_free_discount_net_usage_den )
      {
         discounted_size_for_pruned_data *= cfg.context_free_discount_net_usage_num;
         discounted_size_for_pruned_data =  ( discounted_size_for_pruned_data + cfg.context_free_discount_net_usage_den - 1)
                                                                                    / cfg.context_free_discount_net_usage_den; // rounds up
      }

      uint64_t initial_net_usage = static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                                    + packed_trx_unprunable_size + discounted_size_for_pruned_data;

      if( trx.delay_sec.value > 0 ) {
          // If delayed, also charge ahead of time for the additional net usage needed to retire the delayed transaction
          // whether that be by successfully executing, soft failure, hard failure, or expiration.
         initial_net_usage += static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                               + static_cast<uint64_t>(config::transaction_id_net_usage);
      }

      published = control.pending_block_time();
      is_input = true;
      if (!control.skip_trx_checks()) {
         if ( !is_read_only() ) {
            control.validate_expiration(trx);
            control.validate_tapos(trx);
         }
         validate_referenced_accounts( trx, enforce_whiteblacklist && control.is_speculative_block() );
      }

      init( initial_net_usage );
      if ( !is_read_only() ) {
         record_transaction( id, trx.expiration );
      }
   }
   
   void transaction_context::init_for_deferred_trx( fc::time_point p )
   {
      const transaction& trx = packed_trx.get_transaction();
      if( (trx.expiration.sec_since_epoch() != 0) && (trx.transaction_extensions.size() > 0) ) {
         disallow_transaction_extensions( "no transaction extensions supported yet for deferred transactions" );
      }
      // If (trx.expiration.sec_since_epoch() == 0) then it was created after NO_DUPLICATE_DEFERRED_ID activation,
      // and so validation of its extensions was done either in:
      //   * apply_context::schedule_deferred_transaction for contract-generated transactions;
      //   * or transaction_context::init_for_input_trx for delayed input transactions.

      published = p;
      trace->scheduled = true;
      apply_context_free = false;
      init( 0 );
   }

   void transaction_context::exec() {
      SYS_ASSERT( is_initialized, transaction_exception, "must first initialize" );

      const transaction& trx = packed_trx.get_transaction();
      if( apply_context_free ) {
         for( const auto& act : trx.context_free_actions ) {
            schedule_action( act, act.account, true, 0, 0 );
         }
      }

      if( delay == fc::microseconds() ) {
         for( const auto& act : trx.actions ) {
            schedule_action( act, act.account, false, 0, 0 );
         }
      }

      auto& action_traces = trace->action_traces;
      uint32_t num_original_actions_to_execute = action_traces.size();
      for( uint32_t i = 1; i <= num_original_actions_to_execute; ++i ) {
         execute_action( i, 0 );
      }

      if( delay != fc::microseconds() ) {
         schedule_transaction();
      }
   }

   void transaction_context::finalize() {
      SYS_ASSERT(is_initialized, transaction_exception, "must first initialize");

      // read-only transactions only need net_usage and elapsed in the trace
      if ( is_read_only() ) {
         net_usage = ((net_usage + 7)/8)*8; // Round up to nearest multiple of word size (8 bytes)
         trace->elapsed = fc::time_point::now() - start;
         return;
      }

      if( is_input ) {
         const transaction& trx = packed_trx.get_transaction();
         auto& am = control.get_mutable_authorization_manager();
         for (const auto& act : trx.actions) {
            for (const auto& auth : act.authorization) {
               if (auth.permission != config::sysio_payer_name) {
                  am.update_permission_usage(am.get_permission(auth));
               }
            }
         }
      }

      auto& rl = control.get_mutable_resource_limits_manager();

      // Verify that all accounts that incurred RAM usage in this transaction have valid RAM usage
      for (auto a : validate_ram_usage) {
         rl.verify_account_ram_usage(a);
      }

      // Calculate limits based on what billed accounts can afford
      int64_t account_net_limit = 0;
      int64_t account_cpu_limit = 0;
      bool greylisted_net = false, greylisted_cpu = false;
      std::tie(account_net_limit, account_cpu_limit, greylisted_net, greylisted_cpu) = max_bandwidth_billed_accounts_can_pay();

      net_limit_due_to_greylist |= greylisted_net;
      cpu_limit_due_to_greylist |= greylisted_cpu;

      // Possibly lower net_limit based on what accounts can pay
      if (static_cast<uint64_t>(account_net_limit) <= net_limit) {
         net_limit = static_cast<uint64_t>(account_net_limit);
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit based on what accounts can pay
      if (account_cpu_limit <= objective_duration_limit.count()) {
         objective_duration_limit = fc::microseconds(account_cpu_limit);
         billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
         tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
      }

      // Round up net_usage to the nearest multiple of 8 bytes and verify
      net_usage = ((net_usage + 7)/8)*8;
      eager_net_limit = net_limit;
      check_net_usage();

      auto now = fc::time_point::now();
      trace->elapsed = now - start;

      // Update CPU time and validate CPU usage
      update_billed_cpu_time( now );

      validate_cpu_usage_to_bill( billed_cpu_time_us, account_cpu_limit, true, subjective_cpu_bill_us );

      rl.add_transaction_usage( bill_to_accounts, static_cast<uint64_t>(billed_cpu_time_us), net_usage,
                                block_timestamp_type(control.pending_block_time()).slot, is_transient() ); // Should never fail
   }



   void transaction_context::squash() {
      if (undo_session) undo_session->squash();
   }

   void transaction_context::undo() {
      if (undo_session) undo_session->undo();
   }

   void transaction_context::check_net_usage()const {
      // This pre-check was checking the wrong resource limits and, if needed, must be updated for ROA policy awareness.
      // Is needed to support caller-specified per-transaction limits, but old implementation caused too many issues.
   }

   std::string transaction_context::get_tx_cpu_usage_exceeded_reason_msg(fc::microseconds& limit) const {
      switch( tx_cpu_usage_reason ) {
         case tx_cpu_usage_exceeded_reason::account_cpu_limit:
            limit = objective_duration_limit;
            return " reached account cpu limit ${limit}us";
         case tx_cpu_usage_exceeded_reason::on_chain_consensus_max_transaction_cpu_usage:
            limit = objective_duration_limit;
            return " reached on chain max_transaction_cpu_usage ${limit}us";
         case tx_cpu_usage_exceeded_reason::user_specified_trx_max_cpu_usage_ms:
            limit = objective_duration_limit;
            return " reached trx specified max_cpu_usage_ms ${limit}us";
         case tx_cpu_usage_exceeded_reason::node_configured_max_transaction_time:
            limit = max_transaction_time_subjective;
            return " reached node configured max-transaction-time ${limit}us";
         case tx_cpu_usage_exceeded_reason::speculative_executed_adjusted_max_transaction_time:
            limit = max_transaction_time_subjective;
            return " reached speculative executed adjusted trx max time ${limit}us";
      }
      return "unknown tx_cpu_usage_exceeded";
   }

   void transaction_context::checktime()const {
      if(BOOST_LIKELY(transaction_timer.expired == false))
         return;

      auto now = fc::time_point::now();
      if( explicit_billed_cpu_time || deadline_exception_code == deadline_exception::code_value ) {
         SYS_THROW( deadline_exception, "deadline exceeded ${billing_timer}us",
                     ("billing_timer", now - pseudo_start)("now", now)("deadline", _deadline)("start", start) );
      } else if( deadline_exception_code == block_cpu_usage_exceeded::code_value ) {
         SYS_THROW( block_cpu_usage_exceeded,
                     "not enough time left in block to complete executing transaction ${billing_timer}us",
                     ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
      } else if( deadline_exception_code == tx_cpu_usage_exceeded::code_value ) {
         std::string assert_msg = "transaction ${id} was executing for too long ${billing_timer}us";
         if (subjective_cpu_bill_us > 0) {
            assert_msg += " with a subjective cpu of (${subjective} us)";
         }
         fc::microseconds limit;
         assert_msg += get_tx_cpu_usage_exceeded_reason_msg(limit);
         if (cpu_limit_due_to_greylist) {
            assert_msg = "greylisted " + assert_msg;
            SYS_THROW( greylist_cpu_usage_exceeded, assert_msg, ("id", packed_trx.id())
                     ("billing_timer", now - pseudo_start)("subjective", subjective_cpu_bill_us)("limit", limit) );
         } else {
            SYS_THROW( tx_cpu_usage_exceeded, assert_msg, ("id", packed_trx.id())
                     ("billing_timer", now - pseudo_start)("subjective", subjective_cpu_bill_us)("limit", limit) );
         }
      } else if( deadline_exception_code == leeway_deadline_exception::code_value ) {
         SYS_THROW( leeway_deadline_exception,
                     "the transaction was unable to complete by deadline, "
                     "but it is possible it could have succeeded if it were allowed to run to completion ${billing_timer}",
                     ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
      }
      SYS_ASSERT( false,  transaction_exception, "unexpected deadline exception code ${code}", ("code", deadline_exception_code) );
   }

   void transaction_context::pause_billing_timer() {
      if( explicit_billed_cpu_time || pseudo_start == fc::time_point() ) return; // either irrelevant or already paused

      paused_time = fc::time_point::now();
      billed_time = paused_time - pseudo_start;
      pseudo_start = fc::time_point();
      transaction_timer.stop();
   }

   void transaction_context::resume_billing_timer() {
      if( explicit_billed_cpu_time || pseudo_start != fc::time_point() ) return; // either irrelevant or already running

      auto now = fc::time_point::now();
      auto paused = now - paused_time;

      pseudo_start = now - billed_time;
      _deadline += paused;

      // do not allow to go past block wall clock deadline
      if( block_deadline < _deadline ) {
         deadline_exception_code = deadline_exception::code_value;
         _deadline = block_deadline;
      }

      transaction_timer.start(_deadline);
   }

   void transaction_context::validate_cpu_usage_to_bill( int64_t billed_us, int64_t account_cpu_limit, bool check_minimum, int64_t subjective_billed_us )const {
      // This pre-check was checking the wrong resource limits and, if needed, must be updated for ROA policy awareness.
   }

   void transaction_context::validate_account_cpu_usage( int64_t billed_us, int64_t account_cpu_limit, int64_t subjective_billed_us)const {
      // This pre-check was checking the wrong resource limits and, if needed, must be updated for ROA policy awareness.
   }

   void transaction_context::validate_account_cpu_usage_estimate( int64_t prev_billed_us, int64_t account_cpu_limit, int64_t subjective_billed_us )const {
      // prev_billed_us can be 0, but so can account_cpu_limit
      if( (prev_billed_us >= 0) && !control.skip_trx_checks() ) {
         const bool cpu_limited_by_account = (account_cpu_limit <= objective_duration_limit.count());

         if( !cpu_limited_by_account && (billing_timer_exception_code == block_cpu_usage_exceeded::code_value) ) {
            SYS_ASSERT( prev_billed_us < objective_duration_limit.count(),
                        block_cpu_usage_exceeded,
                        "estimated CPU time (${billed} us) is not less than the billable CPU time left in the block (${billable} us)",
                        ("billed", prev_billed_us)( "billable", objective_duration_limit.count() )
            );
         } else {
            auto graylisted = cpu_limit_due_to_greylist && cpu_limited_by_account;
            // exceeds trx.max_cpu_usage_ms or cfg.max_transaction_cpu_usage if objective_duration_limit is greater
            auto account_limit = graylisted ? account_cpu_limit : (cpu_limited_by_account ? account_cpu_limit : objective_duration_limit.count());

            if( prev_billed_us >= account_limit ) {
               std::string assert_msg;
               assert_msg.reserve(1024);
               assert_msg += "estimated CPU time (${billed} us) is not less than the maximum";
               assert_msg += graylisted ? " greylisted" : "";
               assert_msg += " billable CPU time for the transaction (${billable} us)";
               assert_msg += subjective_billed_us > 0 ? " with a subjective cpu of (${subjective} us)" : "";
               assert_msg += " reached account cpu limit ${limit}us";

               if( graylisted ) {
                  FC_THROW_EXCEPTION( greylist_cpu_usage_exceeded, std::move(assert_msg),
                                      ("billed", prev_billed_us)("billable", account_limit)("subjective", subjective_billed_us)("limit", account_limit) );
               } else {
                  FC_THROW_EXCEPTION( tx_cpu_usage_exceeded, std::move(assert_msg),
                                      ("billed", prev_billed_us)("billable", account_limit)("subjective", subjective_billed_us)("limit", account_limit) );
               }
            }
         }
      }
   }

   void transaction_context::add_ram_usage( account_name account, int64_t ram_delta ) {
      // wlog("Calling add_ram_usage with account: ${account}, ram_delta: ${ram_delta}",
      //      ("account", account)("ram_delta", ram_delta));
      auto& rl = control.get_mutable_resource_limits_manager();
      rl.add_pending_ram_usage( account, ram_delta );
      if( ram_delta > 0 ) {
         validate_ram_usage.insert( account );
      }
   }

   uint32_t transaction_context::update_billed_cpu_time( fc::time_point now ) {
      if( explicit_billed_cpu_time ) return static_cast<uint32_t>(billed_cpu_time_us);

      const auto& cfg = control.get_global_properties().configuration;
      billed_cpu_time_us = std::max( (now - pseudo_start).count(), static_cast<int64_t>(cfg.min_transaction_cpu_usage) );

      return static_cast<uint32_t>(billed_cpu_time_us);
   }

   std::tuple<int64_t, int64_t, bool, bool> transaction_context::max_bandwidth_billed_accounts_can_pay( bool force_elastic_limits ) const{
      // Assumes rl.update_account_usage( bill_to_accounts, block_timestamp_type(control.pending_block_time()).slot ) was already called prior

      // Calculate the new highest network usage and CPU time that all of the billed accounts can afford to be billed
      auto& rl = control.get_mutable_resource_limits_manager();
      const static int64_t large_number_no_overflow = std::numeric_limits<int64_t>::max()/2;
      int64_t account_net_limit = large_number_no_overflow;
      int64_t account_cpu_limit = large_number_no_overflow;
      bool greylisted_net = false;
      bool greylisted_cpu = false;

      uint32_t specified_greylist_limit = control.get_greylist_limit();
      for( const auto& a : bill_to_accounts ) {
         uint32_t greylist_limit = config::maximum_elastic_resource_multiplier;
         if( !force_elastic_limits && control.is_speculative_block() ) {
            if( control.is_resource_greylisted(a) ) {
               greylist_limit = 1;
            } else {
               greylist_limit = specified_greylist_limit;
            }
         }
         auto [net_limit, net_was_greylisted] = rl.get_account_net_limit(a, greylist_limit);
         if( net_limit >= 0 ) {
            account_net_limit = std::min( account_net_limit, net_limit );
            greylisted_net |= net_was_greylisted;
         }
         auto [cpu_limit, cpu_was_greylisted] = rl.get_account_cpu_limit(a, greylist_limit);
         if( cpu_limit >= 0 ) {
            account_cpu_limit = std::min( account_cpu_limit, cpu_limit );
            greylisted_cpu |= cpu_was_greylisted;
         }
      }

      SYS_ASSERT( (!force_elastic_limits && control.is_speculative_block()) || (!greylisted_cpu && !greylisted_net),
                  transaction_exception, "greylisted when not producing block" );

      return std::make_tuple(account_net_limit, account_cpu_limit, greylisted_net, greylisted_cpu);
   }

   action_trace& transaction_context::get_action_trace( uint32_t action_ordinal ) {
      SYS_ASSERT( 0 < action_ordinal && action_ordinal <= trace->action_traces.size() ,
                  transaction_exception,
                  "action_ordinal ${ordinal} is outside allowed range [1,${max}]",
                  ("ordinal", action_ordinal)("max", trace->action_traces.size())
      );
      return trace->action_traces[action_ordinal-1];
   }

   const action_trace& transaction_context::get_action_trace( uint32_t action_ordinal )const {
      SYS_ASSERT( 0 < action_ordinal && action_ordinal <= trace->action_traces.size() ,
                  transaction_exception,
                  "action_ordinal ${ordinal} is outside allowed range [1,${max}]",
                  ("ordinal", action_ordinal)("max", trace->action_traces.size())
      );
      return trace->action_traces[action_ordinal-1];
   }

   uint32_t transaction_context::schedule_action( const action& act, account_name receiver, bool context_free,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.emplace_back( *trace, act, receiver, context_free,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   uint32_t transaction_context::schedule_action( action&& act, account_name receiver, bool context_free,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.emplace_back( *trace, std::move(act), receiver, context_free,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   uint32_t transaction_context::schedule_action( uint32_t action_ordinal, account_name receiver, bool context_free,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.reserve( new_action_ordinal );

      const action& provided_action = get_action_trace( action_ordinal ).act;

      // The reserve above is required so that the emplace_back below does not invalidate the provided_action reference.

      trace->action_traces.emplace_back( *trace, provided_action, receiver, context_free,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   void transaction_context::execute_action( uint32_t action_ordinal, uint32_t recurse_depth ) {
      apply_context acontext( control, *this, action_ordinal, recurse_depth );

      if (recurse_depth == 0) {
         if (auto dm_logger = control.get_deep_mind_logger(is_transient())) {
            dm_logger->on_input_action();
         }
      }

      acontext.exec();
   }

   void transaction_context::schedule_transaction() {
      throw std::runtime_error("Scheduled transaction implementation has been removed");
   }

   void transaction_context::record_transaction( const transaction_id_type& id, fc::time_point_sec expire ) {
      try {
          control.mutable_db().create<transaction_object>([&](transaction_object& transaction) {
              transaction.trx_id = id;
              transaction.expiration = expire;
          });
      } catch( const boost::interprocess::bad_alloc& ) {
         throw;
      } catch ( ... ) {
          SYS_ASSERT( false, tx_duplicate,
                     "duplicate transaction ${id}", ("id", id ) );
      }
   } /// record_transaction

   void transaction_context::validate_referenced_accounts( const transaction& trx, bool enforce_actor_whitelist_blacklist )const {
      const auto& db = control.db();
      const auto& auth_manager = control.get_authorization_manager();

      if( !trx.context_free_actions.empty() && !control.skip_trx_checks() ) {
         for( const auto& a : trx.context_free_actions ) {
            auto* code = db.find<account_object, by_name>( a.account );
            SYS_ASSERT( code != nullptr, transaction_exception,
                        "action's code account '${account}' does not exist", ("account", a.account) );
            SYS_ASSERT( a.authorization.size() == 0, transaction_exception,
                        "context-free actions cannot have authorizations" );
         }
      }

      flat_set<account_name> actors;

      bool one_auth = false;
      for( const auto& a : trx.actions ) {
         auto* code = db.find<account_object, by_name>(a.account);
         SYS_ASSERT( code != nullptr, transaction_exception,
                     "action's code account '${account}' does not exist", ("account", a.account) );
         if ( is_read_only() ) {
            SYS_ASSERT( a.authorization.size() == 0, transaction_exception,
                       "read-only action '${name}' cannot have authorizations", ("name", a.name) );
         }
         name payer;
         for (const auto &auth: a.authorization) {
            if (auth.permission == config::sysio_payer_name) {
               SYS_ASSERT(payer.empty(), transaction_exception,
                          "action cannot have multiple payers");

               auto *actor = db.find<account_object, by_name>(auth.actor);
               SYS_ASSERT(actor != nullptr, transaction_exception,
                          "action's paying actor '${account}' does not exist", ("account", auth.actor));
               payer = auth.actor;
            } else {
               one_auth = true;
               auto *actor = db.find<account_object, by_name>(auth.actor);
               SYS_ASSERT(actor != nullptr, transaction_exception,
                          "action's authorizing actor '${account}' does not exist", ("account", auth.actor));
               SYS_ASSERT(auth_manager.find_permission(auth) != nullptr, transaction_exception,
                          "action's authorizations include a non-existent permission: ${permission}",
                          ("permission", auth));
               if (enforce_actor_whitelist_blacklist)
                  actors.insert(auth.actor);
            }
         }
         if (!payer.empty()) {
            bool authPayer = false;
            for (const auto &auth: a.authorization) {
               if (auth.permission == config::sysio_payer_name) {
                  continue;
               }
               if (auth.actor == payer) {
                  authPayer = true;
                  break;
               }
            }
            SYS_ASSERT(authPayer, transaction_exception,
                       "Payer '${account}' did not authorize this action", ("account", payer));
         }
      }
      SYS_ASSERT(one_auth || is_read_only(), tx_no_auths, "transaction must have at least one authorization" );

      if( enforce_actor_whitelist_blacklist ) {
         control.check_actor_list( actors );
      }
   }


} } /// sysio::chain
