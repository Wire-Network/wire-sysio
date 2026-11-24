#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio/slug_name.hpp>
#include <sysio.opreg/opreg_constants.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <sysio.opp.common/wire_asset.hpp>
#include <sysio.opp.common/shadow_custody_types.hpp>
#include <magic_enum/magic_enum.hpp>

#include <optional>

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
      static constexpr name OPREG_ACCOUNT  = name{opreg_names::account};
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;

      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name LIQ_ACCOUNT    = "sysio.liq"_n;
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;

      /// Economic epochs between a withdrawal request and its release: the wall-clock wait.
      static constexpr uint32_t WITHDRAW_WAIT_ECONOMIC_EPOCHS = 2;

      /// Consensus epochs each waited-on chain must also close after the request before release (see
      /// `withdraw_request::consensus_waits`). Non-canonical deliverers are slashed when their chain's
      /// epoch closes, and a dispute pause stops that chain's index, so this guard keeps a deliverer's
      /// collateral in place until its deliveries have been judged, however far the economic clock
      /// runs ahead.
      static constexpr uint32_t WITHDRAW_WAIT_EPOCHS = 2;

      /// Per-(operator, chain, token) cap on pending collateral withdrawals
      /// (WIRE-376 / WNS-41). The cap covers both request entry points. A
      /// cancellation or flush erases the row and permits the next request for
      /// that collateral bucket.
      static constexpr uint32_t MAX_OUTSTANDING_WITHDRAWS_PER_COLLATERAL_BUCKET = 1;

      /// Safety rail on collateral-withdraw flush work (SEC-78 / WSA-166).
      /// MAX_WTDW_FLUSH_PER_EPOCH bounds the matured rows flushed per `flushwtdw`;
      /// undrained rows stay queued (collateral stays in the operator's balance
      /// until flushed) and flush a later economic epoch. Conservatively sized to
      /// stay well under the transaction CPU ceiling shared with the rest of
      /// `tickeconomy`'s fan-out.
      static constexpr uint32_t MAX_WTDW_FLUSH_PER_EPOCH = 32;

      /// Economically matured rows still held by a consensus wait that one `flushwtdw` steps over
      /// before it stops; the next flush resumes at the row it stopped on. A held row is only read,
      /// so this budget is larger than the per-flush bound on released rows.
      static constexpr uint32_t MAX_WTDW_HELD_SKIPS_PER_FLUSH = 64;

      /// Rolling delivery-buffer thresholds for evicting a batch operator from one chain. Missing a
      /// delivery is NOT a slash; consistent missing on a chain evicts the operator from that chain.
      ///
      /// All three thresholds live in `op_config` so tests can override them
      /// without recompiling. These `DEFAULT_*` constants are the values
      /// production bootstrap should install.
      static constexpr uint32_t DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES = 5;
      static constexpr uint32_t DEFAULT_TERMINATE_MAX_PCT_MISSES_24H     = 5;   // percent
      static constexpr uint64_t DEFAULT_TERMINATE_WINDOW_MS              = 24ULL * 60 * 60 * 1000;

      /// Lowest accepted threshold for the consecutive-miss eviction rail.
      static constexpr uint32_t MIN_TERMINATE_MAX_CONSECUTIVE_MISSES = 1;

      /// Highest launch-approved consecutive-miss threshold; larger values can
      /// make miss-based recovery unreachable within the intended operating envelope.
      /// Deliberately an independent literal: bumping the production default must
      /// not silently widen this security ceiling.
      static constexpr uint32_t MAX_TERMINATE_MAX_CONSECUTIVE_MISSES = 5;

      /// Lowest accepted threshold for the rolling-window percent-miss eviction rail.
      static constexpr uint32_t MIN_TERMINATE_MAX_PCT_MISSES_24H = 1;

      /// Highest accepted percent-miss threshold. A 100% threshold can never be
      /// exceeded by an all-miss window while `termcheck` uses strict breach semantics.
      static constexpr uint32_t MAX_TERMINATE_MAX_PCT_MISSES_24H = 99;

      /// Denominator of the whole-percent miss rate the percent rail compares with its threshold.
      static constexpr uint32_t PERCENT_SCALE = 100;

      /// Duty epochs recorded since the operator joined the chain before the percent rail applies; the
      /// consecutive rail applies from the first. At the default, one early miss alone stays at the default
      /// percent threshold rather than above it.
      static constexpr uint32_t DEFAULT_TERMINATE_MIN_PCT_SAMPLES =
         PERCENT_SCALE / DEFAULT_TERMINATE_MAX_PCT_MISSES_24H;

      /// Lowest accepted percent-rail sample: the rail applies from the first duty epoch.
      static constexpr uint32_t MIN_TERMINATE_MIN_PCT_SAMPLES = 1;

      /// Highest accepted percent-rail sample: one miss in this many duty epochs stays at the lowest accepted
      /// percent threshold, so no threshold needs a longer grace.
      static constexpr uint32_t MAX_TERMINATE_MIN_PCT_SAMPLES = PERCENT_SCALE / MIN_TERMINATE_MAX_PCT_MISSES_24H;

      static_assert(MIN_TERMINATE_MAX_CONSECUTIVE_MISSES <= DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES &&
                    DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES <= MAX_TERMINATE_MAX_CONSECUTIVE_MISSES,
                    "production default consecutive-miss threshold must lie inside the accepted bounds");
      static_assert(MIN_TERMINATE_MAX_PCT_MISSES_24H <= DEFAULT_TERMINATE_MAX_PCT_MISSES_24H &&
                    DEFAULT_TERMINATE_MAX_PCT_MISSES_24H <= MAX_TERMINATE_MAX_PCT_MISSES_24H,
                    "production default percent-miss threshold must lie inside the accepted bounds");
      static_assert(MIN_TERMINATE_MIN_PCT_SAMPLES <= DEFAULT_TERMINATE_MIN_PCT_SAMPLES &&
                    DEFAULT_TERMINATE_MIN_PCT_SAMPLES <= MAX_TERMINATE_MIN_PCT_SAMPLES,
                    "production default percent-rail sample must lie inside the accepted bounds");

      /// Minimum accepted `terminate_window_ms` for a given consecutive-miss
      /// threshold and one outpost chain's epoch schedule. Delivery records
      /// accrue one per DUTY epoch: `sysio.epoch::advance` records each member
      /// of the chain's expiring group once. The schedule is a
      /// sliding window of `batch_op_groups` groups, so a resident operator's
      /// duty interval is `batch_op_groups` epochs. The rolling window must
      /// span the full terminating run (`consecutive_misses + 1` records,
      /// `consecutive_misses` duty intervals end to end) plus one duty interval
      /// of boundary slack, or records age out (and are pruned) before
      /// `termcheck` can observe the run, leaving the consecutive rail
      /// structurally vacuous. Operators
      /// benched by a surplus roster accrue no records while benched, so no
      /// finite window observes them; the bound governs resident operators,
      /// whose duty interval the sliding schedule pins at `batch_op_groups`
      /// epochs. Enforced from both `opreg::setconfig` (against every chain's
      /// stored epoch config) and `sysio.epoch::setconfig` (against the stored
      /// opreg config) so no ordering of the two setters can accept a vacuous pair.
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

      /// Set operator registry configuration. The four eviction settings
      /// (`terminate_max_consecutive_misses`, `terminate_max_pct_misses_24h`,
      /// `terminate_min_pct_samples`, `terminate_window_ms`) drive `termcheck`;
      /// tests can dial them down (e.g. `terminate_max_consecutive_misses=2`,
      /// `terminate_window_ms=60_000`) to make the miss → eviction path
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
      void setconfig(uint64_t terminate_prune_delay_ms,
                     uint32_t terminate_max_consecutive_misses,
                     uint32_t terminate_max_pct_misses_24h,
                     uint32_t terminate_min_pct_samples,
                     uint64_t terminate_window_ms,
                     std::vector<chain_min_bond> req_prod_collat,
                     std::vector<chain_min_bond> req_batchop_collat,
                     std::vector<chain_min_bond> req_uw_collat);

      /// Register a new operator. A terminated account registers again only once its balances are
      /// drained and it holds no seat in the batch-operator schedule; the new registration serves no
      /// chain. An operator registering itself needs a `sysio.authex` link for every active outpost,
      /// except a batch operator, whose links `joinchain` checks chain by chain.
      [[sysio::action]]
      void regoperator(name account,
                       opp::types::OperatorType type,
                       bool is_bootstrapped);

      /// Serve outpost `chain_code`: the operator becomes SERVING there and draws new schedule seats on
      /// it. Auth = `account`, or `sysio.opreg` (the bootstrap path). Requires a batch operator that is
      /// neither SLASHED nor TERMINATED, an active outpost, and the operator's `sysio.authex` link for
      /// the chain's kind. A LEFT or EVICTED operator rejoins only once it holds no seat in that chain's
      /// schedule window; rejoining restarts the chain's miss rails.
      [[sysio::action]]
      void joinchain(name account, sysio::slug_name chain_code);

      /// Stop serving outpost `chain_code`: SERVING becomes LEFT. The operator draws no new seat there
      /// and still owes the duty of every seat it holds. Auth = `account`.
      [[sysio::action]]
      void leavechain(name account, sysio::slug_name chain_code);

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

      /// Internal: release matured rows from `wtdwqueue`. Called inline from `sysio.epoch::tickeconomy`
      /// each economic epoch with the current economic index. A row is matured once its economic wait
      /// has passed and each chain in its `consensus_waits` has reached the row's index for it. The walk
      /// runs in request order from where the last flush stopped and ends at the first row whose
      /// economic wait has not passed; it skips a row still held by a consensus wait, and stops after
      /// `MAX_WTDW_FLUSH_PER_EPOCH` released rows or `MAX_WTDW_HELD_SKIPS_PER_FLUSH` skipped ones.
      [[sysio::action]]
      void flushwtdw(uint32_t economic_epoch);

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

      /// Record a batch operator's result for one duty epoch of outpost `chain_code` in the rolling
      /// buffer. Called inline from `sysio.epoch::advance(chain_code)` once per expiring-group member,
      /// whatever its status at the close; `delivered` only if it delivered to that chain. Each write
      /// also prunes up to `MAX_DELLOG_PRUNE_PER_WRITE` rows, of any chain, that have aged out of the
      /// window, so no dedicated crank is needed.
      [[sysio::action]]
      void recorddel(name account, sysio::slug_name chain_code, uint32_t epoch, bool delivered);

      /// Evict the operator from outpost `chain_code` when its rolling buffer on that chain holds more
      /// than `terminate_max_consecutive_misses` consecutive misses, or, once it holds at least
      /// `terminate_min_pct_samples` duty epochs, a whole-percent miss rate (rounded down) above
      /// `terminate_max_pct_misses_24h`. Both rails count the chain's duty epochs
      /// recorded after the second of the operator's current registration and after the second of its
      /// latest join of the chain; a return to ACTIVE resets neither. The membership becomes EVICTED
      /// with the rail's reason, and the operator forfeits its unsettled batch duty on the chain
      /// (`sysio.system::forfeitduty`, once `initt5` has run); its status, bond, other chains and
      /// their duty are untouched. ACTIVE and UNKNOWN operators are both evaluated; bootstrapped
      /// operators and an EVICTED membership are not. Called by `sysio.epoch::advance(chain_code)`
      /// after every `recorddel`.
      [[sysio::action]]
      void termcheck(name account, sysio::slug_name chain_code);

      /// Terminate the operator globally (governance): its collateral is credited to `remitclaims`.
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

         /// Whether the operator has entered a permanent punishment or removal state (SLASHED or
         /// TERMINATED) that collateral changes must never reverse.
         bool has_terminal_status() const {
            return status == opp::types::OperatorStatus::OPERATOR_STATUS_SLASHED ||
                   status == opp::types::OperatorStatus::OPERATOR_STATUS_TERMINATED;
         }

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

      static_assert(name{opreg_names::table_operators} == "operators"_n &&
                    name{opreg_names::index_bytype} == "bytype"_n &&
                    name{opreg_names::index_bystatus} == "bystatus"_n,
                    "the host reads the roster under the shared names");

      /// A batch operator's standing on one outpost chain, shared with the host (`opreg_constants.hpp`).
      using chain_service_status = opreg_names::chain_service_status;

      /// Key of `servchains`: the chain first, so one chain's members are one contiguous range.
      struct served_chain_key {
         sysio::slug_name chain_code;
         name             account;
         SYSLIB_SERIALIZE(served_chain_key, (chain_code)(account))
      };

      /// One batch operator's membership of one outpost chain, written by `joinchain` and kept until
      /// the operator row is pruned or the account registers again.
      struct [[sysio::table("servchains")]] served_chain {
         name                              account;
         sysio::slug_name                  chain_code;
         opreg_names::chain_service_status status        = opreg_names::chain_service_status::SERVING;
         uint64_t                          joined_at_ms  = 0;   ///< the latest `joinchain` that made the row SERVING
         uint64_t                          updated_at_ms = 0;   ///< the latest status change
         std::string                       status_reason;       ///< why the row was EVICTED; empty otherwise

         uint64_t by_account() const { return account.value; }

         SYSLIB_SERIALIZE(served_chain,
            (account)(chain_code)(status)(joined_at_ms)(updated_at_ms)(status_reason))
      };

      using servchains_t = sysio::kv::table<"servchains"_n, served_chain_key, served_chain,
         sysio::kv::index<"byaccount"_n,
            sysio::const_mem_fun<served_chain, uint64_t, &served_chain::by_account>>
      >;

      static_assert(name{opreg_names::table_servchains} == "servchains"_n &&
                    name{opreg_names::index_byaccount} == "byaccount"_n,
                    "the host reads the memberships under the shared names");

      /// `account`'s membership row on outpost `chain_code`, or nullopt when it has none. Never throws.
      static std::optional<served_chain> membership_of(name account, sysio::slug_name chain_code) {
         return servchains_t(OPREG_ACCOUNT).try_get(served_chain_key{chain_code, account});
      }

      /// True when `account` may deliver the seats it holds on outpost `chain_code`: it has a membership
      /// row there that is not EVICTED. A LEFT member still delivers its remaining seats. Never throws.
      static bool delivers_on_chain(name account, sysio::slug_name chain_code) {
         const std::optional<served_chain> membership = membership_of(account, chain_code);
         return membership && membership->status != chain_service_status::EVICTED;
      }

      /// True when `account`'s membership of outpost `chain_code` is EVICTED. Never throws.
      static bool is_evicted_from(name account, sysio::slug_name chain_code) {
         const std::optional<served_chain> membership = membership_of(account, chain_code);
         return membership && membership->status == chain_service_status::EVICTED;
      }

      /// Operator registry configuration singleton.
      struct [[sysio::table("opconfig")]] op_config {
         std::vector<chain_min_bond> req_prod_collat;
         std::vector<chain_min_bond> req_batchop_collat;
         std::vector<chain_min_bond> req_uw_collat;
         uint64_t terminate_prune_delay_ms         = 86400000; // 24hrs
         uint32_t terminate_max_consecutive_misses = DEFAULT_TERMINATE_MAX_CONSECUTIVE_MISSES;
         uint32_t terminate_max_pct_misses_24h     = DEFAULT_TERMINATE_MAX_PCT_MISSES_24H;
         uint32_t terminate_min_pct_samples        = DEFAULT_TERMINATE_MIN_PCT_SAMPLES;
         uint64_t terminate_window_ms              = DEFAULT_TERMINATE_WINDOW_MS;

         SYSLIB_SERIALIZE(op_config,
            (req_prod_collat)(req_batchop_collat)(req_uw_collat)
            (terminate_prune_delay_ms)
            (terminate_max_consecutive_misses)(terminate_max_pct_misses_24h)(terminate_min_pct_samples)
            (terminate_window_ms))
      };

      using opconfig_t = sysio::kv::global<"opconfig"_n, op_config>;

      /// Pending-withdraw row keyed by sequential `request_id`. Drained by
      /// `flushwtdw` once `eligible_at_economic_epoch <= economic_epoch` and every chain in
      /// `consensus_waits` has reached its `eligible_at_epoch`. Subtracted by `available()` so the
      /// queued amount can't be double-used.
      struct withdraw_key {
         uint64_t request_id;
         uint64_t primary_key() const { return request_id; }
         SYSLIB_SERIALIZE(withdraw_key, (request_id))
      };

      /// One outpost chain a withdrawal waits on: released only once the chain's consensus epoch index
      /// has reached `eligible_at_epoch`, its index at the request + WITHDRAW_WAIT_EPOCHS.
      struct consensus_wait {
         sysio::slug_name chain_code;
         uint32_t         eligible_at_epoch = 0;

         SYSLIB_SERIALIZE(consensus_wait, (chain_code)(eligible_at_epoch))
      };

      struct [[sysio::table("wtdwqueue")]] withdraw_request {
         uint64_t          request_id          = 0;
         name              account;
         sysio::slug_name   chain_code;
         sysio::slug_name   token_code;
         uint64_t          amount              = 0;
         /// requested_at_economic_epoch + WITHDRAW_WAIT_ECONOMIC_EPOCHS
         uint32_t          eligible_at_economic_epoch  = 0;
         /// The chains the request waits on, each among those whose epoch had started at the request:
         /// for a batch operator the chains it serves or still holds a seat in, every chain for any
         /// other operator type. Empty: only the economic wait applies.
         std::vector<consensus_wait> consensus_waits;
         /// The economic epoch the grid was in at the request (`sysio.epoch::economic_epoch_index_at`), which
         /// can be ahead of the last epoch ticked.
         uint32_t          requested_at_economic_epoch = 0;

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
         /// Per-account scan (cancelwtdw lookup convenience).
         uint64_t  by_account()  const { return account.value; }

         SYSLIB_SERIALIZE(withdraw_request,
            (request_id)(account)(chain_code)(token_code)(amount)
            (eligible_at_economic_epoch)(consensus_waits)(requested_at_economic_epoch))
      };

      using wtdwqueue_t = sysio::kv::table<"wtdwqueue"_n, withdraw_key, withdraw_request,
         sysio::kv::index<"byaccount"_n,
            sysio::const_mem_fun<withdraw_request, uint64_t, &withdraw_request::by_account>>
      >;

      /// Key of the per-chain rolling delivery buffer: chain, then operator, then time, so one
      /// operator's rows on one chain are a single time-ordered range `termcheck` seeks into at its
      /// window's opening edge. `log_id` keeps two rows of the same second distinct.
      struct delivery_key {
         sysio::slug_name chain_code;
         name             account;
         uint64_t         ts_ms  = 0;
         uint64_t         log_id = 0;
         SYSLIB_SERIALIZE(delivery_key, (chain_code)(account)(ts_ms)(log_id))
      };

      /// One row per (outpost chain, operator, duty epoch), `delivered` only if the operator delivered
      /// to that chain. Rows older than `terminate_window_ms` are discarded oldest-first, across
      /// chains, by `prune` and on write.
      struct [[sysio::table("dellog")]] delivery_log_entry {
         uint64_t         log_id    = 0;
         name             account;
         sysio::slug_name chain_code;
         uint32_t         epoch     = 0;   ///< the chain's consensus epoch the row is for
         bool             delivered = false;
         uint64_t         ts_ms     = 0;

         /// Allocation order, which is also time order across every chain: the retention sweep's cursor.
         uint64_t by_log_id() const { return log_id; }

         SYSLIB_SERIALIZE(delivery_log_entry, (log_id)(account)(chain_code)(epoch)(delivered)(ts_ms))
      };

      using dellog_t = sysio::kv::table<"dellog"_n, delivery_key, delivery_log_entry,
         sysio::kv::index<"bylogid"_n,
            sysio::const_mem_fun<delivery_log_entry, uint64_t, &delivery_log_entry::by_log_id>>
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

      /// Singleton holding the next-issued `request_id` / `log_id`, which keeps the auto-increment
      /// monotonic across action calls, and the withdraw flush's resume point.
      struct [[sysio::table("opcounters")]] op_counters {
         uint64_t next_withdraw_id = 1;
         uint64_t next_dellog_id   = 1;
         /// The `request_id` the next `flushwtdw` walk starts at; 0 starts at the head of the queue.
         uint64_t flush_resume_id  = 0;

         SYSLIB_SERIALIZE(op_counters, (next_withdraw_id)(next_dellog_id)(flush_resume_id))
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
