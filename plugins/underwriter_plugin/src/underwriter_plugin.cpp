#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/io/raw.hpp>
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

      // ── Check 3: non-zero balance per outpost chain ──────────────────
      // Reuses read_credit_lines() (refreshed every cycle anyway) for
      // consistency with the live scan path.
      read_credit_lines();
      for (auto& [outpost_id, chain_kind] : outpost_chain_kinds) {
         const auto ck_int = magic_enum::enum_integer(chain_kind);
         bool has_balance = false;
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == static_cast<int>(ck_int) && cl.balance > 0) {
               has_balance = true;
               break;
            }
         }
         if (!has_balance) {
            elog("underwriter preflight: zero balance on outpost {} "
                 "(chain_kind={}) — bootstrap must deposit collateral for "
                 "this account on every outpost chain",
                 outpost_id,
                 std::string{sysio::opp::types::ChainKind_Name(chain_kind)});
            return false;
         }
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
            const int      chain   = magic_enum::enum_integer(be["chain"].as<ChainKind>());
            const int      token   = magic_enum::enum_integer(be["token_kind"].as<TokenKind>());
            const uint64_t balance = be["balance"].as_uint64();
            credit_lines.push_back(credit_line{chain, token, balance});
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
         const int      chain  = magic_enum::enum_integer(obj["chain"].as<ChainKind>());
         const int      token  = magic_enum::enum_integer(obj["token_kind"].as<TokenKind>());
         const uint64_t amount = obj["amount"].as_uint64();
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
         const int      chain  = magic_enum::enum_integer(obj["chain"].as<ChainKind>());
         const int      token  = magic_enum::enum_integer(obj["token_kind"].as<TokenKind>());
         const uint64_t amount = obj["amount"].as_uint64();
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == chain && cl.token_kind == token) {
               cl.balance = (cl.balance > amount) ? (cl.balance - amount) : 0;
               break;
            }
         }
      }

      for (auto& cl : credit_lines) {
         ilog("underwriter: credit line chain_kind={} token_kind={} available={}",
              cl.chain_kind, cl.token_kind, cl.balance);
      }
   }

   /// Per-(chain, token_kind) availability predicate — replaces the
   /// per-chain `is_available()` so `select_coverable` and any future
   /// per-token gate can use the same lookup.
   bool has_credit(ChainKind chain, TokenKind token_kind) const {
      const int ck = magic_enum::enum_integer(chain);
      const int tk = magic_enum::enum_integer(token_kind);
      for (auto& cl : credit_lines) {
         if (cl.chain_kind == ck && cl.token_kind == tk && cl.balance > 0) return true;
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
            if (t == "ATTESTATION_TYPE_SWAP_REQUEST") req.attestation_type = ATTESTATION_TYPE_SWAP_REQUEST;
            else continue; // Only handle SWAP_REQUEST attestations
         } else {
            req.attestation_type = static_cast<int>(obj["type"].as_uint64());
            if (req.attestation_type != ATTESTATION_TYPE_SWAP_REQUEST) continue;
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
   /// given (uwreq_id, outpost_id) leg. Returns an empty vector on any
   /// failure (no signature provider, serialize failure, etc.).
   ///
   /// Digest semantics: the underwriter signs `sha256(serialize(uic with
   /// signature blanked))`. The depot's `try_select_winner` rebuilds the
   /// same digest from the bytes it received and verifies the embedded
   /// signature against the underwriter's WIRE account permissions via
   /// `get_permission_lower_bound` — see `sysio.uwrit::verify_uic_signature`.
   std::vector<char> build_signed_uic_bytes(uint64_t uwreq_id, uint64_t outpost_id) {
      opp_att::UnderwriteIntentCommit uic;
      uic.mutable_uw_account()->set_name(underwriter_account.to_string());
      uic.set_uw_request_id(uwreq_id);
      uic.set_outpost_id(outpost_id);
      // uw_ext_chain_addr left default-constructed (empty kind/address) for
      // v1 — the per-leg outpost_id is the binding the depot's verify path
      // needs, and the signature ties the whole UIC together regardless.
      uic.clear_signature();

      std::string blanked;
      if (!uic.SerializeToString(&blanked)) {
         elog("underwriter: UIC serialize failed (blank phase) for uwreq {}", uwreq_id);
         return {};
      }

      auto digest = fc::sha256::hash(blanked.data(), blanked.size());

      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      auto wire_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_wire, fc::crypto::chain_key_type_wire);
      if (wire_providers.empty()) {
         elog("underwriter: no WIRE K1 signature provider available for uwreq {}",
              uwreq_id);
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
      ilog("underwriter: submitting commit pair for uwreq {} src_chain={} dst_chain={}",
           req.id, static_cast<int>(req.src_chain), static_cast<int>(req.dst_chain));

      auto submit_one = [this](ChainKind chain, uint64_t uw_request_id) {
         auto outpost_id_opt = find_outpost_id(chain);
         if (!outpost_id_opt) {
            elog("underwriter: no outpost registered for chain_kind={} (uwreq {})",
                 static_cast<int>(chain), uw_request_id);
            return;
         }
         auto uic_bytes = build_signed_uic_bytes(uw_request_id, *outpost_id_opt);
         if (uic_bytes.empty()) return;   // already logged

         if (chain == CHAIN_KIND_ETHEREUM)      submit_commit_eth(uw_request_id, uic_bytes);
         else if (chain == CHAIN_KIND_SOLANA)   submit_commit_sol(uw_request_id, uic_bytes);
         else elog("underwriter: unsupported chain={} for commit (uwreq {})",
                   static_cast<int>(chain), uw_request_id);
      };
      submit_one(req.src_chain, req.id);
      submit_one(req.dst_chain, req.id);
   }

   /**
    * Call `commit(bytes uicBytes)` on the ETH outpost's OperatorRegistry —
    * an opaque relay of the underwriter's signed UnderwriteIntentCommit.
    * The contract auth-checks msg.sender (active underwriter) and emits
    * the bytes verbatim onto the OPP outbound queue back to the depot.
    */
   void submit_commit_eth(uint64_t uw_request_id, const std::vector<char>& uic_bytes) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: ETH client '{}' not found", eth_client_id);
         return;
      }
      if (eth_opreg_addr.empty()) {
         elog("underwriter: ETH OperatorRegistry address not configured");
         return;
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
         return;
      }

      try {
         std::vector<uint8_t> uic_bytes_u8(uic_bytes.begin(), uic_bytes.end());
         auto tx = entry->client->create_default_tx(eth_opreg_addr, *commit_abi,
            {fc::variant(uic_bytes_u8)});
         auto result = entry->client->execute_contract_tx_fn(tx, *commit_abi);
         ilog("underwriter: ETH commit submitted uwreq={} bytes={} result={}",
              uw_request_id, uic_bytes.size(), result.as_string());
      } catch (const fc::exception& e) {
         elog("underwriter: ETH commit failed: {}", e.to_detail_string());
      }
   }

   /**
    * Call `commit_underwrite(bytes uic_bytes)` on the SOL outpost's
    * opp-outpost program — an opaque relay of the underwriter's signed
    * UnderwriteIntentCommit. The program auth-checks the Signer (active
    * underwriter) and pushes the bytes verbatim onto the outbound buffer.
    */
   void submit_commit_sol(uint64_t uw_request_id, const std::vector<char>& uic_bytes) {
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

         if (!program_client->has_idl("commit_underwrite")) {
            elog("underwriter: SOL commit_underwrite IDL missing — deploy bug "
                 "(opp-outpost program does not expose commit_underwrite). "
                 "Skipping SOL leg for uwreq {}", uw_request_id);
            return;
         }
         auto& instr = program_client->get_idl("commit_underwrite");
         auto accounts = program_client->resolve_accounts(instr);
         std::vector<uint8_t> uic_bytes_u8(uic_bytes.begin(), uic_bytes.end());
         program_client->execute_tx(instr, accounts,
            {fc::variant(fc::mutable_variant_object()("uic_bytes", uic_bytes_u8))});
         ilog("underwriter: SOL commit_underwrite submitted uwreq={} bytes={}",
              uw_request_id, uic_bytes.size());
      } catch (const fc::exception& e) {
         elog("underwriter: SOL commit_underwrite failed: {}", e.to_detail_string());
      }
   }

   // The plugin previously carried a `push_action()` helper for signing
   // and pushing WIRE-chain actions; after the commit refactor (T9 + T14)
   // the underwriter does not push any WIRE-chain actions on its own —
   // commits go via the outpost RPC clients in `submit_commit_eth` /
   // `submit_commit_sol`. The signature_provider_manager_plugin dependency
   // is still required because `build_signed_uic_bytes` uses it to sign
   // the UIC digest with the underwriter's WIRE K1 key.
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
