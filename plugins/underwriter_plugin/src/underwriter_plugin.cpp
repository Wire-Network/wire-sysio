#include <fc/log/logger.hpp>
#include <fc/slug_name.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/io/raw.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/task/retry.hpp>
#include <fc/variant_object.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/endian/conversion.hpp>
#include <magic_enum/magic_enum.hpp>

#include <cassert>

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>
#include <sysio/underwriter_plugin/source_deposit_constants.hpp>
#include <sysio/underwriter_plugin/solana_source_deposit_scanner.hpp>
#include <sysio/underwriter_plugin/routing_detail.hpp>
#include <sysio/underwriter_plugin/sync_detail.hpp>
#include <sysio/depot/opreg_status.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/types/types.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>

#include <mutex>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace sysio {

using namespace chain_apis;
using namespace sysio::opp::types;
namespace eth = fc::network::ethereum;
namespace opp_att = sysio::opp::attestations;

// SEC-13/WSA-027: exact-(chain_code, token_code, reserve_code) routing /
// accounting keys, lifted to a testable detail header. These replace the
// former ChainKind/TokenKind-collapsed in-memory keys so two active chains of
// the same VM family (e.g. two EVM outposts) never share a credit bucket or a
// commit-dedup slot.
using underwriter_detail::bucket_key;
using underwriter_detail::leg_bond;
using underwriter_detail::credit_buckets;
using underwriter_detail::commit_key;

namespace {

/// Ethereum JSON-RPC method used to inspect the finalized source-chain head.
constexpr std::string_view ETH_GET_BLOCK_BY_NUMBER_METHOD = "eth_getBlockByNumber";

} // namespace

// ---------------------------------------------------------------------------
//  Underwrite request — read directly from sysio.uwrit::uwreqs table.
//
//  Per Task 3's uwrit refactor, the row carries src/dst (chain, token_kind,
//  amount) fields populated by createuwreq from the originating SwapRequest.
//  No more chasing through sysio.msgch::attestations to decode the
//  attestation payload — the data we need is right on the uwreq row.
// ---------------------------------------------------------------------------
struct uw_request {
   uint64_t                id;                // attestation ID (PK of uwreqs table)
   AttestationType         attestation_type;  // AttestationType that needs underwriting (e.g., SWAP)
   UnderwriteRequestStatus status;            // typed status (always PENDING for plugin-selected rows)
   std::string             uw_name;           // assigned underwriter ('' if unassigned, populated post race-resolve)
   ChainKind               src_chain;
   TokenKind               src_token_kind;
   uint64_t                src_amount;
   ChainKind               dst_chain;
   TokenKind               dst_token_kind;
   uint64_t                dst_amount;
   /// Per-leg slug_name triples (v6 data-model). These are the authoritative
   /// identifiers for the depot's `rcrdcommit` routing and the
   /// `UnderwriteIntentCommit` (`chain_code` / `token_code` / `reserve_code`)
   /// payload populated in `build_signed_uic_bytes`. The `ChainKind` /
   /// `TokenKind` siblings above are retained only for credit-line bucketing
   /// against `sysio.opreg::operators.balances`, which still surfaces those
   /// enums for now.
   fc::slug_name            src_chain_code{};
   fc::slug_name            src_token_code{};
   fc::slug_name            src_reserve_code{};
   fc::slug_name            dst_chain_code{};
   fc::slug_name            dst_token_code{};
   fc::slug_name            dst_reserve_code{};
   /// Variance tolerance the user attached to the original SwapRequest
   /// (as basis points). Verified in the source-deposit hash binding
   /// so the underwriter cannot collude with a stale on-chain
   /// `SwapDeposit` whose terms differ from the depot's UWREQ.
   uint32_t                 variance_tolerance_bps{};
   /// Source-chain identifier. ETH and SOL both use the 8-byte big-endian
   /// `SwapDeposit.id` minted by their source outpost's monotonic counter.
   /// Populated by `createuwreq` from `SwapRequest.source_tx_id`. The
   /// underwriter's source-deposit verifier interprets the bytes
   /// per-chain.
   std::vector<char>       source_tx_id;

   /// Depositor's address on the source chain (decoded from
   /// `SwapRequest.actor.address`). The plugin's verify_source_deposit
   /// step cross-references the source-chain tx's `from` field (ETH) or
   /// fee-payer (SOL) against this to confirm the recorded depositor
   /// actually authorized the deposit.
   std::vector<char>       depositor;

   /// Per-leg depot flags — true when the leg's chain_code is the WIRE
   /// depot's own registry row (`sysio.chains` `is_depot`). A depot leg is
   /// not underwritten: no outpost commit, no bond, no source-deposit
   /// verification. Stamped by `scan_pending_requests` from the exact
   /// depot code captured in `read_outpost_registry` — NOT inferred from
   /// `CHAIN_KIND_UNKNOWN`, which also matches genuinely-unregistered
   /// chains.
   bool                    src_is_depot = false;
   bool                    dst_is_depot = false;
};

// ---------------------------------------------------------------------------
//  Credit line — per-(chain_code, token_code) bond from sysio.opreg::operators
//
//  Reads the `balances` field (one aggregate balance per EXACT
//  (chain_code, token_code) — SEC-13/WSA-027: keyed by the v6 slug codes, NOT
//  the coarse (ChainKind, TokenKind) family, so two same-family chains hold
//  independent collateral). Note this is the RAW balance — the authoritative
//  `available` rollup also subtracts active locks + pending withdraws via
//  `sysio.opreg::available()`. v1 of the plugin treats raw balance as a
//  sufficient gate; the depot's race resolver (sysio.uwrit::try_select_winner)
//  re-validates via the rollup.
//
//  Codes held as raw `fc::slug_name::value` (uint64) to match `bucket_key`.
// ---------------------------------------------------------------------------
struct credit_line {
   uint64_t chain_code;
   uint64_t token_code;
   uint64_t balance;
};

