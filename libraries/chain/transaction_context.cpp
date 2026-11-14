#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/deep_mind.hpp>
#include <sysio/chain/subjective_billing.hpp>

#include <bit>
#include <ranges>

namespace sysio::chain {
   static constexpr int64_t large_number_no_overflow = std::numeric_limits<int64_t>::max()/2;

   transaction_checktime_timer::transaction_checktime_timer(platform_timer& timer)
         : _timer(timer) {
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
                                             transaction_checktime_timer&& tmr,
                                             fc::time_point s,
                                             transaction_metadata::trx_type type,
                                             const std::optional<fc::microseconds>& subjective_cpu_leeway,
                                             const fc::time_point& block_deadline,
                                             const fc::microseconds& max_transaction_time_subjective,
                                             bool explicit_billed_cpu_time,
                                             const accounts_billing_t& prev_accounts_billing,
                                             const cpu_usage_t& billed_cpu_us
                                             )
   :control(c)
   ,packed_trx(t)
   ,undo_session()
   ,trace(std::make_shared<transaction_trace>())
   ,start(s)
   ,executed_action_receipts()
   ,transaction_timer(std::move(tmr))
   ,trx_type(type)
   ,leeway(subjective_cpu_leeway.value_or(fc::microseconds(config::default_subjective_cpu_leeway_us)))
    // set maximum to a semi-valid deadline (six months) to allow for pause math and conversion to dates for logging
   ,enforce_deadline(block_deadline != fc::time_point::maximum())
   ,block_deadline(block_deadline == fc::time_point::maximum() ? s + fc::hours(24*7*26) : block_deadline)
   ,max_transaction_time_subjective(max_transaction_time_subjective)
   ,explicit_billed_cpu_time(explicit_billed_cpu_time)
   ,prev_accounts_billing(prev_accounts_billing)
   ,billed_cpu_us(billed_cpu_us)
   ,pseudo_start(s)
   {
      initialize();

      if(auto dm_logger = control.get_deep_mind_logger(is_transient())) {
         dm_logger->on_start_transaction();
      }
   }

   void transaction_context::reset() {
      undo();
      auto net_usage = trace->net_usage; // doesn't change during execution
      *trace = transaction_trace{}; // reset trace
      trace->net_usage = net_usage;
      initialize();
      // reset() runs only on the sys-vm-oc tier-up interrupt retry, which re-runs exec() but NOT
      // init_for_input_trx. The undo() above reverted the dedup record made in init_for_input_trx,
      // so it must be re-issued here under the freshly-initialized undo session -- otherwise an
      // interrupting validator silently drops this transaction from its dedup set (diverging the
      // integrity hash from non-interrupting nodes, and letting a still-unexpired resubmission
      // double-execute). Gated exactly as the original record: input and non-transient.
      if (is_input && !is_transient())
         record_transaction(packed_trx.id(), packed_trx.get_transaction().expiration);
      if (!explicit_billed_cpu_time)
         billed_cpu_us.clear();
      trx_blk_context = trx_block_context{};
      transaction_timer.stop();
      if (paused_timer) {
         resume_billing_timer();
      } else {
         transaction_timer.start(active_deadline);
      }

      executed_action_receipts = action_digests_t{};
      // accounts_billing should only be updated in init(), not updated during transaction execution. Receiver
      // entries are billed after execution, so the retry starts them over.
      validate_ram_usage.clear();
      frames.clear();
      receiver_entries.clear();
   }

   void transaction_context::initialize() {
      if (!control.skip_db_sessions() && !is_read_only()) {
         undo_session.emplace(control.mutable_db());
      }

      trace->id = packed_trx.id();
      trace->block_num = control.head().block_num() + 1;
      trace->block_time = control.pending_block_time();
      trace->producer_block_id = control.pending_producer_block_id();

      const transaction& trx = packed_trx.get_transaction();
      if (explicit_billed_cpu_time) {
         // One entry per input action, then one per frame whose receiver pays for its own code. Those are known only
         // after execution (bill_explicit_receiver_entries).
         SYS_ASSERT(billed_cpu_us.size() >= trx.total_actions(), transaction_exception,
                    "Transaction receipt cpu usage count ({}) is less than the action count ({})",
                    billed_cpu_us.size(), trx.total_actions());
         trace->total_cpu_usage_us = billed_cpu_sum();
      } else {
         billed_cpu_us.reserve(trx.total_actions());
      }
   }

   transaction_context::~transaction_context()
   {
      if(auto dm_logger = control.get_deep_mind_logger(is_transient()))
      {
         dm_logger->on_end_transaction();
      }
   }

   bool transaction_context::has_undo() const {
      return !control.skip_db_sessions()
             && !is_read_only()
             && control.get_deep_mind_logger(is_transient()) == nullptr;
   }

