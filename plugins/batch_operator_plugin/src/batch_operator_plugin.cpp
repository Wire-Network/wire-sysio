#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/attestations/attestations.pb.h>

namespace sysio {

using namespace chain_apis;
using namespace sysio::opp::types;
namespace eth = fc::network::ethereum;
namespace sol = fc::network::solana;

namespace {
   constexpr auto DELIVERY_TIMEOUT_MS = 15000;
   constexpr auto EPOCH_POLL_MS = 15000;
}

// ---------------------------------------------------------------------------
//  Outpost descriptor — one per registered outpost
// ---------------------------------------------------------------------------
struct outpost_descriptor {
   uint64_t    id          = 0;
   ChainKind   chain_kind  = CHAIN_KIND_UNKNOWN;
   uint32_t    chain_id    = 0;

   // Per-epoch transient state
   std::string           outbound_envelope_hash_hex;
   std::vector<char>     outbound_raw_envelope;
   std::string           inbound_chain_hash_hex;
   std::vector<char>     inbound_raw_messages;
   uint32_t              inbound_msg_count = 0;

   void reset_transient() {
      outbound_envelope_hash_hex.clear();
      outbound_raw_envelope.clear();
      inbound_chain_hash_hex.clear();
      inbound_raw_messages.clear();
      inbound_msg_count = 0;
   }
};

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct batch_operator_plugin::impl {
   // Configuration
   chain::name  operator_account;
   bool         enabled             = false;
   uint32_t     epoch_poll_ms       = EPOCH_POLL_MS;
   uint32_t     delivery_timeout_ms = DELIVERY_TIMEOUT_MS;
   std::string  eth_client_id;
   std::string  sol_client_id;
   std::string  eth_opp_inbound_addr;  // hex address of OPPInbound on ETH
   std::string  eth_opp_addr;          // hex address of OPP on ETH
   std::string  sol_program_id;        // base58 address of opp-solana-outpost

   // Epoch state tracked across polls
   uint32_t                 current_epoch = 0;
   uint32_t                 last_delivered_epoch = 0;
   uint8_t                  my_group = 255;
   bool                     is_elected = false;
   std::vector<chain::name> current_group_members;
   std::vector<outpost_descriptor> outposts;

   // Plugin references
   chain_plugin*                     chain_plug = nullptr;
   cron_plugin*                      cron_plug  = nullptr;
   outpost_ethereum_client_plugin*   eth_plug   = nullptr;
   outpost_solana_client_plugin*     sol_plug   = nullptr;

   // Cron job handle
   cron_service::job_id_t            epoch_poll_job_id = 0;
   bool                              shutting_down = false;

   // -----------------------------------------------------------------------
   //  Table read helper
   // -----------------------------------------------------------------------

   read_only::get_table_rows_result read_table(const std::string& code,
                                                const std::string& scope,
                                                const std::string& table,
                                                uint32_t limit = 100) {
      auto ro = chain_plug->get_read_only_api(fc::microseconds(200000));
      read_only::get_table_rows_params p;
      p.json  = true;
      p.code  = chain::name(code);
      p.scope = scope;
      p.table = chain::name(table);
      p.limit = limit;
      auto deadline = fc::time_point::now() + fc::milliseconds(delivery_timeout_ms);
      auto result_fn = ro.get_table_rows(p, deadline);
      auto result = result_fn();
      if (auto* err = std::get_if<fc::exception_ptr>(&result)) {
         elog("batch_operator: table read failed {}::{} — {}", code, table, (*err)->to_string());
         return {};
      }
      return std::get<read_only::get_table_rows_result>(result);
   }

   // -----------------------------------------------------------------------
   //  Epoch state polling
   // -----------------------------------------------------------------------

   void poll_epoch_state() {
      if (shutting_down || !enabled) return;
      try {
         do_poll_epoch_state();
      } FC_LOG_AND_DROP();
   }

   /**
    * Parse epoch state into local fields.
    * Returns {true, epoch_index} on success, {false, 0} if state is unavailable.
    */
   std::pair<bool, uint32_t> parse_epoch_state() {
      auto state_rows = read_table("sysio.epoch", "sysio.epoch", "epochstate", 1);
      if (state_rows.rows.empty()) return {false, 0};

      auto obj = state_rows.rows[0].get_object();
      uint32_t epoch_index = static_cast<uint32_t>(obj["current_epoch_index"].as_uint64());
      uint8_t  cur_group   = static_cast<uint8_t>(obj["current_batch_op_group"].as_uint64());
      bool     paused      = obj["is_paused"].as_bool();

      if (paused) {
         if (is_elected) {
            ilog("batch_operator: epoch paused, suspending");
            is_elected = false;
         }
         return {false, 0};
      }

      // Determine group assignment
      my_group = 255;
      current_group_members.clear();
      auto groups_arr = obj["batch_op_groups"].get_array(); // copy, not reference

      for (uint8_t g = 0; g < groups_arr.size(); ++g) {
         auto grp = groups_arr[g].get_array(); // copy
         for (auto& member : grp) {
            if (chain::name(member.as_string()) == operator_account) {
               my_group = g;
            }
         }
         if (g == cur_group) {
            for (auto& member : grp) {
               current_group_members.push_back(chain::name(member.as_string()));
            }
         }
      }

      is_elected = (my_group == cur_group);

      // Ensure enough time remains in the epoch to complete delivery.
      // Skip this check if next_epoch_start is 0 (epoch not yet advanced).
      if (is_elected) {
         fc::time_point next_epoch_tp;
         fc::from_variant(obj["next_epoch_start"], next_epoch_tp);
         if (next_epoch_tp != fc::time_point()) {
            auto deadline = (fc::time_point::now() + fc::milliseconds(delivery_timeout_ms)).time_since_epoch().count() / 1000;
            auto next_epoch_ms = next_epoch_tp.time_since_epoch().count() / 1000; // convert to ms
            ilog("batch_operator: current_epoch={}, next_epoch_ms={}, deadline={}",
                 epoch_index, next_epoch_ms, deadline);
            if (deadline >= next_epoch_ms) {
               ilog("batch_operator: elected but insufficient time before next epoch (need {}ms)", delivery_timeout_ms);
               is_elected = false;
            }
         }
      }

      return {true, epoch_index};
   }

   void do_poll_epoch_state() {
      auto [ok, epoch_index] = parse_epoch_state();
      if (!ok) return;

      // ALL operators call advance regardless of election status.
      // The contract is idempotent: returns silently if epoch hasn't elapsed.
      // This ensures the chain can recover from downtime — any operator can
      // crank the epoch forward, not just the elected group.
      push_action("sysio.epoch", "advance", operator_account, fc::mutable_variant_object());

      // Re-read state after advance (epoch may have moved forward)
      auto [ok2, epoch_index2] = parse_epoch_state();
      if (ok2) epoch_index = epoch_index2;

      if (!is_elected) {
         if (epoch_index != current_epoch) {
            ilog("batch_operator: not elected for epoch {} (my_group={}, active_group={})",
                 epoch_index, my_group, (epoch_index % 3));
         }
         return;
      }

      // Already delivered successfully for this epoch — sleep until next poll
      if (epoch_index == last_delivered_epoch) return;

      ilog("batch_operator: ELECTED for epoch {} (group {}, {} members)",
           epoch_index, my_group, current_group_members.size());

      refresh_outposts();

      // Track the epoch for the cycle, but only mark as successfully delivered
      // after the cycle completes without error. If it fails, current_epoch
      // stays at the previous value so the next poll retries.
      auto prev_epoch = current_epoch;
      current_epoch = epoch_index;
      try {
         run_epoch_cycle(epoch_index);
         last_delivered_epoch = epoch_index;
      } catch (...) {
         current_epoch = prev_epoch;
         throw;
      }
   }

   // -----------------------------------------------------------------------
   //  Outpost registry
   // -----------------------------------------------------------------------

   void refresh_outposts() {
      outposts.clear();
      auto rows = read_table("sysio.epoch", "sysio.epoch", "outposts", 50);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         outpost_descriptor od;
         od.id         = obj["id"].as_uint64();
         od.chain_kind = obj["chain_kind"].as<ChainKind>();
         od.chain_id   = static_cast<uint32_t>(obj["chain_id"].as_uint64());
         outposts.push_back(std::move(od));
      }
      ilog("batch_operator: loaded {} outposts", outposts.size());
   }

