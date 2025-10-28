#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/transaction_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/deep_mind.hpp>

#include <chrono>
#include <bit>
#include <ranges>

namespace sysio::chain {

   static constexpr int64_t large_number_no_overflow = std::numeric_limits<int64_t>::max()/2;

   transaction_checktime_timer::transaction_checktime_timer(platform_timer& timer)
         : expired(timer.expired), _timer(timer) {
      expired = false;
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

   void transaction_context::init()
   {
      assert(!is_initialized);

      published = control.pending_block_time();

      // set maximum to a semi-valid deadline to allow for pause math and conversion to dates for logging
      const fc::time_point six_months = start + fc::hours(24*7*52);
      if( block_deadline == fc::time_point::maximum() )
         block_deadline = six_months; // half-year

      const auto& cfg = control.get_global_properties().configuration;
      auto& rl = control.get_mutable_resource_limits_manager();

      //
      // net (which is always objective) and objective cpu
      //
      trx_net_limit = rl.get_block_net_limit();

      if (is_read_only() && !control.is_write_window()) {
         // read_only trx do not have objective limits, however, objective_duration_limit used to limit run time
         objective_duration_limit = block_deadline - start;
      } else {
         objective_duration_limit = fc::microseconds( rl.get_block_cpu_limit() );
      }
      trx_deadline = start + objective_duration_limit;

      // Possibly lower net_limit to the maximum net usage a transaction is allowed to be billed
      if( cfg.max_transaction_net_usage <= trx_net_limit && !is_read_only() ) {
         trx_net_limit = cfg.max_transaction_net_usage;
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to the maximum cpu usage a transaction is allowed to be billed
      if( cfg.max_transaction_cpu_usage <= objective_duration_limit.count() && !is_read_only() ) {
         objective_duration_limit = fc::microseconds(cfg.max_transaction_cpu_usage);
         billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
         tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::on_chain_consensus_max_transaction_cpu_usage;
         trx_deadline = start + objective_duration_limit;
      }

      const transaction& trx = packed_trx.get_transaction();
      // Possibly lower net_limit to optional limit set in the transaction header
      uint64_t trx_specified_net_usage_limit = static_cast<uint64_t>(trx.max_net_usage_words.value) * 8;
      if( trx_specified_net_usage_limit > 0 && trx_specified_net_usage_limit <= trx_net_limit ) {
         trx_net_limit = trx_specified_net_usage_limit;
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to optional limit set in transaction header
      if( trx.max_cpu_usage_ms > 0 ) {
         auto trx_specified_cpu_usage_limit = fc::milliseconds(trx.max_cpu_usage_ms);
         if( trx_specified_cpu_usage_limit <= objective_duration_limit ) {
            objective_duration_limit = trx_specified_cpu_usage_limit;
            billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
            tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::user_specified_trx_max_cpu_usage_ms;
            trx_deadline = start + objective_duration_limit;
         }
      }

      leeway_trx_net_limit = trx_net_limit; // no leeway for block, cfg.max_transaction_net_usage, or trx.max_net_usage_words

      if ( !is_read_only() ) {
         if (explicit_billed_cpu_time) {
            SYS_ASSERT(billed_cpu_us.size() == trx.total_actions(), transaction_exception, "No transaction receipt cpu usage");
            trace->total_cpu_usage_us = std::ranges::fold_left(billed_cpu_us, 0l, std::plus());
            SYS_ASSERT(trace->total_cpu_usage_us > 0, transaction_exception, "Invalid transaction receipt cpu usage");
            validate_trx_billed_cpu();
         } else {
            billed_cpu_us.reserve(trx.total_actions());
         }
      }

      std::array all_actions = {std::views::all(trx.context_free_actions), std::views::all(trx.actions)};
      assert(std::ranges::distance(std::views::join(all_actions)) == trx.total_actions());
      for (const auto& [i, act] : std::views::enumerate(std::views::join(all_actions))) {
         // For each action, add either the explicit payer (if present) or the contract (if no payer)
         account_name a = act.payer();
         auto& b = accounts_billing[a];
         if (is_input) {
            uint64_t billable_size = packed_trx.get_action_billable_size(i);
            b.net_usage += billable_size;
            trace->net_usage += billable_size;
         }
         if (explicit_billed_cpu_time) {
            assert(!is_read_only());
            assert(billed_cpu_us.size() > i);
            b.cpu_usage_us += billed_cpu_us[i];
         }
      }
      check_trx_net_usage(); // Fail early if current net usage exceeds limit

      if ( !is_read_only() ) {
         validate_ram_usage.reserve(accounts_billing.size());
         // Update usage windows for all candidate accounts (user + contract)
         rl.update_account_usage( accounts_billing, block_timestamp_type(control.pending_block_time()).slot );

         // validate account net with objective net_usage_leeway
         for (auto& [account, bill] : accounts_billing) {
            verify_net_usage(account, bill.net_usage, cfg.net_usage_leeway);
            load_cpu_limit(account, bill);
         }
      }

      //
      // cpu
      //

      if (!explicit_billed_cpu_time) {
         // Possibly limit deadline to subjective max_transaction_time
         if( max_transaction_time_subjective != fc::microseconds::maximum() && (start + max_transaction_time_subjective) <= trx_deadline ) {
            trx_deadline = start + max_transaction_time_subjective;
            tx_cpu_usage_reason = !prev_accounts_billing.empty()
                                     ? tx_cpu_usage_exceeded_reason::speculative_executed_adjusted_max_transaction_time
                                     : tx_cpu_usage_exceeded_reason::node_configured_max_transaction_time;
            billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
         }
      }

      // Possibly limit deadline to caller provided wall clock block deadline
      if( block_deadline < trx_deadline ) {
         trx_deadline = block_deadline;
         billing_timer_exception_code = deadline_exception::code_value;
      }

      if ( !is_read_only() ) {
         if( !explicit_billed_cpu_time ) {
            // fail early for subjectively billed accounts
            validate_account_cpu_usage_estimate();
         }
      }

      // Explicit billed_cpu_time_us used
      if( explicit_billed_cpu_time ) {
         trx_deadline = block_deadline;
         deadline_exception_code = deadline_exception::code_value;
      } else {
         deadline_exception_code = billing_timer_exception_code;
      }

      if(control.skip_trx_checks()) {
         trx_deadline = block_deadline;
      }
      if (trx_deadline >= six_months) {
         enforce_deadline = false;
         active_deadline = fc::time_point::maximum();
         transaction_timer.start( active_deadline );
      }

      is_initialized = true;
   }

   void transaction_context::init_for_implicit_trx()
   {
      assert( packed_trx.get_transaction().delay_sec.value == 0 );
      assert( packed_trx.get_compression() == packed_transaction::compression_type::none );
      assert( !is_read_only() );

      init();
   }

   void transaction_context::init_for_input_trx()
   {
      const transaction& trx = packed_trx.get_transaction();
      // delayed and compressed transactions are not supported by wire
      SYS_ASSERT( trx.delay_sec.value == 0, transaction_exception, "transaction cannot be delayed" );
      SYS_ASSERT( packed_trx.get_compression() == packed_transaction::compression_type::none,
                  tx_compression_not_allowed, "packed transaction cannot be compressed");

      is_input = true;
      if (!control.skip_trx_checks()) {
         if ( !is_read_only() ) {
            control.validate_expiration(trx);
            control.validate_tapos(trx);
         }
         validate_referenced_accounts( trx, enforce_whiteblacklist && control.is_speculative_block() );
      }

      init();
      if ( !is_read_only() ) {
         record_transaction( id, trx.expiration );
      }
   }
   
   void transaction_context::exec() {
      assert( is_initialized );
      const transaction& trx = packed_trx.get_transaction();

      auto add_trace_net = [&]( size_t idx ) {
         if (!is_input) return;
         assert(trace->action_traces.size() == idx + 1);
         trace->action_traces[idx].net_usage = packed_trx.get_action_billable_size(idx);
      };

      size_t idx = 0;
      for( const auto& act : trx.context_free_actions ) {
         schedule_action( act, act.account, true, 0, 0 );
         add_trace_net(idx);
         ++idx;
      }

      for( const auto& act : trx.actions ) {
         schedule_action( act, act.account, false, 0, 0 );
         add_trace_net(idx);
         ++idx;
      }

      auto& action_traces = trace->action_traces;
      const uint32_t num_original_actions_to_execute = action_traces.size();
      assert( num_original_actions_to_execute == idx );
      for( uint32_t i = 1; i <= num_original_actions_to_execute; ++i ) {
         action_start = fc::time_point::now();
         const auto& act = action_traces[i-1].act;
         auto _ = fc::make_scoped_exit([this, org_code=billing_timer_exception_code, org_reason=tx_cpu_usage_reason](){
            billing_timer_exception_code = org_code;
            tx_cpu_usage_reason = org_reason;
         });
         account_name a = act.payer();
         auto& b = accounts_billing[a];
         active_deadline = trx_deadline;
         if (!explicit_billed_cpu_time) {
            int64_t account_cpu_limit = b.cpu_limit_us - subjective_cpu_bill[a].count() + leeway.count(); // Add leeway to allow powerup
            // Possibly limit deadline to account subjective cpu left
            if( action_start + fc::microseconds(account_cpu_limit) < trx_deadline ) {
               active_deadline = action_start + fc::microseconds(account_cpu_limit);
               billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
               tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
            }
         }
         if (enforce_deadline) {
            transaction_timer.start( active_deadline );
            checktime(); // Fail early if deadline already exceeded
         }
         try {
            execute_action( i, 0 );
         } catch ( ... ) {
            if (!explicit_billed_cpu_time) {
               auto billed_time = fc::time_point::now() - action_start;
               assert(billed_cpu_us.size() == i-1);
               billed_cpu_us.emplace_back( billed_time.count() );
            }
            throw;
         }
         if (enforce_deadline)
            transaction_timer.stop();
         auto billed_time = fc::time_point::now() - action_start;
         if (explicit_billed_cpu_time) {
            action_traces[i-1].cpu_usage_us = billed_cpu_us[i-1];
         } else {
            assert(billed_cpu_us.size() == i-1);
            billed_cpu_us.emplace_back( billed_time.count() ); // will be updated to include trx time in finalize
         }
      }
   }

   void transaction_context::finalize() {
      assert(is_initialized);

      // read-only transactions only need net_usage and elapsed in the trace
      if ( is_read_only() ) {
         trace->elapsed = fc::time_point::now() - start;
         trace->total_cpu_usage_us = trace->elapsed.count();
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

      leeway_trx_net_limit = trx_net_limit; // reset with no leeway==0
      constexpr uint32_t net_leeway = 0;
      for (auto& [account, bill]: accounts_billing) {
         verify_net_usage(account, bill.net_usage, net_leeway);
         load_cpu_limit(account, bill);
      }

      auto now = fc::time_point::now();
      trace->elapsed = now - start;

      // Update CPU time and validate CPU usage
      assert(billed_cpu_us.size() == packed_trx.get_transaction().total_actions());
      assert(trace->action_traces.size() >= billed_cpu_us.size());
      update_billed_cpu_time( now );

      validate_cpu_minimum();
      if (!explicit_billed_cpu_time) {
         // validated in init() when explicit_billed_cpu_time (validating trx in block)
         validate_trx_billed_cpu();

         for (const auto& [account, b] : accounts_billing) {
            validate_available_account_cpu( account, b.cpu_usage_us, b.cpu_limit_us, b.cpu_greylisted );
         }
      }

      rl.add_transaction_usage( accounts_billing, trace->total_cpu_usage_us, trace->net_usage,
                                block_timestamp_type(control.pending_block_time()).slot, is_transient() ); // Should never fail
   }



   void transaction_context::squash() {
      if (undo_session) undo_session->squash();
   }

   void transaction_context::undo() {
      if (undo_session) undo_session->undo();
   }

   void transaction_context::check_trx_net_usage()const {
      if (is_input && !control.skip_trx_checks()) {
         if( BOOST_UNLIKELY(trace->net_usage > trx_net_limit) ) {
            if ( net_limit_due_to_block ) {
               SYS_THROW( block_net_usage_exceeded,
                          "not enough space left in block: ${net_usage} > ${net_limit}",
                          ("net_usage", trace->net_usage)("net_limit", trx_net_limit) );
            } else {
               SYS_THROW( tx_net_usage_exceeded,
                          "transaction net usage is too high: ${net_usage} > ${net_limit}",
                          ("net_usage", trace->net_usage)("net_limit", trx_net_limit) );
            }
         }
      }
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
                     ("billing_timer", now - pseudo_start)("now", now)("deadline", trx_deadline)("start", start) );
      } else if( deadline_exception_code == block_cpu_usage_exceeded::code_value ) {
         SYS_THROW( block_cpu_usage_exceeded,
                     "not enough time left in block to complete executing transaction ${billing_timer}us",
                     ("now", now)("deadline", trx_deadline)("start", start)("billing_timer", now - pseudo_start) );
      } else if( deadline_exception_code == tx_cpu_usage_exceeded::code_value ) {
         std::string assert_msg = "transaction ${id} was executing for too long ${billing_timer}us";
         if (!subjective_cpu_bill.empty()) {
            assert_msg += " with a subjective cpu of (${subjective} us)";
         }
         fc::microseconds limit;
         assert_msg += get_tx_cpu_usage_exceeded_reason_msg(limit);
         if (cpu_limit_due_to_greylist) {
            assert_msg = "greylisted " + assert_msg;
            SYS_THROW( greylist_cpu_usage_exceeded, assert_msg, ("id", packed_trx.id())
                     ("billing_timer", now - pseudo_start)("subjective", subjective_cpu_bill)("limit", limit) );
         } else {
            SYS_THROW( tx_cpu_usage_exceeded, assert_msg, ("id", packed_trx.id())
                     ("billing_timer", now - pseudo_start)("subjective", subjective_cpu_bill)("limit", limit) );
         }
      }
      SYS_ASSERT( false,  transaction_exception, "unexpected deadline exception code ${code}", ("code", deadline_exception_code) );
   }

   void transaction_context::pause_billing_timer() {
      if( !enforce_deadline || explicit_billed_cpu_time ) return; // irrelevant
      assert(!paused_timer);

      paused_time = fc::time_point::now();
      paused_timer = true;
      transaction_timer.stop();
   }

   void transaction_context::resume_billing_timer() {
      if( !enforce_deadline || explicit_billed_cpu_time ) return; // irrelevant
      assert(paused_timer);

      auto now = fc::time_point::now();
      auto paused = now - paused_time;

      pseudo_start += paused;
      action_start += paused;
      active_deadline += paused;
      trx_deadline += paused;

      // do not allow to go past block wall clock deadline
      if( block_deadline < active_deadline ) {
         deadline_exception_code = deadline_exception::code_value;
         active_deadline = block_deadline;
      }
      if( block_deadline < trx_deadline ) {
         deadline_exception_code = deadline_exception::code_value;
         trx_deadline = block_deadline;
      }

      paused_timer = false;
      transaction_timer.start(active_deadline);
   }

   void transaction_context::validate_cpu_minimum()const {
      // validate minimum must be done at the end of the trx because the trx might have modified the cfg.min_transaction_cpu_usage
      if (control.skip_trx_checks())
         return;
      const auto& cfg = control.get_global_properties().configuration;
      SYS_ASSERT( trace->total_cpu_usage_us >= cfg.min_transaction_cpu_usage, transaction_exception,
                  "cannot bill CPU time ${b} less than the minimum of ${m} us",
                  ("b", trace->total_cpu_usage_us)("m", cfg.min_transaction_cpu_usage) );
   }

   void transaction_context::validate_trx_billed_cpu() const {
      assert(trace->total_cpu_usage_us > 0);
      if (control.skip_trx_checks())
         return;

      // validate objective cpu limits
      if( billing_timer_exception_code == block_cpu_usage_exceeded::code_value ) {
         SYS_ASSERT( trace->total_cpu_usage_us <= objective_duration_limit.count(),
                     block_cpu_usage_exceeded,
                     "billed CPU time (${billed} us) is greater than the billable CPU time left in the block (${billable} us)",
                     ("billed", trace->total_cpu_usage_us)( "billable", objective_duration_limit.count() )
         );
      } else {
         // exceeds trx.max_cpu_usage_ms or cfg.max_transaction_cpu_usage if objective_duration_limit is greater
         auto limit = objective_duration_limit.count();

         if( trace->total_cpu_usage_us > limit ) {
            fc::microseconds tx_limit;
            std::string assert_msg;
            assert_msg.reserve(1024);
            assert_msg += "billed CPU time (${billed} us) is greater than the maximum";
            assert_msg += " billable CPU time for the transaction (${billable} us)";
            assert_msg += get_tx_cpu_usage_exceeded_reason_msg( tx_limit );

            FC_THROW_EXCEPTION( tx_cpu_usage_exceeded, std::move(assert_msg),
                                ("billed", trace->total_cpu_usage_us)("billable", limit)("limit", tx_limit) );
         }
      }
   }

   void transaction_context::validate_account_cpu_usage_estimate()const {
      // verify account has cpu available with leeway and subjective billing
      if (control.skip_trx_checks())
         return;

      if (!prev_accounts_billing.empty() && billing_timer_exception_code == block_cpu_usage_exceeded::code_value) {
         int64_t total_cpu_us = [&]() {
            int64_t result = 0;
            for (const auto& [a, b] : prev_accounts_billing) {
               result += b.cpu_usage_us;
               if (auto i = subjective_cpu_bill.find(a); i != subjective_cpu_bill.end()) {
                  result += i->second.count();
               }
            }
            return result;
         }();
         SYS_ASSERT( total_cpu_us < objective_duration_limit.count(), block_cpu_usage_exceeded,
                     "estimated CPU time (${ec} us) is not less than the billable CPU time left in the block (${bb} us)",
                     ("ec", total_cpu_us)("bb", objective_duration_limit.count()) );
      }

      for (const auto& [a, b] : accounts_billing) {
         int64_t prev_cpu_usage_us = 0, subjective_cpu_usage_us = 0;
         if (auto i = prev_accounts_billing.find(a); i != prev_accounts_billing.end())
            prev_cpu_usage_us = i->second.cpu_usage_us;
         if (auto i = subjective_cpu_bill.find(a); i != subjective_cpu_bill.end())
            subjective_cpu_usage_us = i->second.count();
         int64_t validate_account_cpu_limit = b.cpu_limit_us - subjective_cpu_usage_us + leeway.count(); // Add leeway to allow powerup
         // Fail early if amount of the previous speculative execution is within 10% of remaining account cpu available
         if( validate_account_cpu_limit > 0 )
            validate_account_cpu_limit -= SYS_PERCENT( validate_account_cpu_limit, 10 * config::percent_1 );
         if( validate_account_cpu_limit < 0 )
            validate_account_cpu_limit = 0;

         if( prev_cpu_usage_us >= validate_account_cpu_limit ) { // if none available
            std::string assert_msg;
            assert_msg.reserve(1024);
            assert_msg += "estimated CPU time (${pb} us) is not less than the maximum";
            if (b.cpu_greylisted)
               assert_msg += " greylisted";
            assert_msg += " billable CPU time for the account ${a} (${b} us)";
            if (subjective_cpu_usage_us > 0)
               assert_msg += " with a subjective cpu of (${s} us)";
            assert_msg += " reached account cpu limit ${l}us";
            if( b.cpu_greylisted ) {
               FC_THROW_EXCEPTION( greylist_cpu_usage_exceeded, std::move(assert_msg),
                                   ("pb", prev_cpu_usage_us)("a", a)("b", validate_account_cpu_limit)
                                   ("s", subjective_cpu_usage_us)("l", validate_account_cpu_limit) );
            } else {
               FC_THROW_EXCEPTION( tx_cpu_usage_exceeded, std::move(assert_msg),
                                   ("pb", prev_cpu_usage_us)("a", a)("b", validate_account_cpu_limit)
                                   ("s", subjective_cpu_usage_us)("l", validate_account_cpu_limit) );
            }
         }
      }
   }

   void transaction_context::validate_available_account_cpu( account_name account, int64_t billed_us, int64_t account_limit, bool greylisted )const {
      // verify account has cpu available with leeway and subjective billing
      if (control.skip_trx_checks())
         return;

      if( billed_us > account_limit ) {
         std::string assert_msg;
         assert_msg.reserve(1024);
         assert_msg += "billed CPU time (${b} us) exceeded the maximum";
         if (greylisted)
            assert_msg += " greylisted";
         assert_msg += " billable CPU time for the account ${a} (${l} us)";

         if( greylisted ) {
            FC_THROW_EXCEPTION( greylist_cpu_usage_exceeded, std::move(assert_msg),
                                ("b", billed_us)("a", account)("l", account_limit) );
         } else {
            FC_THROW_EXCEPTION( tx_cpu_usage_exceeded, std::move(assert_msg),
                                ("b", billed_us)("a", account)("l", account_limit) );
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

   void transaction_context::update_billed_cpu_time( fc::time_point now ) {
      if( explicit_billed_cpu_time ) return; // updated in init() for explicit_billed_cpu

      trace->total_cpu_usage_us = std::ranges::fold_left(billed_cpu_us, 0l, std::plus());
      const auto& cfg = control.get_global_properties().configuration;
      int64_t total_cpu_time_us = std::max( (now - pseudo_start).count(), static_cast<int64_t>(cfg.min_transaction_cpu_usage) );
      SYS_ASSERT(total_cpu_time_us - trace->total_cpu_usage_us >= 0, tx_cpu_usage_exceeded, "Invalid CPU usage calculation");
      if (!billed_cpu_us.empty()) {
         // +1 so total is above min_transaction_cpu_usage
         int64_t delta_per_action = (( total_cpu_time_us - trace->total_cpu_usage_us ) / billed_cpu_us.size()) + 1;
         total_cpu_time_us = 0;
         for (auto&& [i, b] : std::views::enumerate(billed_cpu_us)) {
            // if exception thrown, action_traces may not be the same size as billed_cpu_us
            auto& act_trace = trace->action_traces[i];
            accounts_billing[act_trace.act.payer()].cpu_usage_us += delta_per_action;
            b.value += delta_per_action;
            total_cpu_time_us += b.value;
            act_trace.cpu_usage_us = b.value;
         }
      }
      trace->total_cpu_usage_us = total_cpu_time_us;
   }

   void transaction_context::verify_net_usage(account_name a, int64_t net_usage, uint32_t net_usage_leeway) {
      // Assumes rl.update_account_usage() was already called prior

      // Calculate the new highest network usage and CPU time that the billed account can afford to be billed
      const auto& rl = control.get_mutable_resource_limits_manager();
      int64_t account_net_limit = large_number_no_overflow;
      bool greylisted_net = false;

      uint32_t specified_greylist_limit = control.get_greylist_limit();
      uint32_t greylist_limit = config::maximum_elastic_resource_multiplier;
      if( control.is_speculative_block() ) {
         if( control.is_resource_greylisted(a) ) {
            greylist_limit = 1;
         } else {
            greylist_limit = specified_greylist_limit;
         }
      }
      auto [net_limit, net_was_greylisted] = rl.get_account_net_limit(a, greylist_limit);
      if( net_limit >= 0 ) {
         account_net_limit = net_limit;
         greylisted_net = net_was_greylisted;
      }

      SYS_ASSERT( control.is_speculative_block() || !greylisted_net,
                  transaction_exception, "greylisted when not producing block" );

      const auto leeway_net_limit = account_net_limit + net_usage_leeway;
      if (net_usage > leeway_net_limit) {
         if (greylisted_net) {
            SYS_THROW( greylist_net_usage_exceeded, "greylisted account ${a} net usage is too high: ${nu} > ${nl}",
                       ("a", a)("nu", net_usage)("nl", leeway_net_limit) );
         } else {
            SYS_THROW( tx_net_usage_exceeded, "account ${a} net usage is too high: ${nu} > ${nl}",
                       ("a", a)("nu", net_usage)("nl", leeway_net_limit) );
         }
      }
   }

   void transaction_context::load_cpu_limit(account_name a, account_billing& b) {
      // Assumes rl.update_account_usage() was already called prior

      // Calculate the new highest network usage and CPU time that the billed account can afford to be billed
      const auto& rl = control.get_mutable_resource_limits_manager();
      int64_t account_cpu_limit = large_number_no_overflow;
      bool greylisted_cpu = false;

      uint32_t specified_greylist_limit = control.get_greylist_limit();
      uint32_t greylist_limit = config::maximum_elastic_resource_multiplier;
      if( control.is_speculative_block() ) {
         if( control.is_resource_greylisted(a) ) {
            greylist_limit = 1;
         } else {
            greylist_limit = specified_greylist_limit;
         }
      }
      auto [cpu_limit, cpu_was_greylisted] = rl.get_account_cpu_limit(a, greylist_limit);
      if( cpu_limit >= 0 ) {
         account_cpu_limit = cpu_limit;
         greylisted_cpu = cpu_was_greylisted;
      }

      SYS_ASSERT( control.is_speculative_block() || !greylisted_cpu,
                  transaction_exception, "greylisted when not producing block" );

      b.cpu_limit_us = account_cpu_limit;
      b.cpu_greylisted |= greylisted_cpu;
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

      trace->action_traces.reserve( std::bit_ceil(new_action_ordinal) ); // bit_ceil to avoid vector copy on every reserve call.

      const action& provided_action = get_action_trace( action_ordinal ).act;

      // The reserve above is required so that the emplace_back below does not invalidate the provided_action reference,
      // which references an action within the `trace->action_traces` vector we are appending to.

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

} /// sysio::chain
