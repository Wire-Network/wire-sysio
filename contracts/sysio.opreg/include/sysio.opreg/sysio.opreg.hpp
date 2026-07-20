#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <magic_enum/magic_enum.hpp>

namespace sysio {

   /**
    * @brief sysio.opreg — operator registry on WIRE.
    *
    * Holds the **authoritative** collateral ledger for every operator type
    * (producers, batch operators, underwriters, plus their standby tiers).
    * Per the corrected ledger model in
    * `CLAUDE-WIRE-OPERATOR-COLLATERAL-IMPL-PLAN.md`:
    *
    * - One aggregate balance per (operator, chain, token_kind) — NOT a
    *   vector of collateral entries, NOT a locked/available/amount trio.
    * - `deposit` (operator-callable, WIRE-direct) and `depositinle`
    *   (msgch-dispatched from outposts) credit the matching balance row;
    *   `withdraw` (operator-callable, WIRE-direct) and `withdrawinle`
    *   (msgch-dispatched from outposts) enqueue a delayed subtraction;
    *   `slash` zeros the unlocked portion immediately and leaves locks
    *   for `sysio.uwrit::release` to slash deferred.
    * - Underwriter locks live entirely in `sysio.uwrit::locks` — opreg
    *   reads them directly via the public `sysio::uwrit::locks_t` table
    *   type to compute `available()`.
    * - Pending withdraws are also subtracted by `available()` so an
    *   operator can't double-use queued funds; `cancelwtdw` lets them
    *   walk back a queued withdraw before it executes.
    * - Termination is administrative (status -> TERMINATED, balance
    *   remitted to the operator's authex destination); slashing is
    *   punitive (status -> SLASHED, balance routed to the matching LP).
    */
   class [[sysio::contract("sysio.opreg")]] opreg : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name UWRIT_ACCOUNT  = "sysio.uwrit"_n;
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      // Core token symbol — currently SYS, may change to WIRE
      static constexpr symbol CORE_SYM = symbol("SYS", 4);

      // 2-epoch wait between `queue_withdraw` and `flushwithdraws` releasing
      // funds. Long enough that an operator who would drop below the role
      // minimum is demoted before the funds physically leave.
      static constexpr uint32_t WITHDRAW_WAIT_EPOCHS = 2;

      /// Safety rails on the collateral withdraw queue (SEC-78 / WSA-166).
      /// `withdraw` / `withdrawinle` are operator-driven and bounded only by
      /// available collateral, so without a row cap an operator could split
      /// collateral into an unbounded number of system-paid `wtdwqueue` rows and
      /// force `flushwtdw` to process them all inside one `sysio.epoch::advance`
      /// transaction. That transaction's CPU budget (~150 ms) is a hard,
      /// uncatchable deadline, so an oversized queue would abort every advance
      /// and stall epoch progress chain-wide.
      ///
      /// MAX_WTDW_FLUSH_PER_EPOCH bounds the matured rows flushed per advance;
      /// undrained rows stay queued (collateral stays in the operator's balance
      /// until flushed) and flush a later epoch. Ingress is already bounded
      /// economically -- withdraw rows can never exceed the operator's real
      /// deposited collateral, and direct `withdraw` bills the operator CPU/NET
      /// per call -- so the flush bound alone is the liveness rail; there is no
      /// per-account row cap. Conservatively sized to stay well under the
      /// transaction CPU ceiling shared with the rest of advance's fan-out.
      static constexpr uint32_t MAX_WTDW_FLUSH_PER_EPOCH = 32;

      /// Rolling delivery-buffer thresholds for batch-op termination. Per the
      /// plan §1: missing a delivery is NOT a slash; consistent missing IS
      /// grounds for administrative termination.
      ///
      /// All three thresholds live in `op_config` so tests can override them
      /// without recompiling. These `DEFAULT_*` constants are the values
      /// production bootstrap should install.
      static constexpr uint32_t DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES = 5;
      static constexpr uint32_t DEFAULT_TERMINATE_MAX_PCT_MISSES_24H     = 5;   // percent
      static constexpr uint64_t DEFAULT_TERMINATE_WINDOW_MS              = 24ULL * 60 * 60 * 1000;

      /// Lowest accepted threshold for consecutive miss termination.
      static constexpr uint32_t MIN_TERMINATE_MAX_CONSECUTIVE_MISSES = 1;

