#include <fc/log/logger.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/io/raw.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
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
namespace eth = fc::network::ethereum;
namespace sol = fc::network::solana;

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
   /// Source-chain id of the deposit transaction. ETH = 32-byte tx hash;
   /// SOL = 64-byte signature. Populated by `createuwreq` from
   /// `SwapRequest.source_tx_id`. The depot rejects SwapRequests with an
   /// empty `source_tx_id` (emits SwapRevert), so by the time a uwreq
   /// reaches the plugin's scan this MUST be non-empty.
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

   // Outpost chain_kind cache: outpost_id -> ChainKind
   std::map<uint64_t, ChainKind>     outpost_chain_kinds;

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
         elog("underwriter preflight: account {} not in OPERATOR_STATUS_ACTIVE — "
              "fix the depot-side opreg state before starting the plugin",
              underwriter_account.to_string());
         return false;
      }

      // Populate the outpost-chain cache (also used by the scan loop) so
      // the link + balance coverage checks know what to look for.
      read_outpost_registry();

      if (outpost_chain_kinds.empty()) {
         elog("underwriter preflight: no outposts registered in sysio.epoch::outposts — "
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
      for (auto& [outpost_id, chain_kind] : outpost_chain_kinds) {
         if (!linked_chains.count(chain_kind)) {
            elog("underwriter preflight: missing sysio.authex link for outpost {} "
                 "(chain_kind={}) — bootstrap must call sysio.authex::createlink for "
                 "this account on every outpost chain",
                 outpost_id,
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
      for (auto& [outpost_id, chain_kind] : outpost_chain_kinds) {
         const int ck = magic_enum::enum_integer(chain_kind);
         auto it = raw_balance_by_chain.find(ck);
         if (it == raw_balance_by_chain.end() || it->second == 0) {
            elog("underwriter preflight: zero raw balance on outpost {} "
                 "(chain_kind={}) — bootstrap must deposit collateral for "
                 "this account on every outpost chain (locks are NOT "
                 "deducted here; a fully-locked underwriter still passes "
                 "this check)",
                 outpost_id,
                 std::string{sysio::opp::types::ChainKind_Name(chain_kind)});
            return false;
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
      auto rows = read_all("sysio.epoch", "sysio.epoch", "outposts");
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         uint64_t id = obj["id"].as_uint64();
         // FC_REFLECT_ENUM in sysio/opp/opp.hpp gives us a direct enum
         // round-trip — the variant carries the symbolic name and `.as<T>()`
         // recovers the typed value without a string switch.
         outpost_chain_kinds[id] = obj["chain_kind"].as<ChainKind>();
      }
   }

   // -----------------------------------------------------------------------
   //  Read credit lines from sysio.opreg::operators
   // -----------------------------------------------------------------------

   void read_credit_lines() {
      credit_lines.clear();

      // ── Step 1: raw balances from sysio.opreg::operators[underwriter] ──
      // Per-(chain, token_kind) row on `balances` (one balance, not a
      // stake-vector). Mirrors what `sysio.opreg::available()` reads as
      // its starting point.
      auto ops_rows = read_all("sysio.opreg", "sysio.opreg", "operators");
      for (auto& row : ops_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
         if (!obj.contains("balances") || !obj["balances"].is_array()) break;
         for (auto& bal_entry : obj["balances"].get_array()) {
            auto be = bal_entry.get_object();
            if (!be.contains("chain") || !be.contains("token_kind") ||
                !be.contains("balance")) continue;
            credit_lines.push_back(credit_line{
               .chain_kind = be["chain"].as<ChainKind>(),
               .token_kind = be["token_kind"].as<TokenKind>(),
               .balance    = be["balance"].as_uint64(),
            });
         }
         break;
      }

      // ── Step 2: subtract active locks (sysio.uwrit::locks) ─────────────
      // Mirror of sysio.uwrit's local `sum_locks_inline` helper. Sum amounts
      // by (chain, token_kind) for any row whose underwriter matches us;
      // subtract from the matching credit_line. Locks that exceed the raw
      // balance clamp to 0 — same convention as the depot's available().
      auto lock_rows = read_all("sysio.uwrit", "sysio.uwrit", "locks");
      for (auto& row : lock_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["underwriter"].as_string()) != underwriter_account) continue;
         const ChainKind chain  = obj["chain"].as<ChainKind>();
         const TokenKind token  = obj["token_kind"].as<TokenKind>();
         const uint64_t  amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == chain && cl.token_kind == token) {
               cl.balance = (cl.balance > amount) ? (cl.balance - amount) : 0;
               break;
            }
         }
      }

      // ── Step 3: subtract pending withdraws (sysio.opreg::wtdwqueue) ────
      // Mirror of sysio.opreg::available()'s pending_withdraws subtract.
      auto wq_rows = read_all("sysio.opreg", "sysio.opreg", "wtdwqueue");
      for (auto& row : wq_rows.rows) {
         auto obj = row.get_object();
         if (chain::name(obj["account"].as_string()) != underwriter_account) continue;
         const ChainKind chain  = obj["chain"].as<ChainKind>();
         const TokenKind token  = obj["token_kind"].as<TokenKind>();
         const uint64_t  amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == chain && cl.token_kind == token) {
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
      for (auto& [outpost_id, chain_kind] : outpost_chain_kinds) {
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

      // `uwreqs.bystatus` is a uint64 secondary index on
      // `static_cast<uint64_t>(status)`. Scan exactly the
      // `UNDERWRITE_REQUEST_STATUS_PENDING (0)` slice — [0, 1) — instead of
      // paging the whole table and filtering in C++.
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name("sysio.uwrit");
      p.scope       = "sysio.uwrit";
      p.table       = "uwreqs";
      p.index_name  = "bystatus";
      constexpr auto PENDING_STATUS = magic_enum::enum_integer(
         UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING);
      p.lower_bound = std::format("{{\"bystatus\":{}}}", PENDING_STATUS);
      p.upper_bound = std::format("{{\"bystatus\":{}}}", PENDING_STATUS + 1);
      p.limit       = 0; // paginate all pending rows
      p.values_only = true;
      auto rows = read_table(std::move(p));
      for (auto& row : rows.rows) {
         auto obj = row.get_object();

         // Skip if already assigned to another underwriter
         auto uw_name = obj["uw_name"].as_string();
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

         // New schema: src/dst (chain, token_kind, amount) live directly on
         // the uwreq row (populated by uwrit::createuwreq from the
         // originating SwapRequest). No more parse_swap_from_attestation
         // detour through sysio.msgch::attestations. FC_REFLECT_ENUM in
         // sysio/opp/opp.hpp provides the variant ↔ typed-enum round-trip.
         if (!obj.contains("src_chain") || !obj.contains("src_amount")
             || !obj.contains("dst_chain") || !obj.contains("dst_amount")) {
            // Row not yet populated (createuwreq writes them inline so this
            // should be unreachable for SWAP-derived UWREQs). Skip safely.
            continue;
         }
         req.src_chain      = obj["src_chain"].as<ChainKind>();
         req.src_token_kind = obj["src_token_kind"].as<TokenKind>();
         req.src_amount     = obj["src_amount"].as_uint64();
         req.dst_chain      = obj["dst_chain"].as<ChainKind>();
         req.dst_token_kind = obj["dst_token_kind"].as<TokenKind>();
         req.dst_amount     = obj["dst_amount"].as_uint64();
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
   /// given `(uwreq_id, outpost_id, token_kind)` leg. Returns an empty
   /// vector on any failure (no signature provider, serialize failure, etc.).
   ///
   /// `token_kind` discriminates which leg of the uwreq this UIC covers —
   /// for same-chain swaps (e.g. ERC20→ETH-native on one outpost) both
   /// legs share `outpost_id` but differ on `token_kind`. The depot's
   /// `rcrdcommit` routes the UIC into the source-leg or dest-leg slot
   /// based on the `(from_chain, token_kind)` pair.
   ///
   /// Digest semantics: the underwriter signs `sha256(serialize(uic with
   /// signature blanked))`. The depot's `try_select_winner` rebuilds the
   /// same digest from the bytes it received and verifies the embedded
   /// signature against the underwriter's WIRE account permissions
   /// (`owner` / `active` only) — see `sysio.uwrit::verify_uic_signature`.
   std::vector<char> build_signed_uic_bytes(uint64_t uwreq_id,
                                              uint64_t outpost_id,
                                              TokenKind token_kind) {
      opp_att::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(underwriter_account.to_string());
      uic.set_uw_request_id(uwreq_id);
      uic.set_outpost_id(outpost_id);
      uic.set_token_kind(token_kind);
      // uw_ext_chain_addr left default-constructed (empty kind/address) for
      // v1 — the (outpost_id, token_kind) pair is the binding the depot's
      // routing path needs, and the signature ties the whole UIC together.
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
         case ChainKind::CHAIN_KIND_ETHEREUM:
            return verify_source_deposit_eth(req);
         case ChainKind::CHAIN_KIND_SOLANA:
            return verify_source_deposit_sol(req);
         default:
            elog("underwriter: cannot verify source deposit for chain={} (uwreq {})",
                 ChainKind_Name(req.src_chain), req.id);
            return false;
      }
   }

   /// ETH-side source-deposit verification. `req.source_tx_id` is the
   /// raw 32-byte tx hash captured at swap-emit time. The verify path:
   ///
   ///   1. `eth_getTransactionByHash(source_tx_id)` — tx must exist.
   ///   2. `tx.to` must equal `--underwriter-eth-source-contract-addr` (case-insensitive).
   ///   3. `tx.from` must equal `req.depositor` (case-insensitive 20-byte ETH address).
   ///   4. `tx.input[0..4]` must equal `--underwriter-eth-source-deposit-selector` (the
   ///      4-byte function selector for the swap-deposit call).
   ///   5. `eth_getTransactionReceipt(source_tx_id)` must report status != "0x0".
   ///   6. `eth_blockNumber() - receipt.blockNumber >= ETH_MIN_CONFIRMATIONS` so we don't
   ///      accept a tx in a chain tip that may still reorg.
   ///
   /// Every required option (contract address, selector) is checked in
   /// preflight; if any is unset the plugin refuses to start. Returns
   /// true only when all six checks pass.
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

      const std::string tx_hash_hex = "0x" +
         fc::to_hex(req.source_tx_id.data(), req.source_tx_id.size());

      try {
         auto tx = entry->client->get_transaction_by_hash(tx_hash_hex);
         if (tx.is_null()) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "eth_getTransactionByHash({}) returned null",
                 req.id, tx_hash_hex);
            bump_mismatch();
            return false;
         }
         const auto tx_obj = tx.get_object();

         // (2) tx.to == source contract address (resolved at preflight
         //     from the matching ABI entry's `contract_address`).
         std::string to_addr;
         if (tx_obj.contains("to") && tx_obj["to"].is_string()) {
            to_addr = tx_obj["to"].as_string();
         }
         if (!boost::iequals(to_addr, resolved_eth_source_contract_addr)) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "tx.to={} != resolved source contract {}",
                 req.id, to_addr, resolved_eth_source_contract_addr);
            bump_mismatch();
            return false;
         }

         // (3) tx.from == req.depositor. The depot stores the 20-byte
         //     ETH address verbatim in `depositor`; the RPC returns the
         //     same address as a "0x"-prefixed lower-case hex string.
         std::string from_addr;
         if (tx_obj.contains("from") && tx_obj["from"].is_string()) {
            from_addr = tx_obj["from"].as_string();
         }
         const std::string req_depositor = "0x" +
            fc::to_hex(req.depositor.data(), req.depositor.size());
         if (!boost::iequals(from_addr, req_depositor)) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "tx.from={} != req.depositor={}", req.id, from_addr,
                 req_depositor);
            bump_mismatch();
            return false;
         }

         // (4) Function selector match. tx.input is "0x"-prefixed hex.
         std::string input_hex;
         if (tx_obj.contains("input") && tx_obj["input"].is_string()) {
            input_hex = tx_obj["input"].as_string();
         }
         // Strip "0x" prefix; selector is the first 4 bytes (8 hex chars).
         std::string_view input_no_prefix = input_hex;
         if (input_no_prefix.size() >= 2 && input_no_prefix[0] == '0' &&
             (input_no_prefix[1] == 'x' || input_no_prefix[1] == 'X')) {
            input_no_prefix.remove_prefix(2);
         }
         if (input_no_prefix.size() < 8) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "tx.input too short ({} hex chars) to contain a 4-byte selector",
                 req.id, input_no_prefix.size());
            bump_mismatch();
            return false;
         }
         std::vector<uint8_t> got_selector(4);
         for (size_t i = 0; i < 4; ++i) {
            got_selector[i] = static_cast<uint8_t>(std::stoul(
               std::string{input_no_prefix.substr(i * 2, 2)}, nullptr, 16));
         }
         if (got_selector != resolved_eth_source_deposit_selector) {
            const std::string got_hex  = fc::to_hex(reinterpret_cast<const char*>(got_selector.data()),
                                                     got_selector.size());
            const std::string want_hex = fc::to_hex(
               reinterpret_cast<const char*>(resolved_eth_source_deposit_selector.data()),
               resolved_eth_source_deposit_selector.size());
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "tx.input[0..4]={} != resolved selector={} for function '{}'",
                 req.id, got_hex, want_hex, eth_source_deposit_function_name);
            bump_mismatch();
            return false;
         }

         // (5) Receipt must exist + status not == "0x0".
         auto receipt = entry->client->get_transaction_receipt(tx_hash_hex);
         if (receipt.is_null()) {
            elog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "no receipt for tx {} (not yet mined). Skip + retry next cycle.",
                 req.id, tx_hash_hex);
            // Not a mismatch — the tx exists but isn't yet receipt-ready.
            return false;
         }
         const auto rcpt_obj = receipt.get_object();
         if (rcpt_obj.contains("status")
             && rcpt_obj["status"].as_string() == "0x0") {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "tx {} reverted on-chain", req.id, tx_hash_hex);
            bump_mismatch();
            return false;
         }

         // (6) Confirmation depth. Reorgs of `ETH_MIN_CONFIRMATIONS`
         //     blocks are not statistically meaningful on a healthy
         //     PoS ETH chain; this guards against a tx being mined into
         //     a block that's later orphaned.
         if (!rcpt_obj.contains("blockNumber")
             || !rcpt_obj["blockNumber"].is_string()) {
            elog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "receipt missing blockNumber for tx {}", req.id, tx_hash_hex);
            return false;
         }
         const uint64_t rcpt_blk = std::stoull(
            rcpt_obj["blockNumber"].as_string().substr(2), nullptr, 16);
         const uint64_t head_blk =
            entry->client->get_block_number().convert_to<uint64_t>();
         if (head_blk < rcpt_blk
             || (head_blk - rcpt_blk) < underwriter::ETH_MIN_CONFIRMATIONS) {
            elog("underwriter: source-deposit verify deferred for uwreq {} — "
                 "insufficient confirmations: head={} receipt={} need={}",
                 req.id, head_blk, rcpt_blk,
                 underwriter::ETH_MIN_CONFIRMATIONS);
            return false;
         }

         ilog("underwriter: source-deposit verify passed for uwreq {} "
              "(tx {} → {} from {}; selector ok; depth={})",
              req.id, tx_hash_hex, to_addr, from_addr, head_blk - rcpt_blk);
         return true;
      } catch (const fc::exception& e) {
         elog("underwriter: source-deposit verify failed for uwreq {} — "
              "RPC error: {}", req.id, e.to_detail_string());
         bump_mismatch();
         return false;
      }
   }

   /// SOL-side source-deposit verification. `req.source_tx_id` is the
   /// raw 64-byte Solana transaction signature captured at swap-emit
   /// time. The verify path:
   ///
   ///   1. `getTransaction(base58(source_tx_id), commitment=SOL_COMMITMENT)` — tx must exist.
   ///   2. `tx.meta.err` must be null.
   ///   3. `sol_program_id` must appear in `tx.transaction.message.accountKeys`.
   ///   4. The deposit instruction (the instruction targeting our program) must:
   ///      a. start with the configured 8-byte anchor discriminator
   ///         (`--underwriter-sol-source-deposit-discriminator`), and
   ///      b. carry the depositor pubkey as the first signer in `accountKeys` matching `req.depositor`.
   ///
   /// Returns true only when all checks pass.
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

      // Solana signatures are 64 bytes encoded as base58 strings on the
      // wire. The batch operator stored the raw 64 bytes in source_tx_id.
      const std::string sig_b58 = fc::to_base58(
         req.source_tx_id.data(), req.source_tx_id.size(),
         fc::yield_function_t{});

      try {
         auto tx = entry->client->get_transaction(sig_b58,
            underwriter::SOL_COMMITMENT);
         if (tx.is_null()) {
            elog("underwriter: source-deposit verify failed for uwreq {} — "
                 "getTransaction({}) returned null", req.id, sig_b58);
            bump_mismatch();
            return false;
         }
         const auto tx_obj = tx.get_object();
         // (2) tx.meta.err must be null for success.
         if (tx_obj.contains("meta") && tx_obj["meta"].is_object()) {
            const auto meta = tx_obj["meta"].get_object();
            if (meta.contains("err") && !meta["err"].is_null()) {
               elog("underwriter: source-deposit verify failed for uwreq {} — "
                    "tx {} failed on-chain (meta.err={})", req.id, sig_b58,
                    meta["err"].as_string());
               bump_mismatch();
               return false;
            }
         }

         // (3) sol_program_id must appear in accountKeys. We also record
         //     its index because Solana instructions reference accounts
         //     by index into this array.
         std::vector<std::string> account_keys;
         std::optional<size_t> program_idx;
         if (tx_obj.contains("transaction") && tx_obj["transaction"].is_object()) {
            const auto inner = tx_obj["transaction"].get_object();
            if (inner.contains("message") && inner["message"].is_object()) {
               const auto msg = inner["message"].get_object();
               if (msg.contains("accountKeys") && msg["accountKeys"].is_array()) {
                  size_t i = 0;
                  for (const auto& k : msg["accountKeys"].get_array()) {
                     if (k.is_string()) {
                        account_keys.push_back(k.as_string());
                        if (account_keys.back() == sol_program_id) {
                           program_idx = i;
                        }
                     }
                     ++i;
                  }
                  if (!program_idx) {
                     elog("underwriter: source-deposit verify failed for uwreq {} — "
                          "tx {} does not reference SOL outpost program {}",
                          req.id, sig_b58, sol_program_id);
                     bump_mismatch();
                     return false;
                  }
                  // (4b) Depositor must equal accountKeys[0] (Solana fee
                  //      payer / first signer). `req.depositor` is the
                  //      raw 32-byte Ed25519 pubkey; base58-encode it
                  //      to compare against the RPC's string form.
                  if (account_keys.empty()) {
                     elog("underwriter: source-deposit verify failed for uwreq {} — "
                          "tx {} has empty accountKeys", req.id, sig_b58);
                     bump_mismatch();
                     return false;
                  }
                  const std::string depositor_b58 = fc::to_base58(
                     req.depositor.data(), req.depositor.size(),
                     fc::yield_function_t{});
                  if (account_keys.front() != depositor_b58) {
                     elog("underwriter: source-deposit verify failed for uwreq {} — "
                          "fee-payer={} != req.depositor={}",
                          req.id, account_keys.front(), depositor_b58);
                     bump_mismatch();
                     return false;
                  }
               }
               // (4a) Discriminator match on the instruction targeting our
               //      program. The RPC's `message.instructions[].programIdIndex`
               //      points into accountKeys; we want the instruction whose
               //      programIdIndex == our resolved index.
               if (msg.contains("instructions") && msg["instructions"].is_array()) {
                  bool disc_seen = false;
                  for (const auto& ix : msg["instructions"].get_array()) {
                     if (!ix.is_object()) continue;
                     const auto ix_obj = ix.get_object();
                     if (!ix_obj.contains("programIdIndex")) continue;
                     if (ix_obj["programIdIndex"].as_uint64() != *program_idx) continue;

                     // `data` is base58-encoded in the JSON-RPC response
                     // (default encoding). Decode + compare the leading 8
                     // bytes to the configured discriminator.
                     std::string data_b58;
                     if (ix_obj.contains("data") && ix_obj["data"].is_string()) {
                        data_b58 = ix_obj["data"].as_string();
                     }
                     if (data_b58.empty()) continue;
                     std::vector<char> decoded;
                     try {
                        // fc::from_base58 returns vector<char>
                        decoded = fc::from_base58(data_b58);
                     } catch (...) {
                        continue;
                     }
                     if (decoded.size() < 8) continue;
                     if (std::equal(
                            resolved_sol_source_deposit_discriminator.begin(),
                            resolved_sol_source_deposit_discriminator.end(),
                            reinterpret_cast<const uint8_t*>(decoded.data()))) {
                        disc_seen = true;
                        break;
                     }
                  }
                  if (!disc_seen) {
                     const std::string want = fc::to_hex(
                        reinterpret_cast<const char*>(resolved_sol_source_deposit_discriminator.data()),
                        resolved_sol_source_deposit_discriminator.size());
                     elog("underwriter: source-deposit verify failed for uwreq {} — "
                          "no instruction targeting program {} carries the "
                          "resolved discriminator {} for instruction '{}'",
                          req.id, sol_program_id, want,
                          sol_source_deposit_instruction_name);
                     bump_mismatch();
                     return false;
                  }
               }
            }
         }

         ilog("underwriter: source-deposit verify passed for uwreq {} "
              "(SOL tx {} touches program {}; discriminator + depositor ok)",
              req.id, sig_b58, sol_program_id);
         return true;
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
           "src=({},{}) dst=({},{})",
           req.id,
           ChainKind_Name(req.src_chain), TokenKind_Name(req.src_token_kind),
           ChainKind_Name(req.dst_chain), TokenKind_Name(req.dst_token_kind));

      // Per-leg dispatch keyed on `(chain, token_kind)`. Same-chain swaps
      // (e.g. ERC20 → ETH-native on one outpost) share `chain` between the
      // two legs but differ on `token_kind`; the UIC payload carries the
      // `token_kind` so the depot's `rcrdcommit` can route to the correct
      // source/dest slot on `commit_entry`.
      //
      // Confirmation discipline: we skip any leg whose
      // `(uwreq_id, chain, token_kind)` triple is already in
      // `confirmed_commits` (a previous cycle's tx confirmed on-chain).
      // After a successful confirm we record the triple so the next scan
      // doesn't resubmit. Per project rules: confirm BEFORE recording so
      // a partial-landing in the map cannot happen without OPP breakage.
      auto submit_one = [this](ChainKind chain, TokenKind token_kind,
                                uint64_t uw_request_id) {
         const commit_key key{uw_request_id, chain, token_kind};
         if (confirmed_commits.contains(key)) {
            ilog("underwriter: skip already-confirmed commit uwreq={} chain={} token={}",
                 uw_request_id, ChainKind_Name(chain), TokenKind_Name(token_kind));
            return;
         }

         auto outpost_id_opt = find_outpost_id(chain);
         if (!outpost_id_opt) {
            elog("underwriter: no outpost registered for chain_kind={} (uwreq {})",
                 ChainKind_Name(chain), uw_request_id);
            return;
         }
         auto uic_bytes = build_signed_uic_bytes(
            uw_request_id, *outpost_id_opt, token_kind);
         if (uic_bytes.empty()) return;   // already logged

         bool confirmed = false;
         switch (chain) {
            case ChainKind::CHAIN_KIND_ETHEREUM:
               confirmed = submit_commit_eth(uw_request_id, uic_bytes); break;
            case ChainKind::CHAIN_KIND_SOLANA:
               confirmed = submit_commit_sol(uw_request_id, uic_bytes); break;
            default:
               elog("underwriter: unsupported chain={} for commit (uwreq {})",
                    ChainKind_Name(chain), uw_request_id);
               return;
         }
         std::lock_guard lk{stats_mutex};
         if (confirmed) {
            confirmed_commits.insert(key);
            commits_confirmed_count++;
         } else {
            commits_failed_count++;
         }
      };
      submit_one(req.src_chain, req.src_token_kind, req.id);
      submit_one(req.dst_chain, req.dst_token_kind, req.id);
   }

   /**
    * Call `commit(bytes uicBytes)` on the ETH outpost's OperatorRegistry —
    * an opaque relay of the underwriter's signed UnderwriteIntentCommit.
    * Submits, then waits for on-chain inclusion via the libfc client's
    * `wait_for_confirmation`. Returns true iff the tx confirmed; the caller
    * uses that to decide whether to record the leg in `confirmed_commits`.
    */
   bool submit_commit_eth(uint64_t uw_request_id, const std::vector<char>& uic_bytes) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: ETH client '{}' not found", eth_client_id);
         return false;
      }
      if (eth_opreg_addr.empty()) {
         elog("underwriter: ETH OperatorRegistry address not configured");
         return false;
      }

      auto& abis = eth_plug->get_abi_files();
      const eth::abi::contract* commit_abi = nullptr;
      for (auto& [path, contracts] : abis) {
         for (auto& c : contracts) {
            if (c.name == "commit") { commit_abi = &c; break; }
         }
         if (commit_abi) break;
      }
      if (!commit_abi) {
         elog("underwriter: ETH commit ABI not found in loaded ABI files");
         return false;
      }

      try {
         std::vector<uint8_t> uic_bytes_u8(uic_bytes.begin(), uic_bytes.end());
         auto tx = entry->client->create_default_tx(eth_opreg_addr, *commit_abi,
            {fc::variant(uic_bytes_u8)});
         auto result   = entry->client->execute_contract_tx_fn(tx, *commit_abi);
         auto tx_hash  = result.as_string();
         // Wait for on-chain inclusion; throws on revert / timeout. Per
         // the user's directive: confirming inclusion BEFORE recording the
         // commit means partial-landing in the local map cannot happen
         // without an OPP-level breakage.
         entry->client->wait_for_confirmation(tx_hash);
         ilog("underwriter: ETH commit confirmed uwreq={} tx_hash={} bytes={}",
              uw_request_id, tx_hash, uic_bytes.size());
         return true;
      } catch (const fc::exception& e) {
         elog("underwriter: ETH commit failed for uwreq={}: {}",
              uw_request_id, e.to_detail_string());
         return false;
      }
   }

   /**
    * Call `commit_underwrite(bytes uic_bytes)` on the SOL outpost's
    * opp-outpost program — an opaque relay of the underwriter's signed
    * UnderwriteIntentCommit. Uses `execute_tx_and_confirm` (libfc helper)
    * so the call returns only after the tx is included + confirmed.
    * Returns true iff the tx confirmed; the caller decides whether to
    * record the leg in `confirmed_commits`.
    */
   bool submit_commit_sol(uint64_t uw_request_id, const std::vector<char>& uic_bytes) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: SOL client '{}' not found", sol_client_id);
         return false;
      }
      if (sol_program_id.empty()) {
         elog("underwriter: SOL program ID not configured");
         return false;
      }
      try {
         auto program_key = fc::crypto::solana::solana_public_key::from_base58_string(sol_program_id);
         auto& idl_files = sol_plug->get_idl_files();
         std::vector<sol::idl::program> program_idls;
         for (auto& [path, programs] : idl_files) {
            for (auto& p : programs) {
               if (p.name == "opp_solana_outpost") {
                  program_idls.push_back(p);
                  break;
               }
            }
         }
         if (program_idls.empty()) {
            elog("underwriter: opp_solana_outpost IDL not found");
            return false;
         }
         auto program_client = std::make_shared<sol::solana_program_client>(
            entry->client, program_key, program_idls);

         if (!program_client->has_idl("commit_underwrite")) {
            elog("underwriter: SOL commit_underwrite IDL missing — deploy bug "
                 "(opp-outpost program does not expose commit_underwrite). "
                 "Skipping SOL leg for uwreq {}", uw_request_id);
            return false;
         }
         auto& instr = program_client->get_idl("commit_underwrite");
         auto accounts = program_client->resolve_accounts(instr);
         std::vector<uint8_t> uic_bytes_u8(uic_bytes.begin(), uic_bytes.end());
         // execute_tx_and_confirm: submits then awaits inclusion; throws
         // on timeout / failure. Same confirm-before-record discipline as
         // the ETH path above.
         auto signature = program_client->execute_tx_and_confirm(
            instr, accounts,
            {fc::variant(fc::mutable_variant_object()("uic_bytes", uic_bytes_u8))});
         ilog("underwriter: SOL commit_underwrite confirmed uwreq={} signature={} bytes={}",
              uw_request_id, signature, uic_bytes.size());
         return true;
      } catch (const fc::exception& e) {
         elog("underwriter: SOL commit_underwrite failed for uwreq={}: {}",
              uw_request_id, e.to_detail_string());
         return false;
      }
   }

   // The plugin previously carried a `push_action()` helper for signing
   // and pushing WIRE-chain actions; after the commit refactor (T9 + T14)
   // the underwriter does not push any WIRE-chain actions on its own —
   // commits go via the outpost RPC clients in `submit_commit_eth` /
   // `submit_commit_sol`. The signature_provider_manager_plugin dependency
   // is still required because `build_signed_uic_bytes` uses it to sign
   // the UIC digest with the underwriter's WIRE K1 key.

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