   void transaction_context::init()
   {
      assert(!is_initialized);

      published = control.pending_block_time();

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

      if ( !is_read_only() && explicit_billed_cpu_time ) {
         validate_trx_billed_cpu();
      }

      // init_for_input_trx rejects context-free actions, so every billed action is a regular one.
      assert(trx.context_free_actions.empty());
      size_t action_index = 0;
      for (const auto& act : trx.actions) {
         // For each action, add either the explicit payer (if present) or the contract (if no payer)
         account_name a = act.payer();
         auto& b = accounts_billing[a];
         if (is_input) {
            uint64_t billable_size = packed_trx.get_action_billable_size(action_index);
            b.net_usage += billable_size;
            trace->net_usage += billable_size;
         }
         if (explicit_billed_cpu_time) {
            assert(!is_read_only());
            assert(billed_cpu_us.size() > action_index);
            b.cpu_usage_us += billed_cpu_us[action_index];
         }
         ++action_index;
      }
      check_trx_net_usage(); // Fail early if current net usage exceeds limit

      if ( !is_read_only() ) {
         validate_ram_usage.reserve(accounts_billing.size());
         // Update usage windows for all candidate accounts (user + contract)
         rl.update_account_usage( accounts_billing, block_timestamp_type(control.pending_block_time()).slot );

         // validate account net with objective net_usage_leeway
         for (auto& [account, bill] : accounts_billing) {
            verify_net_usage(account, bill.net_usage, cfg.net_usage_leeway);
            if (!explicit_billed_cpu_time) {
               std::tie(bill.cpu_limit_us, bill.cpu_greylisted, std::ignore) = get_cpu_limit(account);
            }
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

      trx_cpu_usage_reason = tx_cpu_usage_reason;
      active_deadline = enforce_deadline ? trx_deadline : fc::time_point::maximum();
      transaction_timer.start( active_deadline );
      checktime(); // Fail early if deadline as already been exceeded

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
      // delayed, compressed, extension-bearing and context-free transactions are not supported by wire
      SYS_ASSERT( trx.delay_sec.value == 0, transaction_exception, "transaction cannot be delayed" );
      SYS_ASSERT( packed_trx.get_compression() == packed_transaction::compression_type::none,
                  tx_compression_not_allowed, "packed transaction cannot be compressed");
      SYS_ASSERT( trx.transaction_extensions.empty(), invalid_transaction_extension,
                  "transaction extensions are not currently supported" );
      // packed_transaction::init() requires one context-free data entry per context-free action, so this leaves the
      // transaction no context-free data either.
      SYS_ASSERT( trx.context_free_actions.empty(), transaction_exception,
                  "context-free actions are not supported" );
      SYS_ASSERT( std::ranges::none_of( trx.actions, is_onblock_action ), action_validate_exception,
                  "{}::{} runs only as the implicit transaction at the start of a block",
                  config::system_account_name, config::onblock_action_name );

      is_input = true;
      if (!control.skip_trx_checks()) {
         if ( !is_read_only() ) {
            verify_init_subjective_billing();
            control.validate_expiration(trx);
            control.validate_tapos(trx);
         }
         validate_referenced_accounts( trx, enforce_whiteblacklist && control.is_speculative_block() );
      }

      init();
      // Record unconditionally for every applied non-transient transaction. The dedup set is
      // folded into the integrity hash and snapshots, so gating the record on any node-local
      // observation (e.g. the previously used pending LIB time, which differs between a replaying
      // node and a live one) makes otherwise-identical nodes diverge on auxiliary state.
      // Expired entries are pruned deterministically by clear_expired at the next block start.
      // Transient transactions (read-only, dry-run) never persist in a block, so they must not
      // leave dedup entries -- and read-only transactions additionally execute concurrently on
      // other threads, where mutating shared dedup state would be a data race. This matches
      // Spring, where record_transaction is gated on !is_transient(); it also means a dry-run of
      // an already-known transaction executes instead of failing tx_duplicate.
      if ( !is_transient() ) {
         record_transaction( packed_trx.id(), trx.expiration );
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

      trace->action_traces.reserve(trx.actions.size());

      for (int oc_retry = 0; oc_retry < 2; ++oc_retry) { // interrupt_oc_exception can only happen once
         try {
            size_t idx = 0;

            for (const auto& act : trx.actions) {
               schedule_action(act, act.account, 0, 0);
               add_trace_net(idx);
               ++idx;
            }

            auto& action_traces = trace->action_traces;
            const uint32_t num_original_actions_to_execute = action_traces.size();
            assert(num_original_actions_to_execute == idx);
            for (uint32_t i = 1; i <= num_original_actions_to_execute; ++i) {
               action_start = fc::time_point::now();
               const auto& act = action_traces[i - 1].act;
               auto _ = fc::make_scoped_exit(
                  [this, org_code=billing_timer_exception_code, org_reason=tx_cpu_usage_reason]() {
                     billing_timer_exception_code = org_code;
                     tx_cpu_usage_reason = org_reason;
                  });
               const account_name payer = act.payer();
               const auto& b = accounts_billing[payer];
               set_billing_deadline(payer, b.cpu_limit_us, b.cpu_greylisted, action_start);
               if (enforce_deadline)
                  checktime(); // Fail early if deadline already exceeded
               try {
                  execute_action(i, 0);
               } catch (const fc::exception& e) {
                  if (!explicit_billed_cpu_time && e.code() != interrupt_oc_exception::code_value) {
                     // clamp to 0 due to possible clock skew
                     auto billed_time = std::max(fc::time_point::now() - action_start, fc::microseconds(0));
                     assert(billed_cpu_us.size() == i-1);
                     billed_cpu_us.emplace_back(billed_time.count());
                  }
                  throw;
               }
               if (enforce_deadline)
                  transaction_timer.stop();
               // clamp to 0 due to possible clock skew
               auto billed_time = std::max(fc::time_point::now() - action_start, fc::microseconds(0));
               if (explicit_billed_cpu_time) {
                  action_traces[i - 1].cpu_usage_us = billed_cpu_us[i - 1];
               } else {
                  assert(billed_cpu_us.size() == i-1);
                  billed_cpu_us.emplace_back(billed_time.count()); // will be updated to include trx time in finalize
               }
            }

            break; // only loop on interrupt_oc_exception
         } catch(const fc::exception& e) {
            if (oc_retry == 0 && e.code() == interrupt_oc_exception::code_value) {
               reset();
               continue;
            }
            throw;
         }
      }

      if (explicit_billed_cpu_time)
         bill_explicit_receiver_entries();
   }

   void transaction_context::finalize() {
      assert(is_initialized);

      // read-only transactions only need net_usage and elapsed in the trace
      if ( is_read_only() ) {
         auto now = fc::time_point::now();
         trace->elapsed = std::max(now - start, fc::microseconds(0));
         update_billed_cpu_time(now);
         return;
      }

      auto& rl = control.get_mutable_resource_limits_manager();

      // Verify that all accounts that incurred RAM usage in this transaction have valid RAM usage
      for (auto a : validate_ram_usage) {
         rl.verify_account_ram_usage(a);
      }

      constexpr uint32_t net_leeway = 0;
      for (auto& [account, bill]: accounts_billing) {
         if (!bill.pays_net)
            continue;
         verify_net_usage(account, bill.net_usage, net_leeway);
         if (!explicit_billed_cpu_time) {
            std::tie(bill.cpu_limit_us, bill.cpu_greylisted, std::ignore) = get_cpu_limit(account);
         }
      }

      auto now = fc::time_point::now();
      trace->elapsed = std::max(now - start, fc::microseconds(0));

      // Update CPU time and validate CPU usage
      assert(billed_cpu_us.size() ==
             packed_trx.get_transaction().total_actions() + (explicit_billed_cpu_time ? receiver_entries.size() : 0));
      assert(trace->action_traces.size() >= billed_cpu_us.size());

      // The checks below can still fail the transaction, so its subjective bill waits for them. A check that fails
      // undoes the CPU billing, and the controller then bills the transaction as a failed one.
      cpu_usage_t                   measured_cpu_us;
      accounts_billing_t            unbilled;
      account_subjective_cpu_bill_t authorizers_cpu;
      if (!explicit_billed_cpu_time) {
         measured_cpu_us = billed_cpu_us;
         unbilled        = accounts_billing;
      }
      try {
         if (!explicit_billed_cpu_time) {
            authorizers_cpu = bill_cpu( now );
            // A contract billed for its own code is checked against its limit as the transaction leaves it, which
            // counts CPU the transaction granted it. Its usage window was not updated in this block.
            for (auto& [account, bill] : accounts_billing) {
               if (!bill.pays_net) {
                  std::tie(bill.cpu_limit_us, bill.cpu_greylisted, std::ignore) =
                     get_cpu_limit(account, block_timestamp_type(control.pending_block_time()));
               }
            }
         }

         validate_cpu_minimum();
         if (!explicit_billed_cpu_time) {
            // validated in init() when explicit_billed_cpu_time (validating trx in block)
            validate_trx_billed_cpu();

            for (const auto& [account, b] : accounts_billing) {
               validate_available_account_cpu( account, b.cpu_usage_us, b.cpu_limit_us, b.cpu_greylisted );
            }
         }

         // Should never fail
         rl.add_transaction_usage( accounts_billing, trace->total_cpu_usage_us, trace->net_usage,
                                   block_timestamp_type(control.pending_block_time()).slot, is_transient() );
      } catch (...) {
         if (!explicit_billed_cpu_time)
            unbill_cpu( std::move(measured_cpu_us), std::move(unbilled) );
         throw;
      }

      if (!explicit_billed_cpu_time)
         bill_subjectively( authorizers_cpu );
   }



   void transaction_context::squash() {
      if (undo_session)
         undo_session->squash();
      control.apply_trx_block_context(trx_blk_context);
      transaction_timer.stop();
   }

   void transaction_context::undo() {
      if (undo_session)
         undo_session->undo();
      transaction_timer.stop();
   }

   void transaction_context::check_trx_net_usage()const {
      if (is_input && !control.skip_trx_checks()) {
         if( BOOST_UNLIKELY(trace->net_usage > trx_net_limit) ) {
            if ( net_limit_due_to_block ) {
               SYS_THROW( block_net_usage_exceeded, "not enough space left in block: {} > {}", trace->net_usage, trx_net_limit );
            } else {
               SYS_THROW( tx_net_usage_exceeded, "transaction net usage is too high: {} > {}", trace->net_usage, trx_net_limit );
            }
         }
      }
   }

   std::string transaction_context::get_tx_cpu_usage_exceeded_reason_msg(fc::microseconds& limit) const {
      switch( tx_cpu_usage_reason ) {
         case tx_cpu_usage_exceeded_reason::account_cpu_limit:
            limit = objective_duration_limit;
            return " reached account cpu limit {}us";
         case tx_cpu_usage_exceeded_reason::on_chain_consensus_max_transaction_cpu_usage:
            limit = objective_duration_limit;
            return " reached on chain max_transaction_cpu_usage {}us";
         case tx_cpu_usage_exceeded_reason::user_specified_trx_max_cpu_usage_ms:
            limit = objective_duration_limit;
            return " reached trx specified max_cpu_usage_ms {}us";
         case tx_cpu_usage_exceeded_reason::node_configured_max_transaction_time:
            limit = max_transaction_time_subjective;
            return " reached node configured max-transaction-time {}us";
         case tx_cpu_usage_exceeded_reason::speculative_executed_adjusted_max_transaction_time:
            limit = max_transaction_time_subjective;
            return " reached speculative executed adjusted trx max time {}us";
      }
      return "unknown tx_cpu_usage_exceeded {}us";
   }

   void transaction_context::checktime()const {
      platform_timer::state_t expired = transaction_timer.timer_state();
      if(BOOST_LIKELY(expired == platform_timer::state_t::running))
         return;

      auto now = fc::time_point::now();
      if (expired == platform_timer::state_t::interrupted) {
         SYS_THROW( interrupt_exception, "interrupt signaled, ran {}us, start {}", now - pseudo_start, start );
      } else if( explicit_billed_cpu_time || deadline_exception_code == deadline_exception::code_value ) {
         SYS_THROW( deadline_exception, "deadline {} exceeded {}us, start {}",
                    active_deadline, now - pseudo_start, start );
      } else if( deadline_exception_code == block_cpu_usage_exceeded::code_value ) {
         SYS_THROW( block_cpu_usage_exceeded,
                     "not enough time left in block {} to complete executing transaction {}us, start {}",
                     active_deadline, now - pseudo_start, start );
      } else if( deadline_exception_code == tx_cpu_usage_exceeded::code_value ) {
         std::string assert_msg = "transaction {} was executing for too long {}us";
         assert_msg += " with a subjective cpu of ({} us)";
         fc::microseconds limit;
         assert_msg += get_tx_cpu_usage_exceeded_reason_msg(limit);
         if (cpu_limit_due_to_greylist) {
            assert_msg = "greylisted " + assert_msg;
            SYS_THROW_RUNTIME_FORMAT( greylist_cpu_usage_exceeded, assert_msg, packed_trx.id(), now - pseudo_start, subjective_cpu_bill, limit );
         } else {
            SYS_THROW_RUNTIME_FORMAT( tx_cpu_usage_exceeded, assert_msg, packed_trx.id(), now - pseudo_start, subjective_cpu_bill, limit );
         }
      }
      SYS_ASSERT( false,  transaction_exception, "unexpected deadline exception code {}", deadline_exception_code );
   }

   void transaction_context::pause_billing_timer() {
      if( !enforce_deadline || explicit_billed_cpu_time ) return; // irrelevant
      assert(!paused_timer);

      paused_time = fc::time_point::now();
      paused_timer = true;
      transaction_timer.stop();
   }

   void transaction_context::resume_billing_timer(fc::time_point resume_from) {
      if( !enforce_deadline || explicit_billed_cpu_time ) return; // irrelevant
      if (resume_from != fc::time_point()) {
         paused_time = resume_from;
      } else {
         assert(paused_timer);
      }

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
                  "cannot bill CPU time {} less than the minimum of {} us",
                  trace->total_cpu_usage_us, cfg.min_transaction_cpu_usage );
   }

   void transaction_context::validate_trx_billed_cpu() const {
      if (control.skip_trx_checks())
         return;

      // validate objective cpu limits
      if( billing_timer_exception_code == block_cpu_usage_exceeded::code_value ) {
         SYS_ASSERT( trace->total_cpu_usage_us <= objective_duration_limit.count(),
                     block_cpu_usage_exceeded,
                     "billed CPU time ({} us) is greater than the billable CPU time left in the block ({} us)",
                     trace->total_cpu_usage_us, objective_duration_limit.count()
         );
      } else {
         // exceeds trx.max_cpu_usage_ms or cfg.max_transaction_cpu_usage if objective_duration_limit is greater
         auto limit = objective_duration_limit.count();

         if( trace->total_cpu_usage_us > limit ) {
            fc::microseconds tx_limit;
            std::string assert_msg;
            assert_msg.reserve(256);
            assert_msg += "billed CPU time ({} us) is greater than the maximum";
            assert_msg += " billable CPU time for the transaction ({} us)";
            assert_msg += get_tx_cpu_usage_exceeded_reason_msg( tx_limit );

            SYS_THROW_RUNTIME_FORMAT( tx_cpu_usage_exceeded, std::move(assert_msg),
                                      trace->total_cpu_usage_us, limit, tx_limit );
         }
      }
   }

   void transaction_context::validate_account_cpu_usage_estimate()const {
      // verify account has cpu available with leeway and subjective billing
      if (control.skip_trx_checks())
         return;

      const auto pending_block_time = control.pending_block_time();
      if (!prev_accounts_billing.empty() && billing_timer_exception_code == block_cpu_usage_exceeded::code_value) {
         int64_t total_cpu_us = [&]() {
            int64_t result = 0;
            auto& subjective_billing = control.get_subjective_billing();
            for (const auto& [a, b] : prev_accounts_billing) {
               result += b.cpu_usage_us;
               result += subjective_billing.get_subjective_bill(a, pending_block_time).count();
            }
            return result;
         }();
         SYS_ASSERT( total_cpu_us < objective_duration_limit.count(), block_cpu_usage_exceeded,
                     "estimated CPU time ({} us) is not less than the billable CPU time left in the block ({} us)",
                     total_cpu_us, objective_duration_limit.count() );
      }

      auto& subjective_billing = control.get_subjective_billing();
      for (const auto& [a, b] : accounts_billing) {
         int64_t prev_cpu_usage_us = 0;
         if (auto i = prev_accounts_billing.find(a); i != prev_accounts_billing.end())
            prev_cpu_usage_us = i->second.cpu_usage_us;
         int64_t subjective_cpu_usage_us = subjective_billing.get_subjective_bill(a, pending_block_time).count();
         int64_t validate_account_cpu_limit = b.cpu_limit_us - subjective_cpu_usage_us + leeway.count(); // Add leeway to allow powerup
         // Fail early if amount of the previous speculative execution is within 10% of remaining account cpu available
         if( validate_account_cpu_limit > 0 )
            validate_account_cpu_limit -= SYS_PERCENT( validate_account_cpu_limit, 10 * config::percent_1 );
         if( validate_account_cpu_limit < 0 )
            validate_account_cpu_limit = 0;

         if( prev_cpu_usage_us >= validate_account_cpu_limit ) { // if none available
            std::string assert_msg;
            assert_msg.reserve(1024);
            assert_msg += "estimated CPU time ({} us) is not less than the maximum";
            if (b.cpu_greylisted)
               assert_msg += " greylisted";
            assert_msg += " billable CPU time for the account {} ({} us)";
            assert_msg += " with a subjective cpu of ({} us)";
            assert_msg += " reached account cpu limit {} us";
            if( b.cpu_greylisted ) {
               SYS_THROW_RUNTIME_FORMAT( greylist_cpu_usage_exceeded, std::move(assert_msg),
                                         prev_cpu_usage_us, a, validate_account_cpu_limit,
                                         subjective_cpu_usage_us, validate_account_cpu_limit );
            } else {
               SYS_THROW_RUNTIME_FORMAT( tx_cpu_usage_exceeded, std::move(assert_msg),
                                         prev_cpu_usage_us, a, validate_account_cpu_limit,
                                         subjective_cpu_usage_us, validate_account_cpu_limit );
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
         assert_msg += "billed CPU time ({} us) exceeded the maximum";
         if (greylisted)
            assert_msg += " greylisted";
         assert_msg += " billable CPU time for the account {} ({} us)";

         if( greylisted ) {
            SYS_THROW_RUNTIME_FORMAT( greylist_cpu_usage_exceeded, std::move(assert_msg),
                                      billed_us, account, account_limit );
         } else {
            SYS_THROW_RUNTIME_FORMAT( tx_cpu_usage_exceeded, std::move(assert_msg),
                                      billed_us, account, account_limit );
         }
      }
   }

   void transaction_context::add_ram_usage( account_name account, int64_t ram_delta ) {
      auto& rl = control.get_mutable_resource_limits_manager();
      // Pass is_transient() so read-only / dry-run transactions do not emit deep-mind RAM events.
      rl.add_pending_ram_usage( account, ram_delta, is_transient() );
      if( ram_delta > 0 ) {
         validate_ram_usage.insert( account );
      }
   }

   void transaction_context::update_billed_cpu_time( fc::time_point now ) {
      if( explicit_billed_cpu_time || is_cpu_updated ) return; // updated in init() for explicit_billed_cpu
      bill_subjectively( bill_cpu( now ) );
   }

   account_subjective_cpu_bill_t transaction_context::bill_cpu( fc::time_point now ) {
      assert(!explicit_billed_cpu_time && !is_cpu_updated);

      // Read-only trxs are never billed (objective or subjective): they don't change chain state,
      // they may legitimately have zero authorizations, and any accounts_billing accumulation here
      // would be discarded by the read-only rollback anyway.  Implicit trxs (onblock is the only
      // type) are objectively billed to sysio but skip the subjective path -- they're chain-generated,
      // never appear in block->transactions (so on_block() can't clear a subjective entry), and the
      // sysio authorizer doesn't need subjective throttling.
      const bool bill_objectively    = !is_read_only();
      const bool billed_subjectively = bill_objectively && !is_implicit();

      account_subjective_cpu_bill_t authorizers_cpu;
      const bool subjectively_bill_payer_disabled = control.get_subjective_billing().is_payer_billing_disabled();
      const size_t input_entries = billed_cpu_us.size();
      int64_t receivers_cpu_us = 0;
      // Receiver entries exist only on a receipt, so only a transaction that succeeded gets them. A failed one keeps
      // all of its CPU on the input entries, which take its receivers' time as part of the time outside them below.
      if (!trace->except) {
         billed_cpu_us.reserve(input_entries + receiver_entries.size());
         for (const auto& e : receiver_entries) {
            const uint32_t cpu =
               static_cast<uint32_t>(std::min<int64_t>(e.cpu_us, std::numeric_limits<uint32_t>::max()));
            billed_cpu_us.emplace_back(cpu);
            receivers_cpu_us += cpu;
            trace->action_traces[e.ordinal - 1].cpu_usage_us = cpu;
            if (bill_objectively) {
               bill_receiver(e, cpu);
               // Subjectively, the input action's first authorizer is billed, as for the input entry, and not twice
               // when it is the receiver and payer billing bills it as a payer.
               if (billed_subjectively) {
                  const account_name first_auth = input_first_authorizer(e.input);
                  if (first_auth != e.account || subjectively_bill_payer_disabled)
                     authorizers_cpu[first_auth] += fc::microseconds{cpu};
               }
            }
         }
      }

      trace->total_cpu_usage_us = billed_cpu_sum();
      const auto& cfg = control.get_global_properties().configuration;
      int64_t total_cpu_time_us = std::max( (now - pseudo_start).count(), static_cast<int64_t>(cfg.min_transaction_cpu_usage) );
      SYS_ASSERT(total_cpu_time_us - trace->total_cpu_usage_us >= 0, tx_cpu_usage_exceeded,
                 "Invalid CPU usage calculation {} - {}", total_cpu_time_us, trace->total_cpu_usage_us);
      if (input_entries > 0) {
         assert(trace->action_traces.size() >= billed_cpu_us.size());
         // +1 so total is above min_transaction_cpu_usage. The time outside any entry's frames (signature recovery,
         // setup) goes on the input entries only.
         int64_t delta_per_action = (( total_cpu_time_us - trace->total_cpu_usage_us ) / input_entries) + 1;
         total_cpu_time_us = receivers_cpu_us;
         for (size_t i = 0; i < input_entries; ++i) {
            auto& b = billed_cpu_us[i];
            // if exception thrown, action_traces may not be the same size as billed_cpu_us
            auto& act_trace = trace->action_traces[i];
            b.value += delta_per_action;
            total_cpu_time_us += b.value;
            act_trace.cpu_usage_us = b.value;  // trace bookkeeping for the response, regardless of billing
            if (bill_objectively) {
               auto payer = act_trace.act.payer();
               accounts_billing[payer].cpu_usage_us += b.value;
               if (billed_subjectively) {
                  auto first_auth = input_first_authorizer(i);
                  // validate_referenced_accounts rejects non-read-only, non-implicit trxs with zero
                  // auths (tx_no_auths), so first_auth must be populated by here on the subjective path.
                  assert(!first_auth.empty());
                  // When an action has an explicit sysio.payer at position 0, first_authorizer()
                  // returns the payer (same as payer()), so the guard below skips the insert.
                  // Only the payer bears the subjective cost in that case -- the non-payer
                  // authorizer (e.g. {active, Y} at position 1) is intentionally NOT tracked
                  // separately, matching the objective-billing semantic where the payer absorbs
                  // the full cost.
                  if (first_auth != payer || subjectively_bill_payer_disabled) // don't subjectively bill payer twice if billing payer
                     authorizers_cpu[first_auth] += fc::microseconds{b.value};
               }
            }
         }
      }
      trace->total_cpu_usage_us = total_cpu_time_us;

      is_cpu_updated = true;
      return authorizers_cpu;
   }

   void transaction_context::unbill_cpu( cpu_usage_t measured_cpu_us, accounts_billing_t unbilled ) {
      assert(!explicit_billed_cpu_time);
      // A failed transaction gets no receiver entries, so its traces carry no receiver CPU either.
      for (const auto& e : receiver_entries)
         trace->action_traces[e.ordinal - 1].cpu_usage_us.reset();
      billed_cpu_us    = std::move(measured_cpu_us);
      accounts_billing = std::move(unbilled);
      is_cpu_updated   = false;
   }

   void transaction_context::bill_subjectively( const account_subjective_cpu_bill_t& authorizers_cpu ) {
      // Read-only and implicit trxs are not billed subjectively; see bill_cpu().
      if (is_read_only() || is_implicit())
         return;
      auto& subjective_bill = control.get_mutable_subjective_billing();
      if( trace->except ) {
         const fc::exception& e = *trace->except;
         if( !exception_is_exhausted( e ) && e.code() != tx_duplicate::code_value) {
            subjective_bill.subjective_bill_failure(accounts_billing, authorizers_cpu, control.pending_block_time());
         }
      } else if (!control.is_producing_block()) {
         // if producing then trx is in objective cpu account billing. Also no block will be received to remove the
         // billing.
         subjective_bill.subjective_bill(packed_trx.id(), packed_trx.expiration(), accounts_billing, authorizers_cpu);
      }
   }

   uint32_t transaction_context::billed_cpu_sum() const {
      const int64_t cpu_sum = std::ranges::fold_left(billed_cpu_us, int64_t{0}, std::plus());
      SYS_ASSERT(cpu_sum >= 0 && cpu_sum <= std::numeric_limits<uint32_t>::max(),
                 transaction_exception,
                 "Transaction total cpu usage {} us overflows uint32", cpu_sum);
      return static_cast<uint32_t>(cpu_sum);
   }

   void transaction_context::bill_explicit_receiver_entries() {
      assert(explicit_billed_cpu_time);
      const size_t input_entries = packed_trx.get_transaction().total_actions();
      const size_t expected      = input_entries + receiver_entries.size();
      SYS_ASSERT(billed_cpu_us.size() <= expected, transaction_exception,
                 "Transaction receipt cpu usage count ({}) is more than the expected {}: {} for input actions and {} "
                 "for contracts billed for their own code", billed_cpu_us.size(), expected, input_entries,
                 receiver_entries.size());
      // An entry the receipt leaves out bills nothing. In a block, the receipt built from billed_cpu_us then differs
      // from the block's receipt, and the block is rejected.
      billed_cpu_us.resize(expected);
      for (size_t k = 0; k < receiver_entries.size(); ++k) {
         const auto& e = receiver_entries[k];
         const uint32_t cpu = billed_cpu_us[input_entries + k].value;
         bill_receiver(e, cpu);
         trace->action_traces[e.ordinal - 1].cpu_usage_us = cpu;
      }
   }

   void transaction_context::bill_receiver( const receiver_entry& e, uint32_t cpu_us ) {
      if (cpu_us == 0)
         return; // a contract whose stipend covered its code owes nothing, whatever its CPU window holds
      // A receiver pays CPU only: its NET window is neither updated nor checked.
      auto& bill = accounts_billing.try_emplace(e.account, account_billing{ .pays_net = false }).first->second;
      bill.cpu_usage_us += cpu_us;
   }

   account_name transaction_context::input_first_authorizer( uint32_t input ) const {
      auto first_auth = trace->action_traces[input].act.first_authorizer();
      return first_auth.empty() ? packed_trx.get_transaction().first_authorizer() : first_auth;
   }

   bool transaction_context::begin_frame( uint32_t action_ordinal, bool system_receiver, bool receiver_has_code ) {
      if (is_implicit())
         return false; // no receipt, so every frame bills the input payer
      const action_trace& act_trace     = get_action_trace(action_ordinal);
      const uint32_t      creator       = act_trace.creator_action_ordinal.value; // the frame that notified or sent it
      const uint32_t      input_entries = packed_trx.get_transaction().total_actions();
      uint32_t            entry         = action_ordinal - 1;
      if (creator == 0) {
         assert(action_ordinal <= input_entries);
         if (act_trace.act.has_explicit_payer())
            return false; // the explicit payer pays for every frame below it
      } else {
         if (creator > frames.size() || !frames[creator - 1].tracked)
            return false; // below an explicit payer
         const frame_billing& parent = frames[creator - 1];
         entry = parent.entry;
         if (parent.system && !system_receiver && receiver_has_code) {
            const bool         parent_on_input = parent.entry < input_entries;
            const account_name parent_payer    = parent_on_input
                                                    ? get_action_trace(parent.entry + 1).act.payer()
                                                    : receiver_entries[parent.entry - input_entries].account;
            if (act_trace.receiver != parent_payer) {
               // A notification reaches a contract that cannot refuse it, so the parent's entry covers its first
               // stipend. An action sent to the contract itself bills it from the start, as calling it directly does.
               const bool notified = act_trace.receiver != act_trace.act.account;
               receiver_entry e{ .account         = act_trace.receiver,
                                 .input           = parent_on_input
                                                       ? parent.entry
                                                       : receiver_entries[parent.entry - input_entries].input,
                                 .ordinal         = action_ordinal,
                                 .stipend_entry   = parent.entry,
                                 .stipend_left_us =
                                    notified ? control.get_global_properties().configuration.notify_cpu_stipend : 0 };
               if (!explicit_billed_cpu_time && !is_read_only()) {
                  // Its usage window was not updated in this block, so its usage decays to the pending block.
                  std::tie(e.cpu_limit_us, e.cpu_greylisted, std::ignore) =
                     get_cpu_limit(e.account, block_timestamp_type(control.pending_block_time()));
               }
               entry = input_entries + receiver_entries.size();
               receiver_entries.push_back(e);
            }
         }
      }
      if (frames.size() < action_ordinal)
         frames.resize(action_ordinal);
      frames[action_ordinal - 1] = { .entry = entry, .system = system_receiver, .tracked = true };
      if (explicit_billed_cpu_time || entry < input_entries)
         return false; // the receipt carries the CPU, or the input entry measures its whole action

      // The receiver pays for this frame past its stipend, so the frame runs against what is left of the stipend and
      // then the receiver's remaining CPU. The stipend's payer is checked against its limit in finalize().
      const receiver_entry& e = receiver_entries[entry - input_entries];
      int64_t used_us = 0;
      for (const auto& r : receiver_entries)
         if (r.account == e.account)
            used_us += r.cpu_us;
      const fc::time_point now = fc::time_point::now();
      frame_begin = now - action_start;
      set_billing_deadline(e.account, e.cpu_limit_us - used_us, e.cpu_greylisted, now,
                           fc::microseconds(e.stipend_left_us));
      if (enforce_deadline)
         checktime(); // fail early if the receiver has no CPU left
      return true;
   }

   void transaction_context::end_frame( uint32_t action_ordinal ) {
      const uint32_t   input_entries = packed_trx.get_transaction().total_actions();
      receiver_entry&  e             = receiver_entries[frames[action_ordinal - 1].entry - input_entries];
      // action_start moves forward by any pause, so this leaves paused time out
      const fc::microseconds cpu = std::max(fc::time_point::now() - action_start - frame_begin, fc::microseconds(0));
      const int64_t stipend_us = std::min(cpu.count(), e.stipend_left_us);
      e.stipend_left_us -= stipend_us;
      e.cpu_us += cpu.count() - stipend_us;
      if (e.stipend_entry < input_entries) {
         // The input entry measures its action's whole time apart from receiver entries' CPU, so leaving the stipend
         // in the action's clock bills it to the input entry.
         action_start += cpu - fc::microseconds(stipend_us);
      } else {
         receiver_entries[e.stipend_entry - input_entries].cpu_us += stipend_us;
         // The frame's CPU is not the input payer's: the action's clock and deadline move past it.
         action_start += cpu;
      }
      const account_name payer = get_action_trace(e.input + 1).act.payer();
      const auto&        b     = accounts_billing.at(payer);
      set_billing_deadline(payer, b.cpu_limit_us, b.cpu_greylisted, action_start);
   }

   void transaction_context::set_billing_deadline( account_name account, int64_t cpu_limit_us, bool greylisted,
                                                   fc::time_point from, fc::microseconds stipend ) {
      active_deadline     = trx_deadline;
      tx_cpu_usage_reason = trx_cpu_usage_reason;
      if (!explicit_billed_cpu_time) {
         cpu_limit_due_to_greylist = greylisted;
         if (!is_read_only()) {
            subjective_cpu_bill =
               control.get_subjective_billing().get_subjective_bill(account, control.pending_block_time());
            // Add leeway to allow powerup
            int64_t account_cpu_limit = cpu_limit_us - subjective_cpu_bill.count() + leeway.count();
            // An account with no CPU left still runs for its stipend, which another entry pays for.
            if (stipend > fc::microseconds(0))
               account_cpu_limit = stipend.count() + std::max<int64_t>(account_cpu_limit, 0);
            // Possibly limit deadline to account subjective cpu left
            if (from + fc::microseconds(account_cpu_limit) < trx_deadline) {
               active_deadline              = from + fc::microseconds(account_cpu_limit);
               billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
               tx_cpu_usage_reason          = tx_cpu_usage_exceeded_reason::account_cpu_limit;
            }
         }
      }
      if (enforce_deadline) {
         transaction_timer.stop();
         transaction_timer.start(active_deadline);
      }
   }

   void transaction_context::verify_init_subjective_billing() const {
      const chain::subjective_billing& subjective_bill = control.get_subjective_billing();
      if (explicit_billed_cpu_time || subjective_bill.is_disabled())
         return;

      const fc::microseconds subjective_cpu_allowed = subjective_bill.get_subjective_account_cpu_allowed();
      const auto& trx = packed_trx.get_transaction();
      const action_payers_t auths = trx.first_authorizers();
      const action_payers_t payers = trx.payers();
      std::vector<account_name> auths_not_payers;
      // payers subjective billing will be handled by transaction_context during exec
      std::ranges::set_difference(auths, payers, std::back_inserter(auths_not_payers));
      // verify any auths that are not payers subjective billing
      for (const auto& a : auths_not_payers) {
         if (!subjective_bill.is_account_disabled(a)) {
            const auto&[cpu_limit, greylisted, unlimited] = get_cpu_limit(a);
            if (!unlimited) { // else is unlimited
               const auto sub_bill = subjective_bill.get_subjective_bill(a, control.pending_block_time());
               const int64_t available = subjective_cpu_allowed.count() + cpu_limit - sub_bill.count();
               if (available <= 0) {
                  std::string assert_msg;
                  assert_msg.reserve(256);
                  assert_msg += "Subjectively terminated trx {}. Authorized";
                  if (greylisted)
                     assert_msg += " greylisted";
                  assert_msg += " account {} exceeded subjective CPU limit {}us by {}us"
                                " with an objective cpu limit of {}us.";
                  if( greylisted ) {
                     SYS_THROW_RUNTIME_FORMAT( greylist_cpu_usage_exceeded, std::move(assert_msg),
                                               packed_trx.id(), a, subjective_cpu_allowed, -available, cpu_limit );
                  } else {
                     SYS_THROW_RUNTIME_FORMAT( tx_cpu_usage_exceeded, std::move(assert_msg),
                                               packed_trx.id(), a, subjective_cpu_allowed, -available, cpu_limit );
                  }

               }
            }
         }
      }
   }

   std::tuple<int64_t, bool, bool>
   transaction_context::get_cpu_limit(account_name a, const std::optional<block_timestamp_type>& current_time) const {
      return control.get_cpu_limit(a, current_time);
   }

   void transaction_context::verify_net_usage(account_name a, int64_t net_usage, uint32_t net_usage_leeway) {
      // Assumes rl.update_account_usage() was already called prior

      // Calculate the new highest network usage and CPU time that the billed account can afford to be billed
      const auto& rl = control.get_resource_limits_manager();
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
            SYS_THROW( greylist_net_usage_exceeded, "greylisted account {} net usage is too high: {} > {}",
                       a, net_usage, leeway_net_limit );
         } else {
            SYS_THROW( tx_net_usage_exceeded, "account {} net usage is too high: {} > {}",
                       a, net_usage, leeway_net_limit );
         }
      }
   }

   action_trace& transaction_context::get_action_trace( uint32_t action_ordinal ) {
      SYS_ASSERT( 0 < action_ordinal && action_ordinal <= trace->action_traces.size() ,
                  transaction_exception,
                  "action_ordinal {} is outside allowed range [1,{}]",
                  action_ordinal, trace->action_traces.size()
      );
      return trace->action_traces[action_ordinal-1];
   }

   const action_trace& transaction_context::get_action_trace( uint32_t action_ordinal )const {
      SYS_ASSERT( 0 < action_ordinal && action_ordinal <= trace->action_traces.size() ,
                  transaction_exception,
                  "action_ordinal {} is outside allowed range [1,{}]",
                  action_ordinal, trace->action_traces.size()
      );
      return trace->action_traces[action_ordinal-1];
   }

   uint32_t transaction_context::schedule_action( const action& act, account_name receiver,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.emplace_back( *trace, act, receiver,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   uint32_t transaction_context::schedule_action( action&& act, account_name receiver,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.emplace_back( *trace, std::move(act), receiver,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   uint32_t transaction_context::schedule_action( uint32_t action_ordinal, account_name receiver,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.reserve( std::bit_ceil(new_action_ordinal) ); // bit_ceil to avoid vector copy on every reserve call.

      const action& provided_action = get_action_trace( action_ordinal ).act;

      // The reserve above is required so that the emplace_back below does not invalidate the provided_action reference,
      // which references an action within the `trace->action_traces` vector we are appending to.

      trace->action_traces.emplace_back( *trace, provided_action, receiver,
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
      control.record_transaction(id, expire);
   } /// record_transaction

   void transaction_context::validate_referenced_accounts( const transaction& trx, bool enforce_actor_whitelist_blacklist )const {
      const auto& db = control.db();
      const auto& auth_manager = control.get_authorization_manager();

      flat_set<account_name> actors;

      bool one_auth = false;
      for( const auto& a : trx.actions ) {
         auto* code = db.find<account_object, by_name>(a.account);
         SYS_ASSERT( code != nullptr, transaction_exception,
                     "action's code account '{}' does not exist", a.account );
         if ( is_read_only() ) {
            SYS_ASSERT( a.authorization.size() == 0, transaction_exception,
                       "read-only action '{}' cannot have authorizations", a.name );
         }
         name payer;
         for (size_t i = 0; i < a.authorization.size(); ++i) {
            const auto& auth = a.authorization[i];
            if (auth.permission == config::sysio_payer_name) {
               SYS_ASSERT(payer.empty(), transaction_exception,
                          "action cannot have multiple payers");
               SYS_ASSERT(i == 0, transaction_exception,
                          "Explicit payer must be the first declared authorization");

               auto *actor = db.find<account_object, by_name>(auth.actor);
               SYS_ASSERT(actor != nullptr, transaction_exception,
                          "action's paying actor '{}' does not exist", auth.actor);
               payer = auth.actor;
            } else {
               one_auth = true;
               auto *actor = db.find<account_object, by_name>(auth.actor);
               SYS_ASSERT(actor != nullptr, transaction_exception,
                          "action's authorizing actor '{}' does not exist", auth.actor);
               SYS_ASSERT(auth_manager.find_permission(auth) != nullptr, transaction_exception,
                          "action's authorizations include a non-existent permission: {}",
                          auth);
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
            SYS_ASSERT(authPayer, transaction_exception, "Payer '{}' did not authorize this action", payer);
         }
      }
      SYS_ASSERT(one_auth || is_read_only(), tx_no_auths, "transaction must have at least one authorization" );

      if( enforce_actor_whitelist_blacklist ) {
         control.check_actor_list( actors );
      }
   }

   int64_t transaction_context::set_proposed_producers(vector<producer_authority> producers) {
      if (producers.empty())
         return -1; // SAVANNA depends on DISALLOW_EMPTY_PRODUCER_SCHEDULE

      SYS_ASSERT(producers.size() <= config::max_proposers, wasm_execution_error,
                 "Producer schedule exceeds the maximum proposer count for this chain");

      trx_blk_context.proposed_schedule_block_num = control.head().block_num() + 1;
      // proposed_schedule.version is set in assemble_block
      trx_blk_context.proposed_schedule.producers = std::move(producers);

      return std::numeric_limits<uint32_t>::max();
   }

   void transaction_context::set_proposed_finalizers(finalizer_policy&& fin_pol) {
      trx_blk_context.proposed_fin_pol_block_num = control.head().block_num() + 1;
      trx_blk_context.proposed_fin_pol = std::move(fin_pol);
   }

} /// sysio::chain