      /// Highest launch-approved consecutive-miss threshold; larger values can
      /// make miss-based recovery unreachable within the intended operating envelope.
      /// Deliberately an independent literal: bumping the production default must
      /// not silently widen this security ceiling.
      static constexpr uint32_t MAX_TERMINATE_MAX_CONSECUTIVE_MISSES = 5;

      /// Lowest accepted threshold for rolling-window percent-miss termination.
      static constexpr uint32_t MIN_TERMINATE_MAX_PCT_MISSES_24H = 1;

      /// Highest accepted percent-miss threshold. A 100% threshold can never be
      /// exceeded by an all-miss window while `termcheck` uses strict breach semantics.
      static constexpr uint32_t MAX_TERMINATE_MAX_PCT_MISSES_24H = 99;

      static_assert(MIN_TERMINATE_MAX_CONSECUTIVE_MISSES <= DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES &&
                    DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES <= MAX_TERMINATE_MAX_CONSECUTIVE_MISSES,
                    "production default consecutive-miss threshold must lie inside the accepted bounds");
      static_assert(MIN_TERMINATE_MAX_PCT_MISSES_24H <= DEFAULT_TERMINATE_MAX_PCT_MISSES_24H &&
                    DEFAULT_TERMINATE_MAX_PCT_MISSES_24H <= MAX_TERMINATE_MAX_PCT_MISSES_24H,
                    "production default percent-miss threshold must lie inside the accepted bounds");

      /// Minimum accepted `terminate_window_ms` for a given consecutive-miss
      /// threshold and epoch duration: the rolling window must span the full
      /// terminating run -- `consecutive_misses` per-epoch delivery records --
      /// plus one epoch of boundary slack, or records age out of the window
      /// before `termcheck` can observe the run, leaving the consecutive rail
      /// structurally vacuous and only the percent rail live (SEC-28 residual).
      /// Enforced from both `opreg::setconfig` (against the stored epoch
      /// duration) and `sysio.epoch::setconfig` (against the stored opreg
      /// config) so no ordering of the two setters can accept a vacuous pair.
      /// Inputs are pre-bounded by those setters (misses <= 5, duration <= 30
      /// days), so the product cannot overflow uint64.
      static constexpr uint64_t min_terminate_window_ms(uint32_t consecutive_misses,
                                                        uint32_t epoch_duration_sec) {
         constexpr uint64_t ms_per_sec = 1000;
         return (uint64_t{consecutive_misses} + 1) * epoch_duration_sec * ms_per_sec;
      }

      /// Bounded sweep sizes for delivery-log rows that have aged out of the
      /// rolling termination window. The write-path cap only has to outpace
      /// insertion (each `recorddel` adds one row); the `prune` cap clears
      /// backlog faster when cranked.
      static constexpr uint32_t MAX_DELLOG_PRUNE_PER_WRITE = 4;
      static constexpr uint32_t MAX_DELLOG_PRUNE_PER_CRANK = 64;

      // Per-operator audit log: ring-buffer cap (newest-in / oldest-out) and
      // per-entry error_message length cap. Operators read recent_actions to
      // diagnose dropped requests; the log must stay bounded so a long-lived
      // operator's row doesn't grow unbounded.
      static constexpr size_t   MAX_RECENT_ACTIONS       = 5;
      static constexpr size_t   MAX_ERROR_MESSAGE_BYTES  = 2048;

      // -----------------------------------------------------------------------
      //  Forward types
      // -----------------------------------------------------------------------

      /// Per-(chain, token) minimum-bond row stored in `opconfig`'s
      /// per-role requirement vectors and accepted as `setconfig` input.
      /// Per the v6 data-model refactor: `chain` / `token` identifiers are
      /// `sysio::slug_name` (uint64-packed) instead of the old enums.
      struct chain_min_bond {
         sysio::slug_name  chain_code;
         sysio::slug_name  token_code;
         uint64_t         min_bond            = 0;
         uint64_t         config_timestamp_ms = 0;

