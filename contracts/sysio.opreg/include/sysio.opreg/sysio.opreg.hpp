#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <sysio.opp.common/wire_asset.hpp>
#include <sysio.opp.common/shadow_custody_types.hpp>
#include <magic_enum/magic_enum.hpp>

namespace sysio {

   /**
    * @brief sysio.opreg — operator registry on WIRE.
    *
    * Authoritative depot-native operator collateral ledger.
    * Deposits custody WIRE or shadow LIQ; withdrawals reserve balances until
    * maturity, then credit pull claims. Termination returns the remaining
    * balance through claims. Slashing removes the balance and retains custody.
    * Pending withdrawals reduce availability; shadow collateral earns yield.
    */
   class [[sysio::contract("sysio.opreg")]] opreg : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;

      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name LIQ_ACCOUNT    = "sysio.liq"_n;
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      // 2-epoch wait between `queue_withdraw` and `flushwithdraws` releasing
      // funds. Long enough that an operator who would drop below the role
      // minimum is demoted before the funds physically leave.
      static constexpr uint32_t WITHDRAW_WAIT_EPOCHS = 2;

      /// Per-(operator, chain, token) cap on pending collateral withdrawals
      /// (WIRE-376 / WNS-41). The cap covers both request entry points. A
      /// cancellation or flush erases the row and permits the next request for
      /// that collateral bucket.
      static constexpr uint32_t MAX_OUTSTANDING_WITHDRAWS_PER_COLLATERAL_BUCKET = 1;

      /// Safety rail on collateral-withdraw flush work (SEC-78 / WSA-166).
      /// MAX_WTDW_FLUSH_PER_EPOCH bounds the matured rows flushed per advance;
      /// undrained rows stay queued (collateral stays in the operator's balance
      /// until flushed) and flush a later epoch. Conservatively sized to stay
      /// well under the transaction CPU ceiling shared with the rest of
      /// advance's fan-out.
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
      /// threshold and epoch schedule. Delivery records accrue only on an
      /// operator's DUTY epochs: `sysio.epoch::advance` runs `recorddel` for
      /// the expiring group alone, and the schedule is a sliding window of
      /// `batch_op_groups` groups, so a resident operator's duty interval is
      /// `batch_op_groups` epochs. The rolling window must span the full
      /// terminating run -- `consecutive_misses` duty-epoch records -- plus
      /// one duty interval of boundary slack, or records age out (and are
      /// pruned) before `termcheck` can observe the run, leaving the
      /// consecutive rail structurally vacuous (SEC-28 residual). Operators
      /// benched by a surplus roster accrue no records while benched, so no
      /// finite window observes them; the bound governs resident operators,
      /// whose duty interval the sliding schedule pins at `batch_op_groups`
      /// epochs. Enforced from both `opreg::setconfig` (against the stored
      /// epoch config) and `sysio.epoch::setconfig` (against the stored opreg
      /// config) so no ordering of the two setters can accept a vacuous pair.
      /// Inputs are pre-bounded by those setters (misses <= 5, duration <= 30
      /// days, groups <= 255), so the product cannot overflow uint64.
      static constexpr uint64_t min_terminate_window_ms(uint32_t consecutive_misses,
                                                        uint32_t epoch_duration_sec,
                                                        uint32_t batch_op_groups) {
         constexpr uint64_t ms_per_sec = 1000;
         return (uint64_t{consecutive_misses} + 1) * batch_op_groups * epoch_duration_sec * ms_per_sec;
      }

      /// Bounded sweep sizes for delivery-log rows that have aged out of the
      /// rolling termination window. The write-path cap only has to outpace
      /// insertion (each `recorddel` adds one row); the `prune` cap clears
      /// backlog faster when cranked.
      static constexpr uint32_t MAX_DELLOG_PRUNE_PER_WRITE = 4;
      static constexpr uint32_t MAX_DELLOG_PRUNE_PER_CRANK = 64;

      static constexpr uint32_t MAX_OPERATOR_PRUNE_PER_CRANK = 20;

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
      /// Per the data-model refactor: `chain` / `token` identifiers are
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

