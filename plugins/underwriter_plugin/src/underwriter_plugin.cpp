#include <fc/log/logger.hpp>
#include <fc/slug_name.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/io/raw.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/task/retry.hpp>
#include <fc/variant_object.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/endian/conversion.hpp>
#include <magic_enum/magic_enum.hpp>

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>
#include <sysio/underwriter_plugin/source_deposit_constants.hpp>
#include <sysio/depot/opreg_status.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/types/types.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>

#include <mutex>

#include <algorithm>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_set>

namespace sysio {

using namespace chain_apis;
using namespace sysio::opp::types;
namespace opp_att = sysio::opp::attestations;

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
   /// Source-chain identifier. ETH = 8-byte big-endian `SwapDeposit.id`
   /// (the outpost-local monotonic counter); SOL = 64-byte signature.
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
};

// ---------------------------------------------------------------------------
//  Credit line — per-(chain_kind, token_kind) bond from sysio.opreg::operators
//
//  Reads the `balances` field added in opreg's Task 2 refactor (one
//  aggregate balance per (chain, token_kind), replacing the old
//  std::vector<stake_entry>). Note this is the RAW balance — the
//  authoritative `available` rollup also subtracts active locks +
//  pending withdraws via `sysio.opreg::available()`. v1 of the plugin
//  treats raw balance as a sufficient gate; the depot's race resolver
//  (sysio.uwrit::try_select_winner) re-validates via the rollup.
// ---------------------------------------------------------------------------
struct credit_line {
   ChainKind   chain_kind;
   TokenKind   token_kind;
   uint64_t    balance;
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
   std::string  eth_client_id;
   std::string  sol_client_id;
   std::string  eth_opreg_addr;             // OperatorRegistry contract address on ETH
   /// Minimum ETH block confirmations required before a SwapDeposit
   /// log is accepted as source-deposit-verified. Default mirrors
   /// `underwriter::ETH_MIN_CONFIRMATIONS` (12, mainnet-safe). The
   /// test cluster overrides via `--underwriter-eth-min-confirmations 1`.
   uint64_t     eth_min_confirmations =
      sysio::underwriter::ETH_MIN_CONFIRMATIONS;
   /// opp-outpost program ID on SOL. Not a CLI option — resolved at
   /// preflight from the loaded IDL's top-level `address` field (or
   /// `metadata.address` on older IDLs).
   std::string  sol_program_id;
   /// Configured names of the swap-deposit function (ETH) / instruction
   /// (SOL). The contract address + function selector / instruction
   /// discriminator are resolved at preflight time from the ABI / IDL
   /// files registered with the outpost client plugins
   /// (`outpost_ethereum_client_plugin::get_abi_files()` /
   /// `outpost_solana_client_plugin::get_idl_files()`) so we don't
   /// duplicate that configuration here. Both are required at preflight.
   std::string  eth_source_deposit_function_name;
   std::string  sol_source_deposit_instruction_name;

   /// Resolved-at-preflight verify-path state derived from the above
   /// + the outpost client plugins' ABI / IDL surfaces. Used directly by
   /// `verify_source_deposit_{eth,sol}` — these are populated only after
   /// `run_preflight()` has succeeded.
   std::string  resolved_eth_source_contract_addr;
   std::vector<uint8_t> resolved_eth_source_deposit_selector;
   std::vector<uint8_t> resolved_sol_source_deposit_discriminator;

   // ── Diagnostic counters surfaced via the `/v1/underwriter/*` HTTP API
   //   (and the future `clio opp uw stats` wrapper).
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

   // Protects `confirmed_commits` and the diagnostic counters from
   // concurrent access between the cron-callback (single-threaded) and
   // the HTTP handler threads. The cron callback takes the lock around
   // mutations; the HTTP handlers take it around reads.
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

   // Outpost chain_kind cache: chain_code -> ChainKind
   std::map<uint64_t, ChainKind>     outpost_chain_kinds;

   /// SPI handles to the configured outposts, keyed by `ChainKind`. Built
   /// once at `plugin_startup` (after preflight) via the
   /// `outpost_{ethereum,solana}_client_plugin::create_outpost_client`
   /// factories. The relay loop selects by chain_kind and calls
   /// `outpost->uw_commit(...)` — every chain-specific concern (ABI /
   /// IDL discovery, address encoding, on-chain confirmation) lives in
   /// the concrete. Per `outpost-client-spi.md`.
   std::map<ChainKind, sysio::outpost_client_ptr> outpost_by_chain;
   /// v6 cross-walk: token slug_name → TokenKind enum. Refreshed each
   /// scan cycle by `read_credit_lines` (which reads `sysio.tokens::tokens`
   /// for the lookup); used by `scan_pending_requests` to translate the
   /// uwreq row's `src/dst_token_code` slug into the `TokenKind` the
   /// `select_coverable` bucket lookup needs.
   std::map<uint64_t, TokenKind>     token_kind_by_code;

   // ── Outstanding commit tracking (one entry per CONFIRMED leg) ───────
   // Per `feedback`: an underwriter that confirmed a commit tx for a leg
   // should NOT resubmit on the next scan cycle. Same-chain swaps share a
   // chain between legs, so `(uwreq_id, chain, token_kind)` is the
   // smallest discriminator. The set is pruned at the end of each scan
   // cycle to drop entries whose uwreq is no longer PENDING (the depot
   // has resolved the race), keeping the set bounded.
   struct commit_key {
      uint64_t  uwreq_id;
      ChainKind chain;
      TokenKind token_kind;
      friend auto operator<=>(const commit_key&, const commit_key&) = default;
   };
   std::set<commit_key>              confirmed_commits;

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
   //    2. `sysio.authex::links` covers every chain in the
   //       `sysio.epoch::outposts` registered set — without an authex link
   //       for a chain the underwriter cannot sign a commit on that chain.
   //    3. Non-zero balance on at least one TokenKind for every registered
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
      // ── Check 1: operator status ─────────────────────────────────────
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