         SYSLIB_SERIALIZE(chain_min_bond, (chain_code)(token_code)(min_bond)(config_timestamp_ms))
      };

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set operator registry configuration. The three `terminate_*`
      /// thresholds drive `termcheck`'s rolling-buffer evaluation; tests
      /// can dial them down (e.g. `terminate_max_consecutive_misses=2`,
      /// `terminate_window_ms=60_000`) to make the miss → terminate path
      /// observable inside a flow-test's timeout budget.
      ///
      /// The three `req_*_collat` vectors are the per-role eligibility
      /// requirements: each entry is a `(chain, token_kind, min_bond)`
      /// triple the operator's `available(account, chain, token_kind)`
      /// must meet or exceed for that role. The chain set is closed
      /// implicitly — an operator that isn't bonded on an entry's chain
      /// has `available(...) == 0` and fails the predicate. This is the
      /// only mechanism that enforces "ACTIVE requires deposit on every
      /// active outpost"; `meets_role_min` (in this contract) iterates
      /// the matching vector for each eligibility evaluation.
      ///
      /// `config_timestamp_ms` on each entry is overwritten by
      /// `setconfig` with the on-chain `current_time_ms()`, so the
      /// caller's clock isn't trusted for staleness comparisons. Within
      /// each vector, every `(chain, token_kind)` pair must be unique;
      /// duplicates fail the action.
      [[sysio::action]]
      void setconfig(uint32_t max_available_producers,
                     uint32_t max_available_batch_ops,
                     uint32_t max_available_underwriters,
                     uint64_t terminate_prune_delay_ms,
                     uint32_t terminate_max_consecutive_misses,
                     uint32_t terminate_max_pct_misses_24h,
                     uint64_t terminate_window_ms,
                     std::vector<chain_min_bond> req_prod_collat,
                     std::vector<chain_min_bond> req_batchop_collat,
                     std::vector<chain_min_bond> req_uw_collat);

      /// Register a new operator.
      [[sysio::action]]
      void regoperator(name account,
                       opp::types::OperatorType type,
                       bool is_bootstrapped);

      /// Operator-callable: lock CORE_SYM tokens directly as the operator's
      /// WIRE-side collateral. The tokens transfer in the same transaction;
      /// the corresponding (operator, WIRE, WIRE_TOKEN) balance row is
      /// credited. Reverts on validation failure (no escrow exists yet —
      /// failure surfaces in the operator's signing tx so they can retry).
      [[sysio::action]]
      void deposit(name account, uint64_t amount);

      /// Internal: credit an outpost-side collateral row. Called by
      /// `sysio.msgch` when it dispatches an `OPERATOR_ACTION(DEPOSIT_REQUEST)`
      /// attestation that came in from an outpost.
      ///
      /// Validation failures (unknown account, slashed/terminated operator,
      /// zero amount) DO NOT revert — they are recorded in the operator's
      /// `recent_actions` log (when an entry exists) and trigger an outbound
      /// `DEPOSIT_REVERT` attestation back to the source outpost so escrowed
      /// funds get refunded to the depositor (minus the outpost-side gas
      /// penalty). Reverting would abort the entire envelope's dispatch.
      ///
      /// `actor_chain` + `actor_address` form the depositor's source-chain
      /// `ChainAddress` (refund target on DEPOSIT_REVERT). They're split
      /// here per the no-proto-messages-in-actions rule —
      /// `opp::types::ChainAddress` would leak `bytes` typedefs into the
      /// ABI. `original_message_id` is the OPP message id of the inbound
      /// DEPOSIT_REQUEST attestation — outposts match on it to scope the
      /// refund to one specific in-flight deposit.
      [[sysio::action]]
      void depositinle(name                  account,
                       sysio::slug_name       chain_code,
                       sysio::slug_name       token_code,
                       uint64_t              amount,
                       opp::types::ChainKind actor_chain,
                       std::vector<char>     actor_address,
                       checksum256           original_message_id);

      /// Operator-callable: queue a WIRE-direct collateral withdrawal subject
      /// to the WITHDRAW_WAIT_EPOCHS wait. Outpost-held collateral is
      /// withdrawn by calling the holding outpost's withdraw entry point —
      /// the outpost emits an OPERATOR_ACTION(WITHDRAW_REQUEST) inbound that
      /// reaches `withdrawinle` instead. Validation failures DO NOT revert —
      /// they are appended to the operator's `recent_actions` ring buffer
      /// (matching the OPP-dispatched path's failure semantics).
      [[sysio::action]]
      void withdraw(name account, uint64_t amount);