// ---------------------------------------------------------------------------
//  Outpost endpoint — operator-supplied wiring for ONE active chain
//
//  SEC-13/WSA-027: the underwriter (one process) multiplexes every chain it
//  serves, so it holds one of these per EXACT `chain_code` — two chains of the
//  same VM family (e.g. two EVM outposts) are configured and routed
//  independently. The chain-agnostic deposit selector / instruction
//  discriminator + event decoding still come from the shared ABI / IDL files;
//  only the per-chain *addresses* and the RPC `client_id` live here.
//
//    ETH chain:  commit_addr        = OperatorRegistry address (uw_commit)
//                source_deposit_addr = SwapDeposit-emitting contract (verify)
//    SOL chain:  commit_addr = source_deposit_addr = opp-outpost program id
// ---------------------------------------------------------------------------
struct outpost_endpoint {
   ChainKind   kind = ChainKind::CHAIN_KIND_UNKNOWN;
   std::string client_id;            ///< RPC connection id in the outpost client plugin
   std::string commit_addr;          ///< ETH OperatorRegistry addr / SOL program id
   std::string source_deposit_addr;  ///< ETH SwapDeposit contract / SOL program id
};

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct underwriter_plugin::impl {
   // Configuration
   chain::name  underwriter_account;
   bool         enabled             = underwriter_defaults::enabled;
   uint32_t     scan_interval_ms    = underwriter_defaults::scan_interval_ms;
   uint32_t     action_timeout_ms   = underwriter_defaults::action_timeout_ms;
   /// SEC-13/WSA-027: per-chain outpost wiring, keyed by EXACT `chain_code`
   /// slug value. One entry per chain the underwriter serves (operator-supplied
   /// via `--underwriter-{eth,sol}-outpost`). Replaces the former single
   /// eth/sol client-id + address, which could not distinguish two chains of
   /// the same VM family.
   std::map<uint64_t, outpost_endpoint> outpost_endpoints;
   /// Per-chain external (numeric) chain id, captured from `sysio.chains`
   /// (`external_chain_id`) by `read_outpost_registry`, fed to
   /// `create_outpost_client` so each client carries its chain's real id.
   std::map<uint64_t, uint32_t>         outpost_external_chain_ids;
   /// Maximum number of recent EVM blocks one `eth_getLogs`
   /// source-deposit lookup may cover. Keeping this window bounded prevents
   /// invalid or stale deposit ids from forcing whole-history RPC scans.
   uint64_t     eth_source_deposit_lookback_blocks =
      sysio::underwriter::ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS;
   /// Configured names of the swap-deposit function (ETH) / instruction
   /// (SOL). The CHAIN-AGNOSTIC function selector / instruction discriminator
   /// are resolved at preflight from the ABI / IDL files registered with the
   /// outpost client plugins; the per-chain contract / program ADDRESS comes
   /// from `outpost_endpoints` instead (SEC-13/WSA-027 — the selector is shared
   /// across same-family chains, only the address varies). Both names required.
   std::string  eth_source_deposit_function_name;
   std::string  sol_source_deposit_instruction_name;

   /// Resolved-at-preflight CHAIN-AGNOSTIC verify-path state (the deposit
   /// selector / discriminator), derived from the outpost client plugins'
   /// ABI / IDL surfaces. Populated only after `run_preflight()` succeeds; the
   /// per-chain address is looked up from `outpost_endpoints` at verify time.
   std::vector<uint8_t> resolved_eth_source_deposit_selector;
   std::vector<uint8_t> resolved_sol_source_deposit_discriminator;

   // ── Diagnostic counters surfaced via the `/v1/underwriter/*` HTTP API.
   //
   //   `source_deposit_mismatch_count` increments every time a source-
   //   deposit verification fails for a uwreq the plugin tried to cover.
   //   The other counters tally session-level outcomes — they reset on
   //   plugin restart (no on-disk persistence; the read endpoint is for
   //   live monitoring, not historical accounting).
   uint64_t     source_deposit_mismatch_count = 0;
   uint64_t     commits_confirmed_count       = 0;
   uint64_t     commits_failed_count          = 0;
   uint64_t     uwreqs_seen_pending_count     = 0;

   // Protects `confirmed_commits`, `sol_source_deposit_scan_cursors`, and
   // the diagnostic counters from concurrent access between the
   // cron-callback (single-threaded) and the HTTP handler threads. The cron
   // callback takes the lock around mutations; the HTTP handlers take it
   // around reads.
   mutable std::mutex                stats_mutex;

   // Credit lines (read from sysio.opreg::operators each cycle)
   std::vector<credit_line> credit_lines;

   // Awareness: own status from `sysio.opreg::operators[underwriter_account]`.
   // SLASHED / TERMINATED short-circuits the relay loop. Refreshed each cycle
   // by `poll_own_status()` (mirror of batch_operator_plugin's awareness).
   bool                     is_active = true;

   // Plugin references
   chain_plugin*                     chain_plug = nullptr;
   cron_plugin*                      cron_plug  = nullptr;
   outpost_ethereum_client_plugin*   eth_plug   = nullptr;
   outpost_solana_client_plugin*     sol_plug   = nullptr;

   // Cron job handle
   cron_service::job_id_t            scan_job_id = 0;
   std::atomic<bool>                 shutting_down{false};

   /// Sync gate: `channels::irreversible_block` subscription that arms
   /// {@link run_deferred_startup} once `controller::is_synced()` holds — a LIB
   /// advance is the only event that can turn the predicate true. Unsubscribed
   /// after arming; otherwise the handle's destructor releases it (no shutdown
   /// unsubscribe needed: posted channel deliveries are drained without
   /// executing after quit, and the slot checks `shutting_down` first).
   chain::plugin_interface::channels::irreversible_block::channel_type::handle sync_gate_subscription;
   /// When the sync gate armed — bounds the preflight retry grace window.
   fc::time_point                    startup_armed_at;
   /// Bounded-grace preflight retry timer (main io_context; main-thread wait).
   std::optional<boost::asio::steady_timer> preflight_retry_timer;
   /// Deferred-startup lifecycle surfaced by the `/v1/underwriter/*` handlers
   /// (which register in `plugin_startup`, BEFORE the gate arms). Written on
   /// the main thread by {@link run_deferred_startup}; read from the HTTP
   /// handlers' read-only queue — hence atomic. Also the single source of
   /// truth for "has the deferred startup been armed" ({@link
   /// startup_attempted}) — every armed attempt leaves `waiting_for_sync`
   /// before it returns, so no separate armed flag exists to drift.
   std::atomic<underwriter_detail::startup_state> gate_state{
      underwriter_detail::startup_state::waiting_for_sync};

   /// True once the deferred startup has been armed: {@link
   /// attempt_deferred_startup} stores a non-waiting `gate_state` on every
   /// path (retrying / a terminal failure / active), so "still
   /// `waiting_for_sync`" IS "not yet armed". Guards the gate callback and
   /// its posted task against re-arming (each site also checks
   /// `shutting_down`, which covers the only path that returns before the
   /// first store).
   bool startup_attempted() const {
      return gate_state.load() != underwriter_detail::startup_state::waiting_for_sync;
   }

   // Outpost chain_kind cache: chain_code -> ChainKind
   std::map<uint64_t, ChainKind>     outpost_chain_kinds;

   /// The WIRE depot's own chain code (the `is_depot` row of
   /// `sysio.chains::chains`), captured by `read_outpost_registry` each
   /// cycle. Used for exact per-leg depot detection on uwreq rows —
   /// to/from-WIRE swaps have one depot leg that the plugin must skip
   /// (no commit, no bond) rather than error on.
   std::optional<uint64_t>           depot_chain_code;

   /// SPI handles to the configured outposts, keyed by EXACT `chain_code` slug
   /// value (SEC-13/WSA-027 — NOT `ChainKind`, so two chains of the same VM
   /// family each get their own client). Built at `plugin_startup` (after
   /// preflight) from `outpost_endpoints` via the
   /// `outpost_{ethereum,solana}_client_plugin::create_outpost_client`
   /// factories. The relay loop selects by the leg's `chain_code` and calls
   /// `outpost->uw_commit(...)` — every chain-specific concern (ABI / IDL
   /// discovery, address encoding, on-chain confirmation) lives in the
   /// concrete. Per `outpost-client-spi.md`.
   std::map<uint64_t, sysio::outpost_client_ptr> outpost_by_chain;
   /// v6 cross-walk: token slug_name → TokenKind enum. Refreshed each
   /// scan cycle by `read_credit_lines` (which reads `sysio.tokens::tokens`
   /// for the lookup); used by `scan_pending_requests` to translate the
   /// uwreq row's `src/dst_token_code` slug into the `TokenKind` the
   /// `select_coverable` bucket lookup needs.
   std::map<uint64_t, TokenKind>     token_kind_by_code;

   // ── Outstanding commit tracking (one entry per CONFIRMED leg) ───────
   // Per `feedback`: an underwriter that confirmed a commit tx for a leg
   // should NOT resubmit on the next scan cycle. The de-dup key is the EXACT
   // v6 leg identity `(uwreq_id, chain_code, token_code, reserve_code)`
   // (`underwriter_detail::commit_key`) — NOT the coarse (ChainKind,
   // TokenKind), so two legs differing only by chain or reserve are tracked
   // independently (SEC-13/WSA-027). The set is pruned at the end of each scan
   // cycle to drop entries whose uwreq is no longer PENDING (the depot has
   // resolved the race), keeping the set bounded.
   std::set<commit_key>              confirmed_commits;

   /// Per-pending-UWREQ Solana scan cursors. Persisting `before` across
   /// retry attempts and outer scan cycles prevents unrelated newer
   /// opp-outpost traffic from keeping a valid source deposit forever
   /// outside the newest-page window. Terminal negative entries remain until
   /// the UWREQ leaves PENDING so failed scans do not re-walk full history or
   /// re-bump mismatch counters every outer scan cycle.
   underwriter::solana_source_deposit_scan_cursor_map sol_source_deposit_scan_cursors;

   /// Returns the current SOL source-deposit scan cursor for `key`, creating
   /// one at the newest page when this is the first scan attempt for a UWREQ.
   underwriter::solana_source_deposit_scan_cursor get_or_create_sol_source_deposit_scan_cursor(
      const underwriter::solana_source_deposit_scan_key& key) {
      std::lock_guard lk{stats_mutex};
      return underwriter::get_or_create_solana_source_deposit_scan_cursor(
         sol_source_deposit_scan_cursors, key);
   }

   /// Returns a terminal SOL source-deposit failure for `key`, when one has
   /// already been recorded while the UWREQ remains PENDING.
   std::optional<underwriter::solana_source_deposit_scan_cursor>
   get_sol_source_deposit_terminal_failure(
      const underwriter::solana_source_deposit_scan_key& key) const {
      std::lock_guard lk{stats_mutex};
      return underwriter::get_solana_source_deposit_terminal_failure(
         sol_source_deposit_scan_cursors, key);
   }

   /// Persists the next SOL source-deposit `before` cursor after a clean page
   /// that did not contain the target marker.
   underwriter::solana_source_deposit_scan_cursor advance_sol_source_deposit_scan_cursor(
      const underwriter::solana_source_deposit_scan_key& key,
      std::string                        before,
      size_t                             signature_count) {
      std::lock_guard lk{stats_mutex};
      return underwriter::advance_solana_source_deposit_scan_cursor(
         sol_source_deposit_scan_cursors, key, std::move(before), signature_count);
   }

   /// Records a terminal SOL source-deposit failure while keeping the scan
   /// state until the UWREQ leaves PENDING.
   underwriter::solana_source_deposit_terminal_failure_result
   record_sol_source_deposit_terminal_failure(
      const underwriter::solana_source_deposit_scan_key& key,
      std::string reason,
      size_t signature_count) {
      std::lock_guard lk{stats_mutex};
      return underwriter::record_solana_source_deposit_terminal_failure(
         sol_source_deposit_scan_cursors, key, std::move(reason), signature_count);
   }

   /// Drops SOL source-deposit scan progress for a UWREQ once it either
   /// matches or leaves the PENDING set.
   void erase_sol_source_deposit_scan_cursor(
      const underwriter::solana_source_deposit_scan_key& key) {
      std::lock_guard lk{stats_mutex};
      underwriter::erase_solana_source_deposit_scan_cursor(
         sol_source_deposit_scan_cursors, key);
   }

   // -----------------------------------------------------------------------
   //  Table read helper
   // -----------------------------------------------------------------------

   /// Thin delegate to `chain_plugin::read_table_rows`, which posts the scan onto the app executor's read_only queue
   /// so chainbase iteration runs during the controller's read window instead of racing with block apply.
   sysio::chain_apis::read_only::get_table_rows_result
   read_table(sysio::chain_apis::read_only::get_table_rows_params p) {
      return chain_plug->read_table_rows(std::move(p), fc::milliseconds(action_timeout_ms),
                                         "underwriter", shutting_down);
   }

   /// Shortcut for the common scan shape: walk every row from a code/scope/table and return unwrapped values.
   sysio::chain_apis::read_only::get_table_rows_result
   read_all(std::string_view code, std::string_view scope, std::string_view table) {
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(code);
      p.scope       = scope;
      p.table       = table;
      p.all_rows    = true;
      p.values_only = true;
      return read_table(std::move(p));
   }

   // -----------------------------------------------------------------------
   //  Pre-flight checks — unconditional, no dev escape hatch
   //
   //  Verifies that the configured `underwriter_account` is set up to
   //  participate in the race BEFORE any cron job is scheduled. Failure
   //  prevents the scan loop from starting; the cluster bootstrap is
   //  responsible for establishing whatever state is missing.
   //
   //  Checks (all required):
   //    1. Operator exists in `sysio.opreg::operators` and status == ACTIVE.
   //    2. Every ACTIVE outpost chain in `sysio.chains::chains` has a
   //       configured `--underwriter-{eth,sol}-outpost` endpoint of the
   //       matching VM family. The served set is derived from the registry
   //       while the outpost clients are built from config, so a gap would
   //       let the scan loop pick a request it cannot fully commit. Inactive
   //       (not-yet-activated) chains are excluded, so registering a future
   //       chain does not block startup before its endpoint is configured.
   //    3. `sysio.authex::links` covers every chain in the
   //       `sysio.chains::chains` registered set; without an authex link
   //       for a chain the underwriter cannot sign a commit on that chain.
   //    4. Non-zero balance on at least one TokenKind for every registered
   //       outpost chain.
   //
   //  Returns true on success. On any failure logs a structured `elog`
   //  naming the specific missing item, and returns false. The caller
   //  (plugin_startup) treats false as "do not schedule the cron".
   //
   //  Per `feedback_no_dev_escape_hatches.md`: NO `--strict=false` option,
   //  no dev fallback. Dev clusters that fail preflight are bootstrap bugs
   //  to fix in `wire-tools-ts/packages/test-cluster-tool`, not workarounds
   //  to ship in the plugin.
   // -----------------------------------------------------------------------
   bool run_preflight() {
      // -- Check 1: operator status --
      bool found_op = false;
      bool active   = false;
      {
         auto rows = read_all("sysio.opreg", "sysio.opreg", "operators");
         for (auto& row : rows.rows) {
            auto obj = row.get_object();
            if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
            found_op = true;
            auto status = obj["status"].as<OperatorStatus>();
            active = (status == OperatorStatus::OPERATOR_STATUS_ACTIVE);
            break;
         }
      }
      if (!found_op) {
         elog("underwriter preflight: account {} not registered in sysio.opreg::operators",
              underwriter_account.to_string());
         return false;
      }
      if (!active) {
         // Non-fatal: depot's collateral-deposit + meets_role_min activation
         // can land AFTER the underwriter node is up (cluster bootstrap
         // orders it that way — Phase 11d deposits collateral on the
         // already-running underwriter node to flip uwrit.a → ACTIVE).
         // The scan loop's `poll_own_status()` re-checks every cycle and
         // skips work until ACTIVE, so we just log and let the cron job
         // pick up the activation transition.
         ilog("underwriter preflight: account {} not yet OPERATOR_STATUS_ACTIVE — "
              "scan loop will start polling and wait for activation",
              underwriter_account.to_string());
      }

      // Populate the outpost-chain cache (also used by the scan loop) so
      // the link + balance coverage checks know what to look for.
      read_outpost_registry();

      if (outpost_chain_kinds.empty()) {
         elog("underwriter preflight: no outposts registered in sysio.chains::chains — "
              "nothing to commit against");
         return false;
      }

      // -- Check 2: outpost-client wiring covers every active chain --
      //
      // The served set is `outpost_chain_kinds` (ACTIVE non-depot chains only,
      // per `read_outpost_registry`) as consumed by `is_available` /
      // `select_coverable`, but the outpost_client handles are built only from
      // operator-supplied `--underwriter-{eth,sol}-outpost` config
      // (`outpost_endpoints`). An active chain that is unconfigured, or
      // configured under the wrong VM family, would let the scan loop SELECT a
      // request for it and land one leg before discovering the other leg has no
      // (or a wrong-kind) client (SEC-13/WSA-027). Fail closed here so a
      // misconfigured underwriter never starts committing partial swaps.
      {
         std::map<uint64_t, int> registered_kinds;
         for (const auto& [code, kind] : outpost_chain_kinds)
            registered_kinds[code] = magic_enum::enum_integer(kind);
         std::map<uint64_t, int> configured_kinds;
         for (const auto& [code, ep] : outpost_endpoints)
            configured_kinds[code] = magic_enum::enum_integer(ep.kind);

         if (auto gap = underwriter_detail::find_endpoint_coverage_gap(
                registered_kinds, configured_kinds)) {
            const auto code_str = fc::slug_name{gap->chain_code}.to_string();
            // Re-derive the typed ChainKind names from the source maps rather
            // than reverse-casting the raw ints; the generated `_Name` helper
            // is the CLAUDE.md-mandated spelling for proto enums.
            const ChainKind reg_kind = outpost_chain_kinds.at(gap->chain_code);
            if (gap->config_kind == underwriter_detail::endpoint_coverage_gap::unconfigured) {
               elog("underwriter preflight: active outpost chain {} (kind={}) has no "
                    "--underwriter-eth-outpost / --underwriter-sol-outpost entry; configure "
                    "one endpoint for every active outpost chain",
                    code_str, std::string{sysio::opp::types::ChainKind_Name(reg_kind)});
            } else {
               const ChainKind cfg_kind = outpost_endpoints.at(gap->chain_code).kind;
               elog("underwriter preflight: outpost chain {} is registered as kind={} but "
                    "configured as kind={}; fix --underwriter-*-outpost to match the registry",
                    code_str,
                    std::string{sysio::opp::types::ChainKind_Name(reg_kind)},
                    std::string{sysio::opp::types::ChainKind_Name(cfg_kind)});
            }
            return false;
         }
      }

      // -- Check 3: authex link coverage per outpost chain --
      std::set<ChainKind> linked_chains;
      {
         auto rows = read_all("sysio.authex", "sysio.authex", "links");
         for (auto& row : rows.rows) {
            auto obj = row.get_object();
            if (chain::name(obj["username"].as_string()) != underwriter_account) continue;
            linked_chains.insert(obj["chain_kind"].as<ChainKind>());
         }
      }
      for (auto& [chain_code, chain_kind] : outpost_chain_kinds) {
         if (!linked_chains.count(chain_kind)) {
            elog("underwriter preflight: missing sysio.authex link for outpost {} "
                 "(chain_kind={}) — bootstrap must call sysio.authex::createlink for "
                 "this account on every outpost chain",
                 chain_code,
                 std::string{sysio::opp::types::ChainKind_Name(chain_kind)});
            return false;
         }
      }

      // -- Check 4: non-zero RAW balance per outpost chain --
      //
      // Reads `sysio.opreg::operators[underwriter].balances` directly and
      // does NOT subtract active locks or pending withdraws. An underwriter
      // with all collateral currently locked is still eligible to remain
      // running — the moment their locks expire (via the chklocks sweep)
      // they can underwrite the next round. Deducting locks here would
      // false-fail a healthy underwriter who is in the middle of an active
      // race.
      std::map<int, uint64_t> raw_balance_by_chain;
      {
         auto ops_rows = read_all("sysio.opreg", "sysio.opreg", "operators");
         for (auto& row : ops_rows.rows) {
            auto obj = row.get_object();
            if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
            if (!obj.contains("balances") || !obj["balances"].is_array()) break;
            for (auto& bal_entry : obj["balances"].get_array()) {
               auto be = bal_entry.get_object();
               if (!be.contains("chain") || !be.contains("balance")) continue;
               const int     chain   = magic_enum::enum_integer(be["chain"].as<ChainKind>());
               const uint64_t balance = be["balance"].as_uint64();
               raw_balance_by_chain[chain] += balance;
            }
            break;
         }
      }
      for (auto& [chain_code, chain_kind] : outpost_chain_kinds) {
         const int ck = magic_enum::enum_integer(chain_kind);
         auto it = raw_balance_by_chain.find(ck);
         if (it == raw_balance_by_chain.end() || it->second == 0) {
            // Non-fatal: collateral may be deposited AFTER plugin startup
            // (cluster bootstrap orders it that way — Phase 11d deposits
            // on the already-running underwriter node to flip uwrit.a →
            // ACTIVE). The scan loop's `is_available()` re-checks every
            // cycle and skips work until the operator has positive
            // balance on every active chain; the plugin then naturally
            // joins the underwriter race once activation lands.
            ilog("underwriter preflight: zero raw balance on outpost {} "
                 "(chain_kind={}) — scan loop will poll for collateral "
                 "deposit and begin committing once available",
                 chain_code,
                 std::string{sysio::opp::types::ChainKind_Name(chain_kind)});
         }
      }

      // -- Check 5: required CLI options + ABI/IDL resolution --
      //
      // The verify_source_deposit path identifies the swap-deposit
      // function (ETH) / instruction (SOL) by NAME. The contract
      // address + function selector + instruction discriminator are
      // resolved from the ABI / IDL files registered with the outpost
      // client plugins (avoiding duplicate plugin options that would
      // need to be kept in sync with `--ethereum-abi-file` /
      // `--solana-idl-file`).
      if (eth_source_deposit_function_name.empty()) {
         elog("underwriter preflight: --underwriter-eth-source-deposit-function is required");
         return false;
      }
      if (sol_source_deposit_instruction_name.empty()) {
         elog("underwriter preflight: --underwriter-sol-source-deposit-instruction is required");
         return false;
      }

      // ETH: walk every loaded ABI for a `function` contract whose name
      // matches. The match yields keccak256(signature) → the CHAIN-AGNOSTIC
      // 4-byte selector (identical on every EVM chain running the same
      // contract). The per-chain deployed contract ADDRESS comes from
      // `outpost_endpoints` (SEC-13/WSA-027), so the ABI's `contract_address`
      // field is no longer consulted here.
      {
         resolved_eth_source_deposit_selector.clear();
         bool found = false;
         for (const auto& [path, contracts] : eth_plug->get_abi_files()) {
            for (const auto& c : contracts) {
               if (c.type != fc::network::ethereum::abi::invoke_target_type::function) continue;
               if (c.name != eth_source_deposit_function_name) continue;
               const auto sel_hash = fc::network::ethereum::abi::to_contract_function_selector(c);
               const uint8_t* sp = sel_hash.data();
               resolved_eth_source_deposit_selector.assign(sp, sp + 4);
               dlog("underwriter preflight: ETH deposit selector for '{}' resolved from ABI '{}'",
                    eth_source_deposit_function_name, path.string());
               found = true;
               break;
            }
            if (found) break;
         }
         if (!found) {
            elog("underwriter preflight: no ETH ABI entry found for function '{}'; "
                 "pass --ethereum-abi-file pointing at the ABI that declares it",
                 eth_source_deposit_function_name);
            return false;
         }
      }

      // SOL: walk every loaded IDL for the named instruction. The IDL parser
      // populates each instruction's 8-byte anchor discriminator
      // (`sha256("global:<instruction_name>")[0..8]`) — the CHAIN-AGNOSTIC
      // identifier the verify path matches. The per-chain program id comes from
      // `outpost_endpoints` (SEC-13/WSA-027), not the IDL's `address`.
      {
         resolved_sol_source_deposit_discriminator.clear();
         bool found = false;
         for (const auto& [path, programs] : sol_plug->get_idl_files()) {
            for (const auto& p : programs) {
               if (const auto* ix = p.find_instruction(sol_source_deposit_instruction_name); ix) {
                  resolved_sol_source_deposit_discriminator.assign(
                     ix->discriminator.begin(), ix->discriminator.end());
                  dlog("underwriter preflight: SOL deposit discriminator for '{}' resolved from IDL '{}'",
                       sol_source_deposit_instruction_name, path.string());
                  found = true;
                  break;
               }
            }
            if (found) break;
         }
         if (!found) {
            elog("underwriter preflight: no SOL IDL instruction found named '{}'; "
                 "pass --solana-idl-file pointing at the IDL that declares it",
                 sol_source_deposit_instruction_name);
            return false;
         }
      }

      // ── Check 5: signature providers — 3-provider minimum ────────────
      //
      // The underwriter signs UIC digests on WIRE (K1), and submits
      // commit transactions on each active outpost (ETH + SOL) — so
      // three sig-provider entries are required at startup:
      //   • exactly one (chain=wire, key-type=wire) — for UIC signing.
      //     more than one would silently arbitrate at `.front()`; we
      //     refuse the ambiguity.
      //   • at least one (chain=ethereum, key-type=ethereum) — for
      //     paying gas on ETH outpost commits.
      //   • at least one (chain=solana, key-type=solana) — for SOL
      //     outpost commits.
      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      auto wire_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_wire, fc::crypto::chain_key_type_wire);
      if (wire_providers.size() != 1) {
         elog("underwriter preflight: expected exactly 1 WIRE K1 signature provider, "
              "got {} — configure exactly one --signature-provider entry whose chain=wire "
              "and key-type=wire", wire_providers.size());
         return false;
      }
      auto eth_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_ethereum, fc::crypto::chain_key_type_ethereum);
      if (eth_providers.empty()) {
         elog("underwriter preflight: at least 1 Ethereum signature provider is required "
              "(chain=ethereum, key-type=ethereum) for paying gas on ETH outpost commits");
         return false;
      }
      auto sol_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_solana, fc::crypto::chain_key_type_solana);
      if (sol_providers.empty()) {
         elog("underwriter preflight: at least 1 Solana signature provider is required "
              "(chain=solana, key-type=solana) for SOL outpost commits");
         return false;
      }

      // ── Check 6: signature self-test ─────────────────────────────────
      //
      // Sign a fixed test digest with the configured provider, recover
      // the pubkey, and confirm it is on the underwriter account's
      // `owner` or `active` permission. Catches "wrong key configured"
      // at startup instead of after a live uwreq is silently rejected
      // by the depot's verify_uic_signature.
      try {
         const fc::sha256 self_test_digest = fc::sha256::hash(std::string{
            "wire.underwriter_plugin.signature_self_test.v1"});
         const fc::crypto::signature sig = wire_providers.front()->sign(self_test_digest);
         const fc::crypto::public_key recovered =
            fc::crypto::public_key::recover(sig, self_test_digest);

         // Look up the underwriter account's `owner` + `active` keys via
         // the controller's authorization manager (read window is open
         // during plugin_startup; this is a const accessor on the
         // immutable state of the account).
         auto& ctrl = chain_plug->chain();
         const auto& am = ctrl.get_authorization_manager();
         auto matches_perm = [&](chain::name perm_name) {
            try {
               const auto& p = am.get_permission({underwriter_account, perm_name});
               for (const auto& kw : p.auth.keys) {
                  if (kw.key.to_public_key() == recovered) return true;
               }
            } catch (...) {
               // Permission doesn't exist; treat as no-match.
            }
            return false;
         };
         if (!matches_perm(chain::config::owner_name) &&
             !matches_perm(chain::config::active_name)) {
            elog("underwriter preflight: signature self-test failed — the configured "
                 "WIRE K1 signature provider's recovered pubkey ({}) is not present on "
                 "the underwriter account's `owner` or `active` permission. The depot "
                 "will reject every commit signed by this provider.",
                 recovered.to_string(fc::yield_function_t{}));
            return false;
         }
      } catch (const fc::exception& e) {
         elog("underwriter preflight: signature self-test threw: {}", e.to_detail_string());
         return false;
      }

      ilog("underwriter preflight: all checks passed (account={} outposts={})",
           underwriter_account.to_string(),
           outpost_chain_kinds.size());
      return true;
   }

   // -----------------------------------------------------------------------
   //  Sync-gated startup
   // -----------------------------------------------------------------------

   /// Arm the deferred startup: runs AT MOST once (re-entry is guarded by
   /// {@link startup_attempted}), on the main thread from the sync-gate channel
   /// callback once `controller::is_synced()` holds. The body itself lives
   /// in {@link attempt_deferred_startup}, which may retry the preflight
   /// within a bounded grace and shuts the node down on terminal failure
   /// (fail-fast).
   void run_deferred_startup() {
      startup_armed_at = fc::time_point::now();
      // Release the gate subscription; the callback already unsubscribed
      // before calling here, so this is an idempotent cleanup, not the
      // load-bearing disconnect.
      sync_gate_subscription.unsubscribe();
      attempt_deferred_startup();
   }

   /// One attempt of the deferred startup body: preflight → outpost client
   /// wiring → cron scheduling. A failing preflight within
   /// `underwriter_defaults::preflight_retry_grace_ms` of the gate arming is
   /// NOT terminal — rows the harness confirmed final on the producer can
   /// land in the LOCAL irreversible state a beat after the gate arms
   /// (observed live: `regoperator` in block 405 readable 50ms after a
   /// preflight read at LIB 402), so the attempt re-schedules itself on
   /// `preflight_retry_interval_ms` until the grace expires. Past the grace,
   /// a preflight failure on a synced chain is a terminal bootstrap bug —
   /// stored as a diagnosable gate state, logged, and answered with a
   /// fail-fast node shutdown (see {@link quit_if_startup_failed_terminally}).
   ///
   /// Escapes are contained FIRST (unwinding out of a posted channel delivery
   /// would skip the diagnosable gate-state store and the shutdown log) and
   /// then routed through the same fail-fast shutdown. The expected
   /// preflight/wiring failures inside the body set their own more specific
   /// states; the containment catches what they don't (a throwing table
   /// decode in the preflight or registry read, a non-fc exception from
   /// client wiring, a cron add_job failure). FC_LOG_AND_DROP is the tree's
   /// containment idiom (see scan_cycle below) and deliberately rethrows
   /// boost::interprocess::bad_alloc — chainbase shared-memory exhaustion
   /// stays immediately fatal.
   void attempt_deferred_startup() {
      if (shutting_down) {
         return;
      }
      bool completed = false;
      try {
         attempt_deferred_startup_body();
         completed = true;
      } FC_LOG_AND_DROP("underwriter_plugin: deferred startup failed unexpectedly:");
      if (!completed) {
         gate_state = underwriter_detail::startup_state::startup_failed;
      }
      // Tripwire for the {@link startup_attempted} derivation: every attempt
      // that returns normally must have left `waiting_for_sync` — a future
      // early return in the body before its first gate_state store would
      // otherwise strand the node in undiagnosable limbo (gate subscription
      // already released, nothing left to re-arm).
      assert(startup_attempted());
      quit_if_startup_failed_terminally();
   }

   /// The uniform fail-fast policy: a terminal deferred-startup failure shuts
   /// the node down instead of leaving a quiet, non-underwriting node behind a
   /// running process — an operator daemon that is not performing its duties
   /// must be supervisor-visible (it has liveness/slashing consequences), not
   /// hidden behind one log line. The specific failure was already stored as a
   /// gate state and logged by the attempt; this holds the shutdown decision in
   /// ONE place so the policy cannot diverge between the first attempt and the
   /// bounded preflight retries.
   void quit_if_startup_failed_terminally() {
      const auto state = gate_state.load();
      if (!underwriter_detail::is_terminal_failure(state)) {
         return;
      }
      elog("underwriter_plugin: deferred startup failed terminally (state={}) — "
           "shutting down node (fail-fast)", magic_enum::enum_name(state));
      app().quit();
   }

   /// The deferred startup body proper — see {@link attempt_deferred_startup}
   /// for the retry semantics and the exception containment around it.
   void attempt_deferred_startup_body() {
      // Pre-flight: bail (no cron job) if the depot-side state for this
      // underwriter is incomplete. Cluster bootstrap is responsible for
      // establishing the missing state — there is no dev escape hatch.
      // The already-registered HTTP endpoints keep reporting the gate state
      // so a not-yet-live underwriter is diagnosable over HTTP.
      if (!run_preflight()) {
         const auto since_armed = fc::time_point::now() - startup_armed_at;
         if (since_armed <
             fc::milliseconds(underwriter_defaults::preflight_retry_grace_ms)) {
            gate_state = underwriter_detail::startup_state::preflight_retrying;
            ilog("underwriter_plugin: preflight incomplete {}ms after the sync gate "
                 "armed — retrying in {}ms (grace {}ms); the error above is "
                 "transient until the grace expires",
                 since_armed.count() / 1000,
                 underwriter_defaults::preflight_retry_interval_ms,
                 underwriter_defaults::preflight_retry_grace_ms);
            schedule_preflight_retry();
            return;
         }
         gate_state = underwriter_detail::startup_state::preflight_failed;
         elog("underwriter_plugin: pre-flight failed — cron job NOT registered");
         return;
      }

      // Materialize one outpost_client SPI handle per CONFIGURED chain
      // (SEC-13/WSA-027: keyed by EXACT chain_code, so two chains of the same VM
      // family each get their own client + RPC). The underwriter never sees raw
      // `ethereum_client` / `solana_client` instances after this point — every
      // outpost-side action goes through the SPI virtuals. Per `outpost-client-spi.md`:
      //   * ETH client carries only the OperatorRegistry address (the uw_commit
      //     target); the underwriter neither consumes nor emits OPP envelopes, so
      //     OPP / OPPInbound addresses are left empty.
      //   * SOL client carries the opp-outpost program id; the typed wrapper
      //     exposes `commit_underwrite` directly.
      // `external_chain_id` comes from `sysio.chains` (read here so the registry
      // caches are warm); a chain configured but not yet in the registry builds
      // with id 0 (harmless — no leg references it until it is active).
      read_outpost_registry();
      try {
         for (const auto& [chain_code, ep] : outpost_endpoints) {
            const auto     code_str = fc::slug_name{chain_code}.to_string();
            const uint32_t ext_id   = [&] {
               auto it = outpost_external_chain_ids.find(chain_code);
               return it != outpost_external_chain_ids.end() ? it->second : 0u;
            }();
            if (ep.kind == ChainKind::CHAIN_KIND_EVM) {
               outpost_by_chain[chain_code] =
                  eth_plug->create_outpost_client(ep.client_id, chain_code, ext_id,
                                                  /*opp_addr=*/"", /*opp_inbound_addr=*/"",
                                                  ep.commit_addr);
               ilog("underwriter_plugin: wired ETH outpost_client chain={} (client_id='{}', opreg={})",
                    code_str, ep.client_id, ep.commit_addr);
            } else if (ep.kind == ChainKind::CHAIN_KIND_SVM) {
               outpost_by_chain[chain_code] =
                  sol_plug->create_outpost_client(ep.client_id, chain_code, ext_id,
                                                  ep.commit_addr /*program_id*/);
               ilog("underwriter_plugin: wired SOL outpost_client chain={} (client_id='{}', program={})",
                    code_str, ep.client_id, ep.commit_addr);
            } else {
               wlog("underwriter_plugin: outpost_endpoint chain={} has unknown kind — skipped",
                    code_str);
            }
         }
         if (outpost_by_chain.empty()) {
            wlog("underwriter_plugin: NO outpost_clients wired — pass "
                 "--underwriter-eth-outpost / --underwriter-sol-outpost for each served chain");
         }
      } catch (const fc::exception& e) {
         gate_state = underwriter_detail::startup_state::wiring_failed;
         elog("underwriter_plugin: failed to build outpost_client(s): {}",
              e.to_detail_string());
         return;
      }

      cron_service::job_schedule sched;
      sched.milliseconds = {cron_service::job_schedule::step_value{scan_interval_ms}};

      cron_service::job_metadata_t meta;
      meta.label         = "underwriter_scan";
      meta.one_at_a_time = true;

      scan_job_id = cron_plug->add_job(
         sched,
         [this]() { scan_cycle(); },
         meta
      );
      // Stored immediately after the job exists — anything thrown between a
      // successful add_job and this store would otherwise leave the scan cron
      // running while the gate reports a terminal failure. The
      // `/v1/underwriter/*` endpoints (registered back in `plugin_startup`)
      // switch from gate-state reporting to the real payloads here.
      gate_state = underwriter_detail::startup_state::active;

      ilog("underwriter_plugin: scheduled scan (id={}, interval={}ms)",
           scan_job_id, scan_interval_ms);
   }

   /// Re-run {@link attempt_deferred_startup} after one retry interval, on
   /// the main thread (appbase-wrapped wait on the app io_context).
   void schedule_preflight_retry() {
      if (!preflight_retry_timer) {
         preflight_retry_timer.emplace(app().get_io_context());
      }
      preflight_retry_timer->expires_after(
         std::chrono::milliseconds(underwriter_defaults::preflight_retry_interval_ms));
      // Plain wait on the main io_context, then POST the attempt to the app
      // queue — the same pattern the sync-gate callback uses, so the body
      // runs in the same main-thread context plugin_startup does.
      preflight_retry_timer->async_wait([this](const boost::system::error_code& ec) {
         if (ec || shutting_down) {
            return;
         }
         app().executor().post(appbase::priority::medium, [this]() {
            // Proceed only while the retry grace is the live state: a
            // terminal state stored between the timer firing and this task
            // running must stay terminal. Structural guard — today no such
            // interleaving exists, but the terminal-stays-terminal invariant
            // should not rest on statement order in schedule_preflight_retry.
            if (shutting_down ||
                gate_state.load() != underwriter_detail::startup_state::preflight_retrying) {
               return;
            }
            attempt_deferred_startup();
         });
      });
   }

   // -----------------------------------------------------------------------
   //  Main scan cycle
   // -----------------------------------------------------------------------

   void scan_cycle() {
      if (shutting_down || !enabled) return;
      try {
         do_scan_cycle();
      } FC_LOG_AND_DROP();
   }

   void do_scan_cycle() {
      // Step 0: refresh own status. SLASHED / TERMINATED operators must NOT
      // call commit() on outposts — the depot rejects (or simply doesn't
      // select them as winner), but the wasted JSON-RPC tx + on-chain
      // attestation is observable noise. Halting locally is cleaner.
      poll_own_status();
      if (!is_active) return;

      // Step 1: Read outpost registry for chain_kind mappings
      read_outpost_registry();

      // Step 2: Read our credit lines from sysio.opreg::operators
      read_credit_lines();

      // Step 3: Check if we are AVAILABLE (any credit > 0 on all active chains)
      if (!is_available()) {
         return; // Not available to underwrite — skip this cycle
      }

      // Step 4: Scan sysio.uwrit::uwreqs for PENDING requests
      auto requests = scan_pending_requests();

      // Step 4b: prune local per-uwreq state whose uwreq is no longer
      // PENDING — the depot has resolved (won/lost/expired) those races, so
      // the local sets should not grow unbounded. This is the same pass that
      // already reads the PENDING set, so it's free.
      {
         std::unordered_set<uint64_t> still_pending;
         still_pending.reserve(requests.size());
         for (auto& r : requests) still_pending.insert(r.id);
         std::lock_guard lk{stats_mutex};
         std::erase_if(confirmed_commits, [&](const commit_key& k) {
            return !still_pending.contains(k.uwreq_id);
         });
         underwriter::prune_solana_source_deposit_scan_cursors(
            sol_source_deposit_scan_cursors, still_pending);
         uwreqs_seen_pending_count = requests.size();
      }

      if (requests.empty()) return;

      ilog("underwriter: found {} pending underwrite requests", requests.size());

      // Step 5: Select requests we can cover (100% on both send and receive chains)
      auto selected = select_coverable(requests);
      if (selected.empty()) {
         ilog("underwriter: no requests coverable with current credit lines");
         return;
      }

      // Step 5b: drop any uwreq whose every REQUIRED leg is already
      // confirmed — the dispatch lambda also gates per-leg, but checking
      // here saves building UIC + signing for nothing. A depot (WIRE) leg
      // is implicitly done: it is never committed, so without this a
      // single-leg swap would be reselected forever.
      std::erase_if(selected, [&](const uw_request& r) {
         const bool src_done = r.src_is_depot
            || confirmed_commits.contains(
                  commit_key{r.id, r.src_chain_code.value, r.src_token_code.value,
                             r.src_reserve_code.value});
         const bool dst_done = r.dst_is_depot
            || confirmed_commits.contains(
                  commit_key{r.id, r.dst_chain_code.value, r.dst_token_code.value,
                             r.dst_reserve_code.value});
         return src_done && dst_done;
      });

      if (selected.empty()) {
         ilog("underwriter: all selected uwreqs already have every required leg confirmed locally");
         return;
      }

      ilog("underwriter: selected {} requests for underwriting", selected.size());

      // Step 6: Submit intent for each selected request
      for (auto& req : selected) {
         submit_intent_to_outpost(req);
      }
   }

   // -----------------------------------------------------------------------
   //  Read outpost registry
   // -----------------------------------------------------------------------

   void read_outpost_registry() {
      outpost_chain_kinds.clear();
      outpost_external_chain_ids.clear();
      // v6 refactor: chain rows moved from `sysio.epoch::outposts` to
      // `sysio.chains::chains`. Each row is a `Chain` with fields:
      //   `code`              — slug_name (the universal chain identifier; the
      //                          v5 `outpost_id` was just this slug's uint64).
      //   `kind`              — ChainKind enum.
      //   `external_chain_id` — the chain's numeric id (1 = ETH mainnet, …);
      //                         fed to `create_outpost_client` (SEC-13/WSA-027).
      //   `is_depot`          — true for the WIRE depot's own row; we filter
      //                          it out since underwriters don't commit to the
      //                          depot itself.
      //   `active`            - false until `sysio.chains::activchain` runs;
      //                          post-bootstrap `regchain` rows start inactive,
      //                          so they are not yet live outposts and are skipped.
      auto rows = read_all("sysio.chains", "sysio.chains", "chains");
      depot_chain_code.reset();
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         // `code` is a `slug_name` — serialised as `{"value": <uint64>}`.
         const auto& code_obj = obj["code"].get_object();
         uint64_t chain_code = code_obj["value"].as_uint64();
         if (obj.contains("is_depot") && obj["is_depot"].as_bool()) {
            // Record the depot's own code for exact per-leg depot
            // detection (to/from-WIRE swaps), then skip caching it as an
            // outpost — underwriters never commit to the depot itself.
            depot_chain_code = chain_code;
            continue;
         }
         // Skip chains not yet activated. `regchain` inserts post-bootstrap
         // rows with `active=false` until `activchain` flips them; treating an
         // inactive (future) chain as a live outpost would make the endpoint-
         // coverage preflight and `is_available()` demand config + collateral
         // for a chain that is not serving yet, blocking startup or halting the
         // already-active chains. Mirrors batch_operator_plugin's
         // `is_depot || !active` outpost filter.
         if (!obj.contains("active") || !obj["active"].as_bool())
            continue;
         // FC_REFLECT_ENUM in sysio/opp/opp.hpp gives us a direct enum
         // round-trip — the variant carries the symbolic name and `.as<T>()`
         // recovers the typed value without a string switch.
         outpost_chain_kinds[chain_code] = obj["kind"].as<ChainKind>();
         if (obj.contains("external_chain_id"))
            outpost_external_chain_ids[chain_code] =
               static_cast<uint32_t>(obj["external_chain_id"].as_uint64());
      }
   }

   /// True iff `code` is the WIRE depot's own chain code. Exact compare
   /// against the registry's `is_depot` row — never inferred from
   /// CHAIN_KIND_UNKNOWN (which also matches unregistered chains).
   bool is_depot_leg(fc::slug_name code) const {
      return depot_chain_code && code.value == *depot_chain_code;
   }

   // -----------------------------------------------------------------------
   //  Read credit lines from sysio.opreg::operators
   // -----------------------------------------------------------------------

   void read_credit_lines() {
      credit_lines.clear();

      // v6 schema: balance / lock / withdraw rows carry `chain_code`
      // and `token_code` slug_names (serialised as `{"value": <u64>}`)
      // — not the v5 `chain` (ChainKind enum) / `token_kind` (TokenKind
      // enum). Translation:
      //   chain_code → ChainKind  via `outpost_chain_kinds` map
      //                            (populated by `read_outpost_registry`).
      //   token_code → TokenKind  via `sysio.tokens::tokens.kind`
      //                            (lazy-loaded into `token_kind_by_code`).
      // Reads happen in this order so the registry caches are warm when
      // the balance loop runs.

      // Refresh token-code → TokenKind cache once per scan (tokens
      // table is small — 5 rows in the test cluster). Stored as a
      // member so `scan_pending_requests` can reuse the same map when
      // translating uwreq slug codes to TokenKind for bucket matching.
      token_kind_by_code.clear();
      {
         auto tk_rows = read_all("sysio.tokens", "sysio.tokens", "tokens");
         for (auto& row : tk_rows.rows) {
            auto obj  = row.get_object();
            uint64_t code = obj["code"].get_object()["value"].as_uint64();
            token_kind_by_code[code] = obj["kind"].as<TokenKind>();
         }
      }

      // Local helper: read `chain_code`/`token_code` slug fields (v6 shape
      // `{"value": <u64>}`) as their EXACT packed slug values. Returns nullopt
      // (row skipped) when the chain isn't a registered non-depot outpost or
      // the token is unknown — neither can back a leg. SEC-13/WSA-027: key by
      // exact code, never collapse to ChainKind/TokenKind.
      auto read_slug_pair = [&](const fc::variant_object& obj)
         -> std::optional<std::pair<uint64_t, uint64_t>> {
         if (!obj.contains("chain_code") || !obj.contains("token_code")) {
            return std::nullopt;
         }
         uint64_t chain_code = obj["chain_code"].get_object()["value"].as_uint64();
         uint64_t token_code = obj["token_code"].get_object()["value"].as_uint64();
         if (!outpost_chain_kinds.contains(chain_code)
             || !token_kind_by_code.contains(token_code)) {
            return std::nullopt;
         }
         return std::make_pair(chain_code, token_code);
      };

      // ── Step 1: raw balances from sysio.opreg::operators[underwriter] ──
      auto ops_rows = read_all("sysio.opreg", "sysio.opreg", "operators");
      for (auto& row : ops_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
         if (!obj.contains("balances") || !obj["balances"].is_array()) break;
         for (auto& bal_entry : obj["balances"].get_array()) {
            auto be = bal_entry.get_object();
            auto codes = read_slug_pair(be);
            if (!codes) continue;
            credit_lines.push_back(credit_line{
               .chain_code = codes->first,
               .token_code = codes->second,
               .balance    = be["balance"].as_uint64(),
            });
         }
         break;
      }

      // ── Step 2: subtract active locks (sysio.uwrit::locks) ─────────────
      // Locks that exceed the raw balance clamp to 0 — same convention as
      // the depot's `available()`.
      auto lock_rows = read_all("sysio.uwrit", "sysio.uwrit", "locks");
      for (auto& row : lock_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["underwriter"].as_string()) != underwriter_account) continue;
         auto codes = read_slug_pair(obj);
         if (!codes) continue;
         const uint64_t amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_code == codes->first && cl.token_code == codes->second) {
               cl.balance = (cl.balance > amount) ? (cl.balance - amount) : 0;
               break;
            }
         }
      }

      // ── Step 3: subtract pending withdraws (sysio.opreg::wtdwqueue) ────
      auto wq_rows = read_all("sysio.opreg", "sysio.opreg", "wtdwqueue");
      for (auto& row : wq_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
         auto codes = read_slug_pair(obj);
         if (!codes) continue;
         const uint64_t amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_code == codes->first && cl.token_code == codes->second) {
               cl.balance = (cl.balance > amount) ? (cl.balance - amount) : 0;
               break;
            }
         }
      }

      for (auto& cl : credit_lines) {
         ilog("underwriter: credit line chain_code={} token_code={} available={}",
              fc::slug_name{cl.chain_code}.to_string(),
              fc::slug_name{cl.token_code}.to_string(),
              cl.balance);
      }
   }

   /**
    * Refresh `is_active` from `sysio.opreg::operators[underwriter_account].status`.
    * Mirror of the awareness poll on batch_operator_plugin — both share
    * the `sysio::depot::opreg_status::compute_is_active` helper so the
    * status spellings + decision table live in one place. Logs once per
    * transition.
    */
   void poll_own_status() {
      auto rows = read_all("sysio.opreg", "sysio.opreg", "operators");
      bool was_active = is_active;
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
         is_active = sysio::depot::opreg_status::compute_is_active(
            obj["status"].as_string(), was_active);
         break;
      }
      if (was_active && !is_active) {
         elog("underwriter: own status flipped to SLASHED / TERMINATED — halting relay loop");
      } else if (!was_active && is_active) {
         ilog("underwriter: own status flipped to ACTIVE — resuming relay loop");
      }
   }

   // -----------------------------------------------------------------------
   //  Availability check — any amount > 0 on ALL active chains
   // -----------------------------------------------------------------------

   bool is_available() {
      if (credit_lines.empty()) {
         ilog("underwriter: not available — no balance rows in sysio.opreg");
         return false;
      }

      // Check that we have > 0 balance on every active outpost chain
      // (any token kind on that chain). Per-(chain, token) coverage is
      // checked downstream in select_coverable for each specific request.
      for (auto& [chain_code, chain_kind] : outpost_chain_kinds) {
         bool found = false;
         for (auto& cl : credit_lines) {
            if (cl.chain_code == chain_code && cl.balance > 0) {
               found = true;
               break;
            }
         }
         if (!found) {
            ilog("underwriter: not available — no balance on chain_kind={}",
                 ChainKind_Name(chain_kind));
            return false;
         }
      }

      return true;
   }

   // -----------------------------------------------------------------------
   //  Scan sysio.uwrit::uwreqs for PENDING requests
   // -----------------------------------------------------------------------

   std::vector<uw_request> scan_pending_requests() {
      std::vector<uw_request> requests;

      // v6: `sysio.uwrit::uwreqs` is now a KV table. The legacy
      // multi_index-style `{"bystatus": <n>}` lower_bound format
      // doesn't traverse the KV secondary index — it returns 0 rows.
      // Until a v6 KV-index query path lands here, scan by primary
      // key and filter PENDING in C++. uwreqs is small (one row per
      // in-flight swap; race-resolved rows transition to other
      // statuses within an epoch), so this is cheap.
      auto rows = read_all("sysio.uwrit", "sysio.uwrit", "uwreqs");
      const auto pending_name = std::string{
         "UNDERWRITE_REQUEST_STATUS_PENDING"};
      for (auto& row : rows.rows) {
         auto obj = row.get_object();

         // Filter to PENDING only. The status field surfaces as the
         // wire-format spelling string under the v6 ABI.
         if (!obj.contains("status")) continue;
         if (obj["status"].is_string()) {
            if (obj["status"].as_string() != pending_name) continue;
         } else {
            if (obj["status"].as_uint64() !=
                magic_enum::enum_integer(
                  UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING))
               continue;
         }

         // Skip if already assigned to another underwriter
         auto uw_name = obj.contains("uw_name") ? obj["uw_name"].as_string() : std::string{};
         if (!uw_name.empty() && chain::name(uw_name) != underwriter_account &&
             chain::name(uw_name) != chain::name()) {
            continue;
         }

         uw_request req;
         req.id = obj["id"].as_uint64();
         // Pre-filtered to PENDING by the bystatus index range above.
         req.status  = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING;
         req.uw_name = uw_name;

         // Parse attestation type. Variant carries either the wire-format
         // spelling (string) or the underlying numeric value (uint64);
         // resolve both into a typed `AttestationType` and skip any value
         // we don't underwrite. Per CLAUDE.md §3, proto-generated enums
         // use the `<EnumName>_Parse` / `<EnumName>_Name` helpers rather
         // than `magic_enum`.
         {
            std::optional<AttestationType> at;
            if (obj["type"].is_string()) {
               AttestationType parsed{};
               if (AttestationType_Parse(obj["type"].as_string(), &parsed)) {
                  at = parsed;
               }
            } else {
               AttestationType parsed{};
               if (AttestationType_Parse(
                     AttestationType_Name(static_cast<AttestationType>(obj["type"].as_uint64())),
                     &parsed)) {
                  at = parsed;
               }
            }
            if (!at || *at != AttestationType::ATTESTATION_TYPE_SWAP_REQUEST) continue;
            req.attestation_type = *at;
         }

         // v6 data-model schema: src/dst identity lives on the uwreq row as
         // `(chain_code, token_code, reserve_code)` slug_name triples plus a
         // `*_amount`. Populated by `sysio.uwrit::createuwreq` from the
         // originating SwapRequest. The ABI surfaces slug_name as
         // `{value: uint64}`; we lift the inner uint64 directly into
         // `fc::slug_name` to mirror the host-side packing.
         if (!obj.contains("src_chain_code") || !obj.contains("src_amount")
             || !obj.contains("dst_chain_code") || !obj.contains("dst_amount")) {
            // Row not yet populated (createuwreq writes them inline so this
            // should be unreachable for SWAP-derived UWREQs). Skip safely.
            continue;
         }
         auto read_codename = [&](const char* key) -> fc::slug_name {
            return fc::slug_name{obj[key]["value"].as_uint64()};
         };
         req.src_chain_code   = read_codename("src_chain_code");
         req.src_token_code   = read_codename("src_token_code");
         req.src_reserve_code = read_codename("src_reserve_code");
         req.src_amount       = obj["src_amount"].as_uint64();
         req.dst_chain_code   = read_codename("dst_chain_code");
         req.dst_token_code   = read_codename("dst_token_code");
         req.dst_reserve_code = read_codename("dst_reserve_code");
         req.dst_amount       = obj["dst_amount"].as_uint64();
         req.variance_tolerance_bps = obj.contains("variance_tolerance_bps")
            ? static_cast<uint32_t>(obj["variance_tolerance_bps"].as_uint64())
            : 0u;
         // Cross-walk the slug_name codes to ChainKind/TokenKind enums
         // for the `select_coverable` bucket-matching path (which keys
         // on the enums). Maps populated by the upstream
         // `read_outpost_registry` (`sysio.chains::chains` → ChainKind)
         // and `read_credit_lines` (`sysio.tokens::tokens` → TokenKind)
         // calls in `do_scan_cycle`. Any uncovered slug — e.g. the
         // depot's WIRE chain (filtered out of outpost_chain_kinds via
         // is_depot) — falls through as UNKNOWN; `select_coverable`
         // then treats the request as out of scope and skips it.
         auto resolve_chain_kind = [&](fc::slug_name code) -> ChainKind {
            auto it = outpost_chain_kinds.find(code.value);
            return it != outpost_chain_kinds.end()
                   ? it->second : ChainKind::CHAIN_KIND_UNKNOWN;
         };
         auto resolve_token_kind = [&](fc::slug_name code) -> TokenKind {
            auto it = token_kind_by_code.find(code.value);
            return it != token_kind_by_code.end()
                   ? it->second : TokenKind::TOKEN_KIND_UNKNOWN;
         };
         req.src_chain      = resolve_chain_kind(req.src_chain_code);
         req.src_token_kind = resolve_token_kind(req.src_token_code);
         req.dst_chain      = resolve_chain_kind(req.dst_chain_code);
         req.dst_token_kind = resolve_token_kind(req.dst_token_code);
         // Depot-leg stamps — exact compare against the registry's
         // is_depot row. A depot (WIRE) leg needs no commit / bond /
         // source verification; downstream stages branch on these.
         req.src_is_depot   = is_depot_leg(req.src_chain_code);
         req.dst_is_depot   = is_depot_leg(req.dst_chain_code);
         // The ABI surfaces `bytes` as a hex string. Decode both
         // source_tx_id and depositor — the depot rejects any SwapRequest
         // with empty source_tx_id at createuwreq (emits SwapRevert), so
         // every uwreq the plugin sees should carry both fields.
         auto decode_hex_field = [&](const char* key, std::vector<char>& out) {
            if (!obj.contains(key)) return;
            auto s = obj[key].as_string();
            if (s.empty()) return;
            out.resize(s.size() / 2);
            fc::from_hex(s, out.data(), out.size());
         };
         decode_hex_field("source_tx_id", req.source_tx_id);
         decode_hex_field("depositor",    req.depositor);

         requests.push_back(std::move(req));
      }

      return requests;
   }

   // -----------------------------------------------------------------------
   //  Select requests coverable by our credit lines
   //  Requires 100% coverage on BOTH src and dst legs of the swap, where
   //  each leg's required bond is per-(chain_kind, token_kind).
   // -----------------------------------------------------------------------

   /// Hard cap on the number of candidates the branch-and-bound search
   /// considers. Above this we fall back to value-sort-descending greedy
   /// to keep the cycle bounded — the upper-bound prune is good but worst
   /// case is still 2^N branches.
   static constexpr size_t MAX_CANDIDATES = 64;

   /// Build a `leg_bond` (exact `(chain_code, token_code)` bucket + bond
   /// requirement) for one leg of `r`. A WIRE depot leg requires zero bond —
   /// it has no outpost, no UIC, and no lock.
   static leg_bond src_bond(const uw_request& r) {
      return { bucket_key{r.src_chain_code.value, r.src_token_code.value},
               r.src_is_depot ? 0 : r.src_amount };
   }
   static leg_bond dst_bond(const uw_request& r) {
      return { bucket_key{r.dst_chain_code.value, r.dst_token_code.value},
               r.dst_is_depot ? 0 : r.dst_amount };
   }

   /// Attempt to debit `r`'s per-leg bond requirements from `remaining`,
   /// delegating to the pure `underwriter_detail::try_debit_buckets`. Returns
   /// true (and mutates `remaining`) iff every required leg fits. Buckets are
   /// keyed by the EXACT `(chain_code, token_code)` slug pair (SEC-13/WSA-027),
   /// so two same-VM-family chains hold independent credit and never collide.
   /// Same-bucket dual-leg swaps (e.g. ERC20 → native on one chain) debit both
   /// legs from the single shared row; a both-depot swap is rejected.
   static bool try_debit_buckets(credit_buckets& remaining, const uw_request& r) {
      return underwriter_detail::try_debit_buckets(remaining, src_bond(r), dst_bond(r));
   }

   /// Branch-and-bound search that returns the subset of `candidates`
   /// maximizing `Σ(src_amount + dst_amount)` while each per-(chain,
   /// token_kind) credit bucket stays non-negative. Recurses depth-first
   /// in two branches per candidate (include / skip); on each include
   /// branch verifies feasibility before descending, and prunes the
   /// subtree on infeasibility OR when the upper-bound estimate (current
   /// value + every remaining candidate's value, regardless of fit) can't
   /// beat the current best.
   ///
   /// Same-chain swaps (e.g. ERC20 → ETH-native) draw from a single
   /// bucket when `src` and `dst` keys coincide; the include branch
   /// debits both legs from the same row in that case.
   void knapsack_dfs(size_t                                   i,
                      const std::vector<uw_request>&            candidates,
                      const std::vector<uint64_t>&              suffix_value,
                      credit_buckets                            remaining,
                      std::vector<size_t>                       cur_indices,
                      uint64_t                                  cur_value,
                      uint64_t&                                 best_value,
                      std::vector<size_t>&                      best_indices) {
      // Upper-bound prune: even if every remaining candidate fit, the
      // resulting value couldn't beat the current best.
      if (cur_value + suffix_value[i] <= best_value) return;

      if (i == candidates.size()) {
         if (cur_value > best_value) {
            best_value   = cur_value;
            best_indices = cur_indices;
         }
         return;
      }

      const auto& r = candidates[i];
      credit_buckets after = remaining;
      const bool feasible = try_debit_buckets(after, r);

      // Branch 1: include (if feasible).
      if (feasible) {
         cur_indices.push_back(i);
         knapsack_dfs(i + 1, candidates, suffix_value,
                      std::move(after), cur_indices,
                      cur_value + r.src_amount + r.dst_amount,
                      best_value, best_indices);
         cur_indices.pop_back();
      }

      // Branch 2: skip.
      knapsack_dfs(i + 1, candidates, suffix_value,
                   std::move(remaining), std::move(cur_indices),
                   cur_value, best_value, best_indices);
   }

   /// Greedy fallback for above-cap candidate counts. Sorts by total leg
   /// value descending and picks anything that still fits.
   std::vector<uw_request>
   greedy_fallback(std::vector<uw_request> requests,
                    credit_buckets remaining) const {
      std::sort(requests.begin(), requests.end(),
                [](const uw_request& a, const uw_request& b) {
                   return (a.src_amount + a.dst_amount)
                        > (b.src_amount + b.dst_amount);
                });
      std::vector<uw_request> picked;
      for (auto& r : requests) {
         if (!try_debit_buckets(remaining, r)) continue;
         picked.push_back(r);
      }
      return picked;
   }

   std::vector<uw_request> select_coverable(std::vector<uw_request>& requests) {
      // Seed bucket credits from `read_credit_lines`' output. Per the T11
      // mirror, these already have active locks + pending withdraws
      // subtracted, so the search operates on truly-spendable balances.
      credit_buckets initial_credit;
      for (auto& cl : credit_lines) {
         initial_credit[bucket_key{cl.chain_code, cl.token_code}] = cl.balance;
      }

      // Pre-filter requests that can never fit in isolation (no bucket
      // even matches), so the search space stays small. Depot legs cost
      // zero, so a single-leg (to/from-WIRE) request only needs its one
      // real leg's bucket to cover.
      std::vector<uw_request> feasible_in_isolation;
      feasible_in_isolation.reserve(requests.size());
      for (auto& r : requests) {
         auto scratch = initial_credit;
         if (!try_debit_buckets(scratch, r)) continue;
         feasible_in_isolation.push_back(r);
      }

      std::vector<uw_request> selected;
      if (feasible_in_isolation.size() > MAX_CANDIDATES) {
         wlog("underwriter: {} feasible candidates exceeds knapsack cap ({}); "
              "falling back to value-sort-descending greedy",
              feasible_in_isolation.size(), MAX_CANDIDATES);
         selected = greedy_fallback(std::move(feasible_in_isolation), initial_credit);
      } else if (!feasible_in_isolation.empty()) {
         // Suffix sum of per-candidate value — the upper-bound prune in
         // knapsack_dfs uses this to skip subtrees whose maximum possible
         // remaining value can't beat the current best.
         const size_t n = feasible_in_isolation.size();
         std::vector<uint64_t> suffix_value(n + 1, 0);
         for (size_t k = n; k > 0; --k) {
            suffix_value[k - 1] = suffix_value[k]
                                + feasible_in_isolation[k - 1].src_amount
                                + feasible_in_isolation[k - 1].dst_amount;
         }

         uint64_t            best_value = 0;
         std::vector<size_t> best_indices;
         knapsack_dfs(/*i=*/0, feasible_in_isolation, suffix_value,
                      initial_credit, /*cur_indices=*/{}, /*cur_value=*/0,
                      best_value, best_indices);

         selected.reserve(best_indices.size());
         for (size_t idx : best_indices) {
            selected.push_back(feasible_in_isolation[idx]);
         }
      }

      for (auto& r : selected) {
         ilog("underwriter: selected request {} — "
              "src(chain={},token={},amt={}) dst(chain={},token={},amt={})",
              r.id,
              ChainKind_Name(r.src_chain), TokenKind_Name(r.src_token_kind), r.src_amount,
              ChainKind_Name(r.dst_chain), TokenKind_Name(r.dst_token_kind), r.dst_amount);
      }
      return selected;
   }

   // -----------------------------------------------------------------------
   //  Submit intent to outpost contract
   //  The outpost locks capital and emits UNDERWRITE_INTENT via OPP
   // -----------------------------------------------------------------------

   /// Build a verbatim, signed `UnderwriteIntentCommit` payload for the
   /// given leg's `(uwreq_id, chain_code, chain_code, token_code,
   /// reserve_code)`. Returns an empty vector on any failure (no signature
   /// provider, serialize failure, etc.).
   ///
   /// The slug_name triple `(chain_code, token_code, reserve_code)` is the
   /// v6 routing scalar set the depot's `rcrdcommit` uses to disambiguate
   /// src vs dst legs — same-chain swaps with multiple reserves on a single
   /// `(chain, token)` pair are still resolvable because `reserve_code`
   /// breaks the tie. `chain_code` carries the chain identity at the OPP
   /// envelope level (the originating-outpost field on the inbound
   /// envelope); `chain_code` is the same chain as a slug_name and is what
   /// the depot indexes against `sysio.uwrit::uw_request_t.*_chain_code`.
   ///
   /// Digest semantics: the underwriter signs `sha256(serialize(uic with
   /// signature blanked))`. The depot's `try_select_winner` rebuilds the
   /// same digest from the bytes it received and verifies the embedded
   /// signature against the underwriter's WIRE account permissions
   /// (`owner` / `active` only) — see `sysio.uwrit::verify_uic_signature`.
   std::vector<char> build_signed_uic_bytes(uint64_t        uwreq_id,
                                            ChainKind       leg_chain_kind,
                                            fc::slug_name    chain_code,
                                            fc::slug_name    token_code,
                                            fc::slug_name    reserve_code) {
      opp_att::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(underwriter_account.to_string());
      uic.set_uw_request_id(uwreq_id);
      // v6 data-model: leg identity is the slug_name triple. The wire format
      // for each field is the packed uint64 slug_name value (alphabet
      // `[A-Z0-9_]+`, ≤8 chars). The depot decodes these back to
      // `sysio::slug_name` via `sysio::slug_name{uic.chain_code}` etc. in
      // `sysio.msgch::dispatch_underwrite_commit`.
      uic.set_chain_code(chain_code.value);
      uic.set_token_code(token_code.value);
      uic.set_reserve_code(reserve_code.value);
      // FORCE serialization of `uw_ext_chain_addr` AND its inner `kind`
      // enum to a non-zero value. Background: proto3 C++ omits unset
      // message fields and zero-valued enums from `SerializeToString`,
      // but the depot's CDT decoder uses `zpp::bits::pb_members<N>`
      // which ALWAYS emits every declared member (and every enum) on
      // re-encode. If host omits field 2, or sets it to a default-
      // constructed ChainAddress with `kind=CHAIN_KIND_UNSPECIFIED=0`,
      // then the depot's verify-side
      // decode → blank-signature → re-encode round-trip produces extra
      // bytes for field 2 and/or its nested `kind` enum, the digests
      // diverge, and `verify_uic_signature` fails.
      //
      // Setting `kind` to the leg's actual ChainKind makes both encoders
      // emit the same bytes. The `address` bytes vector stays empty —
      // empty containers are skipped by BOTH encoders, so that's safe.
      // See `wire-sysio/.claude/rules/zpp-bits-is-cdt-only.md` for the
      // full encoder-divergence rationale.
      uic.mutable_uw_ext_chain_addr()->set_kind(leg_chain_kind);
      uic.clear_signature();

      std::string blanked;
      if (!uic.SerializeToString(&blanked)) {
         elog("underwriter: UIC serialize failed (blank phase) for uwreq {}", uwreq_id);
         return {};
      }

      auto digest = fc::sha256::hash(blanked.data(), blanked.size());

      // Preflight validates that exactly one WIRE K1 provider is
      // configured AND that its recovered pubkey is on the underwriter
      // account's owner/active permission. The cron job won't start if
      // either check fails, so by the time we reach here the assumption
      // is safe — but cheap to assert again in case the provider set
      // mutates (it shouldn't; appbase plugins don't re-init on the fly).
      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      auto wire_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_wire, fc::crypto::chain_key_type_wire);
      if (wire_providers.size() != 1) {
         elog("underwriter: expected exactly 1 WIRE K1 signature provider, got {} — "
              "preflight should have caught this. Aborting commit for uwreq {}.",
              wire_providers.size(), uwreq_id);
         return {};
      }
      auto fc_sig = wire_providers.front()->sign(digest);

      // Pack the fc::crypto::signature via fc::raw — the byte format matches
      // what the depot-side `datastream >> sysio::signature` expects (variant
      // tag + variant payload, both `fc` and `sysio` share the wire layout).
      std::vector<char> sig_bytes = fc::raw::pack(fc_sig);
      uic.set_signature(std::string(sig_bytes.begin(), sig_bytes.end()));

      std::string final_bytes;
      if (!uic.SerializeToString(&final_bytes)) {
         elog("underwriter: UIC serialize failed (final phase) for uwreq {}", uwreq_id);
         return {};
      }
      return std::vector<char>(final_bytes.begin(), final_bytes.end());
   }

   /// Before committing collateral, independently verify the source-chain
   /// deposit that funded this swap. `req.source_tx_id` is the chain-native
   /// deposit id captured at swap-emit time (ETH/SOL: 8-byte monotonic
   /// `SwapDeposit.id`). The verify path locates the source-chain event or
   /// log for that id, confirms the transaction succeeded against the
   /// configured source contract / program, and cross-references every
   /// argument we can decode against the uwreq:
   ///
   ///   * `depositor`   — `tx.from` (ETH) / fee-payer (SOL) must match `req.depositor`.
   ///   * source contract — `tx.to` (ETH) / program-id (SOL) must match the configured address.
   ///   * function selector / instruction discriminator — must match the configured value.
   ///   * receipt status (ETH) / meta.err (SOL) — must indicate success.
   ///   * finality — ETH logs are queried only through `finalized`; SOL txs
   ///                are fetched at commitment `SOL_COMMITMENT`.
   ///
   /// Hard-fail on empty `source_tx_id` — the depot's `createuwreq` rejects
   /// SwapRequests without one (emits SwapRevert), so by the time a uwreq
   /// reaches the plugin every row MUST carry one. A `req.source_tx_id`
   /// empty here means either the depot's reject regressed OR the plugin
   /// read a row pre-validation; either way the safe move is to refuse to
   /// commit until the data is whole.
   bool verify_source_deposit(const uw_request& req) {
      if (req.src_is_depot) {
         // Swap-from-WIRE: the source funds were escrowed ON the depot by
         // `sysio.uwrit::swapfromwire` before the uwreq existed — there is
         // no outpost SwapDeposit to verify, and the synthetic
         // source_tx_id is just the depot-origin queue id. This branch
         // must precede the empty-source_tx_id hard-fail below.
         dlog("underwriter: uwreq {} source is the WIRE depot — skipping "
              "source-deposit verification", req.id);
         return true;
      }
      if (req.source_tx_id.empty()) {
         elog("underwriter: REFUSING to commit uwreq {} — source_tx_id empty. "
              "Every SwapRequest is required to carry a populated source_tx_id; "
              "the depot's createuwreq must have regressed.", req.id);
         std::lock_guard lk{stats_mutex};
         source_deposit_mismatch_count++;
         return false;
      }
      switch (req.src_chain) {
         case ChainKind::CHAIN_KIND_EVM:
            return verify_source_deposit_eth(req);
         case ChainKind::CHAIN_KIND_SVM:
            return verify_source_deposit_sol(req);
         default:
            elog("underwriter: cannot verify source deposit for chain={} (uwreq {})",
                 ChainKind_Name(req.src_chain), req.id);
            return false;
      }
   }

   /// ETH-side source-deposit verification, event-scan flavour.
   ///
   /// `req.source_tx_id` is an 8-byte big-endian uint64 — the outpost-
   /// local monotonic counter value written into the SwapRequest
   /// envelope by `ReserveManager.requestSwap`. The corresponding
   /// `SwapDeposit(uint64 indexed id, bytes32 hash)` event is emitted
   /// in the same tx. Verification:
   ///
   ///   1. Parse `source_tx_id` → `id` (uint64). Reject empty / wrong-size.
   ///   2. `eth_getLogs` filtered by:
   ///        - address = resolved ReserveManager contract address
   ///        - topic[0] = keccak256("SwapDeposit(uint64,bytes32)")
   ///        - topic[1] = abi-encoded `id` (32-byte BE-padded uint64)
   ///      Must return exactly one matching log.
   ///   3. Decode the log's `data` field as the emitted hash bytes.
   ///   4. Recompute the same hash from UWREQ fields:
   ///        keccak256(packed
   ///          depositor[20]
   ///          src_amount [u64 BE]   src_token_code [u64 BE]   src_reserve_code [u64 BE]
   ///          dst_chain_code [u64 BE]   dst_token_code [u64 BE]   dst_reserve_code [u64 BE]
   ///          dst_amount [u64 BE]
   ///          variance_tolerance_bps [u32 BE])
   ///      The depot's UWREQ row carries every input; matches must be
   ///      bit-exact against the contract-emitted `hash`.
   ///   5. Pull the matching log's `transactionHash`. Receipt must exist
   ///      and status != "0x0". The log lookup itself is bounded by
   ///      Ethereum's finalized head; if the finalized head is unavailable
   ///      the verifier defers without widening to `latest`.
   ///
   /// A non-matching hash is a hard mismatch — the depositor's swap
   /// params disagree with what's recorded on the source chain. A
   /// missing log / receipt without mismatched fields is a deferred
   /// retry (returns false but no mismatch counter bump).
   bool verify_source_deposit_eth(const uw_request& req) {
      // SEC-13/WSA-027: resolve this leg's outpost wiring by EXACT chain_code —
      // the RPC client and the SwapDeposit contract address are per-chain, so a
      // second EVM chain is verified against ITS endpoint, not the first one's.
      const auto ep_it = outpost_endpoints.find(req.src_chain_code.value);
      if (ep_it == outpost_endpoints.end()) {
         elog("underwriter: no outpost endpoint configured for source chain={} "
              "(uwreq {})", req.src_chain_code.to_string(), req.id);
         return false;
      }
      const std::string& eth_client_id = ep_it->second.client_id;
      const std::string& resolved_eth_source_contract_addr = ep_it->second.source_deposit_addr;

      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: ETH client '{}' not found for source-deposit verify "
              "(uwreq {})", eth_client_id, req.id);
         return false;
      }
      auto bump_mismatch = [&]() {
         std::lock_guard lk{stats_mutex};
         source_deposit_mismatch_count++;
      };

      // (1) Decode source_tx_id as an 8-byte big-endian uint64 id.
      if (req.source_tx_id.size() != 8) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "source_tx_id has wrong size ({} bytes; expected 8 for ETH "
              "monotonic id)", req.id, req.source_tx_id.size());
         bump_mismatch();
         return false;
      }
      uint64_t deposit_id = 0;
      for (size_t i = 0; i < 8; ++i) {
         deposit_id = (deposit_id << 8) |
                      static_cast<uint8_t>(req.source_tx_id[i]);
      }

      // (2) eth_getLogs filter for SwapDeposit(uint64 indexed id, bytes32 hash).
      // The event topic[0] is keccak256 of the canonical signature.
      static const std::string SWAP_DEPOSIT_TOPIC =
         "0x" + fc::to_hex(reinterpret_cast<const char*>(
            fc::crypto::keccak256::hash(std::string{
               "SwapDeposit(uint64,bytes32)"}).data()), 32);
      // Indexed uint64 → left-pad to 32 bytes (Solidity uses 0-padded
      // big-endian; topics are always 32 bytes).
      std::string id_topic = "0x";
      id_topic.reserve(66);
      id_topic.append(48, '0');           // 24 leading zero bytes = 48 hex chars
      for (int shift = 56; shift >= 0; shift -= 8) {
         uint8_t b = static_cast<uint8_t>((deposit_id >> shift) & 0xff);
         char buf[3]; std::snprintf(buf, sizeof buf, "%02x", b);
         id_topic.append(buf, 2);
      }

      uint64_t finalized_blk = 0;
      try {
         const auto finalized_var = entry->client->execute(
            std::string{ETH_GET_BLOCK_BY_NUMBER_METHOD},
            fc::variants{eth::to_block_tag(eth::block_tag_t::finalized), false});
         if (finalized_var.is_null()) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "ETH finalized head unavailable", req.id);
            return false;
         }
         if (!finalized_var.is_object()) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "ETH finalized head response is not an object", req.id);
            return false;
         }
         const auto finalized_obj = finalized_var.get_object();
         if (!finalized_obj.contains("number") || !finalized_obj["number"].is_string()) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "ETH finalized head missing numeric block number", req.id);
            return false;
         }
         const auto parsed_finalized_blk =
            sysio::underwriter::eth_parse_block_quantity(finalized_obj["number"].as_string());
         if (!parsed_finalized_blk) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "ETH finalized head block number is malformed ('{}')",
                 req.id, finalized_obj["number"].as_string());
            return false;
         }
         finalized_blk = *parsed_finalized_blk;
      } catch (const fc::exception& e) {
         ilog("underwriter: source-deposit verify deferred for uwreq {} — "
              "ETH finalized head unavailable: {}", req.id, e.to_detail_string());
         return false;
      } catch (const std::exception& e) {
         ilog("underwriter: source-deposit verify deferred for uwreq {} — "
              "ETH finalized head response is malformed: {}", req.id, e.what());
         return false;
      }

      try {
         const uint64_t from_blk = sysio::underwriter::eth_source_deposit_from_block(
            finalized_blk, eth_source_deposit_lookback_blocks);
         const std::string from_block = sysio::underwriter::eth_block_quantity(from_blk);
         const std::string to_block = sysio::underwriter::eth_block_quantity(finalized_blk);

         fc::mutable_variant_object filter;
         filter("address",   resolved_eth_source_contract_addr);
         filter("fromBlock", from_block);
         filter("toBlock",   to_block);
         filter("topics", fc::variants{
            fc::variant{SWAP_DEPOSIT_TOPIC},
            fc::variant{id_topic},
         });
         // eth_getLogs takes its filter as the first element of the JSON-RPC
         // `params` array — wrap the filter object in a one-element array.
         auto logs_var = entry->client->get_logs(
            fc::variant{fc::variants{fc::variant{filter}}});
         if (!logs_var.is_array() || logs_var.size() == 0) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "no SwapDeposit log in recent block window (id={}, "
                 "contract={}, range={}..{})",
                 req.id, deposit_id, resolved_eth_source_contract_addr,
                 from_block, to_block);
            return false;
         }
         // Should be exactly one match — id is unique per outpost. Use
         // the first; warn (don't fail) on duplicates so we surface a
         // bug without halting the underwriter.
         if (logs_var.size() > 1) {
            wlog("underwriter: source-deposit verify uwreq {} — got {} "
                 "SwapDeposit logs for id={}; using the first",
                 req.id, logs_var.size(), deposit_id);
         }
         const auto& log = logs_var.get_array().at(0).get_object();

         // (3) Decode log.data = abi.encodePacked(bytes32 hash) = 32 bytes.
         std::string data_hex;
         if (log.contains("data") && log["data"].is_string()) {
            data_hex = log["data"].as_string();
         }
         std::string_view data_view = data_hex;
         if (data_view.size() >= 2 && data_view[0] == '0' &&
             (data_view[1] == 'x' || data_view[1] == 'X')) {
            data_view.remove_prefix(2);
         }
         if (data_view.size() != 64) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "SwapDeposit log.data has wrong size ({} hex chars; "
                 "expected 64 for bytes32)", req.id, data_view.size());
            bump_mismatch();
            return false;
         }
         std::array<uint8_t, 32> on_chain_hash{};
         for (size_t i = 0; i < 32; ++i) {
            on_chain_hash[i] = static_cast<uint8_t>(std::stoul(
               std::string{data_view.substr(i * 2, 2)}, nullptr, 16));
         }

         // (4) Recompute the hash from the UWREQ row's flat fields.
         //     Layout MUST match ReserveManager.requestSwap's
         //     abi.encodePacked(...) call exactly.
         std::vector<uint8_t> packed;
         packed.reserve(20 + 8 * 7 + 4);
         packed.insert(packed.end(),
                       req.depositor.begin(), req.depositor.end());
         auto append_u64_be = [&](uint64_t v) {
            for (int shift = 56; shift >= 0; shift -= 8) {
               packed.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
            }
         };
         auto append_u32_be = [&](uint32_t v) {
            for (int shift = 24; shift >= 0; shift -= 8) {
               packed.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
            }
         };
         append_u64_be(req.src_amount);
         append_u64_be(req.src_token_code.value);
         append_u64_be(req.src_reserve_code.value);
         append_u64_be(req.dst_chain_code.value);
         append_u64_be(req.dst_token_code.value);
         append_u64_be(req.dst_reserve_code.value);
         append_u64_be(req.dst_amount);
         append_u32_be(req.variance_tolerance_bps);
         if (packed.size() != 20 + 8 * 7 + 4) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "packed buffer size {} != expected {}",
                 req.id, packed.size(), 20 + 8 * 7 + 4);
            bump_mismatch();
            return false;
         }
         auto recomputed = fc::crypto::keccak256::hash(
            std::span<const uint8_t>{packed.data(), packed.size()});
         if (std::memcmp(recomputed.data(), on_chain_hash.data(), 32) != 0) {
            const std::string got_hex = fc::to_hex(
               reinterpret_cast<const char*>(on_chain_hash.data()), 32);
            const std::string want_hex = fc::to_hex(
               reinterpret_cast<const char*>(recomputed.data()), 32);
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "SwapDeposit hash mismatch (id={}): on-chain={} recomputed={}",
                 req.id, deposit_id, got_hex, want_hex);
            bump_mismatch();
            return false;
         }

         // (5) Receipt status on the matching finalized tx.
         std::string tx_hash_hex;
         if (log.contains("transactionHash") &&
             log["transactionHash"].is_string()) {
            tx_hash_hex = log["transactionHash"].as_string();
         }
         if (tx_hash_hex.empty()) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "SwapDeposit log missing transactionHash", req.id);
            bump_mismatch();
            return false;
         }
         auto receipt = entry->client->get_transaction_receipt(tx_hash_hex);
         if (receipt.is_null()) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "no receipt yet for matching tx {}", req.id, tx_hash_hex);
            return false;
         }
         const auto rcpt_obj = receipt.get_object();
         if (rcpt_obj.contains("status") &&
             rcpt_obj["status"].as_string() == "0x0") {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "matching tx {} reverted on-chain", req.id, tx_hash_hex);
            bump_mismatch();
            return false;
         }
         ilog("underwriter: source-deposit verify passed for uwreq {} "
              "(SwapDeposit id={} tx={} finalized_head={})",
              req.id, deposit_id, tx_hash_hex, finalized_blk);
         return true;
      } catch (const fc::exception& e) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "RPC error: {}", req.id, e.to_detail_string());
         bump_mismatch();
         return false;
      } catch (const std::exception& e) {
         // std::stoul/std::stoull on a malformed or non-hex RPC field throw std::invalid_argument /
         // std::out_of_range, which are NOT fc::exception; catch them here so one bad response fails
         // just this uwreq instead of escaping and aborting the whole scan cycle for every request.
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "malformed RPC response: {}", req.id, e.what());
         bump_mismatch();
         return false;
      }
   }

   /// SOL-side source-deposit verification. Mirrors
   /// `verify_source_deposit_eth` step-for-step:
   ///
   ///   1. `req.source_tx_id` is an 8-byte big-endian monotonic
   ///      `deposit_id` (minted by `opp-outpost::request_swap`,
   ///      `OutboundMessageBuffer.next_swap_id`). Wrong-size →
   ///      hard mismatch.
   ///   2. `fc::task::retry_until` with
   ///      `SOL_SWAP_DEPOSIT_POLL_INTERVAL` (15s) /
   ///      `SOL_SWAP_DEPOSIT_TOTAL_TIMEOUT` (120s, both in
   ///      `outpost_solana_client_plugin.hpp`) drives the paginated scan:
   ///        a. `getSignaturesForAddress(sol_program_id,
   ///           before=<persisted cursor>, limit=SOL_SIGNATURE_SCAN_PAGE_SIZE)`
   ///           enumerates the next older page of program sigs.
   ///        b. For each sig, `getTransaction(sig)`; inspect
   ///           `meta.err` (skip failed tx) and
   ///           `meta.logMessages[]` for the canonical marker:
   ///           `Program log: opp_outpost: SwapDeposit id=<id> hash=<64hex>`.
   ///        c. Persist the next `before` cursor only after a clean page with
   ///           no match; unfinalized matches and transient tx fetch failures
   ///           retry the same page.
   ///   3. On match, parse the 32-byte on-chain hash from the log.
   ///   4. Recompute the same hash from the UWREQ row's flat fields
   ///      (depositor[32] + 7×u64 BE + u32 BE = 92 packed bytes).
   ///      Bit-exact mismatch → hard mismatch counter.
   ///   5. Finality gate: matching tx's `meta.confirmationStatus`
   ///      (when present on the per-sig listing from step 2a) must
   ///      be `"finalized"` — same finality requirement as ETH's finalized
   ///      log lookup. Tx not yet finalized → deferred retry (no mismatch bump).
   ///
   /// `false` returns:
   ///   - hard mismatch on bad size;
   ///   - terminal source-deposit failure (counter bumped once per pending
   ///     UWREQ) on hash divergence, malformed marker hash, or full-history
   ///     exhaustion without a marker;
   ///   - deferred retry (no counter bump) on `retry_until` deadline
   ///     reached without a matching finalized log — the outer poll loop
   ///     reattempts on its next tick from the persisted cursor.
   bool verify_source_deposit_sol(const uw_request& req) {
      // SEC-13/WSA-027: resolve this leg's outpost wiring by EXACT chain_code.
      const auto ep_it = outpost_endpoints.find(req.src_chain_code.value);
      if (ep_it == outpost_endpoints.end()) {
         elog("underwriter: no outpost endpoint configured for source chain={} "
              "(uwreq {})", req.src_chain_code.to_string(), req.id);
         return false;
      }
      const std::string& sol_client_id  = ep_it->second.client_id;
      const std::string& sol_program_id = ep_it->second.commit_addr;

      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: SOL client '{}' not found for source-deposit verify "
              "(uwreq {})", sol_client_id, req.id);
         return false;
      }
      auto bump_mismatch = [&]() {
         std::lock_guard lk{stats_mutex};
         source_deposit_mismatch_count++;
      };

      // ── (1) source_tx_id → 8-byte big-endian deposit_id ────────────────
      if (req.source_tx_id.size() != 8) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "source_tx_id has wrong size ({} bytes; expected 8 for SOL "
              "monotonic deposit_id mirroring the ETH wire shape)",
              req.id, req.source_tx_id.size());
         bump_mismatch();
         return false;
      }
      uint64_t deposit_id = 0;
      for (size_t i = 0; i < 8; ++i) {
         deposit_id = (deposit_id << 8) |
                      static_cast<uint8_t>(req.source_tx_id[i]);
      }

      // ── (4) Recompute the expected correlation hash from UWREQ fields ──
      // Layout MUST stay synchronized with the producer side
      // (`opp-outpost/src/instructions/request_swap.rs::correlation_hash`).
      // 32 + 7×8 + 4 = 92 bytes total.
      std::vector<uint8_t> packed;
      packed.reserve(32 + 7 * 8 + 4);
      if (req.depositor.size() != 32) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "depositor has wrong size ({} bytes; expected 32 for SVM "
              "Ed25519 pubkey)", req.id, req.depositor.size());
         bump_mismatch();
         return false;
      }
      packed.insert(packed.end(), req.depositor.begin(), req.depositor.end());
      auto append_u64_be = [&](uint64_t v) {
         for (int shift = 56; shift >= 0; shift -= 8) {
            packed.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
         }
      };
      auto append_u32_be = [&](uint32_t v) {
         for (int shift = 24; shift >= 0; shift -= 8) {
            packed.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
         }
      };
      append_u64_be(req.src_amount);
      append_u64_be(req.src_token_code.value);
      append_u64_be(req.src_reserve_code.value);
      append_u64_be(req.dst_chain_code.value);
      append_u64_be(req.dst_token_code.value);
      append_u64_be(req.dst_reserve_code.value);
      append_u64_be(req.dst_amount);
      append_u32_be(req.variance_tolerance_bps);
      if (packed.size() != 32 + 7 * 8 + 4) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "packed buffer size {} != expected {}",
              req.id, packed.size(), 32 + 7 * 8 + 4);
         bump_mismatch();
         return false;
      }
      const auto recomputed_hash = fc::crypto::keccak256::hash(
         std::span<const uint8_t>{packed.data(), packed.size()});

      // Canonical marker the producer emits on `request_swap`. The
      // Solana JSON-RPC `meta.logMessages[]` array contains each
      // `msg!` line verbatim, prefixed with `"Program log: "`.
      const std::string marker_prefix =
         "Program log: opp_outpost: SwapDeposit id=" +
         std::to_string(deposit_id) + " hash=";

      // ── (2) + (3) retry-loop until matched or budget expires ───────────
      // `bool hard_mismatch` distinguishes "found and known-bad" (hash
      // divergence / malformed marker / exhausted history) from "not yet
      // found" (deferred). Terminal SOL failures are cached until the UWREQ
      // leaves PENDING so later scan cycles do not re-walk full history.
      // The retry-callback returns:
      //   - Some(true)                 -> success, exit retry_until
      //   - Some(false) hard_mismatch  -> fail-fast, exit retry_until
      //   - None                       -> deferred, sleep + retry
      // Outer `try { retry_until } catch (timeout_exception)` maps the
      // deadline-without-match case to deferred-no-mismatch-bump, while the
      // cursor survives for the next outer scan tick.
      const underwriter::solana_source_deposit_scan_key scan_key{req.id, deposit_id};
      if (const auto terminal = get_sol_source_deposit_terminal_failure(scan_key)) {
         ilog("underwriter: source-deposit verify skipped for uwreq {} — "
              "previous terminal SOL verification failure: {} "
              "(deposit_id={}, pages_scanned={}, signatures_scanned={})",
              req.id,
              terminal->terminal_failure_reason,
              deposit_id,
              terminal->pages_scanned,
              terminal->signatures_scanned);
         return false;
      }

      bool        hard_mismatch_seen = false;
      std::string matched_sig;

      try {
         fc::task::retry_options opts;
         opts.initial_backoff = SOL_SWAP_DEPOSIT_POLL_INTERVAL;
         opts.max_backoff     = SOL_SWAP_DEPOSIT_POLL_INTERVAL;
         opts.total_timeout   = SOL_SWAP_DEPOSIT_TOTAL_TIMEOUT;
         opts.growth_factor   = 1.0;   // fixed-interval cadence

         std::function<std::optional<bool>()> attempt =
         [&]() -> std::optional<bool> {
            const auto cursor = get_or_create_sol_source_deposit_scan_cursor(scan_key);
            std::vector<fc::variant> sigs;
            try {
               sigs = entry->client->get_signatures_for_address(
                  sol_program_id,
                  cursor.before,
                  std::nullopt,
                  underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE);
            } catch (const fc::exception& e) {
               ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                    "getSignaturesForAddress({}, before={}) RPC error: {}",
                    req.id,
                    sol_program_id,
                    cursor.before.value_or("<newest>"),
                    e.to_detail_string());
               return std::nullopt;
            }

            const underwriter::solana_source_deposit_page_scan_config scan_config{
               .sol_program_id = sol_program_id,
               .marker_prefix = marker_prefix,
               .recomputed_hash = recomputed_hash,
            };
            const auto scan_result = underwriter::scan_solana_source_deposit_signature_page(
               sigs,
               [&](const std::string& sig_b58) {
                  return entry->client->get_transaction(sig_b58, underwriter::SOL_COMMITMENT);
               },
               scan_config);

            using page_status = underwriter::solana_source_deposit_page_status;
            switch (scan_result.status) {
               case page_status::matched:
                  matched_sig = scan_result.matched_signature;
                  erase_sol_source_deposit_scan_cursor(scan_key);
                  return std::optional<bool>(true);
               case page_status::hard_mismatch:
                  elog("underwriter: source-deposit verify failed for uwreq {} — "
                       "{} (deposit_id={}, program={}, before={})",
                       req.id,
                       scan_result.reason,
                       deposit_id,
                       sol_program_id,
                       cursor.before.value_or("<newest>"));
                  hard_mismatch_seen = true;
                  {
                     const auto terminal = record_sol_source_deposit_terminal_failure(
                        scan_key, scan_result.reason, sigs.size());
                     if (terminal.first_failure) bump_mismatch();
                  }
                  return std::optional<bool>(false);
               case page_status::deferred:
                  ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                       "{} (deposit_id={}, program={}, before={})",
                       req.id,
                       scan_result.reason,
                       deposit_id,
                       sol_program_id,
                       cursor.before.value_or("<newest>"));
                  return std::nullopt;
               case page_status::not_found:
                  break;
            }

            if (scan_result.page_exhausted) {
               const std::string reason =
                  "exhausted Solana program signature history without SwapDeposit log";
               elog("underwriter: source-deposit verify failed for uwreq {} — "
                    "{} (deposit_id={}, program={}, before={}, page_size={})",
                    req.id,
                    reason,
                    deposit_id,
                    sol_program_id,
                    cursor.before.value_or("<newest>"),
                    sigs.size());
               hard_mismatch_seen = true;
               {
                  const auto terminal = record_sol_source_deposit_terminal_failure(
                     scan_key, reason, sigs.size());
                  if (terminal.first_failure) bump_mismatch();
               }
               return std::optional<bool>(false);
            }

            if (!scan_result.next_before) {
               ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                    "signature page produced no usable pagination cursor "
                    "(deposit_id={}, program={}, before={}, page_size={})",
                    req.id,
                    deposit_id,
                    sol_program_id,
                    cursor.before.value_or("<newest>"),
                    sigs.size());
               return std::nullopt;
            }

            const auto advanced = advance_sol_source_deposit_scan_cursor(
               scan_key, *scan_result.next_before, sigs.size());
            ilog("underwriter: source-deposit verify paginated for uwreq {} — "
                 "no SwapDeposit log in page (deposit_id={}, program={}, "
                 "before={}, next_before={}, pages_scanned={}, signatures_scanned={})",
                 req.id,
                 deposit_id,
                 sol_program_id,
                 cursor.before.value_or("<newest>"),
                 *scan_result.next_before,
                 advanced.pages_scanned,
                 advanced.signatures_scanned);
            return std::nullopt;   // not found this iteration; sleep + retry
         };

         const bool ok = fc::task::retry_until<bool>(
            "underwriter:verify_source_deposit_sol", opts, attempt);
         if (ok) {
            ilog("underwriter: source-deposit verify passed for uwreq {} "
                 "(deposit_id={} tx={} program={})",
                 req.id, deposit_id, matched_sig, sol_program_id);
            return true;
         }
         // retry_until returned `false` → hard mismatch already counted +
         // logged inside the callback. Propagate the failure unchanged.
         return false;
      } catch (const fc::timeout_exception&) {
         // Deadline reached without finding a matching log. Treat as a
         // deferred retry by the outer underwriter loop — DO NOT bump
         // the mismatch counter, matching the ETH verify's semantics
         // for "log not yet present."
         if (hard_mismatch_seen) {
            // Defensive: shouldn't get here since hard mismatch returns
            // Some(false) above, but if it ever does, keep counting.
            return false;
         }
         ilog("underwriter: source-deposit verify deferred for uwreq {} — "
              "no SwapDeposit log line for deposit_id={} after {}s "
              "(program={}, scan-interval={}s, page-size={}, will retry from persisted cursor)",
              req.id, deposit_id,
              SOL_SWAP_DEPOSIT_TOTAL_TIMEOUT.count() / 1'000'000,
              sol_program_id,
              SOL_SWAP_DEPOSIT_POLL_INTERVAL.count() / 1'000'000,
              underwriter::SOL_SIGNATURE_SCAN_PAGE_SIZE);
         return false;
      } catch (const fc::exception& e) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "RPC error: {}", req.id, e.to_detail_string());
         bump_mismatch();
         return false;
      }
   }

   /**
    * Submit a `commit` JSON-RPC call to BOTH legs of the swap (source +
    * destination outposts). Each outpost queues an UNDERWRITE_INTENT_COMMIT
    * attestation back to the depot; the depot's race resolver
    * (sysio.uwrit::try_select_winner) reconstructs the digest, verifies
    * the signature against the underwriter's account permissions, and
    * promotes the underwriter to winner iff both legs' signatures verify
    * AND both legs' bond covers (via `available()` rollup).
    *
    * Per the corrected ledger model: outposts don't validate the signature
    * or the bond — they just authenticate the caller as a registered
    * active underwriter and relay the UIC bytes verbatim. The depot does
    * the real authorization.
    */
   void submit_intent_to_outpost(const uw_request& req) {
      // T13: confirm the source-chain deposit backing this swap is real
      // before committing collateral. Returns true with a warning log
      // until the swap-emit-site populates source_tx_id (staged rollout).
      if (!verify_source_deposit(req)) {
         ilog("underwriter: skipping uwreq {} — source-deposit verify failed",
              req.id);
         return;
      }

      ilog("underwriter: submitting commit pair for uwreq {} "
           "src=({},{},{}) dst=({},{},{})",
           req.id,
           req.src_chain_code.to_string(),
           req.src_token_code.to_string(),
           req.src_reserve_code.to_string(),
           req.dst_chain_code.to_string(),
           req.dst_token_code.to_string(),
           req.dst_reserve_code.to_string());

      // Per-leg dispatch keyed on the v6 slug_name triple
      // `(chain_code, token_code, reserve_code)`. Same-chain swaps (e.g.
      // ERC20 → native on one outpost) share `chain_code` between the two
      // legs but differ on `token_code`/`reserve_code`; the UIC payload
      // carries the full triple so the depot's `rcrdcommit` can route to
      // the correct source/dest slot on `commit_entry`.
      //
      // Confirmation discipline: we skip any leg whose exact
      // `(uwreq_id, chain_code, token_code, reserve_code)` identity is already
      // in `confirmed_commits` (a previous cycle's tx confirmed on-chain).
      // After a successful confirm we record the key so the next scan doesn't
      // resubmit. Per project rules: confirm BEFORE recording so a
      // partial-landing in the map cannot happen without OPP breakage.
      auto submit_one = [this](ChainKind    chain,
                               fc::slug_name chain_code,
                               fc::slug_name token_code,
                               fc::slug_name reserve_code,
                               uint64_t     uw_request_id,
                               bool         is_depot) {
         if (is_depot) {
            // Intended, not an error: depot (WIRE) legs are never
            // underwritten — no outpost exists, no UIC is built, no bond
            // is consumed. Single-leg swaps commit only their real leg.
            dlog("underwriter: uwreq {} leg chain={} is the WIRE depot — "
                 "no commit needed", uw_request_id, chain_code.to_string());
            return;
         }
         const commit_key key{uw_request_id, chain_code.value, token_code.value,
                              reserve_code.value};
         if (confirmed_commits.contains(key)) {
            ilog("underwriter: skip already-confirmed commit uwreq={} chain={} token={}",
                 uw_request_id,
                 chain_code.to_string(),
                 token_code.to_string());
            return;
         }

         auto uic_bytes = build_signed_uic_bytes(
            uw_request_id, chain, chain_code, token_code, reserve_code);
         if (uic_bytes.empty()) return;   // already logged

         // Chain-agnostic SPI call. The `outpost_by_chain` map carries one
         // `outpost_client` per EXACT `chain_code` (SEC-13/WSA-027), built at
         // startup via the outpost-plugin factories. Each concrete owns its own
         // ABI / IDL discovery, address encoding, and on-chain confirmation
         // discipline — this loop just relays opaque UIC bytes through the
         // virtual. Per `outpost-client-spi.md`.
         auto it = outpost_by_chain.find(chain_code.value);
         if (it == outpost_by_chain.end()) {
            elog("underwriter: no outpost_client wired for chain={} (uwreq {})",
                 chain_code.to_string(), uw_request_id);
            std::lock_guard lk{stats_mutex};
            commits_failed_count++;
            return;
         }
         bool confirmed = false;
         try {
            auto tx_or_sig = it->second->uw_commit(
               uw_request_id, uic_bytes, fc::milliseconds(action_timeout_ms));
            ilog("underwriter: commit landed on {} uwreq={} tx_or_sig={}",
                 it->second->to_string(), uw_request_id, tx_or_sig);
            confirmed = true;
         } catch (const fc::exception& e) {
            elog("underwriter: commit failed on {} for uwreq={}: {}",
                 it->second->to_string(), uw_request_id, e.to_detail_string());
         }
         std::lock_guard lk{stats_mutex};
         if (confirmed) {
            confirmed_commits.insert(key);
            commits_confirmed_count++;
         } else {
            commits_failed_count++;
         }
      };
      submit_one(req.src_chain,
                 req.src_chain_code, req.src_token_code, req.src_reserve_code,
                 req.id, req.src_is_depot);
      submit_one(req.dst_chain,
                 req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
                 req.id, req.dst_is_depot);
   }

   // Outpost commit submission is delegated entirely to the `outpost_client`
   // SPI — see `outpost_by_chain` above and `submit_intent_to_outpost` for
   // the dispatch. Chain-specific ABI / IDL discovery, address encoding,
   // and on-chain confirmation live inside
   // `outpost_ethereum_client::uw_commit` and
   // `outpost_solana_client::uw_commit`. Per `outpost-client-spi.md`.

   // The plugin previously carried a `push_action()` helper for signing
   // and pushing WIRE-chain actions; after the commit refactor (T9 + T14)
   // the underwriter does not push any WIRE-chain actions on its own —
   // commits go via the outpost_client SPI. The
   // signature_provider_manager_plugin dependency is still required
   // because `build_signed_uic_bytes` uses it to sign the UIC digest
   // with the underwriter's WIRE K1 key.

   // ── HTTP API: /v1/underwriter/* ─────────────────────────────────────
   // Read-only diagnostic surface for the operator. Wraps internal state
   // (`confirmed_commits`, counters, account/config) under `stats_mutex`
   // so http_plugin worker threads and the single cron thread don't race.
   //
   // Endpoints:
   //   /v1/underwriter/stats   — session counters + config snapshot
   //   /v1/underwriter/commits — outstanding confirmed commits (per leg)
   //
   // Both carry a `status` discriminator: until the deferred startup body
   // completes they answer with the gate state (`waiting_for_sync` +
   // `head_behind_sec`/`lib_behind_sec`, `preflight_retrying`, or a terminal
   // `preflight_failed`/`wiring_failed`/`startup_failed` with `detail`);
   // once active they serve the payloads below with `status:"active"`.

   fc::variant build_stats_response() {
      std::lock_guard lk{stats_mutex};
      size_t active_sol_source_deposit_cursor_count = 0;
      size_t sol_source_deposit_terminal_failure_count = 0;
      for (const auto& entry : sol_source_deposit_scan_cursors) {
         if (entry.second.terminal_failure) {
            sol_source_deposit_terminal_failure_count++;
         } else {
            active_sol_source_deposit_cursor_count++;
         }
      }
      // SEC-13/WSA-027: surface the per-chain outpost wiring (was the single
      // eth/sol client-id + address) so operators can confirm each served
      // chain is configured.
      std::vector<fc::variant> ep_v;
      ep_v.reserve(outpost_endpoints.size());
      for (const auto& [code, ep] : outpost_endpoints) {
         ep_v.push_back(fc::mutable_variant_object()
            ("chain_code",          fc::slug_name{code}.to_string())
            ("kind",                ChainKind_Name(ep.kind))
            ("client_id",           ep.client_id)
            ("commit_addr",         ep.commit_addr)
            ("source_deposit_addr", ep.source_deposit_addr));
      }
      return fc::variant(fc::mutable_variant_object()
         (underwriter_detail::field::status, std::string{underwriter_detail::active_status})
         ("underwriter_account",            underwriter_account.to_string())
         ("enabled",                        enabled)
         ("is_active",                      is_active)
         ("scan_interval_ms",               scan_interval_ms)
         ("action_timeout_ms",              action_timeout_ms)
         ("outpost_endpoints",              fc::variant(std::move(ep_v)))
         ("eth_source_deposit_function",    eth_source_deposit_function_name)
         ("sol_source_deposit_instruction", sol_source_deposit_instruction_name)
         ("uwreqs_seen_pending",            uwreqs_seen_pending_count)
         ("commits_confirmed",              commits_confirmed_count)
         ("commits_failed",                 commits_failed_count)
         ("source_deposit_mismatch",        source_deposit_mismatch_count)
         ("outstanding_commit_count",       confirmed_commits.size())
         ("sol_source_deposit_cursor_count", active_sol_source_deposit_cursor_count)
         ("sol_source_deposit_terminal_failure_count", sol_source_deposit_terminal_failure_count)
      );
   }

   fc::variant build_commits_response() {
      std::lock_guard lk{stats_mutex};
      std::vector<fc::variant> entries;
      entries.reserve(confirmed_commits.size());
      for (const auto& k : confirmed_commits) {
         entries.push_back(fc::variant(fc::mutable_variant_object()
            ("uwreq_id",     k.uwreq_id)
            ("chain_code",   fc::slug_name{k.chain_code}.to_string())
            ("token_code",   fc::slug_name{k.token_code}.to_string())
            ("reserve_code", fc::slug_name{k.reserve_code}.to_string())
         ));
      }
      return fc::variant(fc::mutable_variant_object()
         (underwriter_detail::field::status, std::string{underwriter_detail::active_status})
         ("count",   entries.size())
         ("entries", std::move(entries))
      );
   }

   /// Answer `cb` with the gate-state payload and return true when the
   /// deferred startup body has not completed (waiting for sync, retrying,
   /// or failed terminally); return false once {@link run_deferred_startup}
   /// reached `active` so the caller serves the real payload.
   bool respond_if_gated(const url_response_callback& cb) {
      const auto state = gate_state.load();
      if (state == underwriter_detail::startup_state::active) {
         return false;
      }
      // Only the waiting_for_sync payload carries the behind-now gaps; skip
      // the chain reads for the other states. `lib_behind_sec` is the gate's
      // actual criterion (reads serve the irreversible state); the head gap
      // distinguishes a finality-stalled node from one still catching up.
      // An absent irreversible root stays a typed empty optional here — the
      // payload builder turns it into the wire's -1 sentinel.
      fc::microseconds                head_behind{};
      std::optional<fc::microseconds> lib_behind;
      if (state == underwriter_detail::startup_state::waiting_for_sync) {
         auto&      chain = chain_plug->chain();
         const auto now   = fc::time_point::now();
         head_behind      = now - chain.head().block_time();
         if (chain.fork_db_has_root()) {
            lib_behind = now - chain.fork_db_root().block_time();
         }
      }
      cb(200, underwriter_detail::startup_gate_payload(state, head_behind, lib_behind));
      return true;
   }

   /// Register the `/v1/underwriter/*` HTTP endpoints. Called once from
   /// `plugin_startup` when the underwriter is enabled, BEFORE the sync gate:
   /// `http_plugin`'s handler map is read lock-free by the HTTP worker
   /// threads, so every registration must happen during plugin startup —
   /// before the posted listener creation runs — never from a task queued
   /// after `exec()` is live. Until the deferred startup body completes the
   /// handlers report the gate state (see {@link respond_if_gated}).
   ///
   /// Both endpoints live in the dedicated `api_category::underwriter`, NOT
   /// `api_category::node`: they expose operator metadata (account identity,
   /// client ids, outpost contract addresses, outstanding commits) that must
   /// not ride the always-on node category onto category-isolated public
   /// listeners. They remain reachable on the default all-category listeners
   /// (`http-server-address` / `unix-socket-path`); on category-isolated
   /// setups the operator opts in with `--http-category-address=underwriter,<addr>`.
   void register_http_endpoints() {
      auto& hp = app().get_plugin<http_plugin>();
      hp.add_api({
         {"/v1/underwriter/stats", api_category::underwriter,
            [this](std::string&& /*url*/,
                    std::string&& /*body*/,
                    url_response_callback&& cb) {
               try {
                  if (respond_if_gated(cb)) {
                     return;
                  }
                  cb(200, build_stats_response());
               } catch (const fc::exception& e) {
                  cb(500, fc::variant(fc::mutable_variant_object()
                     ("error", e.to_detail_string())));
               }
            }},
         {"/v1/underwriter/commits", api_category::underwriter,
            [this](std::string&& /*url*/,
                    std::string&& /*body*/,
                    url_response_callback&& cb) {
               try {
                  if (respond_if_gated(cb)) {
                     return;
                  }
                  cb(200, build_commits_response());
               } catch (const fc::exception& e) {
                  cb(500, fc::variant(fc::mutable_variant_object()
                     ("error", e.to_detail_string())));
               }
            }},
      }, appbase::exec_queue::read_only);

      // Operator metadata should normally stay on loopback / private
      // management networks; a public bind is a deliberate choice worth a
      // startup warning (same pattern as the snapshot_ro exposure notice).
      if (!hp.is_on_loopback(api_category::underwriter)) {
         wlog("underwriter diagnostics API (/v1/underwriter/*) exposed on a non-loopback address");
      }
   }
};