   // -----------------------------------------------------------------------
   //  Epoch cycle orchestration
   // -----------------------------------------------------------------------

   void run_epoch_cycle(uint32_t cycle_epoch = 0) {
      if (shutting_down) return;
      ilog("batch_operator: === EPOCH CYCLE START (epoch {}) ===", current_epoch);

      try {
         // PHASE 1 — OUTBOUND (WIRE -> Outposts)
         // Outbound envelopes are built by sysio.epoch::advance() via inline
         // action to sysio.msgch::buildenv(). Just read and deliver.
         // Phase 1 failures must not block Phase 2 (inbound).
         for (auto& op : outposts) {
            op.reset_transient();
            read_outbound_envelope(op);
            try {
               deliver_to_outpost(op);
            } catch (const fc::exception& e) {
               wlog("batch_operator: outbound delivery failed for outpost {}: {}", op.id, e.to_detail_string());
            }
         }

         // PHASE 2 — INBOUND (Outposts -> WIRE)
         // ETH OPP epoch advances as a consequence of inbound consensus
         // on OPPInbound, NOT via a separate finalizeEpoch call.
         for (auto& op : outposts) {
            read_inbound_chain(op);
            deliver_to_depot(op);
         }

         // PHASE 3 — Consensus is now evaluated inline by the contract
         // via evalcons called from deliver. No plugin action needed.

         ilog("batch_operator: === EPOCH CYCLE COMPLETE (epoch {}) ===", current_epoch);
      } FC_LOG_AND_RETHROW();
   }

