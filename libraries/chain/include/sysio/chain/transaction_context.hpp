#pragma once
#include <sysio/chain/controller.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/platform_timer.hpp>

namespace sysio::benchmark {
   struct interface_in_benchmark; // for benchmark testing
}

namespace sysio::chain {

   struct transaction_checktime_timer {
      public:
         transaction_checktime_timer() = delete;
         transaction_checktime_timer(const transaction_checktime_timer&) = delete;
         transaction_checktime_timer(transaction_checktime_timer&&) = default;
         transaction_checktime_timer(platform_timer& timer);
         ~transaction_checktime_timer();

         void start(fc::time_point tp);
         void stop();
         void set_expired() { _timer.set_expired(); }

         platform_timer::state_t timer_state() const { return _timer.timer_state(); }

         /* Sets a callback for when timer expires. Be aware this could might fire from a signal handling context and/or
            on any particular thread. Only a single callback can be registered at once; trying to register more will
            result in an exception. Use nullptr to disable a previously set callback. */
         void set_expiration_callback(void(*func)(void*), void* user);

      private:
         platform_timer& _timer;

         friend controller_impl;
   };

   struct action_digests_t {
      digests_t digests_s; // savanna

      action_digests_t() = default;

      void append(action_digests_t&& o) {
         fc::move_append(digests_s, std::move(o.digests_s));
      }

      void compute_and_append_digests_from(action_trace& trace) {
         digests_s.emplace_back(trace.digest_savanna());
      }

      size_t size() const {
         return digests_s.size();
      }

      void resize(size_t sz) {
         digests_s.resize(sz);
      }
   };

   // transaction side affects to apply to block when block is assembled
   struct trx_block_context {
      std::optional<block_num_type>       proposed_schedule_block_num;
      producer_authority_schedule         proposed_schedule;

      std::optional<block_num_type>       proposed_fin_pol_block_num;
      finalizer_policy                    proposed_fin_pol;

      void apply(trx_block_context&& rhs) {
         if (rhs.proposed_schedule_block_num) {
            proposed_schedule_block_num = rhs.proposed_schedule_block_num;
            proposed_schedule = std::move(rhs.proposed_schedule);
         }
         if (rhs.proposed_fin_pol_block_num) {
            proposed_fin_pol_block_num = rhs.proposed_fin_pol_block_num;
            proposed_fin_pol = std::move(rhs.proposed_fin_pol);
         }
      }
   };

   class transaction_context {
      private:
         // construction/reset initialization
         void initialize();
         void reset();
         // common init called by init_for_* methods below
         void init();

      public:

         transaction_context( controller& c,
                              const packed_transaction& t,
                              transaction_checktime_timer&& timer,
                              fc::time_point start,
                              transaction_metadata::trx_type type,
                              const std::optional<fc::microseconds>& subjective_cpu_leeway,
                              const fc::time_point& block_deadline,
                              const fc::microseconds& max_transaction_time_subjective,
                              bool explicit_billed_cpu_time,
                              const accounts_billing_t& prev_accounts_billing,
                              const cpu_usage_t& billed_cpu_us );
         ~transaction_context();

         void init_for_implicit_trx();

         void init_for_input_trx();

         void exec();
         void finalize();
         void squash();
         void undo();

         void check_trx_net_usage()const;

         void checktime()const;

         template <typename DigestType>
         inline DigestType hash_with_checktime( const char* data, uint32_t datalen )const {
            const size_t bs = sysio::chain::config::hashing_checktime_block_size;
            typename DigestType::encoder enc;
            while ( datalen > bs ) {
               enc.write( data, bs );
               data    += bs;
               datalen -= bs;
               checktime();
            }
            enc.write( data, datalen );
            return enc.result();
         }

         void pause_billing_timer();
         void resume_billing_timer(fc::time_point resume_from = fc::time_point{});

         void update_billed_cpu_time( fc::time_point now );

         void validate_referenced_accounts( const transaction& trx, bool enforce_actor_whitelist_blacklist )const;

         bool is_dry_run()const { return trx_type == transaction_metadata::trx_type::dry_run; };
         bool is_read_only()const { return trx_type == transaction_metadata::trx_type::read_only; };
         bool is_transient()const { return trx_type == transaction_metadata::trx_type::read_only || trx_type == transaction_metadata::trx_type::dry_run; };
         bool is_implicit()const { return trx_type == transaction_metadata::trx_type::implicit; };
         bool has_undo()const;