// ---------------------------------------------------------------------------
//  Plugin lifecycle
// ---------------------------------------------------------------------------
underwriter_plugin::underwriter_plugin()
   : _impl(std::make_unique<impl>()) {}

underwriter_plugin::~underwriter_plugin() = default;

void underwriter_plugin::set_program_options(options_description& cli,
                                              options_description& cfg) {
   auto opts = cfg.add_options();
   opts("underwriter-account", bpo::value<std::string>(),
        "WIRE account name for this underwriter");
   opts("underwriter-scan-interval-ms", bpo::value<uint32_t>()->default_value(underwriter_defaults::scan_interval_ms),
        "How often to scan for pending underwrite requests (ms)");
   opts("underwriter-action-timeout-ms", bpo::value<uint32_t>()->default_value(underwriter_defaults::action_timeout_ms),
        "Timeout for outpost contract calls and table reads (ms)");
   opts("underwriter-enabled", bpo::value<bool>()->default_value(underwriter_defaults::enabled),
        "Enable underwriter functionality");
   opts("underwriter-eth-outpost",
        bpo::value<std::vector<std::string>>()->composing(),
        "Per-EVM-chain outpost wiring (repeatable, one per EVM chain served). Format: "
        "`<chain_code>,<client_id>,<operator_registry_addr>,<source_deposit_contract_addr>` — "
        "chain_code is the sysio.chains codename (e.g. ETHEREUM); client_id names the RPC "
        "connection registered via --outpost-ethereum-client; operator_registry_addr is the OPP "
        "OperatorRegistry (uw_commit target); source_deposit_contract_addr is the SwapDeposit-"
        "emitting contract scanned by the verify path. SEC-13/WSA-027: keyed by exact chain_code, "
        "so two EVM chains are wired independently.");
   opts("underwriter-sol-outpost",
        bpo::value<std::vector<std::string>>()->composing(),
        "Per-SVM-chain outpost wiring (repeatable, one per SVM chain served). Format: "
        "`<chain_code>,<client_id>,<opp_outpost_program_id>` — client_id names the RPC connection "
        "registered via --outpost-solana-client; program_id is the opp-outpost program (used for "
        "both commit_underwrite and the source-deposit scan).");
   opts("underwriter-eth-source-deposit-function", bpo::value<std::string>(),
        "Name of the ETH swap-deposit function. Resolved at preflight against the ABI "
        "files registered with --ethereum-abi-file; the matching `function` entry's keccak256 "
        "signature yields the chain-agnostic 4-byte selector. The per-chain source contract "
        "address comes from --underwriter-eth-outpost. Required.");
   opts("underwriter-sol-source-deposit-instruction", bpo::value<std::string>(),
        "Name of the SOL swap-deposit instruction. Resolved at preflight against the IDL "
        "files registered with --solana-idl-file; the matching instruction's anchor "
        "discriminator (8 bytes) is used to identify the deposit instruction in source "
        "txs. Required.");
   opts(sysio::underwriter::ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION.data(),
        bpo::value<uint64_t>()->default_value(
           sysio::underwriter::ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS),
        "Maximum number of recent finalized ETH blocks searched for the source "
        "SwapDeposit event. Bounds the per-uwreq eth_getLogs filter so stale "
        "or invalid deposit ids cannot trigger whole-history provider scans.");
}