   // -----------------------------------------------------------------------
   //  Phase 1: Outbound (WIRE -> Outposts)
   // -----------------------------------------------------------------------

   void crank_depot_outbound(outpost_descriptor& op) {
      ilog("batch_operator: cranking depot for outpost {}", op.id);
      push_action("sysio.msgch", "crank", operator_account, fc::mutable_variant_object());
      push_action("sysio.msgch", "buildenv", operator_account,
                  fc::mutable_variant_object()
                     ("batch_op_name", operator_account.to_string())
                     ("outpost_id", op.id));
   }

   void read_outbound_envelope(outpost_descriptor& op) {
      auto rows = read_table("sysio.msgch", "sysio.msgch", "outenvelopes", 50);
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         uint64_t outpost_id  = obj["outpost_id"].as_uint64();
         uint32_t epoch_index = static_cast<uint32_t>(obj["epoch_index"].as_uint64());
         auto status = obj["status"].as<EnvelopeStatus>();

         if (outpost_id == op.id && epoch_index == current_epoch && status == ENVELOPE_STATUS_PENDING_DELIVERY) {
            auto raw_hex = obj["raw_envelope"].as_string();
            auto raw_bytes = fc::from_hex(raw_hex);
            op.outbound_raw_envelope.assign(raw_bytes.begin(), raw_bytes.end());
            op.outbound_envelope_hash_hex = obj["envelope_hash"].as_string();
            ilog("batch_operator: read outbound envelope for outpost {} ({} bytes)",
                 op.id, op.outbound_raw_envelope.size());
            return;
         }
      }
      ilog("batch_operator: no outbound envelope for outpost {} this epoch", op.id);
   }

   void deliver_to_outpost(outpost_descriptor& op) {
      if (op.outbound_raw_envelope.empty()) return;

      if (op.chain_kind == CHAIN_KIND_ETHEREUM) {
         deliver_to_eth_outpost(op);
      } else if (op.chain_kind == CHAIN_KIND_SOLANA) {
         deliver_to_sol_outpost(op);
      }
   }

   void deliver_to_eth_outpost(outpost_descriptor& op) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) {
         elog("batch_operator: ETH client '{}' not found", eth_client_id);
         return;
      }

      auto& abis = eth_plug->get_abi_files();

      // Call epochIn with raw protobuf wire-protocol bytes
      std::string envelope_hex = fc::to_hex(op.outbound_raw_envelope);
      auto epoch_in_abi = find_eth_abi(abis, "epochIn");
      if (epoch_in_abi) {
         auto tx = entry->client->create_default_tx(eth_opp_inbound_addr, *epoch_in_abi,
            {fc::variant(envelope_hex)});
         auto result = entry->client->execute_contract_tx_fn(tx, *epoch_in_abi);
         ilog("batch_operator: ETH epochIn result={}", result.as_string());
      }

      // Call messagesIn
      auto messages_in_abi = find_eth_abi(abis, "messagesIn");
      if (messages_in_abi) {
         auto tx = entry->client->create_default_tx(eth_opp_inbound_addr, *messages_in_abi,
            {fc::variant(envelope_hex)});
         auto result = entry->client->execute_contract_tx_fn(tx, *messages_in_abi);
         ilog("batch_operator: ETH messagesIn result={}", result.as_string());
      }
   }

   void deliver_to_sol_outpost(outpost_descriptor& op) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) {
         elog("batch_operator: SOL client '{}' not found", sol_client_id);
         return;
      }

      auto program_key = sol::solana_public_key::from_base58(sol_program_id);
      auto& idls = sol_plug->get_idl_files();

      // Find opp_solana_outpost IDL
      std::vector<sol::idl::program> program_idls;
      for (auto& [path, programs] : idls) {
         for (auto& p : programs) {
            if (p.name == "opp_solana_outpost") {
               program_idls.push_back(p);
               break;
            }
         }
      }

      auto program_client = std::make_shared<sol::solana_program_client>(
         entry->client, program_key, program_idls);

      // Call epoch_in instruction
      if (program_client->has_idl("epoch_in")) {
         auto& instr = program_client->get_idl("epoch_in");
         auto accounts = program_client->resolve_accounts(instr);
         program_client->execute_tx(instr, accounts,
            {fc::variant(fc::mutable_variant_object()
               ("epoch_index", current_epoch)
               ("envelope", op.outbound_raw_envelope))});
         ilog("batch_operator: SOL epoch_in sent for outpost {}", op.id);
      }

      // Call messages_in instruction
      if (program_client->has_idl("messages_in")) {
         auto& instr = program_client->get_idl("messages_in");
         auto accounts = program_client->resolve_accounts(instr);
         program_client->execute_tx(instr, accounts,
            {fc::variant(fc::mutable_variant_object()
               ("messages", op.outbound_raw_envelope))});
         ilog("batch_operator: SOL messages_in sent for outpost {}", op.id);
      }
   }

   // -----------------------------------------------------------------------
   //  Phase 2: Inbound (Outposts -> WIRE)
   // -----------------------------------------------------------------------

   void crank_outpost_epoch(outpost_descriptor& op) {
      if (op.chain_kind == CHAIN_KIND_ETHEREUM) {
         crank_eth_outpost(op);
      } else if (op.chain_kind == CHAIN_KIND_SOLANA) {
         crank_sol_outpost(op);
      }
   }

   void crank_eth_outpost(outpost_descriptor& op) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) return;

      auto& abis = eth_plug->get_abi_files();
      const eth::abi::contract* finalize_abi = find_eth_abi(abis, "finalizeEpoch");
      if (!finalize_abi) {
         elog("batch_operator: finalizeEpoch ABI not found for ETH");
         return;
      }

      try {
         auto tx = entry->client->create_default_tx(eth_opp_addr, *finalize_abi, {});
         auto result = entry->client->execute_contract_tx_fn(tx, *finalize_abi);
         ilog("batch_operator: ETH finalizeEpoch result={}", result.as_string());
      } catch (const fc::exception& e) {
         // OPP_PreviousEpochSent is expected when no pending epoch exists
         dlog("batch_operator: ETH finalizeEpoch skipped — {}", e.to_string());
      }
   }

   void crank_sol_outpost(outpost_descriptor& op) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) return;

      auto program_key = sol::solana_public_key::from_base58(sol_program_id);
      auto& idls = sol_plug->get_idl_files();

      std::vector<sol::idl::program> program_idls;
      for (auto& [path, programs] : idls) {
         for (auto& p : programs) {
            if (p.name == "opp_solana_outpost") {
               program_idls.push_back(p);
               break;
            }
         }
      }

      auto program_client = std::make_shared<sol::solana_program_client>(
         entry->client, program_key, program_idls);

      if (program_client->has_idl("finalize_epoch")) {
         auto& instr = program_client->get_idl("finalize_epoch");
         auto accounts = program_client->resolve_accounts(instr);
         program_client->execute_tx(instr, accounts,
            {fc::variant(fc::mutable_variant_object()("epoch_index", current_epoch))});
         ilog("batch_operator: SOL finalize_epoch sent for outpost {}", op.id);
      }
   }

   void read_inbound_chain(outpost_descriptor& op) {
      if (op.chain_kind == CHAIN_KIND_ETHEREUM) {
         read_eth_inbound(op);
      } else if (op.chain_kind == CHAIN_KIND_SOLANA) {
         read_sol_inbound(op);
      }
   }

   void read_eth_inbound(outpost_descriptor& op) {
      auto entry = eth_plug->get_client(eth_client_id);
      if (!entry || !entry->client) return;

      auto& abis = eth_plug->get_abi_files();
      // Collect all ABI contracts that have the OPP contract address
      std::vector<eth::abi::contract> opp_abis;
      std::string opp_addr;
      for (auto& [path, contracts] : abis) {
         for (auto& c : contracts) {
            if (!c.contract_address.empty() && opp_addr.empty()) {
               // Use the first ABI entry that has an OPP-matching address
               if (c.contract_address == eth_opp_addr) opp_addr = c.contract_address;
            }
            opp_abis.push_back(c);
         }
      }
      if (opp_addr.empty()) opp_addr = eth_opp_addr;

      // Count event ABIs to verify they're loaded
      uint32_t event_abi_count = 0;
      for (auto& a : opp_abis) {
         if (a.type == eth::abi::invoke_target_type::event) ++event_abi_count;
      }
      ilog("batch_operator: querying ETH events (addr={}, abis={}, events={}, opp_addr={})",
           opp_addr, opp_abis.size(), event_abi_count, eth_opp_addr);

      // Query OPPMessage logs via ethereum_client
      auto events = entry->client->get_events(
         opp_addr,
         {"OPPMessage"},
         opp_abis,
         eth::block_tag_t{std::string("0x0")},
         eth::block_tag_t{std::string(eth::block_tag_latest)});

      ilog("batch_operator: got {} events from ETH for outpost {}", events.size(), op.id);

      std::vector<char> combined_messages;
      uint32_t msg_count = 0;
      for (auto& evt : events) {
         if (evt.event_name == "OPPMessage" && !evt.data.empty()) {
            combined_messages.insert(combined_messages.end(),
               reinterpret_cast<const char*>(evt.data.data()),
               reinterpret_cast<const char*>(evt.data.data() + evt.data.size()));
            ++msg_count;
         }
      }

      if (msg_count > 0) {
         op.inbound_raw_messages = std::move(combined_messages);
         op.inbound_msg_count = msg_count;
         op.inbound_chain_hash_hex = fc::sha256::hash(
            op.inbound_raw_messages.data(), op.inbound_raw_messages.size()).str();
         ilog("batch_operator: read {} ETH inbound messages for outpost {}", msg_count, op.id);
      }
   }

   void read_sol_inbound(outpost_descriptor& op) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) return;

      // Query recent transaction signatures for the program
      auto sigs_result = entry->client->execute(
         "getSignaturesForAddress",
         fc::variants{
            fc::variant(sol_program_id),
            fc::variant(fc::mutable_variant_object()("limit", 20))
         });

      if (!sigs_result.is_array()) return;

      std::vector<char> combined_messages;
      uint32_t msg_count = 0;

      for (auto& sig_entry : sigs_result.get_array()) {
         if (!sig_entry.is_object()) continue;
         auto sig = sig_entry.get_object()["signature"].as_string();
         auto tx_result = entry->client->execute(
            "getTransaction",
            fc::variants{
               fc::variant(sig),
               fc::variant(fc::mutable_variant_object()
                  ("encoding", "json")
                  ("maxSupportedTransactionVersion", 0))
            });

         if (!tx_result.is_object()) continue;
         auto& meta = tx_result.get_object()["meta"];
         if (!meta.is_object()) continue;

         auto& log_messages = meta.get_object()["logMessages"];
         if (!log_messages.is_array()) continue;

         for (auto& log : log_messages.get_array()) {
            auto log_str = log.as_string();
            // sol_log_data emits base64 encoded program data prefixed with "Program data: "
            auto pos = log_str.find("Program data: ");
            if (pos != std::string::npos) {
               auto b64_data = log_str.substr(pos + 14);
               auto decoded = fc::base64_decode(b64_data);
               combined_messages.insert(combined_messages.end(), decoded.begin(), decoded.end());
               ++msg_count;
            }
         }
      }

      if (msg_count > 0) {
         op.inbound_raw_messages = std::move(combined_messages);
         op.inbound_msg_count = msg_count;
         op.inbound_chain_hash_hex = fc::sha256::hash(
            op.inbound_raw_messages.data(), op.inbound_raw_messages.size()).str();
         ilog("batch_operator: read {} SOL inbound messages for outpost {}", msg_count, op.id);
      }
   }

   void deliver_to_depot(outpost_descriptor& op) {
      if (op.inbound_raw_messages.empty()) {
         ilog("batch_operator: no inbound messages for outpost {}", op.id);
         return;
      }

      push_action("sysio.msgch", "deliver", operator_account,
                  fc::mutable_variant_object()
                     ("batch_op_name", operator_account.to_string())
                     ("outpost_id", op.id)
                     ("data", op.inbound_raw_messages));

      ilog("batch_operator: delivered {} inbound messages for outpost {}",
           op.inbound_msg_count, op.id);
   }

   // Phase 3 (Consensus) removed — now handled inline by sysio.msgch::evalcons

   // -----------------------------------------------------------------------
   //  Helpers
   // -----------------------------------------------------------------------

   static const eth::abi::contract* find_eth_abi(
      const std::vector<std::pair<std::filesystem::path, std::vector<eth::abi::contract>>>& abi_files,
      const std::string& name) {
      for (auto& [path, contracts] : abi_files) {
         for (auto& c : contracts) {
            if (c.name == name) return &c;
         }
      }
      return nullptr;
   }

   void push_action(const std::string& contract,
                    const std::string& action_name,
                    chain::name auth_account,
                    const fc::variant_object& data) {
      auto abi_max_time = fc::microseconds(delivery_timeout_ms * 1000);
      auto& chain = chain_plug->chain();

      // Resolve ABI and serialize action data
      auto resolver = make_resolver(chain, abi_max_time, throw_on_yield::no);
      auto abis_opt = resolver(chain::name(contract));
      if (!abis_opt) {
         elog("batch_operator: no ABI found for {}", contract);
         return;
      }

      auto action_type = abis_opt->get_action_type(chain::name(action_name));
      auto action_data = abis_opt->variant_to_binary(
         action_type, fc::variant(data),
         chain::abi_serializer::create_yield_function(abi_max_time));

      // Build the signed transaction
      chain::signed_transaction trx;
      trx.actions.emplace_back(
         std::vector<chain::permission_level>{{auth_account, chain::config::active_name}},
         chain::name(contract), chain::name(action_name), std::move(action_data));

      trx.set_reference_block(chain.head().id());
      trx.expiration = fc::time_point_sec(chain.head().block_time() + fc::seconds(30));

      // Sign with the operator's WIRE K1 key via signature_provider_manager
      auto& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      auto wire_providers = sig_plug.query_providers(
         std::nullopt, fc::crypto::chain_kind_wire, fc::crypto::chain_key_type_wire);
      if (wire_providers.empty()) {
         elog("batch_operator: no WIRE K1 signature provider available");
         return;
      }

      auto chain_id = chain.get_chain_id();
      auto digest = trx.sig_digest(chain_id, trx.context_free_data);
      trx.signatures.push_back(wire_providers.front()->sign(digest));

      // Pack and push
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
               elog("batch_operator: push {}::{} failed — {}", contract, action_name, (*err)->to_string());
            } else {
               ilog("batch_operator: pushed {}::{} ok", contract, action_name);
            }
            done.set_value();
         });

      if (future.wait_for(std::chrono::milliseconds(delivery_timeout_ms)) == std::future_status::timeout) {
         elog("batch_operator: push {}::{} timed out", contract, action_name);
      }
   }
};