      /// Internal: queue an outpost-side collateral withdrawal. Called by
      /// `sysio.msgch` when it dispatches an `OPERATOR_ACTION(WITHDRAW_REQUEST)`
      /// attestation that came in from an outpost. Subject to the
      /// WITHDRAW_WAIT_EPOCHS wait. Validation failures are logged on the
      /// operator's `recent_actions` ring buffer; the dispatch tx commits
      /// so other attestations in the same envelope still apply.
      [[sysio::action]]
      void withdrawinle(name account, sysio::slug_name chain_code, sysio::slug_name token_code, uint64_t amount);

      /// Operator-callable: cancel a previously-queued withdrawal before it
      /// flushes. The reserved amount rejoins the operator's `available()`.
      [[sysio::action]]
      void cancelwtdw(name account, uint64_t request_id);

      /// Internal: drain matured rows from `withdraw_queue`. Called inline
      /// from `sysio.epoch::advance` each tick.
      [[sysio::action]]
      void flushwtdw(uint32_t current_epoch);

      /// Read-only rollup of the operator's spendable balance for a given
      /// (chain, token_kind). Returns 0 if the operator is SLASHED /
      /// TERMINATED, or if no balance row exists. Otherwise returns
      /// `balance - sum(active locks on uwrit) - sum(pending withdraws)`.
      [[sysio::action, sysio::read_only]]
      uint64_t available(name account, sysio::slug_name chain_code, sysio::slug_name token_code);

      /// Type-specific eligibility transitions. Called inline from the
      /// deposit / withdraw / slash / terminate paths when an operator's
      /// available balance crosses the role minimum.
      [[sysio::action]]
      void processprod(name account, bool was_eligible, bool is_eligible);

      [[sysio::action]]
      void processbatch(name account, bool was_eligible, bool is_eligible);

      [[sysio::action]]
      void processuw(name account, bool was_eligible, bool is_eligible);

      /// Slash an operator. Permanent. Called by `sysio.chalg`. Routes the
      /// **immediately slashable** portion (`balance - sum(active locks)`) to
      /// the matching LP on each chain via OPERATOR_ACTION(SLASH) attestations.
      /// The locked portion stays in opreg's balance and is slashed at lock-
      /// release time by `sysio.uwrit::release` (deferred-slash).
      [[sysio::action]]
      void slash(name account, std::string reason);

      /// Called by `sysio.uwrit::release` when an underwriter lock resolves.
      /// Opreg consults its own current status for the operator and routes
      /// the released amount appropriately:
      ///   * SLASHED   — decrement balance, emit OPERATOR_ACTION(SLASH) (deferred-slash).
      ///   * TERMINATED — decrement balance, emit WITHDRAW_REMIT (deferred-remit
      ///                  to the operator's authex destination).
      ///   * else      — no-op (balance was never decremented at lock time;
      ///                  the freed amount naturally reappears in `available()`
      ///                  the moment uwrit erases the lock row).
      [[sysio::action]]
      void releaselock(name account, sysio::slug_name chain_code, sysio::slug_name token_code, uint64_t amount);

      /// Record per-batch-op delivery hit/miss for the rolling 24h buffer.
      /// Called inline from `sysio.epoch::advance` after each delivery cycle.
      /// Each write also sweeps up to `MAX_DELLOG_PRUNE_PER_WRITE` rows that
      /// have aged out of the rolling termination window, keeping the buffer
      /// bounded without a dedicated crank.
      [[sysio::action]]
      void recorddel(name account, uint32_t epoch, bool delivered);

      /// Evaluate the rolling 24h delivery buffer for an operator. If it
      /// breaches the threshold (>3 consecutive misses OR >5% missed in
      /// the trailing 24h), inline `terminate(...)`. Called by
      /// `sysio.epoch::advance` after every `recorddel`.
      [[sysio::action]]
      void termcheck(name account);

      /// Administratively terminate an operator. Status -> TERMINATED. Each
      /// (chain, token_kind) balance is remitted to the operator's authex
      /// destination via `OPERATOR_ACTION(WITHDRAW_REMIT)` attestations.
      /// Locks remain alive — `sysio.uwrit::release` will deferred-remit
      /// each lock at its natural release time.
      [[sysio::action]]
      void terminate(name account, std::string reason);