         int64_t set_proposed_producers(vector<producer_authority> producers);
         void    set_proposed_finalizers(finalizer_policy&& fin_pol);

      private:

         friend struct controller_impl;
         friend class apply_context;
         friend struct benchmark::interface_in_benchmark; // defined in benchmark/bls.cpp

         //        limit,greylisted,unlimited
         std::tuple<int64_t, bool, bool> get_cpu_limit(account_name a) const;

         void verify_init_subjective_billing() const;
         void verify_net_usage(account_name account, int64_t net_usage, uint32_t net_usage_leeway);

         void add_ram_usage( account_name account, int64_t ram_delta );

         action_trace& get_action_trace( uint32_t action_ordinal );
         const action_trace& get_action_trace( uint32_t action_ordinal )const;

         /** invalidates any action_trace references returned by get_action_trace */
         uint32_t schedule_action( const action& act, account_name receiver, bool context_free,
                                   uint32_t creator_action_ordinal, uint32_t closest_unnotified_ancestor_action_ordinal );

         /** invalidates any action_trace references returned by get_action_trace */
         uint32_t schedule_action( action&& act, account_name receiver, bool context_free,
                                   uint32_t creator_action_ordinal, uint32_t closest_unnotified_ancestor_action_ordinal );

         /** invalidates any action_trace references returned by get_action_trace */
         uint32_t schedule_action( uint32_t action_ordinal, account_name receiver, bool context_free,
                                   uint32_t creator_action_ordinal, uint32_t closest_unnotified_ancestor_action_ordinal );

         void execute_action( uint32_t action_ordinal, uint32_t recurse_depth );

         void record_transaction( const transaction_id_type& id, fc::time_point_sec expire );

         void validate_available_account_cpu( account_name account, int64_t billed_us, int64_t account_limit, bool greylisted )const;
         void validate_account_cpu_usage_estimate()const;
         void validate_cpu_minimum()const;
         void validate_trx_billed_cpu()const;

         std::string get_tx_cpu_usage_exceeded_reason_msg(fc::microseconds& limit) const;

      /// Fields:
      public:

         controller&                                 control;
         const packed_transaction&                   packed_trx;
         std::optional<chainbase::database::session> undo_session;
         transaction_trace_ptr                       trace;
         fc::time_point                              start;

         fc::time_point                published;

         action_digests_t              executed_action_receipts;
         accounts_billing_t            accounts_billing;
         flat_set<account_name>        validate_ram_usage;

         /// the maximum number of virtual CPU instructions of the transaction that can be safely billed to the billable accounts
         uint64_t                      initial_max_billable_cpu = 0;

         bool                          is_input           = false;
         bool                          enforce_whiteblacklist = true;

         transaction_checktime_timer   transaction_timer;

   private:
         bool                          is_initialized = false;
         bool                          is_cpu_updated = false;
         const transaction_metadata::trx_type trx_type;
         const fc::microseconds        leeway;
         const bool                    enforce_deadline;
         const fc::time_point          block_deadline;
         const fc::microseconds        max_transaction_time_subjective;
         const bool                    explicit_billed_cpu_time;
         const accounts_billing_t&     prev_accounts_billing;
         cpu_usage_t                   billed_cpu_us;

         uint64_t                      trx_net_limit = 0;
         bool                          net_limit_due_to_block = true;
         uint64_t                      leeway_trx_net_limit = 0;

         bool                          cpu_limit_due_to_greylist = false;
         fc::microseconds              subjective_cpu_bill;

         fc::time_point                paused_time;
         fc::microseconds              objective_duration_limit;
         fc::time_point                trx_deadline = fc::time_point::maximum(); // calculated deadline
         fc::time_point                active_deadline; // either action or transaction deadline to use for timer
         int64_t                       deadline_exception_code = block_cpu_usage_exceeded::code_value;
         int64_t                       billing_timer_exception_code = block_cpu_usage_exceeded::code_value;
         fc::time_point                pseudo_start;
         fc::time_point                action_start; // adjusted for paused timer
         bool                          paused_timer = false;
         trx_block_context             trx_blk_context;

         enum class tx_cpu_usage_exceeded_reason {
            account_cpu_limit, // includes subjective billing
            on_chain_consensus_max_transaction_cpu_usage,
            user_specified_trx_max_cpu_usage_ms,
            node_configured_max_transaction_time,
            speculative_executed_adjusted_max_transaction_time // prev_billed_cpu_time_us > 0
         };
         tx_cpu_usage_exceeded_reason  tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
   };

} // namespace sysio::chain