// ---------------------------------------------------------------------------
//  Plugin lifecycle
// ---------------------------------------------------------------------------
batch_operator_plugin::batch_operator_plugin()
   : _impl(std::make_unique<impl>()) {}

batch_operator_plugin::~batch_operator_plugin() = default;

void batch_operator_plugin::set_program_options(options_description& cli,
                                                 options_description& cfg) {
   auto opts = cfg.add_options();
   opts("batch-operator-account", bpo::value<std::string>(),
        "WIRE account name for this batch operator");
   opts("batch-epoch-poll-ms", bpo::value<uint32_t>()->default_value(EPOCH_POLL_MS),
        "How often to check epoch state (ms)");
   opts("batch-delivery-timeout-ms", bpo::value<uint32_t>()->default_value(DELIVERY_TIMEOUT_MS),
        "Max time to wait for chain delivery confirmation (ms)");
   opts("batch-enabled", bpo::value<bool>()->default_value(false),
        "Enable batch operator functionality");
   opts("batch-eth-client-id", bpo::value<std::string>()->default_value("eth-default"),
        "Ethereum outpost client ID");
   opts("batch-sol-client-id", bpo::value<std::string>()->default_value("sol-default"),
        "Solana outpost client ID");
   opts("batch-eth-opp-inbound-addr", bpo::value<std::string>(),
        "OPPInbound contract address on Ethereum (hex)");
   opts("batch-eth-opp-addr", bpo::value<std::string>(),
        "OPP contract address on Ethereum (hex)");
   opts("batch-sol-program-id", bpo::value<std::string>(),
        "opp-solana-outpost program ID (base58)");
}