      /// Operator-callable: bond depot-native collateral held on WIRE itself rather than
      /// escrowed on an outpost. Two kinds of token qualify:
      ///
      ///   * WIRE, custodied by `sysio.token` (`token_code == opp::wire::token_code`);
      ///   * a shadow LIQ symbol (LIQETH, LIQSOL, ...), custodied by `sysio.liq`, named by the
      ///     liq token's registry code — the `token_code` of its `sysio.liq` `stat` row, never
      ///     the symbol code.
      ///
      /// Any other `token_code` reverts ("unsupported depot-native collateral token"). The tokens
      /// move from `account` to this contract in the same transaction, under the operator's own
      /// authority, and credit the `(opp::wire::chain_code, token_code)` balance row: the chain
      /// is the depot for every depot-native row, because custody is on the depot whichever
      /// outpost the shadow mirrors. Reverts on any validation failure — nothing is escrowed
      /// yet, so the failure surfaces in the operator's signing transaction and they retry.
      ///
      /// A bonded shadow balance sits on this registry's holder row in `sysio.liq`, so the WIRE
      /// yield it earns accrues to that one row. Each bonded row checkpoints `sysio.liq`'s own
      /// index for the symbol (see `balance_entry`), so every bonded unit is attributed exactly
      /// what one unit of the registry's row earns, for as long as it stays bonded. The operator
      /// collects it with `claimyield`, then pulls the WIRE with `claimremit(account, WIRE)`.
      [[sysio::action]]
      void deposit(name account, sysio::slug_name token_code, uint64_t amount);

      [[sysio::action]]
      void withdraw(name account, sysio::slug_name token_code, uint64_t amount);

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
      /// `balance - sum(pending withdraws)`.
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

      [[sysio::action]]
      void slash(name account, std::string reason);

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

      [[sysio::action]]
      void terminate(name account, std::string reason);

      /// Auth = the claiming operator. Pull the depot-native `token_code` collateral credited by
      /// a WIRE-chain remit (withdraw flush, deferred lock release, or termination payout) in a
      /// single transfer from the token's custody contract: `sysio.token` for WIRE, `sysio.liq`
      /// for a shadow LIQ symbol (see `deposit`). Each token is claimed separately; a
      /// `token_code` this contract cannot custody reverts before any claim row is read.
      ///
      /// The remit paths credit rather than transfer because they are reachable from
      /// `sysio.epoch::advance`, which must never abort; see `remitclaims`. This is the only place
      /// such a balance becomes a transfer, and it carries the operator's own authority, so a
      /// hostile transfer-notify handler blocks nothing but this caller's own claim.
      ///
      /// Independent of the operator row: `prune` may have erased a settled TERMINATED operator,
      /// and the collateral remains claimable regardless.
      ///
      /// Shadow yield credited to a WIRE claim is always backed by WIRE the registry has already
      /// pulled from `sysio.liq` (see `yieldpool`), so paying it never draws on WIRE collateral.
      [[sysio::action]]
      void claimremit(name account, sysio::slug_name token_code);

      /// Permissionless crank: claim the WIRE yield `sysio.liq` owes this registry's holder row for
      /// the shadow symbol of depot-native `token_code` into this contract.
      ///
      /// `W` = what the registry's row is owed now (the checked `opp::shadow::owed` over that row
      /// and `sysio.liq`'s index for the symbol); the action checks `W > 0` and pushes
      /// `sysio.liq::claim(self, symbol)` under this contract's `active` authority, which reads the
      /// same row and index with the same formula in the same transaction and so transfers exactly
      /// `W`; `W` is added to `yieldpool[token_code].received` at the push, in that same
      /// transaction. Attribution to operators is fixed by their rows' checkpoints against that
      /// same index, so it does not depend on when this runs. What the sweep changes is coverage:
      /// yield is credited only out of WIRE already received, so an operator whose `claimyield`
      /// reports uncovered yield, or a termination payout that should cover everything, needs the
      /// registry's owed WIRE pulled in first.
      ///
      /// No authority is required, because the action moves nothing but yield owed to the registry
      /// into the registry. Reverts for a token this contract cannot custody, for WIRE (which earns
      /// no shadow yield), and when nothing is owed ("no yield to sweep").
      [[sysio::action]]
      void sweepyield(sysio::slug_name token_code);

