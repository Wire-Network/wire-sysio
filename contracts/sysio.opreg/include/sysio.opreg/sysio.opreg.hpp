#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   /**
    * @brief sysio.opreg — operator registry on WIRE.
    *
    * Holds the **authoritative** bond ledger for every operator type
    * (producers, batch operators, underwriters, plus their standby tiers).
    * Per the corrected ledger model in
    * `CLAUDE-WIRE-OPERATOR-COLLATERAL-IMPL-PLAN.md`:
    *
    * - One aggregate balance per (operator, chain, token_kind) — NOT a
    *   vector of stake entries, NOT a locked/available/amount trio.
    * - `deposit` adds; `queue_withdraw` enqueues a 2-epoch delayed
    *   subtraction; `slash` zeros the unlocked portion immediately and
    *   leaves locks for `sysio.uwrit::release` to slash deferred.
    * - Underwriter locks live entirely in `sysio.uwrit::locks` — opreg
    *   reads them via a kv::table mirror to compute `available()`.
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

      // Rolling delivery-buffer thresholds for batch-op termination. Per the
      // plan §1: missing a delivery is NOT a slash; consistent missing IS
      // grounds for administrative termination.
      static constexpr uint32_t TERMINATE_MAX_CONSECUTIVE_MISSES = 3;
      static constexpr uint32_t TERMINATE_MAX_PCT_MISSES_24H     = 5;   // percent
      static constexpr uint64_t TERMINATE_WINDOW_MS              = 24ULL * 60 * 60 * 1000;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set operator registry configuration.
      [[sysio::action]]
      void setconfig(uint32_t max_available_producers,
                     uint32_t max_available_batch_ops,
                     uint32_t max_available_underwriters,
                     uint64_t terminate_prune_delay_ms);

      /// Register a new operator.
      [[sysio::action]]
      void regoperator(name account,
                       opp::types::OperatorType type,
                       bool is_bootstrapped);

      /// Operator-callable: stake CORE_SYM tokens directly into their WIRE-side
      /// bond. The tokens transfer in the same transaction; the corresponding
      /// (operator, WIRE, WIRE_TOKEN) balance row is credited.
      [[sysio::action]]
      void wirestake(name account, uint64_t amount);

      /// Internal: credit an outpost-side bond. Called by `sysio.msgch` when
      /// it dispatches an `OPERATOR_ACTION(DEPOSIT)` attestation that came in
      /// from an outpost.
      [[sysio::action]]
      void deposit(name account,
                   opp::types::ChainKind chain,
                   opp::types::TokenKind token_kind,
                   uint64_t amount,
                   checksum256 outpost_tx_hash);

      /// Operator-callable: queue a WIRE-side withdrawal subject to the
      /// 2-epoch wait. Equivalent to the operator calling `withdraw` JSON-RPC
      /// on the WIRE chain. The matching tokens leave when `flushwithdraws`
      /// drains the queue at `eligible_at_epoch`.
      [[sysio::action]]
      void wireunstake(name account, uint64_t amount);

      /// Internal: queue an outpost-side withdrawal. Called by `sysio.msgch`
      /// when it dispatches an `OPERATOR_ACTION(WITHDRAW_REQUEST)` attestation
      /// that came in from an outpost. Subject to the 2-epoch wait.
      [[sysio::action]]
      void queuewtdw(name account,
                     opp::types::ChainKind chain,
                     opp::types::TokenKind token_kind,
                     uint64_t amount);

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
      uint64_t available(name account,
                         opp::types::ChainKind chain,
                         opp::types::TokenKind token_kind);

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
      /// the matching LP on each chain via SLASH_OPERATOR attestations. The
      /// locked portion stays in opreg's balance and is slashed at lock-
      /// release time by `sysio.uwrit::release` (deferred-slash).
      [[sysio::action]]
      void slash(name account, std::string reason);

      /// Called by `sysio.uwrit::release` when an underwriter lock resolves.
      /// Opreg consults its own current status for the operator and routes
      /// the released amount appropriately:
      ///   * SLASHED   — decrement balance, emit SLASH_OPERATOR (deferred-slash).
      ///   * TERMINATED — decrement balance, emit WITHDRAW_REMIT (deferred-remit
      ///                  to the operator's authex destination).
      ///   * else      — no-op (balance was never decremented at lock time;
      ///                  the freed amount naturally reappears in `available()`
      ///                  the moment uwrit erases the lock row).
      [[sysio::action]]
      void releaselock(name account,
                       opp::types::ChainKind chain,
                       opp::types::TokenKind token_kind,
                       uint64_t amount);

      /// Record per-batch-op delivery hit/miss for the rolling 24h buffer.
      /// Called inline from `sysio.epoch::advance` after each delivery cycle.
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

      /// Prune terminated operator rows past the delay. Permissionless.
      [[sysio::action]]
      void prune();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Per-(chain, token_kind) aggregate balance row. The locked portion
      /// is implied by `sysio.uwrit::locks` (consulted by `available()`); the
      /// pending-withdraw portion is implied by this contract's
      /// `withdraw_queue` (also consulted by `available()`).
      struct balance_entry {
         opp::types::ChainKind  chain;
         opp::types::TokenKind  token_kind;
         uint64_t               balance         = 0;
         uint64_t               last_updated_ms = 0;

         SYSLIB_SERIALIZE(balance_entry, (chain)(token_kind)(balance)(last_updated_ms))
      };

      /// Operators primary key: account name value.
      struct operator_key {
         uint64_t account;
         uint64_t primary_key() const { return account; }
         SYSLIB_SERIALIZE(operator_key, (account))
      };

      /// Operator entry — the primary roster.
      struct [[sysio::table("operators")]] operator_entry {
         name                          account;
         opp::types::OperatorType      type;
         opp::types::OperatorStatus    status;
         bool                          is_bootstrapped = false;
         std::vector<balance_entry>    balances;
         uint64_t                      registered_at   = 0;
         uint64_t                      available_at    = 0;
         uint64_t                      slashed_at      = 0;
         uint64_t                      terminated_at   = 0;
         std::string                   status_reason;

         uint64_t by_type()   const { return static_cast<uint64_t>(type); }
         uint64_t by_status() const { return static_cast<uint64_t>(status); }

         SYSLIB_SERIALIZE(operator_entry,
            (account)(type)(status)(is_bootstrapped)(balances)
            (registered_at)(available_at)(slashed_at)(terminated_at)(status_reason))
      };

      using operators_t = sysio::kv::table<"operators"_n, operator_key, operator_entry,
         sysio::kv::index<"bytype"_n,
            sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_type>>,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_status>>
      >;

      /// Per-(chain, token_kind) minimum-bond row in opconfig. The schema
      /// requires one entry per supported chain (WIRE / ETHEREUM / SOLANA);
      /// `min_bond` may be 0 when a particular role doesn't actually need
      /// bond on a chain, but the row must still appear so the structure is
      /// uniform across roles.
      struct chain_min_bond {
         opp::types::ChainKind  chain;
         opp::types::TokenKind  token_kind;
         uint64_t               min_bond            = 0;
         uint64_t               config_timestamp_ms = 0;

         SYSLIB_SERIALIZE(chain_min_bond, (chain)(token_kind)(min_bond)(config_timestamp_ms))
      };

      /// Operator registry configuration singleton.
      struct [[sysio::table("opconfig")]] op_config {
         std::vector<chain_min_bond> req_prod_stakes;
         std::vector<chain_min_bond> req_batchop_stakes;
         std::vector<chain_min_bond> req_uw_stakes;
         uint32_t max_available_producers    = 21;
         uint32_t max_available_batch_ops    = 63;
         uint32_t max_available_underwriters = 21;
         uint64_t terminate_prune_delay_ms   = 86400000; // 24hrs

         SYSLIB_SERIALIZE(op_config,
            (req_prod_stakes)(req_batchop_stakes)(req_uw_stakes)
            (max_available_producers)(max_available_batch_ops)(max_available_underwriters)
            (terminate_prune_delay_ms))
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
         uint64_t                request_id          = 0;
         name                    account;
         opp::types::ChainKind   chain;
         opp::types::TokenKind   token_kind;
         uint64_t                amount              = 0;
         uint32_t                eligible_at_epoch   = 0;
         uint32_t                requested_at_epoch  = 0;

         /// Composite (account, chain, token_kind) for available() rollup.
         uint128_t by_account_ck() const {
            return (static_cast<uint128_t>(account.value) << 64)
                 | (static_cast<uint64_t>(chain) << 32)
                 | static_cast<uint64_t>(token_kind);
         }
         /// Eligibility cursor for flushwtdw.
         uint64_t  by_eligible() const { return static_cast<uint64_t>(eligible_at_epoch); }
         /// Per-account scan (cancelwtdw lookup convenience).
         uint64_t  by_account()  const { return account.value; }

         SYSLIB_SERIALIZE(withdraw_request,
            (request_id)(account)(chain)(token_kind)(amount)
            (eligible_at_epoch)(requested_at_epoch))
      };

      using wtdwqueue_t = sysio::kv::table<"wtdwqueue"_n, withdraw_key, withdraw_request,
         sysio::kv::index<"byaccountck"_n,
            sysio::const_mem_fun<withdraw_request, uint128_t, &withdraw_request::by_account_ck>>,
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