void batch_operator_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("batch-operator-account"))
      _impl->operator_account = chain::name(options["batch-operator-account"].as<std::string>());
   _impl->epoch_poll_ms       = options["batch-epoch-poll-ms"].as<uint32_t>();
   _impl->delivery_timeout_ms = options["batch-delivery-timeout-ms"].as<uint32_t>();
   _impl->enabled             = options["batch-enabled"].as<bool>();
   _impl->eth_client_id       = options["batch-eth-client-id"].as<std::string>();
   _impl->sol_client_id       = options["batch-sol-client-id"].as<std::string>();
   if (options.count("batch-eth-opp-inbound-addr"))
      _impl->eth_opp_inbound_addr = options["batch-eth-opp-inbound-addr"].as<std::string>();
   if (options.count("batch-eth-opp-addr"))
      _impl->eth_opp_addr = options["batch-eth-opp-addr"].as<std::string>();
   if (options.count("batch-sol-program-id"))
      _impl->sol_program_id = options["batch-sol-program-id"].as<std::string>();

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug  = &app().get_plugin<cron_plugin>();
   _impl->eth_plug   = &app().get_plugin<outpost_ethereum_client_plugin>();
   _impl->sol_plug   = &app().get_plugin<outpost_solana_client_plugin>();
}