      // ── Check 2: authex link coverage per outpost chain ──────────────
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

      // ── Check 3: non-zero RAW balance per outpost chain ──────────────
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

      // ── Check 4: required CLI options + ABI/IDL resolution ───────────
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
      // matches. The match yields the deployed `contract_address` and
      // the keccak256(signature) from which we derive the 4-byte
      // selector. Both are cached on the plugin for the verify path.
      {
         resolved_eth_source_contract_addr.clear();
         resolved_eth_source_deposit_selector.clear();
         bool found = false;
         for (const auto& [path, contracts] : eth_plug->get_abi_files()) {
            for (const auto& c : contracts) {
               if (c.type != fc::network::ethereum::abi::invoke_target_type::function) continue;
               if (c.name != eth_source_deposit_function_name) continue;
               if (c.contract_address.empty()) {
                  elog("underwriter preflight: ABI '{}' has function '{}' but no "
                       "`contract_address` metadata — populate the address in the "
                       "ABI file so the verify path knows the deployed contract",
                       path.string(), eth_source_deposit_function_name);
                  return false;
               }
               resolved_eth_source_contract_addr = c.contract_address;
               const auto sel_hash = fc::network::ethereum::abi::to_contract_function_selector(c);
               const uint8_t* sp = sel_hash.data();
               resolved_eth_source_deposit_selector.assign(sp, sp + 4);
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

      // SOL: walk every loaded IDL for the named instruction. The IDL
      // parser populates each instruction's 8-byte anchor discriminator
      // (`sha256("global:<instruction_name>")[0..8]`) AND the program's
      // deployed address (`metadata.address` / top-level `address`),
      // so we don't duplicate the program ID in a separate CLI option —
      // both come from the IDL JSON.
      {
         resolved_sol_source_deposit_discriminator.clear();
         sol_program_id.clear();
         bool found = false;
         for (const auto& [path, programs] : sol_plug->get_idl_files()) {
            for (const auto& p : programs) {
               if (const auto* ix = p.find_instruction(sol_source_deposit_instruction_name); ix) {
                  resolved_sol_source_deposit_discriminator.assign(
                     ix->discriminator.begin(), ix->discriminator.end());
                  if (p.address.empty()) {
                     elog("underwriter preflight: SOL IDL '{}' carries instruction '{}' but "
                          "no `address` / `metadata.address` field — the program ID must be "
                          "present in the IDL JSON for the verify path to identify it on-chain",
                          path.string(), sol_source_deposit_instruction_name);
                     return false;
                  }
                  sol_program_id = p.address;
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

      // Step 4b: prune `confirmed_commits` of entries whose uwreq is no
      // longer PENDING — the depot has resolved (won/lost/expired) those
      // races, so the local set should not grow unbounded. This is the
      // same pass that already reads the PENDING set, so it's free.
      {
         std::unordered_set<uint64_t> still_pending;
         still_pending.reserve(requests.size());
         for (auto& r : requests) still_pending.insert(r.id);
         std::lock_guard lk{stats_mutex};
         std::erase_if(confirmed_commits, [&](const commit_key& k) {
            return !still_pending.contains(k.uwreq_id);
         });
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

      // Step 5b: drop any uwreq whose BOTH legs are already confirmed —
      // the dispatch lambda also gates per-leg, but checking here saves
      // building UIC + signing for nothing.
      std::erase_if(selected, [&](const uw_request& r) {
         const commit_key src{r.id, r.src_chain, r.src_token_kind};
         const commit_key dst{r.id, r.dst_chain, r.dst_token_kind};
         return confirmed_commits.contains(src) && confirmed_commits.contains(dst);
      });

      if (selected.empty()) {
         ilog("underwriter: all selected uwreqs already have both legs confirmed locally");
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
      // v6 refactor: chain rows moved from `sysio.epoch::outposts` to
      // `sysio.chains::chains`. Each row is a `Chain` with fields:
      //   `code`       — slug_name (the universal chain identifier; the
      //                  v5 `outpost_id` was just this slug's uint64).
      //   `kind`       — ChainKind enum.
      //   `is_depot`   — true for the WIRE depot's own row; we filter
      //                  it out since underwriters don't commit to the
      //                  depot itself.
      auto rows = read_all("sysio.chains", "sysio.chains", "chains");
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         if (obj.contains("is_depot") && obj["is_depot"].as_bool()) continue;
         // `code` is a `slug_name` — serialised as `{"value": <uint64>}`.
         const auto& code_obj = obj["code"].get_object();
         uint64_t chain_code = code_obj["value"].as_uint64();
         // FC_REFLECT_ENUM in sysio/opp/opp.hpp gives us a direct enum
         // round-trip — the variant carries the symbolic name and `.as<T>()`
         // recovers the typed value without a string switch.
         outpost_chain_kinds[chain_code] = obj["kind"].as<ChainKind>();
      }
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

      // Local helper: read `chain_code`/`token_code` slug_name fields
      // (v6 shape `{"value": <u64>}`) and project to (ChainKind,
      // TokenKind). Returns nullopt when the chain/token isn't in the
      // outpost registry / tokens table — the row gets skipped.
      auto read_slug_pair = [&](const fc::variant_object& obj)
         -> std::optional<std::pair<ChainKind, TokenKind>> {
         if (!obj.contains("chain_code") || !obj.contains("token_code")) {
            return std::nullopt;
         }
         uint64_t chain_code = obj["chain_code"].get_object()["value"].as_uint64();
         uint64_t token_code = obj["token_code"].get_object()["value"].as_uint64();
         auto cki = outpost_chain_kinds.find(chain_code);
         auto tki = token_kind_by_code.find(token_code);
         if (cki == outpost_chain_kinds.end() || tki == token_kind_by_code.end()) {
            return std::nullopt;
         }
         return std::make_pair(cki->second, tki->second);
      };

      // ── Step 1: raw balances from sysio.opreg::operators[underwriter] ──
      auto ops_rows = read_all("sysio.opreg", "sysio.opreg", "operators");
      for (auto& row : ops_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
         if (!obj.contains("balances") || !obj["balances"].is_array()) break;
         for (auto& bal_entry : obj["balances"].get_array()) {
            auto be = bal_entry.get_object();
            auto kinds = read_slug_pair(be);
            if (!kinds) continue;
            credit_lines.push_back(credit_line{
               .chain_kind = kinds->first,
               .token_kind = kinds->second,
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
         auto kinds = read_slug_pair(obj);
         if (!kinds) continue;
         const uint64_t amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == kinds->first && cl.token_kind == kinds->second) {
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
         auto kinds = read_slug_pair(obj);
         if (!kinds) continue;
         const uint64_t amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == kinds->first && cl.token_kind == kinds->second) {
               cl.balance = (cl.balance > amount) ? (cl.balance - amount) : 0;
               break;
            }
         }
      }

      for (auto& cl : credit_lines) {
         ilog("underwriter: credit line chain_kind={} token_kind={} available={}",
              ChainKind_Name(cl.chain_kind),
              TokenKind_Name(cl.token_kind),
              cl.balance);
      }
   }

   /// Per-(chain, token_kind) availability predicate — replaces the
   /// per-chain `is_available()` so `select_coverable` and any future
   /// per-token gate can use the same lookup.
   bool has_credit(ChainKind chain, TokenKind token_kind) const {
      for (auto& cl : credit_lines) {
         if (cl.chain_kind == chain && cl.token_kind == token_kind && cl.balance > 0) return true;
      }
      return false;
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
            if (cl.chain_kind == chain_kind && cl.balance > 0) {
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

   /// Pack a `(chain, token_kind)` pair into a 64-bit credit-bucket key.
   /// `magic_enum::enum_integer` instead of `static_cast` (per
   /// `.claude/rules/code-quality.md` §3 / `enums-are-first-class.md`).
   static uint64_t bucket_key(ChainKind chain, TokenKind token) {
      return (static_cast<uint64_t>(magic_enum::enum_integer(chain)) << 32)
           |  static_cast<uint64_t>(magic_enum::enum_integer(token));
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
                      std::map<uint64_t, uint64_t>              remaining,
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
      const uint64_t src_k = bucket_key(r.src_chain, r.src_token_kind);
      const uint64_t dst_k = bucket_key(r.dst_chain, r.dst_token_kind);

      bool feasible = false;
      std::map<uint64_t, uint64_t> after = remaining;
      if (src_k == dst_k) {
         auto it = after.find(src_k);
         if (it != after.end() && it->second >= r.src_amount + r.dst_amount) {
            it->second -= (r.src_amount + r.dst_amount);
            feasible = true;
         }
      } else {
         auto src_it = after.find(src_k);
         auto dst_it = after.find(dst_k);
         if (src_it != after.end() && dst_it != after.end()
             && src_it->second >= r.src_amount
             && dst_it->second >= r.dst_amount) {
            src_it->second -= r.src_amount;
            dst_it->second -= r.dst_amount;
            feasible = true;
         }
      }

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
                    std::map<uint64_t, uint64_t> remaining) const {
      std::sort(requests.begin(), requests.end(),
                [](const uw_request& a, const uw_request& b) {
                   return (a.src_amount + a.dst_amount)
                        > (b.src_amount + b.dst_amount);
                });
      std::vector<uw_request> picked;
      for (auto& r : requests) {
         const uint64_t src_k = bucket_key(r.src_chain, r.src_token_kind);
         const uint64_t dst_k = bucket_key(r.dst_chain, r.dst_token_kind);
         if (src_k == dst_k) {
            auto it = remaining.find(src_k);
            if (it == remaining.end()
                || it->second < r.src_amount + r.dst_amount) continue;
            it->second -= (r.src_amount + r.dst_amount);
         } else {
            auto src_it = remaining.find(src_k);
            auto dst_it = remaining.find(dst_k);
            if (src_it == remaining.end() || dst_it == remaining.end()
                || src_it->second < r.src_amount
                || dst_it->second < r.dst_amount) continue;
            src_it->second -= r.src_amount;
            dst_it->second -= r.dst_amount;
         }
         picked.push_back(r);
      }
      return picked;
   }

   std::vector<uw_request> select_coverable(std::vector<uw_request>& requests) {
      // Seed bucket credits from `read_credit_lines`' output. Per the T11
      // mirror, these already have active locks + pending withdraws
      // subtracted, so the search operates on truly-spendable balances.
      std::map<uint64_t, uint64_t> initial_credit;
      for (auto& cl : credit_lines) {
         initial_credit[bucket_key(cl.chain_kind, cl.token_kind)] = cl.balance;
      }

      // Pre-filter requests that can never fit in isolation (no bucket
      // even matches), so the search space stays small.
      std::vector<uw_request> feasible_in_isolation;
      feasible_in_isolation.reserve(requests.size());
      for (auto& r : requests) {
         const uint64_t src_k = bucket_key(r.src_chain, r.src_token_kind);
         const uint64_t dst_k = bucket_key(r.dst_chain, r.dst_token_kind);
         if (src_k == dst_k) {
            auto it = initial_credit.find(src_k);
            if (it == initial_credit.end()
                || it->second < r.src_amount + r.dst_amount) continue;
         } else {
            auto src_it = initial_credit.find(src_k);
            auto dst_it = initial_credit.find(dst_k);
            if (src_it == initial_credit.end() || dst_it == initial_credit.end()
                || src_it->second < r.src_amount
                || dst_it->second < r.dst_amount) continue;
         }
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

   /// Look up the depot's outpost id for the given chain via the
   /// `outpost_chain_kinds` cache (populated by `read_outpost_registry`).
   /// Returns `std::nullopt` if no outpost is registered for the chain
   /// (per `feedback_no_zero_sentinels.md` — outpost id 0 is a real id).
   std::optional<uint64_t> find_outpost_id(ChainKind chain) const {
      for (auto& [id, ck] : outpost_chain_kinds) {
         if (ck == chain) return id;
      }
      return std::nullopt;
   }

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
                                            uint64_t        outpost_chain_code,
                                            ChainKind       leg_chain_kind,
                                            fc::slug_name    chain_code,
                                            fc::slug_name    token_code,
                                            fc::slug_name    reserve_code) {
      opp_att::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(underwriter_account.to_string());
      uic.set_uw_request_id(uwreq_id);
      uic.set_outpost_id(outpost_chain_code);
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
   /// transaction id captured at swap-emit time (ETH: 32-byte tx hash;
   /// SOL: 64-byte signature). The verify path fetches that tx, confirms
   /// it succeeded against the configured source contract / program, and
   /// cross-references every argument we can decode against the uwreq:
   ///
   ///   * `depositor`   — `tx.from` (ETH) / fee-payer (SOL) must match `req.depositor`.
   ///   * source contract — `tx.to` (ETH) / program-id (SOL) must match the configured address.
   ///   * function selector / instruction discriminator — must match the configured value.
   ///   * receipt status (ETH) / meta.err (SOL) — must indicate success.
   ///   * inclusion depth — ETH requires `ETH_MIN_CONFIRMATIONS` past the receipt's block.
   ///                       SOL fetches at commitment `SOL_COMMITMENT`.
   ///
   /// Hard-fail on empty `source_tx_id` — the depot's `createuwreq` rejects
   /// SwapRequests without one (emits SwapRevert), so by the time a uwreq
   /// reaches the plugin every row MUST carry one. A `req.source_tx_id`
   /// empty here means either the depot's reject regressed OR the plugin
   /// read a row pre-validation; either way the safe move is to refuse to
   /// commit until the data is whole.
   bool verify_source_deposit(const uw_request& req) {
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
   ///   5. Pull the matching log's `transactionHash`. Receipt must exist,
   ///      status != "0x0", and confirmations >= ETH_MIN_CONFIRMATIONS.
   ///
   /// A non-matching hash is a hard mismatch — the depositor's swap
   /// params disagree with what's recorded on the source chain. A
   /// missing log / receipt without mismatched fields is a deferred
   /// retry (returns false but no mismatch counter bump).
   bool verify_source_deposit_eth(const uw_request& req) {
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

      try {
         fc::mutable_variant_object filter;
         filter("address",   resolved_eth_source_contract_addr);
         filter("fromBlock", std::string{"earliest"});
         filter("toBlock",   std::string{"latest"});
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
                 "no SwapDeposit log yet (id={}, contract={})",
                 req.id, deposit_id, resolved_eth_source_contract_addr);
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

         // (5) Receipt + confirmation depth on the matching tx.
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
         if (!rcpt_obj.contains("blockNumber") ||
             !rcpt_obj["blockNumber"].is_string()) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "receipt missing blockNumber for tx {}", req.id, tx_hash_hex);
            return false;
         }
         const std::string blk_str = rcpt_obj["blockNumber"].as_string();
         // Require a 0x-prefixed, non-empty hex body before substr(2)/stoull so a malformed
         // blockNumber defers this uwreq rather than throwing std::out_of_range / invalid_argument.
         if (blk_str.size() <= 2 || blk_str[0] != '0' ||
             (blk_str[1] != 'x' && blk_str[1] != 'X')) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "receipt blockNumber not 0x-prefixed hex ('{}') for tx {}",
                 req.id, blk_str, tx_hash_hex);
            return false;
         }
         const uint64_t rcpt_blk = std::stoull(blk_str.substr(2), nullptr, 16);
         const uint64_t head_blk =
            entry->client->get_block_number().convert_to<uint64_t>();
         if (head_blk < rcpt_blk ||
             (head_blk - rcpt_blk) < eth_min_confirmations) {
            ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "insufficient confirmations: head={} receipt={} need={}",
                 req.id, head_blk, rcpt_blk,
                 eth_min_confirmations);
            return false;
         }

         ilog("underwriter: source-deposit verify passed for uwreq {} "
              "(SwapDeposit id={} tx={} depth={})",
              req.id, deposit_id, tx_hash_hex, head_blk - rcpt_blk);
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
   ///      `outpost_solana_client_plugin.hpp`) drives the scan loop:
   ///        a. `getSignaturesForAddress(sol_program_id,
   ///           limit=SOL_SCAN_LIMIT)` — enumerate recent program
   ///           sigs.
   ///        b. For each sig, `getTransaction(sig)`; inspect
   ///           `meta.err` (skip failed tx) and
   ///           `meta.logMessages[]` for the canonical marker:
   ///           `Program log: opp_outpost: SwapDeposit id=<id> hash=<64hex>`.
   ///   3. On match, parse the 32-byte on-chain hash from the log.
   ///   4. Recompute the same hash from the UWREQ row's flat fields
   ///      (depositor[32] + 7×u64 BE + u32 BE = 92 packed bytes).
   ///      Bit-exact mismatch → hard mismatch counter.
   ///   5. Confirmation gate: matching tx's `meta.confirmationStatus`
   ///      (when present on the per-sig listing from step 2a) must
   ///      be `"finalized"` — same severity as ETH's `head -
   ///      receipt.blockNumber >= eth_min_confirmations` gate. Tx
   ///      not yet finalized → deferred retry (no mismatch bump).
   ///
   /// `false` returns:
   ///   - hard mismatch (counter bumped) on bad size, hash divergence,
   ///     or tx-level execution error;
   ///   - deferred retry (no counter bump) on `retry_until` deadline
   ///     reached without a matching log — the outer poll loop
   ///     reattempts on its next tick.
   bool verify_source_deposit_sol(const uw_request& req) {
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
      // divergence / tx error) from "not yet found" (deferred). The
      // retry-callback returns:
      //   - Some(true)               → success, exit retry_until
      //   - Some(false) hard_mismatch → fail-fast, exit retry_until
      //   - None                     → deferred, sleep + retry
      // Outer `try { retry_until } catch (timeout_exception)` maps the
      // deadline-without-match case to deferred-no-mismatch-bump,
      // matching the ETH verify's semantics.
      constexpr size_t SOL_SCAN_LIMIT = 50;   // per-iteration sig batch
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
            std::vector<fc::variant> sigs;
            try {
               sigs = entry->client->get_signatures_for_address(
                  sol_program_id, std::nullopt, std::nullopt, SOL_SCAN_LIMIT);
            } catch (const fc::exception& e) {
               ilog("underwriter: source-deposit verify deferred for uwreq {} — "
                    "getSignaturesForAddress({}) RPC error: {}",
                    req.id, sol_program_id, e.to_detail_string());
               return std::nullopt;
            }

            for (const auto& sig_var : sigs) {
               if (!sig_var.is_object()) continue;
               const auto sig_obj = sig_var.get_object();
               if (!sig_obj.contains("signature") ||
                   !sig_obj["signature"].is_string()) continue;
               const std::string sig_b58 = sig_obj["signature"].as_string();

               // Skip failed txs at the listing level — sigs with an `err`
               // field carry execution failures and can't have emitted our
               // marker line successfully.
               if (sig_obj.contains("err") && !sig_obj["err"].is_null()) {
                  continue;
               }

               fc::variant tx;
               try {
                  tx = entry->client->get_transaction(
                     sig_b58, underwriter::SOL_COMMITMENT);
               } catch (const fc::exception& e) {
                  // Transient — move on; the next iteration may succeed.
                  continue;
               }
               if (tx.is_null() || !tx.is_object()) continue;
               const auto tx_obj = tx.get_object();
               if (!tx_obj.contains("meta") || !tx_obj["meta"].is_object()) {
                  continue;
               }
               const auto meta_obj = tx_obj["meta"].get_object();
               // Skip failed txs again at the tx level for completeness.
               if (meta_obj.contains("err") && !meta_obj["err"].is_null()) {
                  continue;
               }
               if (!meta_obj.contains("logMessages") ||
                   !meta_obj["logMessages"].is_array()) {
                  continue;
               }
               // Solana interleaves every invoked program's logs in meta.logMessages. Attribute each
               // "Program log:" payload to the program currently executing by tracking the
               // "Program <id> invoke" / "Program <id> success|failed" bracket lines, and trust the
               // SwapDeposit marker ONLY when opp-outpost (sol_program_id) is the top-of-stack
               // program. Without this, a forged "Program log:" emitted by any attacker program in a
               // transaction that merely references sol_program_id would pass verification.
               std::vector<std::string> program_stack;
               for (const auto& line_var : meta_obj["logMessages"].get_array()) {
                  if (!line_var.is_string()) continue;
                  const std::string line = line_var.as_string();

                  // Maintain the invocation stack from the bracket lines (which are not log payloads).
                  if (boost::algorithm::starts_with(line, "Program ") &&
                      !boost::algorithm::starts_with(line, "Program log:") &&
                      !boost::algorithm::starts_with(line, "Program data:") &&
                      !boost::algorithm::starts_with(line, "Program return:")) {
                     std::string_view rest{line};
                     rest.remove_prefix(8);                  // strip "Program "
                     const size_t sp1 = rest.find(' ');
                     if (sp1 != std::string_view::npos) {
                        const std::string_view prog = rest.substr(0, sp1);
                        std::string_view after = rest.substr(sp1 + 1);
                        const size_t sp2 = after.find(' ');
                        const std::string_view verb =
                           (sp2 == std::string_view::npos) ? after : after.substr(0, sp2);
                        if (verb == "invoke") {
                           program_stack.emplace_back(prog);
                        } else if (verb.starts_with("success") || verb.starts_with("failed")) {
                           if (!program_stack.empty()) program_stack.pop_back();
                        }
                        // "consumed", etc. — no stack change.
                     }
                     continue;
                  }

                  if (!boost::algorithm::starts_with(line, marker_prefix)) {
                     continue;
                  }
                  // Attribution: the marker is a "Program log:" payload of the top-of-stack program.
                  if (program_stack.empty() || program_stack.back() != sol_program_id) {
                     ilog("underwriter: ignoring SwapDeposit marker for uwreq {} in tx {} — not "
                          "emitted by opp-outpost program {} (attributed to {})",
                          req.id, sig_b58, sol_program_id,
                          program_stack.empty() ? std::string{"<none>"} : program_stack.back());
                     continue;
                  }
                  // Parse the 64-char lowercase hex hash that follows
                  // `marker_prefix`. Format MUST match the producer's
                  // `format!("{:02x}", b)` per-byte spelling.
                  const auto hash_hex = std::string_view{line}
                                          .substr(marker_prefix.size());
                  if (hash_hex.size() != 64) {
                     elog("underwriter: source-deposit verify failed for "
                          "uwreq {} — marker found in tx {} but hash field "
                          "has wrong size ({} chars; expected 64)",
                          req.id, sig_b58, hash_hex.size());
                     hard_mismatch_seen = true;
                     bump_mismatch();
                     return std::optional<bool>(false);
                  }
                  std::array<uint8_t, 32> on_chain_hash{};
                  bool parse_ok = true;
                  for (size_t i = 0; i < 32 && parse_ok; ++i) {
                     try {
                        on_chain_hash[i] = static_cast<uint8_t>(std::stoul(
                           std::string{hash_hex.substr(i * 2, 2)},
                           nullptr, 16));
                     } catch (...) {
                        parse_ok = false;
                     }
                  }
                  if (!parse_ok) {
                     elog("underwriter: source-deposit verify failed for "
                          "uwreq {} — marker found in tx {} but hash field "
                          "is not lowercase hex: {}",
                          req.id, sig_b58, std::string(hash_hex));
                     hard_mismatch_seen = true;
                     bump_mismatch();
                     return std::optional<bool>(false);
                  }
                  if (std::memcmp(recomputed_hash.data(),
                                   on_chain_hash.data(), 32) != 0) {
                     const std::string got_hex(hash_hex);
                     const std::string want_hex = fc::to_hex(
                        reinterpret_cast<const char*>(recomputed_hash.data()),
                        32);
                     elog("underwriter: source-deposit verify failed for "
                          "uwreq {} — SwapDeposit hash mismatch "
                          "(deposit_id={} tx={}): on-chain={} recomputed={}",
                          req.id, deposit_id, sig_b58, got_hex, want_hex);
                     hard_mismatch_seen = true;
                     bump_mismatch();
                     return std::optional<bool>(false);
                  }

                  // ── (5) Confirmation gate — must be finalized ──────────
                  std::string conf_status;
                  if (sig_obj.contains("confirmationStatus") &&
                      sig_obj["confirmationStatus"].is_string()) {
                     conf_status = sig_obj["confirmationStatus"].as_string();
                  }
                  if (conf_status != "finalized") {
                     ilog("underwriter: source-deposit verify deferred for "
                          "uwreq {} — matching tx {} not yet finalized "
                          "(confirmationStatus={})",
                          req.id, sig_b58, conf_status);
                     return std::nullopt;
                  }

                  matched_sig = sig_b58;
                  return std::optional<bool>(true);
               }
            }
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
              "(program={}, scan-interval={}s, will retry on next outer tick)",
              req.id, deposit_id,
              SOL_SWAP_DEPOSIT_TOTAL_TIMEOUT.count() / 1'000'000,
              sol_program_id,
              SOL_SWAP_DEPOSIT_POLL_INTERVAL.count() / 1'000'000);
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
      // Confirmation discipline: we skip any leg whose
      // `(uwreq_id, chain, token_kind)` triple is already in
      // `confirmed_commits` (a previous cycle's tx confirmed on-chain).
      // After a successful confirm we record the triple so the next scan
      // doesn't resubmit. Per project rules: confirm BEFORE recording so
      // a partial-landing in the map cannot happen without OPP breakage.
      auto submit_one = [this](ChainKind    chain,
                               TokenKind    token_kind,
                               fc::slug_name chain_code,
                               fc::slug_name token_code,
                               fc::slug_name reserve_code,
                               uint64_t     uw_request_id) {
         const commit_key key{uw_request_id, chain, token_kind};
         if (confirmed_commits.contains(key)) {
            ilog("underwriter: skip already-confirmed commit uwreq={} chain={} token={}",
                 uw_request_id,
                 chain_code.to_string(),
                 token_code.to_string());
            return;
         }

         auto outpost_id_opt = find_outpost_id(chain);
         if (!outpost_id_opt) {
            elog("underwriter: no outpost registered for chain_kind={} (uwreq {})",
                 ChainKind_Name(chain), uw_request_id);
            return;
         }
         auto uic_bytes = build_signed_uic_bytes(
            uw_request_id, *outpost_id_opt, chain, chain_code, token_code, reserve_code);
         if (uic_bytes.empty()) return;   // already logged

         // Chain-agnostic SPI call. The `outpost_by_chain` map carries one
         // `outpost_client` per supported chain kind, built at startup via
         // the outpost-plugin factories. Each concrete owns its own ABI /
         // IDL discovery, address encoding, and on-chain confirmation
         // discipline — this loop just relays opaque UIC bytes through the
         // virtual. Per `outpost-client-spi.md`.
         auto it = outpost_by_chain.find(chain);
         if (it == outpost_by_chain.end()) {
            elog("underwriter: no outpost_client wired for chain={} (uwreq {})",
                 ChainKind_Name(chain), uw_request_id);
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
      submit_one(req.src_chain, req.src_token_kind,
                 req.src_chain_code, req.src_token_code, req.src_reserve_code,
                 req.id);
      submit_one(req.dst_chain, req.dst_token_kind,
                 req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
                 req.id);
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
   // The matching `clio opp uw <stats|commits>` CLI wrapper is planned in
   // a follow-up; today the endpoints are addressable via `curl` against
   // the nodeop HTTP port.
   fc::variant build_stats_response() {
      std::lock_guard lk{stats_mutex};
      return fc::variant(fc::mutable_variant_object()
         ("underwriter_account",            underwriter_account.to_string())
         ("enabled",                        enabled)
         ("is_active",                      is_active)
         ("scan_interval_ms",               scan_interval_ms)
         ("action_timeout_ms",              action_timeout_ms)
         ("eth_client_id",                  eth_client_id)
         ("sol_client_id",                  sol_client_id)
         ("eth_opreg_addr",                 eth_opreg_addr)
         ("sol_program_id",                 sol_program_id)
         ("eth_source_deposit_function",    eth_source_deposit_function_name)
         ("eth_source_contract_addr",       resolved_eth_source_contract_addr)
         ("sol_source_deposit_instruction", sol_source_deposit_instruction_name)
         ("uwreqs_seen_pending",            uwreqs_seen_pending_count)
         ("commits_confirmed",              commits_confirmed_count)
         ("commits_failed",                 commits_failed_count)
         ("source_deposit_mismatch",        source_deposit_mismatch_count)
         ("outstanding_commit_count",       confirmed_commits.size())
      );
   }

   fc::variant build_commits_response() {
      std::lock_guard lk{stats_mutex};
      std::vector<fc::variant> entries;
      entries.reserve(confirmed_commits.size());
      for (const auto& k : confirmed_commits) {
         entries.push_back(fc::variant(fc::mutable_variant_object()
            ("uwreq_id",   k.uwreq_id)
            ("chain",      ChainKind_Name(k.chain))
            ("token_kind", TokenKind_Name(k.token_kind))
         ));
      }
      return fc::variant(fc::mutable_variant_object()
         ("count",   entries.size())
         ("entries", std::move(entries))
      );
   }

   /// Register the `/v1/underwriter/*` HTTP endpoints. Called once from
   /// `plugin_startup` after preflight passes and the cron job is queued.
   void register_http_endpoints() {
      auto& hp = app().get_plugin<http_plugin>();
      hp.add_api({
         {"/v1/underwriter/stats", api_category::node,
            [this](std::string&& /*url*/,
                    std::string&& /*body*/,
                    url_response_callback&& cb) {
               try {
                  cb(200, build_stats_response());
               } catch (const fc::exception& e) {
                  cb(500, fc::variant(fc::mutable_variant_object()
                     ("error", e.to_detail_string())));
               }
            }},
         {"/v1/underwriter/commits", api_category::node,
            [this](std::string&& /*url*/,
                    std::string&& /*body*/,
                    url_response_callback&& cb) {
               try {
                  cb(200, build_commits_response());
               } catch (const fc::exception& e) {
                  cb(500, fc::variant(fc::mutable_variant_object()
                     ("error", e.to_detail_string())));
               }
            }},
      }, appbase::exec_queue::read_only);
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
   opts("underwriter-eth-client-id", bpo::value<std::string>()->default_value(underwriter_defaults::eth_client_id),
        "Ethereum outpost client ID");
   opts("underwriter-sol-client-id", bpo::value<std::string>()->default_value(underwriter_defaults::sol_client_id),
        "Solana outpost client ID");
   opts("underwriter-eth-opreg-addr", bpo::value<std::string>(),
        "OperatorRegistry contract address on Ethereum (hex)");
   opts("underwriter-eth-source-deposit-function", bpo::value<std::string>(),
        "Name of the ETH swap-deposit function. Resolved at preflight against the ABI "
        "files registered with --ethereum-abi-file; the matching `function` entry's "
        "`contract_address` becomes the source contract address, and its keccak256 "
        "signature yields the 4-byte selector. Required.");
   opts("underwriter-sol-source-deposit-instruction", bpo::value<std::string>(),
        "Name of the SOL swap-deposit instruction. Resolved at preflight against the IDL "
        "files registered with --solana-idl-file; the matching instruction's anchor "
        "discriminator (8 bytes) is used to identify the deposit instruction in source "
        "txs. Required.");
   opts("underwriter-eth-min-confirmations",
        bpo::value<uint64_t>()->default_value(
           sysio::underwriter::ETH_MIN_CONFIRMATIONS),
        "Minimum ETH block confirmations the underwriter requires before a "
        "SwapDeposit log is accepted as source-deposit-verified. Default 12 "
        "(mainnet-safe). Lower (e.g. 1) on the local anvil test cluster where "
        "blocks only mine on user txs and waiting for 12 confirmations would "
        "stall the underwriter race.");
}

void underwriter_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("underwriter-account"))
      _impl->underwriter_account = chain::name(options["underwriter-account"].as<std::string>());
   _impl->scan_interval_ms  = options["underwriter-scan-interval-ms"].as<uint32_t>();
   _impl->action_timeout_ms = options["underwriter-action-timeout-ms"].as<uint32_t>();
   _impl->enabled           = options["underwriter-enabled"].as<bool>();
   _impl->eth_client_id     = options["underwriter-eth-client-id"].as<std::string>();
   _impl->sol_client_id     = options["underwriter-sol-client-id"].as<std::string>();
   if (options.count("underwriter-eth-opreg-addr"))
      _impl->eth_opreg_addr = options["underwriter-eth-opreg-addr"].as<std::string>();
   if (options.count("underwriter-eth-source-deposit-function"))
      _impl->eth_source_deposit_function_name =
         options["underwriter-eth-source-deposit-function"].as<std::string>();
   if (options.count("underwriter-sol-source-deposit-instruction"))
      _impl->sol_source_deposit_instruction_name =
         options["underwriter-sol-source-deposit-instruction"].as<std::string>();
   _impl->eth_min_confirmations =
      options["underwriter-eth-min-confirmations"].as<uint64_t>();

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug  = &app().get_plugin<cron_plugin>();
   _impl->eth_plug   = &app().get_plugin<outpost_ethereum_client_plugin>();
   _impl->sol_plug   = &app().get_plugin<outpost_solana_client_plugin>();
}

void underwriter_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("underwriter_plugin: disabled, skipping startup");
      return;
   }

   ilog("underwriter_plugin: starting for account {}", _impl->underwriter_account.to_string());

   // Unconditional pre-flight: bail (no cron job) if the depot-side state
   // for this underwriter is incomplete. Cluster bootstrap is responsible
   // for establishing the missing state — there is no dev escape hatch.
   if (!_impl->run_preflight()) {
      elog("underwriter_plugin: pre-flight failed — cron job NOT registered");
      return;
   }

   // Materialize the outpost_client SPI handles. The underwriter never
   // sees raw `ethereum_client` / `solana_client` instances after this
   // point — every outpost-side action goes through the SPI virtuals.
   // Per `outpost-client-spi.md`:
   //   * ETH outpost is constructed with only the OperatorRegistry
   //     address — the underwriter does not consume / emit OPP
   //     envelopes itself, so OPP / OPPInbound addresses are left empty
   //     and `deliver_outbound_envelope` / `read_inbound_envelope` would
   //     assert if called (they're not, on this code path).
   //   * SOL outpost is constructed with the program id resolved at
   //     preflight from the IDL's `address` field; the typed program
   //     wrapper exposes `commit_underwrite` directly.
   try {
      if (!_impl->eth_client_id.empty() && !_impl->eth_opreg_addr.empty()) {
         _impl->outpost_by_chain[ChainKind::CHAIN_KIND_EVM] =
            _impl->eth_plug->create_outpost_client(_impl->eth_client_id,
                                                   /*chain_code=*/0,
                                                   /*chain_id=*/0,
                                                   /*opp_addr=*/"",
                                                   /*opp_inbound_addr=*/"",
                                                   _impl->eth_opreg_addr);
         ilog("underwriter_plugin: wired ETH outpost_client (opreg={})",
              _impl->eth_opreg_addr);
      } else {
         wlog("underwriter_plugin: ETH outpost_client NOT wired "
              "(eth_client_id='{}', eth_opreg_addr='{}')",
              _impl->eth_client_id, _impl->eth_opreg_addr);
      }
      if (!_impl->sol_client_id.empty() && !_impl->sol_program_id.empty()) {
         _impl->outpost_by_chain[ChainKind::CHAIN_KIND_SVM] =
            _impl->sol_plug->create_outpost_client(_impl->sol_client_id,
                                                   /*chain_code=*/0,
                                                   /*chain_id=*/0,
                                                   _impl->sol_program_id);
         ilog("underwriter_plugin: wired SOL outpost_client (program={})",
              _impl->sol_program_id);
      } else {
         wlog("underwriter_plugin: SOL outpost_client NOT wired "
              "(sol_client_id='{}', sol_program_id='{}')",
              _impl->sol_client_id, _impl->sol_program_id);
      }
   } catch (const fc::exception& e) {
      elog("underwriter_plugin: failed to build outpost_client(s): {}",
           e.to_detail_string());
      return;
   }

   auto& cron = app().get_plugin<cron_plugin>();
   cron_service::job_schedule sched;
   sched.milliseconds = {cron_service::job_schedule::step_value{_impl->scan_interval_ms}};

   cron_service::job_metadata_t meta;
   meta.label = "underwriter_scan";
   meta.one_at_a_time = true;

   _impl->scan_job_id = cron.add_job(
      sched,
      [this]() { _impl->scan_cycle(); },
      meta
   );

   ilog("underwriter_plugin: scheduled scan (id={}, interval={}ms)",
        _impl->scan_job_id, _impl->scan_interval_ms);

   // Register read-only HTTP diagnostics. Endpoints:
   //   GET /v1/underwriter/stats    — session counters + config snapshot
   //   GET /v1/underwriter/commits  — outstanding confirmed commits
   _impl->register_http_endpoints();
}

void underwriter_plugin::plugin_shutdown() {
   _impl->shutting_down = true;

   if (_impl->scan_job_id != 0) {
      auto& cron = app().get_plugin<cron_plugin>();
      cron.cancel_job(_impl->scan_job_id);
   }

   ilog("underwriter_plugin: shutdown complete");
}

} // namespace sysio