      /// Prune terminated operator rows past the delay, plus delivery-log
      /// rows that have aged out of the rolling termination window.
      /// Permissionless.
      [[sysio::action]]
      void prune();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Per-(chain_code, token_code) aggregate balance row. The locked portion
      /// is implied by `sysio.uwrit::locks` (consulted by `available()`); the
      /// pending-withdraw portion is implied by this contract's
      /// `withdraw_queue` (also consulted by `available()`).
      struct balance_entry {
         sysio::slug_name  chain_code;
         sysio::slug_name  token_code;
         uint64_t         balance         = 0;
         uint64_t         last_updated_ms = 0;

         SYSLIB_SERIALIZE(balance_entry, (chain_code)(token_code)(balance)(last_updated_ms))
      };

      /// Operators primary key: account name value.
      struct operator_key {
         uint64_t account;
         uint64_t primary_key() const { return account; }
         SYSLIB_SERIALIZE(operator_key, (account))
      };

      /// Operator entry — the primary roster.
      struct [[sysio::table("operators")]] operator_entry {
         name                                          account;
         opp::types::OperatorType                      type;
         opp::types::OperatorStatus                    status;
         bool                                          is_bootstrapped = false;
         std::vector<balance_entry>                    balances;
         uint64_t                                      registered_at   = 0;
         uint64_t                                      available_at    = 0;
         /// Generic last-mutation timestamp. Bumped any time the operator
         /// row materially changes (status flip, balance write, slash,
         /// termination, etc). Distinct from `terminated_at` / `available_at`,
         /// which are moment-of-event stamps; `updated_at` is "latest touch".
         uint64_t                                      updated_at      = 0;
         uint64_t                                      terminated_at   = 0;
         std::string                                   status_reason;
         /// Newest-first ring buffer (cap = MAX_RECENT_ACTIONS) of every
         /// OperatorAction the depot has applied or rejected for this
         /// operator. `append_action_log` does the truncate-on-overflow.
         /// Operators read this to diagnose dropped DEPOSIT / WITHDRAW_REQUEST
         /// requests and to see slash entries (with reason).
         std::vector<opp::attestations::OperatorActionLog> recent_actions;

         uint64_t by_type()   const { return magic_enum::enum_integer(type); }
         uint64_t by_status() const { return magic_enum::enum_integer(status); }

         SYSLIB_SERIALIZE(operator_entry,
            (account)(type)(status)(is_bootstrapped)(balances)
            (registered_at)(available_at)(updated_at)(terminated_at)
            (status_reason)(recent_actions))
      };

      using operators_t = sysio::kv::table<"operators"_n, operator_key, operator_entry,
         sysio::kv::index<"bytype"_n,
            sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_type>>,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_status>>
      >;

      /// Operator registry configuration singleton.
      struct [[sysio::table("opconfig")]] op_config {
         std::vector<chain_min_bond> req_prod_collat;
         std::vector<chain_min_bond> req_batchop_collat;
         std::vector<chain_min_bond> req_uw_collat;
         uint32_t max_available_producers          = 21;
         uint32_t max_available_batch_ops          = 63;
         uint32_t max_available_underwriters       = 21;
         uint64_t terminate_prune_delay_ms         = 86400000; // 24hrs
         uint32_t terminate_max_consecutive_misses = DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES;
         uint32_t terminate_max_pct_misses_24h     = DEFAULT_TERMINATE_MAX_PCT_MISSES_24H;
         uint64_t terminate_window_ms              = DEFAULT_TERMINATE_WINDOW_MS;

         SYSLIB_SERIALIZE(op_config,
            (req_prod_collat)(req_batchop_collat)(req_uw_collat)
            (max_available_producers)(max_available_batch_ops)(max_available_underwriters)
            (terminate_prune_delay_ms)
            (terminate_max_consecutive_misses)(terminate_max_pct_misses_24h)(terminate_window_ms))
      };

      using opconfig_t = sysio::kv::global<"opconfig"_n, op_config>;

      /// Pending-withdraw row keyed by sequential `request_id`. Drained by
      /// `flushwtdw` once `eligible_at_epoch <= current_epoch`. Subtracted by
      /// `available()` so the queued amount can't be double-used.
      struct withdraw_key {
         uint64_t request_id;
         uint64_t primary_key() const { return request_id; }
         SYSLIB_SERIALIZE(withdraw_key, (request_id))
      };