void batch_operator_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("batch_operator_plugin: disabled, skipping startup");
      return;
   }

   ilog("batch_operator_plugin: starting for account {}", _impl->operator_account.to_string());

   // Schedule epoch polling via cron_plugin
   // milliseconds step: poll every epoch_poll_ms within each minute
   auto& cron = app().get_plugin<cron_plugin>();
   cron_service::job_schedule sched;
   sched.milliseconds = {cron_service::job_schedule::step_value{_impl->epoch_poll_ms}};

   cron_service::job_metadata_t meta;
   meta.label = "batch_operator_epoch_poll";
   meta.one_at_a_time = true;

   _impl->epoch_poll_job_id = cron.add_job(
      sched,
      [this]() { _impl->poll_epoch_state(); },
      meta
   );

   ilog("batch_operator_plugin: scheduled epoch poll (id={}, interval={}ms)",
        _impl->epoch_poll_job_id, _impl->epoch_poll_ms);
}

void batch_operator_plugin::plugin_shutdown() {
   _impl->shutting_down = true;

   if (_impl->epoch_poll_job_id != 0) {
      auto& cron = app().get_plugin<cron_plugin>();
      cron.cancel_job(_impl->epoch_poll_job_id);
   }

   ilog("batch_operator_plugin: shutdown complete");
}

} // namespace sysio
