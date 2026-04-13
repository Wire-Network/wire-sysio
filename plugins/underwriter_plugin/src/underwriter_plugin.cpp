#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>

#include <sysio/underwriter_plugin/underwriter_plugin.hpp>
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
//  Underwrite request — parsed from sysio.uwrit::uwreqs table
// ---------------------------------------------------------------------------
struct uw_request {
   uint64_t    id;                  // attestation ID (PK of uwreqs table)
   int         attestation_type;    // AttestationType that needs underwriting (e.g., SWAP)
   int         status;              // UnderwriteRequestStatus
   std::string uw_name;             // assigned underwriter (empty if unassigned)
   // Decoded from the attestation data:
   ChainKind   source_chain;
   ChainKind   target_chain;
   int64_t     source_amount;
   int64_t     target_amount;
   TokenKind   source_token;
   TokenKind   target_token;
};

// ---------------------------------------------------------------------------
//  Credit line — aggregate stake per chain from sysio.opreg::operators
// ---------------------------------------------------------------------------
struct credit_line {
   int         chain_kind;
   int64_t     total_staked;        // aggregate sum of all stake entries for this chain
};

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct underwriter_plugin::impl {
   // Configuration
   chain::name  underwriter_account;
   bool         enabled             = false;
   uint32_t     scan_interval_ms    = 5000;
   uint32_t     action_timeout_ms   = 15000;
   std::string  eth_client_id;
   std::string  sol_client_id;
   std::string  eth_opreg_addr;        // OperatorRegistry contract address on ETH
   std::string  sol_program_id;        // opp-outpost program ID on SOL

   // Credit lines (read from sysio.opreg::operators each cycle)
   std::vector<credit_line> credit_lines;

   // Plugin references
   chain_plugin*                     chain_plug = nullptr;
   cron_plugin*                      cron_plug  = nullptr;
   outpost_ethereum_client_plugin*   eth_plug   = nullptr;
   outpost_solana_client_plugin*     sol_plug   = nullptr;

   // Cron job handle
   cron_service::job_id_t            scan_job_id = 0;
   bool                              shutting_down = false;

   // Outpost chain_kind cache: outpost_id -> ChainKind
   std::map<uint64_t, ChainKind>     outpost_chain_kinds;

   // -----------------------------------------------------------------------
   //  Table read helper
   // -----------------------------------------------------------------------

   read_only::get_table_rows_result read_table(const std::string& code,
                                                const std::string& scope,
                                                const std::string& table,
                                                uint32_t limit = 100) {
      auto ro = chain_plug->get_read_only_api(fc::microseconds(action_timeout_ms * 1000));
      read_only::get_table_rows_params p;
      p.json  = true;
      p.code  = chain::name(code);
      p.scope = scope;
      p.table = chain::name(table);
      p.limit = limit;
      auto deadline = fc::time_point::now() + fc::milliseconds(action_timeout_ms);
      auto result_fn = ro.get_table_rows(p, deadline);
      auto result = result_fn();
      if (auto* err = std::get_if<fc::exception_ptr>(&result)) {
         elog("underwriter: table read failed {}::{} — {}", code, table, (*err)->to_string());
         return {};
      }
      return std::get<read_only::get_table_rows_result>(result);
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
      auto rows = read_table("sysio.epoch", "sysio.epoch", "outposts", 100);
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

      auto rows = read_table("sysio.opreg", "sysio.opreg", "operators", 100);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         auto acct = obj["account"].as_string();
         if (chain::name(acct) != underwriter_account) continue;

         // Found our operator entry — parse stakes to compute aggregate per chain
         if (!obj.contains("stakes") || !obj["stakes"].is_array()) break;

         std::map<int, int64_t> aggregates; // chain_kind -> aggregate amount
         for (auto& stake_entry : obj["stakes"].get_array()) {
            auto se = stake_entry.get_object();
            if (!se.contains("chain_addr") || !se.contains("amount")) continue;

            auto chain_addr = se["chain_addr"].get_object();
            int chain_kind = 0;
            if (chain_addr["kind"].is_string()) {
               auto kind_str = chain_addr["kind"].as_string();
               if (kind_str == "CHAIN_KIND_ETHEREUM") chain_kind = CHAIN_KIND_ETHEREUM;
               else if (kind_str == "CHAIN_KIND_SOLANA") chain_kind = CHAIN_KIND_SOLANA;
               else if (kind_str == "CHAIN_KIND_WIRE") chain_kind = CHAIN_KIND_WIRE;
            } else {
               chain_kind = static_cast<int>(chain_addr["kind"].as_uint64());
            }

            auto amount_obj = se["amount"].get_object();
            int64_t amt = 0;
            if (amount_obj.contains("amount")) {
               amt = amount_obj["amount"].as_int64();
            }

            aggregates[chain_kind] += amt;
         }

         for (auto& [ck, total] : aggregates) {
            credit_lines.push_back(credit_line{ck, total});
            ilog("underwriter: credit line chain_kind={} total_staked={}", ck, total);
         }
         break; // Found our entry, done
      }
   }

   // -----------------------------------------------------------------------
   //  Availability check — any amount > 0 on ALL active chains
   // -----------------------------------------------------------------------

   bool is_available() {
      if (credit_lines.empty()) {
         ilog("underwriter: not available — no stakes found in sysio.opreg");
         return false;
      }

      // Check that we have > 0 credit on every active outpost chain
      for (auto& [outpost_id, chain_kind] : outpost_chain_kinds) {
         int ck = static_cast<int>(chain_kind);
         bool found = false;
         for (auto& cl : credit_lines) {
            if (cl.chain_kind == ck && cl.total_staked > 0) {
               found = true;
               break;
            }
         }
         if (!found) {
            ilog("underwriter: not available — no stake on chain_kind={}", ck);
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

      auto rows = read_table("sysio.uwrit", "sysio.uwrit", "uwreqs", 100);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();

         // Filter: PENDING status only (UnderwriteRequestStatus = 0)
         int status = 0;
         if (obj["status"].is_string()) {
            auto s = obj["status"].as_string();
            if (s == "UNDERWRITE_REQUEST_STATUS_PENDING") status = 0;
            else continue; // Not PENDING
         } else {
            status = static_cast<int>(obj["status"].as_uint64());
            if (status != 0) continue; // UNDERWRITE_REQUEST_STATUS_PENDING = 0
         }

         // Skip if already assigned to another underwriter
         auto uw_name = obj["uw_name"].as_string();
         if (!uw_name.empty() && chain::name(uw_name) != underwriter_account &&
             chain::name(uw_name) != chain::name()) {
            continue;
         }

         uw_request req;
         req.id = obj["id"].as_uint64();
         req.status = status;
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

         // The uwreq stores the encoded attestation data — we need to determine
         // source/target chains and amounts. For now, read from locked_amounts
         // which will be populated as intents come in. For initial request,
         // we need the original SWAP data which is referenced by the attestation_id.
         // The attestation_id maps back to sysio.msgch::attestations table.
         if (!parse_swap_from_attestation(req)) continue;

         requests.push_back(std::move(req));
      }

      return requests;
   }

   bool parse_swap_from_attestation(uw_request& req) {
      // Read the attestation data from sysio.msgch::attestations by ID
      auto rows = read_table("sysio.msgch", "sysio.msgch", "attestations", 100);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         uint64_t att_id = obj["id"].as_uint64();
         if (att_id != req.id) continue;

         // Found the attestation — parse the raw data as a Swap protobuf
         auto data_hex = obj["data"].as_string();
         auto bytes = fc::from_hex(data_hex);
         if (bytes.empty()) return false;

         opp_att::SwapRequest swap;
         if (!swap.ParseFromString(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
            elog("underwriter: protobuf parse failed for attestation {}", req.id);
            return false;
         }

         if (swap.has_source_amount()) {
            req.source_amount = swap.source_amount().amount();
            req.source_token = static_cast<TokenKind>(swap.source_amount().kind());
         } else {
            return false;
         }

         if (swap.has_target_chain()) {
            req.target_chain = swap.target_chain().kind();
         } else {
            return false;
         }

         req.target_token = swap.target_token();

         // Target amount defaults to source amount (1:1 for now)
         req.target_amount = req.source_amount;

         // Determine source chain from the attestation's outpost_id
         uint64_t outpost_id = obj["outpost_id"].as_uint64();
         auto it = outpost_chain_kinds.find(outpost_id);
         if (it == outpost_chain_kinds.end()) return false;
         req.source_chain = it->second;

         return true;
      }

      elog("underwriter: attestation {} not found in sysio.msgch::attestations", req.id);
      return false;
   }

   // -----------------------------------------------------------------------
   //  Select requests coverable by our credit lines
   //  Requires 100% coverage on BOTH send and receive chains
   // -----------------------------------------------------------------------

   std::vector<uw_request> select_coverable(std::vector<uw_request>& requests) {
      // Build remaining credit per chain
      std::map<int, int64_t> remaining;
      for (auto& cl : credit_lines) {
         remaining[cl.chain_kind] = cl.total_staked;
      }

      // Sort by source_amount ascending (smaller swaps first — fill more requests)
      std::sort(requests.begin(), requests.end(),
                [](const uw_request& a, const uw_request& b) {
                   return a.source_amount < b.source_amount;
                });

      std::vector<uw_request> selected;
      for (auto& req : requests) {
         int src_ck = static_cast<int>(req.source_chain);
         int tgt_ck = static_cast<int>(req.target_chain);

         // Check source chain credit
         auto src_it = remaining.find(src_ck);
         if (src_it == remaining.end() || src_it->second < req.source_amount) {
            ilog("underwriter: skip request {} — insufficient source credit (chain={}, need={}, have={})",
                 req.id, src_ck, req.source_amount,
                 src_it != remaining.end() ? src_it->second : 0);
            continue;
         }

         // Check target chain credit
         auto tgt_it = remaining.find(tgt_ck);
         if (tgt_it == remaining.end() || tgt_it->second < req.target_amount) {
            ilog("underwriter: skip request {} — insufficient target credit (chain={}, need={}, have={})",
                 req.id, tgt_ck, req.target_amount,
                 tgt_it != remaining.end() ? tgt_it->second : 0);
            continue;
         }

         // Reserve credit on both chains
         src_it->second -= req.source_amount;
         tgt_it->second -= req.target_amount;

         selected.push_back(req);
         ilog("underwriter: selected request {} (source: chain={} amount={}, target: chain={} amount={})",
              req.id, src_ck, req.source_amount, tgt_ck, req.target_amount);
      }

      return selected;
   }

   // -----------------------------------------------------------------------
   //  Submit intent to outpost contract
   //  The outpost locks capital and emits UNDERWRITE_INTENT via OPP
   // -----------------------------------------------------------------------

   void submit_intent_to_outpost(const uw_request& req) {
      ilog("underwriter: submitting intent for request {} to source chain {}",
           req.id, static_cast<int>(req.source_chain));

      if (req.source_chain == CHAIN_KIND_ETHEREUM) {
         submit_intent_eth(req);
      } else if (req.source_chain == CHAIN_KIND_SOLANA) {
         submit_intent_sol(req);
      } else {
         elog("underwriter: unsupported source chain {} for request {}",
              static_cast<int>(req.source_chain), req.id);
      }
   }

   void submit_intent_eth(const uw_request& req) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: ETH client '{}' not found", eth_client_id);
         return;
      }

      if (eth_opreg_addr.empty()) {
         elog("underwriter: ETH OperatorRegistry address not configured");
         return;
      }

      // Find the submitUnderwriteIntent ABI from loaded ABI files
      auto& abis = eth_plug->get_abi_files();
      const eth::abi::contract* intent_abi = nullptr;
      for (auto& [path, contracts] : abis) {
         for (auto& c : contracts) {
            if (c.name == "submitUnderwriteIntent") {
               intent_abi = &c;
               break;
            }
         }
         if (intent_abi) break;
      }

      if (!intent_abi) {
         elog("underwriter: ETH submitUnderwriteIntent ABI not found in loaded ABI files");
         return;
      }

      // Call OperatorRegistry.submitUnderwriteIntent(requestId, amount)
      // The outpost contract will:
      // 1. Verify the underwriter has enough deposited collateral
      // 2. Lock the collateral amount
      // 3. Emit UNDERWRITE_INTENT attestation via OPP
      try {
         auto tx = entry->client->create_default_tx(eth_opreg_addr, *intent_abi,
            {fc::variant(req.id), fc::variant(req.source_amount)});
         auto result = entry->client->execute_contract_tx_fn(tx, *intent_abi);
         ilog("underwriter: ETH submitUnderwriteIntent result={}", result.as_string());
      } catch (const fc::exception& e) {
         elog("underwriter: ETH submitUnderwriteIntent failed: {}", e.to_detail_string());
      }
   }

   void submit_intent_sol(const uw_request& req) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) {
         elog("underwriter: SOL client '{}' not found", sol_client_id);
         return;
      }

      if (sol_program_id.empty()) {
         elog("underwriter: SOL program ID not configured");
         return;
      }

      // Build and execute a Solana transaction calling the opp-outpost
      // submit_underwrite_intent instruction. The program will:
      // 1. Verify the underwriter has enough deposited collateral in vault PDA
      // 2. Lock the collateral amount
      // 3. Emit UNDERWRITE_INTENT via sol_log_data
      try {
         auto program_key = sol::solana_public_key::from_base58(sol_program_id);
         auto& idl_files = sol_plug->get_idl_files();

         // Find the opp_solana_outpost program IDL
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

         if (program_client->has_idl("submit_underwrite_intent")) {
            auto& instr = program_client->get_idl("submit_underwrite_intent");
            auto accounts = program_client->resolve_accounts(instr);
            program_client->execute_tx(instr, accounts,
               {fc::variant(fc::mutable_variant_object()
                  ("request_id", req.id)
                  ("amount", req.source_amount))});
            ilog("underwriter: SOL submit_underwrite_intent succeeded for request {}", req.id);
         } else {
            elog("underwriter: submit_underwrite_intent instruction not found in IDL");
         }
      } catch (const fc::exception& e) {
         elog("underwriter: SOL submit_underwrite_intent failed: {}", e.to_detail_string());
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
   opts("underwriter-scan-interval-ms", bpo::value<uint32_t>()->default_value(5000),
        "How often to scan for pending underwrite requests (ms)");
   opts("underwriter-action-timeout-ms", bpo::value<uint32_t>()->default_value(15000),
        "Timeout for outpost contract calls and table reads (ms)");
   opts("underwriter-enabled", bpo::value<bool>()->default_value(false),
        "Enable underwriter functionality");
   opts("underwriter-eth-client-id", bpo::value<std::string>()->default_value("eth-default"),
        "Ethereum outpost client ID");
   opts("underwriter-sol-client-id", bpo::value<std::string>()->default_value("sol-default"),
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