      struct [[sysio::table("wtdwqueue")]] withdraw_request {
         uint64_t          request_id          = 0;
         name              account;
         sysio::slug_name   chain_code;
         sysio::slug_name   token_code;
         uint64_t          amount              = 0;
         uint32_t          eligible_at_epoch   = 0;
         uint32_t          requested_at_epoch  = 0;

         /// Composite (account, chain_code, token_code) for available() rollup.
         /// 3 × uint64 = 192 bits → checksum256.
         checksum256 by_account_ck() const {
            std::array<uint8_t, 24> buf{};
            uint64_t acc_v = account.value;
            std::memcpy(buf.data() +  0, &acc_v,            8);
            std::memcpy(buf.data() +  8, &chain_code.value, 8);
            std::memcpy(buf.data() + 16, &token_code.value, 8);
            return sysio::sha256(reinterpret_cast<const char*>(buf.data()), buf.size());
         }
         /// Eligibility cursor for flushwtdw.
         uint64_t  by_eligible() const { return static_cast<uint64_t>(eligible_at_epoch); }
         /// Per-account scan (cancelwtdw lookup convenience).
         uint64_t  by_account()  const { return account.value; }

         SYSLIB_SERIALIZE(withdraw_request,
            (request_id)(account)(chain_code)(token_code)(amount)
            (eligible_at_epoch)(requested_at_epoch))
      };

      // Per plan §B.2 (mirrors sysio.uwrit::locks): split-index approach.
      // Antelope KV secondary indexes use fixed-width integer keys; the
      // 3-uint64 (account, chain_code, token_code) composite is computed on
      // the row as `by_account_ck()` for cross-contract comparisons but is
      // NOT a table-managed secondary index. Callers scan `byaccount`
      // (uint64) and filter (chain_code, token_code) in memory — cheap
      // because pending-withdraw counts per account are O(1)-ish.
      using wtdwqueue_t = sysio::kv::table<"wtdwqueue"_n, withdraw_key, withdraw_request,
         sysio::kv::index<"byeligible"_n,
            sysio::const_mem_fun<withdraw_request, uint64_t, &withdraw_request::by_eligible>>,
         sysio::kv::index<"byaccount"_n,
            sysio::const_mem_fun<withdraw_request, uint64_t, &withdraw_request::by_account>>
      >;

      /// Per-batch-op rolling delivery buffer. One row per (operator, epoch)
      /// recording whether the operator delivered on schedule. Rows older
      /// than `TERMINATE_WINDOW_MS` are discarded by `prune` / on-write.
      struct delivery_key {
         uint64_t log_id;
         uint64_t primary_key() const { return log_id; }
         SYSLIB_SERIALIZE(delivery_key, (log_id))
      };

      struct [[sysio::table("dellog")]] delivery_log_entry {
         uint64_t  log_id   = 0;
         name      account;
         uint32_t  epoch    = 0;
         bool      delivered = false;
         uint64_t  ts_ms    = 0;

         /// Composite (account, ts_ms) for the rolling 24h scan.
         uint128_t by_account_ts() const {
            return (static_cast<uint128_t>(account.value) << 64) | ts_ms;
         }

         SYSLIB_SERIALIZE(delivery_log_entry, (log_id)(account)(epoch)(delivered)(ts_ms))
      };

      using dellog_t = sysio::kv::table<"dellog"_n, delivery_key, delivery_log_entry,
         sysio::kv::index<"byaccountts"_n,
            sysio::const_mem_fun<delivery_log_entry, uint128_t, &delivery_log_entry::by_account_ts>>
      >;

      /// Singleton holding the next-issued `request_id` / `log_id`. Keeps
      /// the auto-increment monotonic across action calls.
      struct [[sysio::table("opcounters")]] op_counters {
         uint64_t next_withdraw_id = 1;
         uint64_t next_dellog_id   = 1;

         SYSLIB_SERIALIZE(op_counters, (next_withdraw_id)(next_dellog_id))
      };

      using opcounters_t = sysio::kv::global<"opcounters"_n, op_counters>;

   private:

      using OperatorType    = opp::types::OperatorType;
      using OperatorStatus  = opp::types::OperatorStatus;
      using ChainKind       = opp::types::ChainKind;
      using ChainAddress    = opp::types::ChainAddress;
      using TokenKind       = opp::types::TokenKind;
      using TokenAmount     = opp::types::TokenAmount;
      using AttestationType = opp::types::AttestationType;
   };

} // namespace sysio
