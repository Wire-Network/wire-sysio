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

         /// Bills the CPU measured so far, objectively and subjectively, once. The controller calls it for a
         /// transaction that failed; finalize() bills one that succeeds itself, so that its final checks can fail it.
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
         std::tuple<int64_t, bool, bool> get_cpu_limit(account_name a, const std::optional<block_timestamp_type>& current_time = {}) const;

         /**
          * Picks the billed_cpu_us entry that pays for a frame: one receiver's run of one action. An input action bills
          * its payer. A child (a notification or an inline action) bills its parent's entry, unless the parent's
          * receiver is a system account and the child's receiver is a contract outside the system accounts: that
          * contract then pays for its own code, in a new entry after the input entries. When that child is a
          * notification, the parent's entry covers the first chain_config::notify_cpu_stipend of the new entry. Below
          * an explicit payer, and in an implicit transaction, every frame bills the input payer. Called by
          * apply_context::exec_one.
          * @param system_receiver the receiver is a privileged system account
          * @param receiver_has_code the receiver has contract code as the frame starts
          * @return true when the frame's CPU is measured for a receiver entry: end_frame follows if the frame completes
          */
         bool begin_frame( uint32_t action_ordinal, bool system_receiver, bool receiver_has_code );
         /// Adds the CPU of a frame begin_frame measured to its receiver entry, the part its stipend covers to the
         /// entry that pays the stipend, and moves the deadline back to the input action's payer. A frame that throws
         /// fails the transaction and skips this.
         void end_frame( uint32_t action_ordinal );
         /// Sets the deadline for code billed to `account`, which has `cpu_limit_us` left from `from`, within the
         /// transaction's deadline. A `stipend` that another entry pays for runs first, and the account's CPU after it
         /// is never less than none.
         void set_billing_deadline( account_name account, int64_t cpu_limit_us, bool greylisted, fc::time_point from,
                                    fc::microseconds stipend = fc::microseconds(0) );

         /// Sum of billed_cpu_us; throws if it does not fit the trace's 32-bit total.
         uint32_t billed_cpu_sum() const;
         /// Bills the receipt's entries for contracts billed for their own code. An entry the receipt leaves out bills
         /// nothing, so a block receipt that leaves one out differs from the one built here and the block is rejected.
         void bill_explicit_receiver_entries();

         void verify_init_subjective_billing() const;
         void verify_net_usage(account_name account, int64_t net_usage, uint32_t net_usage_leeway);

         void add_ram_usage( account_name account, int64_t ram_delta );

         action_trace& get_action_trace( uint32_t action_ordinal );
         const action_trace& get_action_trace( uint32_t action_ordinal )const;

         /** invalidates any action_trace references returned by get_action_trace */
         uint32_t schedule_action( const action& act, account_name receiver,
                                   uint32_t creator_action_ordinal, uint32_t closest_unnotified_ancestor_action_ordinal );

         /** invalidates any action_trace references returned by get_action_trace */
         uint32_t schedule_action( action&& act, account_name receiver,
                                   uint32_t creator_action_ordinal, uint32_t closest_unnotified_ancestor_action_ordinal );

         /** invalidates any action_trace references returned by get_action_trace */
         uint32_t schedule_action( uint32_t action_ordinal, account_name receiver,
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

         /// RAII guard that manages both the chainbase undo session and the dedup
         /// transaction-level session. On destruction, if neither squash() nor undo()
         /// was called, both sessions are automatically undone.
         struct trx_session {
            chainbase::database::session db_session;

            explicit trx_session(chainbase::database& db)
               : db_session(db.start_undo_session(true)) {}
            trx_session(trx_session&&) = delete;

            // The dedup is a registered chainbase undo participant, so the database session drives
            // its add_undo_session / squash / undo automatically -- including the implicit undo from
            // db_session's destructor when neither squash() nor undo() is called.
            void squash() { db_session.squash(); }
            void undo()   { db_session.undo();   }
         };

         controller&                    control;
         const packed_transaction&      packed_trx;
         std::optional<trx_session>     undo_session;
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
            speculative_executed_adjusted_max_transaction_time // !prev_accounts_billing.empty()
         };
         tx_cpu_usage_exceeded_reason  tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
         /// tx_cpu_usage_reason as init() left it: the reason when no account's CPU sets the deadline.
         tx_cpu_usage_exceeded_reason  trx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;

         /// The billed_cpu_us entry that pays for a frame.
         struct frame_billing {
            uint32_t entry   = 0;     ///< index in billed_cpu_us
            bool     system  = false; ///< the receiver is a privileged system account
            bool     tracked = false; ///< false below an explicit payer, where every frame bills the input payer
         };

         /// A billed_cpu_us entry after the input entries: a frame whose receiver pays for its own code, together with
         /// the frames below it that bill the same contract.
         struct receiver_entry {
            account_name account;
            uint32_t     input           = 0;     ///< index of the input action above it
            uint32_t     ordinal         = 0;     ///< action ordinal of the frame where billing switched
            uint32_t     stipend_entry   = 0;     ///< index in billed_cpu_us of the entry that pays its stipend
            int64_t      stipend_left_us = 0;     ///< stipend not yet used: chain_config::notify_cpu_stipend as a
                                                  ///< notification's entry starts, otherwise 0
            int64_t      cpu_us          = 0;     ///< CPU billed to the contract: its frames past the stipend, plus
                                                  ///< the stipends it pays for other entries
            int64_t      cpu_limit_us    = 0;     ///< CPU the contract has left as the entry starts, for the deadline
            bool         cpu_greylisted  = false;
         };

         std::vector<frame_billing>  frames;           ///< by action ordinal - 1
         std::vector<receiver_entry> receiver_entries; ///< in the order their frames started
         fc::microseconds            frame_begin;      ///< now - action_start as the measured frame began

         /// Bills the measured CPU objectively: adds the receiver entries when the transaction succeeded, puts the time
         /// outside any entry's frames on the input entries, and adds each entry to its account in accounts_billing.
         /// Returns the CPU to bill subjectively to the input actions' first authorizers.
         account_subjective_cpu_bill_t bill_cpu( fc::time_point now );
         /// Undoes bill_cpu() for a transaction that fails after it, so that it can be billed as a failed one:
         /// `measured_cpu_us` and `unbilled` are billed_cpu_us and accounts_billing as bill_cpu() found them.
         void unbill_cpu( cpu_usage_t measured_cpu_us, accounts_billing_t unbilled );
         /// Bills `authorizers_cpu` subjectively, and the accounts in accounts_billing unless payer billing is
         /// disabled: pending until a block includes the transaction when it succeeded, or as a failure.
         void bill_subjectively( const account_subjective_cpu_bill_t& authorizers_cpu );
         /// Bills `cpu_us` of receiver entry `e` to its contract: CPU only, never NET. finalize() reads its CPU limit.
         /// An entry of 0 bills nothing: the contract is not billed, so its CPU window is neither updated nor checked.
         void bill_receiver( const receiver_entry& e, uint32_t cpu_us );
         /// First authorizer of input action `input`, or the transaction's when it has none.
         account_name input_first_authorizer( uint32_t input ) const;
   };

} // namespace sysio::chain
