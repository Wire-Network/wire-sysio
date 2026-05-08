#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>
#include <magic_enum/magic_enum.hpp>

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>
#include <sysio/depot/opreg_status.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/types/types.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>

#include <algorithm>
#include <numeric>

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
   uint64_t    id;                  // attestation ID (PK of uwreqs table)
   int         attestation_type;    // AttestationType that needs underwriting (e.g., SWAP)
   int         status;              // UnderwriteRequestStatus
   std::string uw_name;             // assigned underwriter ('' if unassigned, populated post race-resolve)
   ChainKind   src_chain;
   TokenKind   src_token_kind;
   uint64_t    src_amount;
   ChainKind   dst_chain;
   TokenKind   dst_token_kind;
   uint64_t    dst_amount;
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
   int         chain_kind;
   int         token_kind;
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
   std::string  eth_opreg_addr;        // OperatorRegistry contract address on ETH
   std::string  sol_program_id;        // opp-outpost program ID on SOL

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
      if (requests.empty()) return;

      ilog("underwriter: found {} pending underwrite requests", requests.size());

      // Step 5: Select requests we can cover (100% on both send and receive chains)
      auto selected = select_coverable(requests);
      if (selected.empty()) {
         ilog("underwriter: no requests coverable with current credit lines");
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
         // chain_kind may come as string (ABI enum) or int
         int ck = 0;
         if (obj["chain_kind"].is_string()) {
            auto ck_str = obj["chain_kind"].as_string();
            if (ck_str == "CHAIN_KIND_ETHEREUM") ck = CHAIN_KIND_ETHEREUM;
            else if (ck_str == "CHAIN_KIND_SOLANA") ck = CHAIN_KIND_SOLANA;
            else if (ck_str == "CHAIN_KIND_WIRE") ck = CHAIN_KIND_WIRE;
         } else {
            ck = static_cast<int>(obj["chain_kind"].as_uint64());
         }
         outpost_chain_kinds[id] = static_cast<ChainKind>(ck);
      }
   }

   // -----------------------------------------------------------------------
   //  Read credit lines from sysio.opreg::operators
   // -----------------------------------------------------------------------

   void read_credit_lines() {
      credit_lines.clear();

      // Helper: protobuf enum name -> numeric int. Mirror of the inline
      // string<->enum logic the rest of the plugin uses; centralized here
      // so additions propagate across both ChainKind / TokenKind reads.
      auto chain_kind_from = [](const fc::variant& v) -> int {
         if (v.is_string()) {
            auto s = v.as_string();
            if (s == "CHAIN_KIND_ETHEREUM") return CHAIN_KIND_ETHEREUM;
            if (s == "CHAIN_KIND_SOLANA")   return CHAIN_KIND_SOLANA;
            if (s == "CHAIN_KIND_WIRE")     return CHAIN_KIND_WIRE;
            return 0;
         }
         return static_cast<int>(v.as_uint64());
      };
      auto token_kind_from = [](const fc::variant& v) -> int {
         if (v.is_string()) {
            auto s = v.as_string();
            if (s == "TOKEN_KIND_WIRE")    return 0;     // matches proto value
            if (s == "TOKEN_KIND_ETH")     return 256;
            if (s == "TOKEN_KIND_LIQETH")  return 496;
            if (s == "TOKEN_KIND_SOL")     return 512;
            if (s == "TOKEN_KIND_LIQSOL")  return 752;
            return 0;
         }
         return static_cast<int>(v.as_uint64());
      };

      auto rows = read_all("sysio.opreg", "sysio.opreg", "operators");
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         auto acct = obj["account"].as_string();
         if (chain::name(acct) != underwriter_account) continue;

         // New schema: per-(chain, token_kind) balance rows on `balances`
         // (replacing the old vector<stake_entry> on `stakes`). Each row
         // is one credit line directly — no aggregation needed.
         if (!obj.contains("balances") || !obj["balances"].is_array()) break;

         for (auto& bal_entry : obj["balances"].get_array()) {
            auto be = bal_entry.get_object();
            if (!be.contains("chain") || !be.contains("token_kind")
                || !be.contains("balance")) continue;

            int     chain   = chain_kind_from(be["chain"]);
            int     token   = token_kind_from(be["token_kind"]);
            uint64_t balance = be["balance"].as_uint64();
            credit_lines.push_back(credit_line{chain, token, balance});
            ilog("underwriter: credit line chain_kind={} token_kind={} balance={}",
                 chain, token, balance);
         }
         break;
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
      for (auto& [outpost_id, chain_kind] : outpost_chain_kinds) {
         int ck = static_cast<int>(chain_kind);
         bool found = false;
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == ck && cl.balance > 0) {
               found = true;
               break;
            }
         }
         if (!found) {
            ilog("underwriter: not available — no balance on chain_kind={}", ck);
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
         req.status = magic_enum::enum_integer(
            UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING);
         req.uw_name = uw_name;

         // Parse attestation type
         if (obj["type"].is_string()) {
            auto t = obj["type"].as_string();
            if (t == "ATTESTATION_TYPE_SWAP") req.attestation_type = ATTESTATION_TYPE_SWAP;
            else continue; // Only handle SWAP requests
         } else {
            req.attestation_type = static_cast<int>(obj["type"].as_uint64());
            if (req.attestation_type != ATTESTATION_TYPE_SWAP) continue;
         }

         // New schema: src/dst (chain, token_kind, amount) live directly on
         // the uwreq row (populated by uwrit::createuwreq from the
         // originating SwapRequest). No more parse_swap_from_attestation
         // detour through sysio.msgch::attestations.
         auto chain_from = [](const fc::variant& v) -> ChainKind {
            if (v.is_string()) {
               auto s = v.as_string();
               if (s == "CHAIN_KIND_ETHEREUM") return CHAIN_KIND_ETHEREUM;
               if (s == "CHAIN_KIND_SOLANA")   return CHAIN_KIND_SOLANA;
               if (s == "CHAIN_KIND_WIRE")     return CHAIN_KIND_WIRE;
               return CHAIN_KIND_UNKNOWN;
            }
            return static_cast<ChainKind>(v.as_uint64());
         };
         auto token_from = [](const fc::variant& v) -> TokenKind {
            if (v.is_string()) {
               auto s = v.as_string();
               if (s == "TOKEN_KIND_WIRE")    return static_cast<TokenKind>(0);
               if (s == "TOKEN_KIND_ETH")     return static_cast<TokenKind>(256);
               if (s == "TOKEN_KIND_LIQETH")  return static_cast<TokenKind>(496);
               if (s == "TOKEN_KIND_SOL")     return static_cast<TokenKind>(512);
               if (s == "TOKEN_KIND_LIQSOL")  return static_cast<TokenKind>(752);
               return static_cast<TokenKind>(0);
            }
            return static_cast<TokenKind>(v.as_uint64());
         };

         if (!obj.contains("src_chain") || !obj.contains("src_amount")
             || !obj.contains("dst_chain") || !obj.contains("dst_amount")) {
            // Row not yet populated (createuwreq writes them inline so this
            // should be unreachable for SWAP-derived UWREQs). Skip safely.
            continue;
         }
         req.src_chain      = chain_from(obj["src_chain"]);
         req.src_token_kind = token_from(obj["src_token_kind"]);
         req.src_amount     = obj["src_amount"].as_uint64();
         req.dst_chain      = chain_from(obj["dst_chain"]);
         req.dst_token_kind = token_from(obj["dst_token_kind"]);
         req.dst_amount     = obj["dst_amount"].as_uint64();

         requests.push_back(std::move(req));
      }

      return requests;
   }

   // -----------------------------------------------------------------------
   //  Select requests coverable by our credit lines
   //  Requires 100% coverage on BOTH src and dst legs of the swap, where
   //  each leg's required bond is per-(chain_kind, token_kind).
   // -----------------------------------------------------------------------

   std::vector<uw_request> select_coverable(std::vector<uw_request>& requests) {
      // Build remaining credit per (chain_kind, token_kind). Pack the pair
      // into a 64-bit key so std::map iteration stays cheap.
      auto key = [](int c, int t) -> uint64_t {
         return (static_cast<uint64_t>(c) << 32) | static_cast<uint64_t>(t);
      };
      std::map<uint64_t, uint64_t> remaining;
      for (auto& cl : credit_lines) {
         remaining[key(cl.chain_kind, cl.token_kind)] = cl.balance;
      }

      // Sort by src_amount ascending (smaller swaps first — fill more requests).
      std::sort(requests.begin(), requests.end(),
                [](const uw_request& a, const uw_request& b) {
                   return a.src_amount < b.src_amount;
                });

      std::vector<uw_request> selected;
      for (auto& req : requests) {
         uint64_t src_k = key(static_cast<int>(req.src_chain),
                              static_cast<int>(req.src_token_kind));
         uint64_t dst_k = key(static_cast<int>(req.dst_chain),
                              static_cast<int>(req.dst_token_kind));

         // Check source-leg credit
         auto src_it = remaining.find(src_k);
         if (src_it == remaining.end() || src_it->second < req.src_amount) {
            ilog("underwriter: skip request {} — insufficient src credit (chain={} token={} need={} have={})",
                 req.id,
                 static_cast<int>(req.src_chain),
                 static_cast<int>(req.src_token_kind),
                 req.src_amount,
                 src_it != remaining.end() ? src_it->second : 0);
            continue;
         }

         // Check destination-leg credit
         auto tgt_it = remaining.find(dst_k);
         if (tgt_it == remaining.end() || tgt_it->second < req.dst_amount) {
            ilog("underwriter: skip request {} — insufficient dst credit (chain={} need={} have={})",
                 req.id,
                 static_cast<int>(req.dst_chain),
                 req.dst_amount,
                 tgt_it != remaining.end() ? tgt_it->second : 0);
            continue;
         }

         // Reserve credit on both legs (avoid double-using the same balance
         // across multiple selected requests this cycle).
         src_it->second -= req.src_amount;
         tgt_it->second -= req.dst_amount;

         selected.push_back(req);
         ilog("underwriter: selected request {} — src(chain={},token={},amt={}) dst(chain={},token={},amt={})",
              req.id,
              static_cast<int>(req.src_chain), static_cast<int>(req.src_token_kind), req.src_amount,
              static_cast<int>(req.dst_chain), static_cast<int>(req.dst_token_kind), req.dst_amount);
      }

      return selected;
   }

   // -----------------------------------------------------------------------
   //  Submit intent to outpost contract
   //  The outpost locks capital and emits UNDERWRITE_INTENT via OPP
   // -----------------------------------------------------------------------

   /**
    * Submit a `commit` JSON-RPC call to BOTH legs of the swap (source +
    * destination outposts). Each outpost queues an UNDERWRITE_INTENT_COMMIT
    * attestation back to the depot; the depot's race resolver
    * (sysio.uwrit::try_select_winner) selects the underwriter whose pair
    * lands first AND whose available() rollup covers both legs.
    *
    * Per the corrected ledger model: outposts don't validate bond — they
    * just authenticate the caller as a registered underwriter and queue the
    * attestation. The depot does the bond check.
    */
   void submit_intent_to_outpost(const uw_request& req) {
      ilog("underwriter: submitting commit pair for uwreq {} src_chain={} dst_chain={}",
           req.id, static_cast<int>(req.src_chain), static_cast<int>(req.dst_chain));

      auto submit_one = [this](ChainKind chain, uint64_t uw_request_id) {
         if (chain == CHAIN_KIND_ETHEREUM) submit_commit_eth(uw_request_id);
         else if (chain == CHAIN_KIND_SOLANA) submit_commit_sol(uw_request_id);
         else elog("underwriter: unsupported chain={} for commit (uwreq {})",
                   static_cast<int>(chain), uw_request_id);
      };
      submit_one(req.src_chain, req.id);
      submit_one(req.dst_chain, req.id);
   }

   /**
    * Call `commit(uint64 uwRequestId, bytes signature)` on the ETH outpost's
    * OperatorRegistry contract (introduced in wire-ethereum commit 14639ec
    * for Task 7). The outpost queues an UNDERWRITE_INTENT_COMMIT attestation
    * back to the depot.
    *
    * Signature is empty bytes for v1 — the depot's race resolver doesn't
    * validate it yet (signature is for "defends against an outpost compromised
    * relay forging commits in their name", a hardening phase that lands later).
    */
   void submit_commit_eth(uint64_t uw_request_id) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: ETH client '{}' not found", eth_client_id);
         return;
      }
      if (eth_opreg_addr.empty()) {
         elog("underwriter: ETH OperatorRegistry address not configured");
         return;
      }

      // Find the `commit` ABI from loaded ABI files (replaced the legacy
      // submitUnderwriteIntent in Task 7's OperatorRegistry refactor).
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
         return;
      }

      try {
         std::vector<uint8_t> empty_sig;
         auto tx = entry->client->create_default_tx(eth_opreg_addr, *commit_abi,
            {fc::variant(uw_request_id), fc::variant(empty_sig)});
         auto result = entry->client->execute_contract_tx_fn(tx, *commit_abi);
         ilog("underwriter: ETH commit submitted uwreq={} result={}",
              uw_request_id, result.as_string());
      } catch (const fc::exception& e) {
         elog("underwriter: ETH commit failed: {}", e.to_detail_string());
      }
   }

   /**
    * Solana-side commit submission. The matching `commit_underwrite`
    * Anchor instruction is part of Task 8's follow-up scope (the v1
    * Solana commit only landed schema + SLASH_OPERATOR dispatch). For now
    * the call falls through to a log so the dual-leg flow on a SOL-touching
    * UWREQ is observable in test clusters even though only the ETH leg
    * actually relays. Once Task 8 follow-up adds `commit_underwrite`, the
    * IDL lookup below activates.
    */
   void submit_commit_sol(uint64_t uw_request_id) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: SOL client '{}' not found", sol_client_id);
         return;
      }
      if (sol_program_id.empty()) {
         elog("underwriter: SOL program ID not configured");
         return;
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
            return;
         }
         auto program_client = std::make_shared<sol::solana_program_client>(
            entry->client, program_key, program_idls);

         if (program_client->has_idl("commit_underwrite")) {
            auto& instr = program_client->get_idl("commit_underwrite");
            auto accounts = program_client->resolve_accounts(instr);
            std::vector<uint8_t> empty_sig;
            program_client->execute_tx(instr, accounts,
               {fc::variant(fc::mutable_variant_object()
                  ("uw_request_id", uw_request_id)
                  ("signature", empty_sig))});
            ilog("underwriter: SOL commit_underwrite submitted uwreq={}", uw_request_id);
         } else {
            wlog("underwriter: SOL commit_underwrite IDL not found — Solana leg skipped (pending Task 8 follow-up) uwreq={}",
                 uw_request_id);
         }
      } catch (const fc::exception& e) {
         elog("underwriter: SOL commit_underwrite failed: {}", e.to_detail_string());
      }
   }

   // -----------------------------------------------------------------------
   //  Action push helper (for WIRE chain actions)
   // -----------------------------------------------------------------------

   void push_action(const std::string& contract,
                    const std::string& action_name,
                    chain::name auth_account,
                    const fc::variant_object& data) {
      auto& chain = chain_plug->chain();
      auto abi_max_time = fc::microseconds(action_timeout_ms * 1000);

      auto resolver = make_resolver(chain, abi_max_time, throw_on_yield::no);
      auto abis_opt = resolver(chain::name(contract));
      auto action_type = abis_opt->get_action_type(chain::name(action_name));
      auto action_data = abis_opt->variant_to_binary(
         action_type, fc::variant(data),
         chain::abi_serializer::create_yield_function(abi_max_time));

      chain::signed_transaction trx;
      trx.actions.emplace_back(
         std::vector<chain::permission_level>{{auth_account, chain::config::active_name}},
         chain::name(contract), chain::name(action_name), std::move(action_data));
      trx.set_reference_block(chain.head().id());
      trx.expiration = fc::time_point_sec(chain.head().block_time() + fc::seconds(30));

      // Sign with WIRE K1 key
      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      auto wire_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_wire, fc::crypto::chain_key_type_wire);
      if (wire_providers.empty()) {
         elog("underwriter: no WIRE K1 signature provider available");
         return;
      }
      auto chain_id = chain.get_chain_id();
      auto digest = trx.sig_digest(chain_id, trx.context_free_data);
      trx.signatures.push_back(wire_providers.front()->sign(digest));

      auto packed = chain::packed_transaction(std::move(trx), chain::packed_transaction::compression_type::none);
      auto rw = chain_plug->get_read_write_api(abi_max_time);
      fc::variant packed_var;
      chain::to_variant(packed, packed_var);

      std::promise<void> done;
      auto future = done.get_future();

      rw.push_transaction(
         packed_var.get_object(),
         [&done, &contract, &action_name](const auto& result) {
            if (auto* err = std::get_if<fc::exception_ptr>(&result)) {
               elog("underwriter: push {}::{} failed — {}", contract, action_name, (*err)->to_string());
            } else {
               ilog("underwriter: pushed {}::{} ok", contract, action_name);
            }
            done.set_value();
         });

      if (future.wait_for(std::chrono::milliseconds(action_timeout_ms)) == std::future_status::timeout) {
         elog("underwriter: push {}::{} timed out", contract, action_name);
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
   opts("underwriter-eth-client-id", bpo::value<std::string>()->default_value(underwriter_defaults::eth_client_id),
        "Ethereum outpost client ID");
   opts("underwriter-sol-client-id", bpo::value<std::string>()->default_value(underwriter_defaults::sol_client_id),
        "Solana outpost client ID");
   opts("underwriter-eth-opreg-addr", bpo::value<std::string>(),
        "OperatorRegistry contract address on Ethereum (hex)");
   opts("underwriter-sol-program-id", bpo::value<std::string>(),
        "OPP outpost program ID on Solana (base58)");
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
   if (options.count("underwriter-sol-program-id"))
      _impl->sol_program_id = options["underwriter-sol-program-id"].as<std::string>();

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