      /// Permissionless: no authority is required, because the credit can only land in `account`'s
      /// own `remitclaims{account, WIRE}` row. Banked yield survives pruning and re-registration in
      /// `yielddebts`; no operator record is required to collect that debt. First, when the registry's own `sysio.liq`
      /// row is owed anything, pull it in (`opp::shadow::custody::pull`: push
      /// `sysio.liq::claim(self, symbol)` and add it to `yieldpool[token_code].received`); a failure
      /// in that claim reaches only whoever signed this action. Then settle the operator's
      /// `(opp::wire::chain_code, token_code)` row at `sysio.liq`'s current index and credit
      /// `min(owed, received - credited)` to
      /// `remitclaims{account, opp::wire::token_code}`, taking it off the row; the operator pulls
      /// the WIRE with `claimremit(account, WIRE)`. Works in every status, including SLASHED (yield
      /// earned before a slash stays claimable) and TERMINATED. Neither removing the operator row
      /// nor re-registering expires or redirects earned yield; unpaid debt never accrues new yield.
      ///
      /// Reverts with "no yield owed" when the row has earned nothing (or is not a shadow row), and
      /// with a rounding-dust message when all it is owed is dust the pool does not cover -- the
      /// action has just pulled everything `sysio.liq` owed the registry, so only later slack can
      /// cover it. The remainder stays on the row either way.
      ///
      /// Solvency: yield credits never exceed the WIRE actually received from `sysio.liq` for the
      /// symbol (`yieldpool`), so a WIRE claim holding yield is always backed by swept WIRE and never
      /// by WIRE collateral. Each bonded unit is attributed `Δindex / SCALE` of `sysio.liq`'s own
      /// index, the rate the registry's holder row earns. But `sysio.liq` floors the registry row at
      /// every one of its settles, while an operator row floors once over its whole interval, so the
      /// operator rows can be owed up to one atomic WIRE per registry settle more than was received.
      /// That dust stays owed until slack covers it: yield on shadow the registry holds for no bond
      /// (seized by a slash, awaiting `claimremit`), which is credited to no operator. The pool is
      /// first-come-first-served across operators, so a dust shortfall falls on whoever claims last,
      /// not on the row whose flooring created it. The mechanism
      /// is the `opp::shadow::custody` library; this contract supplies only the policy (which rows
      /// earn and where credits go). Debt and live yield share this same backing cap. A full WIRE
      /// remit rejects the claim atomically rather than saturating away earned value.
      [[sysio::action]]
      void claimyield(name account, sysio::slug_name token_code);

      [[sysio::action]]
      void prune();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      struct balance_entry {
         sysio::slug_name                   chain_code;
         sysio::slug_name                   token_code;
         uint64_t                           balance         = 0;
         uint64_t                           last_updated_ms = 0;
         opp::shadow::custody::position     shadow_yield;   ///< yield state of `balance` (shadow rows only)

         SYSLIB_SERIALIZE(balance_entry, (chain_code)(token_code)(balance)(last_updated_ms)(shadow_yield))
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

      struct remitclaim_key {
         uint64_t         account;
         sysio::slug_name token_code;
         SYSLIB_SERIALIZE(remitclaim_key, (account)(token_code))
      };

      /// Returned collateral and credited yield remain available indefinitely, independently of operator records.
      struct [[sysio::table("remitclaims")]] remit_claim {
         sysio::name      account;
         sysio::slug_name token_code;            ///< Depot-native token owed: WIRE or a shadow LIQ code.
         uint64_t         balance        = 0;   ///< Atomic units of `token_code` owed, not yet claimed.

         SYSLIB_SERIALIZE(remit_claim, (account)(token_code)(balance))
      };

      using remitclaims_t = sysio::kv::table<"remitclaims"_n, remitclaim_key, remit_claim>;

      /// Earned WIRE not yet covered by a shadow token's yield pool, retained across operator
      /// pruning and re-registration. No new yield accrues here; only banked debt is transferred.
      /// The wider accumulator preserves debts from repeated registrations without saturation.
      struct [[sysio::table("yielddebts")]] yield_debt {
         sysio::name      account;
         sysio::slug_name token_code;
         uint128_t        owed_wire = 0;
         SYSLIB_SERIALIZE(yield_debt, (account)(token_code)(owed_wire))
      };

      using yielddebts_t = sysio::kv::table<"yielddebts"_n, remitclaim_key, yield_debt>;

      /// Key of a `yieldpool` row: the depot-native shadow token whose yield the row accounts for.
      struct yield_pool_key {
         sysio::slug_name token_code;
         SYSLIB_SERIALIZE(yield_pool_key, (token_code))
      };

      /// Per depot-native shadow token: the `opp::shadow::custody::yield_pool` capping every yield
      /// credit at the WIRE this registry has actually pulled from `sysio.liq` for it (`received`,
      /// recorded at each pushed claim by `sweepyield` / `claimyield`) minus what it has already
      /// credited to operators' WIRE claims (`credited`). The cap keeps yield credits from ever
      /// drawing on the WIRE collateral held in the same `sysio.token` balance.
      using yieldpool_t = sysio::kv::table<"yieldpool"_n, yield_pool_key, opp::shadow::custody::yield_pool>;

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