void underwriter_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("underwriter-account"))
      _impl->underwriter_account = chain::name(options["underwriter-account"].as<std::string>());
   _impl->scan_interval_ms  = options["underwriter-scan-interval-ms"].as<uint32_t>();
   _impl->action_timeout_ms = options["underwriter-action-timeout-ms"].as<uint32_t>();
   _impl->enabled           = options["underwriter-enabled"].as<bool>();
   // SEC-13/WSA-027: parse the repeatable per-chain outpost wiring into
   // `outpost_endpoints`, keyed by EXACT chain_code. Each entry is a
   // comma-separated `<chain_code>,<client_id>,<addr...>`.
   {
      auto split_csv = [](const std::string& s) {
         std::vector<std::string> out;
         for (size_t start = 0;;) {
            const size_t comma = s.find(',', start);
            out.push_back(s.substr(start, comma == std::string::npos ? comma : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
         }
         return out;
      };
      auto parse_outpost = [&](const char* opt, ChainKind kind, size_t min_fields) {
         if (!options.count(opt)) return;
         for (const auto& spec : options[opt].as<std::vector<std::string>>()) {
            const auto f = split_csv(spec);
            if (f.size() < min_fields || f[0].empty() || f[1].empty() || f[2].empty()) {
               elog("underwriter: ignoring malformed {} entry '{}' (need "
                    ">= {} non-empty comma-separated fields)", opt, spec, min_fields);
               continue;
            }
            try {
               outpost_endpoint ep;
               ep.kind                = kind;
               ep.client_id           = f[1];
               ep.commit_addr         = f[2];
               ep.source_deposit_addr = (f.size() > 3 && !f[3].empty()) ? f[3] : f[2];
               _impl->outpost_endpoints[fc::slug_name{f[0]}.value] = ep;
            } catch (const fc::exception& e) {
               elog("underwriter: ignoring {} entry '{}' — bad chain_code '{}': {}",
                    opt, spec, f[0], e.to_detail_string());
            }
         }
      };
      parse_outpost("underwriter-eth-outpost", ChainKind::CHAIN_KIND_EVM, /*min_fields=*/4);
      parse_outpost("underwriter-sol-outpost", ChainKind::CHAIN_KIND_SVM, /*min_fields=*/3);
   }
   if (options.count("underwriter-eth-source-deposit-function"))
      _impl->eth_source_deposit_function_name =
         options["underwriter-eth-source-deposit-function"].as<std::string>();
   if (options.count("underwriter-sol-source-deposit-instruction"))
      _impl->sol_source_deposit_instruction_name =
         options["underwriter-sol-source-deposit-instruction"].as<std::string>();
   _impl->eth_source_deposit_lookback_blocks =
      options[sysio::underwriter::ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION.data()].as<uint64_t>();
   FC_ASSERT(_impl->eth_source_deposit_lookback_blocks > 0,
             "{} must be positive",
             sysio::underwriter::ETH_SOURCE_DEPOSIT_LOOKBACK_BLOCKS_OPTION);

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug  = &app().get_plugin<cron_plugin>();
   _impl->eth_plug   = &app().get_plugin<outpost_ethereum_client_plugin>();
   _impl->sol_plug   = &app().get_plugin<outpost_solana_client_plugin>();

   // Operator daemons are designed for read-mode = irreversible: the sync gate
   // (controller::is_synced) measures LIB recency and every local table read the
   // preflight and scan cycle perform serves the irreversible view. Any other read
   // mode would validate/underwrite against state that can still fork out.
   FC_ASSERT(!_impl->enabled ||
                _impl->chain_plug->chain().get_read_mode() == chain::db_read_mode::IRREVERSIBLE,
             "underwriter_plugin requires read-mode = irreversible (configured read-mode = {})",
             magic_enum::enum_name(_impl->chain_plug->chain().get_read_mode()));
}

void underwriter_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("underwriter_plugin: disabled, skipping startup");
      return;
   }

   ilog("underwriter_plugin: starting for account {}", _impl->underwriter_account.to_string());

   // Register the `/v1/underwriter/*` endpoints FIRST, before the sync
   // gate: handler registration must complete during plugin startup
   // (before the posted HTTP listener goes live) because the handler map
   // is read lock-free by the HTTP threads — it must never ride the
   // deferred startup body below. Until that body completes, the handlers
   // answer with the gate state, so a cold-booting (or terminally-failed)
   // underwriter is diagnosable over HTTP instead of a single ilog.
   _impl->register_http_endpoints();

   // The preflight validates depot-side state (opreg registration, chain
   // registry, authex links) via LOCAL table reads. On a cold-booting
   // operator node those reads see mid-sync (possibly genesis) state and
   // would fail spuriously, so the whole startup body (preflight → outpost
   // client wiring → cron) is DEFERRED until the node is synced —
   // `controller::is_synced()`: the LAST IRREVERSIBLE block's time within
   // `controller::default_sync_recency_ms` of now (the state the reads
   // actually serve under read-mode = irreversible). The wake-up is the
   // existing `irreversible_block` channel: a LIB advance is the only event
   // that can turn the predicate true, and channel deliveries are posted to
   // the application executor — main thread, AFTER the triggering block
   // fully commits — so the callback may run the startup body directly.
   // There is deliberately no already-synced fast path: operator daemons
   // boot with genesis-stale LIB in every deployment topology (they are
   // never co-hosted with a producer), and a node that somehow is synced at
   // startup is released by the next LIB advance. Post-arming, the preflight
   // retries within a bounded grace (see attempt_deferred_startup) to absorb
   // the LIB boundary race; a genuinely incomplete bootstrap still fails
   // loudly once the grace expires and shuts the node down (fail-fast) —
   // there remains no dev escape hatch.
   auto& chain = _impl->chain_plug->chain();
   ilog("underwriter_plugin: waiting for chain sync before preflight "
        "(head {} is {}s behind now; irreversible state is {}s behind)",
        chain.head().block_num(),
        (fc::time_point::now() - chain.head().block_time()).to_seconds(),
        chain.fork_db_has_root()
           ? (fc::time_point::now() - chain.fork_db_root().block_time()).to_seconds()
           : -1);
   _impl->sync_gate_subscription =
      app().get_channel<chain::plugin_interface::channels::irreversible_block>().subscribe(
         [this](const chain::block_signal_params&) {
            if (_impl->startup_attempted() || _impl->shutting_down ||
                !_impl->chain_plug->chain().is_synced()) {
               return;
            }
            // One-shot consumption: unsubscribe (safe from within the slot) and
            // run the startup body directly — channel deliveries are posted to
            // the application executor, so this already runs on the main thread
            // AFTER the triggering block committed (the exact context the old
            // accepted_block gate had to re-post itself into; running mid
            // block-application, table reads see an incomplete view — observed
            // live: a registered operator row read back EMPTY → spurious
            // preflight failure).
            _impl->sync_gate_subscription.unsubscribe();
            ilog("underwriter_plugin: chain synced — starting deferred startup");
            _impl->run_deferred_startup();
         });
}

void underwriter_plugin::plugin_shutdown() {
   _impl->shutting_down = true;
   if (_impl->preflight_retry_timer) {
      _impl->preflight_retry_timer->cancel();
   }

   if (_impl->scan_job_id != 0) {
      auto& cron = app().get_plugin<cron_plugin>();
      cron.cancel_job(_impl->scan_job_id);
   }

   ilog("underwriter_plugin: shutdown complete");
}

} // namespace sysio
