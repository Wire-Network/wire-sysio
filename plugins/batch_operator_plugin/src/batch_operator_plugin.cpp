#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>
#include <functional>
#include <optional>

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
   constexpr auto EPOCH_EDGE_BUFFER_MS = 2500;
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
   fc::time_point           epoch_start;
   fc::time_point           next_epoch_start;
   std::vector<chain::name> current_group_members;
   std::vector<outpost_descriptor> outposts;

   // Plugin references
   chain_plugin*                     chain_plug = nullptr;
   cron_plugin*                      cron_plug  = nullptr;
   outpost_ethereum_client_plugin*   eth_plug   = nullptr;
   outpost_solana_client_plugin*     sol_plug   = nullptr;

   // Typed ETH contract clients (lazy-initialized)
   std::shared_ptr<opp_contract_client>         opp_client;
   std::shared_ptr<opp_inbound_contract_client> opp_inbound_client;

   // Typed SOL contract client (lazy-initialized)
   std::shared_ptr<opp_solana_outpost_client>   sol_outpost_client;

   // Debugging signal — emitted when an OPP envelope is computed.
   // Only fires when num_slots() > 0 (i.e. external_debugging_plugin is connected).
   signal<void(const opp::debugging::DebugEnvelopeEvent&)> debug_envelope_signal;

   // Cron job handles
   cron_service::job_id_t            epoch_poll_job_id = 0;
   cron_service::job_id_t            inbound_poll_job_id = 0;
   bool                              shutting_down = false;

   // -----------------------------------------------------------------------
   //  Table read helper
   // -----------------------------------------------------------------------

   struct read_table_options {
      std::optional<std::string>                              lower_bound;
      std::optional<std::string>                              upper_bound;
      std::optional<std::string>                              index_name;
      std::optional<std::function<bool(const fc::variant&)>>  filter;
      bool                                                    get_all = false;
   };

   read_only::get_table_rows_result read_table(const std::string& code,
                                                const std::string& scope,
                                                const std::string& table,
                                                uint32_t limit = 100,
                                                std::optional<read_table_options> opts = std::nullopt) {
      auto ro = chain_plug->get_read_only_api(fc::microseconds(200000));
      read_only::get_table_rows_params p;
      p.json  = true;
      p.code  = chain::name(code);
      p.scope = scope;
      p.table = table;
      p.limit = limit;

      if (opts) {
         if (opts->lower_bound)    p.lower_bound    = *opts->lower_bound;
         if (opts->upper_bound)    p.upper_bound    = *opts->upper_bound;
         if (opts->index_name)     p.index_name     = *opts->index_name;
      }

      auto deadline = fc::time_point::now() + fc::milliseconds(delivery_timeout_ms);

      read_only::get_table_rows_result combined;
      while (true) {
         auto result_fn = ro.get_table_rows(p, deadline);
         auto result = result_fn();
         if (auto* err = std::get_if<fc::exception_ptr>(&result)) {
            elog("batch_operator: table read failed {}::{} — {}", code, table, (*err)->to_string());
            return combined;
         }
         auto page = std::get<read_only::get_table_rows_result>(result);
         combined.rows.insert(combined.rows.end(),
            std::make_move_iterator(page.rows.begin()),
            std::make_move_iterator(page.rows.end()));

         if (!opts || !opts->get_all || !page.more || page.next_key.empty()) {
            combined.more = page.more;
            combined.next_key = page.next_key;
            break;
         }
         p.lower_bound = page.next_key;
      }

      // chain_plugin::get_table_rows wraps every row as {"key", "value", [payer]}.
      // Callers of this helper read contract fields directly, so unwrap to value.
      // variant::operator=(const variant&) calls clear() before reading the source,
      // so a direct `row = row.get_object()["value"]` would destroy the source ref
      // mid-assignment. Copy into an independent variant first, then move-assign.
      for (auto& row : combined.rows) {
         if (!row.is_object()) continue;
         const auto& row_obj = row.get_object();
         if (!row_obj.contains("value")) continue;
         fc::variant value{row_obj["value"]};
         row = std::move(value);
      }

      if (opts && opts->filter) {
         std::erase_if(combined.rows, [&](const fc::variant& row) {
            return !(*opts->filter)(row);
         });
      }
      return combined;
   }

   /// Check if this operator already delivered an envelope for the
   /// given outpost + epoch by querying msgch::envelopes via the
   /// byoutepoch secondary index.
   bool has_delivered_envelope(uint64_t outpost_id, uint32_t epoch_index) {
      uint64_t key = (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;
      auto op_account = operator_account;
      // chain_plugin::get_table_rows forwards secondary-index bounds through
      // be_key_codec::encode_key, which unconditionally calls get_object() on
      // the JSON-parsed bound. A bare scalar like "5" therefore throws
      // bad_cast. Wrap the composite key under the "byoutepoch" field to
      // satisfy the codec.
      std::string bound_json = "{\"byoutepoch\":" + std::to_string(key) + "}";
      auto rows = read_table("sysio.msgch", "sysio.msgch", "envelopes", 50,
         read_table_options{
            .lower_bound    = bound_json,
            .upper_bound    = bound_json,
            .index_name     = "byoutepoch",
            .filter         = [op_account](const fc::variant& row) {
               return chain::name(row["batch_op_name"].as_string()) == op_account;
            }
         });
      return !rows.rows.empty();
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

      // Parse epoch timing
      fc::from_variant(obj["current_epoch_start"], epoch_start);
      fc::from_variant(obj["next_epoch_start"], next_epoch_start);

      if (is_elected) {
         ilog("batch_operator: current_epoch={}", epoch_index);
      }

      return {true, epoch_index};
   }

   void do_poll_epoch_state() {
      auto [ok, epoch_index] = parse_epoch_state();
      if (!ok) return;

      // Genesis bootstrap: if epoch has never been advanced (epoch == 0),
      // trigger the first advance via msgch::bootstrap.
      // Post-genesis: advance is triggered by evalcons consensus, not by batch operators.
      if (epoch_index == 0) {
         try {
            push_action("sysio.msgch", "bootstrap", operator_account, fc::mutable_variant_object());
            auto [ok2, epoch_index2] = parse_epoch_state();
            if (ok2) epoch_index = epoch_index2;
         } catch (const fc::exception& e) {
            dlog("batch_operator: bootstrap skipped — {}", e.to_string());
         }
      }

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
      // after the cycle completes AND delivery was actually attempted.
      // If skipped (epoch boundary) or failed, the next poll retries.
      current_epoch = epoch_index;
      if (run_epoch_cycle(epoch_index)) {
         last_delivered_epoch = epoch_index;
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

   /// Returns true if delivery was actually attempted, false if skipped.
   bool run_epoch_cycle(uint32_t cycle_epoch = 0) {
      if (shutting_down) return false;
      if (!within_epoch_window()) {
         dlog("batch_operator: skipping epoch cycle — too close to epoch boundary");
         return false;
      }
      ilog("batch_operator: === EPOCH CYCLE START (epoch {}) ===", current_epoch);

      bool any_delivered = false;
      // OUTBOUND (WIRE -> Outposts)
      for (auto& op : outposts) {
         op.reset_transient();
         read_outbound_envelope(op);
         try {
            deliver_to_outpost(op);
            any_delivered = true;
         } catch (const std::exception& e) {
            wlog("batch_operator: outbound delivery failed for outpost {}: {}", op.id, e.what());
         } catch (...) {
            wlog("batch_operator: outbound delivery failed for outpost {} (unknown error)", op.id);
         }
      }
      ilog("batch_operator: === EPOCH CYCLE COMPLETE (epoch {}, delivered={}) ===", current_epoch, any_delivered);
      return any_delivered;
   }

   /// Returns true if we are within the safe operating window for this epoch.
   /// Blocks operations only in the narrow buffer zones at epoch boundaries.
   /// Once past next_epoch_start, operations are allowed (epoch is overdue).
   bool within_epoch_window() {
      auto now = fc::time_point::now();
      auto buffer = fc::milliseconds(EPOCH_EDGE_BUFFER_MS);
      if (now < epoch_start + buffer) return false;  // too close to epoch start
      // Only block in the narrow window BEFORE next_epoch_start.
      // Once past next_epoch_start, the epoch is overdue — allow operations.
      if (next_epoch_start != fc::time_point() &&
          now > next_epoch_start - buffer &&
          now < next_epoch_start) return false;
      return true;
   }

   /// Inbound poll — runs on a separate cron job from the epoch cycle.
   /// Reads OPPEnvelope events from outposts, delivers to sysio.msgch,
   /// and calls chkcons to advance the epoch once the time window passes.
   void poll_inbound() {
      if (shutting_down || outposts.empty()) return;

      // Re-check election status — epoch may have advanced since last outbound cycle
      auto [ok, epoch_index] = parse_epoch_state();
      if (!ok || !is_elected) return;

      if (within_epoch_window()) {
         for (auto& op : outposts) {
            try {
               read_inbound_chain(op);
               deliver_to_depot(op);
            } catch (const fc::exception& e) {
               wlog("batch_operator: inbound poll failed for outpost {}: {}", op.id, e.to_string());
            }
         }
      }

      // chkcons is safe to call regardless of window — it checks its own time gate.
      try {
         push_action("sysio.msgch", "chkcons", operator_account, fc::mutable_variant_object());
      } catch (const fc::exception& e) {
         dlog("batch_operator: chkcons: {}", e.to_string());
      }
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

            // Emit debugging signal (WIRE -> Outpost direction)
            if (debug_envelope_signal.num_slots() > 0) {
               auto ep_type = (op.chain_kind == CHAIN_KIND_ETHEREUM)
                  ? opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_ETHEREUM
                  : opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_DEPOT_OUTPOST_SOLANA;
               debug_envelope_signal(opp::debugging::DebugEnvelopeEvent{
                  current_epoch, ep_type, operator_account, op.outbound_raw_envelope});
            }

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

   void ensure_eth_clients() {
      if (opp_client && opp_inbound_client) return;
      auto entry = eth_plug->get_client(eth_client_id);
      FC_ASSERT(entry && entry->client, "ETH client '{}' not available", eth_client_id);
      auto& abis = eth_plug->get_abi_files();
      std::vector<eth::abi::contract> all_abis;
      for (auto& [path, contracts] : abis) {
         all_abis.insert(all_abis.end(), contracts.begin(), contracts.end());
      }
      opp_client = entry->client->get_contract<opp_contract_client>(eth_opp_addr, all_abis);
      opp_inbound_client = entry->client->get_contract<opp_inbound_contract_client>(eth_opp_inbound_addr, all_abis);
   }

   void deliver_to_eth_outpost(outpost_descriptor& op) {
      ensure_eth_clients();
      std::string envelope_hex = fc::to_hex(op.outbound_raw_envelope);
      auto result = opp_inbound_client->epoch_in(envelope_hex);
      ilog("batch_operator: ETH epochIn result={}", result.as_string());
   }

   /// Build the typed opp_solana_outpost_client on first use. Mirrors the
   /// ETH side's ensure_eth_clients — cached so subsequent calls reuse the
   /// same IDL resolution.
   void ensure_sol_clients() {
      if (sol_outpost_client) return;

      auto entry = sol_plug->get_client(sol_client_id);
      FC_ASSERT(entry && entry->client, "SOL client '{}' not available", sol_client_id);
      FC_ASSERT(!sol_program_id.empty(), "SOL program id not configured (batch-sol-program-id)");

      auto program_key = fc::crypto::solana::solana_public_key::from_base58_string(sol_program_id);

      // Collect every IDL the outpost_solana_client_plugin has loaded whose
      // program name matches the outpost program.
      std::vector<sol::idl::program> program_idls;
      for (auto& [path, programs] : sol_plug->get_idl_files()) {
         for (auto& p : programs) {
            if (p.name == OPP_SOLANA_OUTPOST_PROGRAM_NAME) {
               program_idls.push_back(p);
            }
         }
      }
      FC_ASSERT(!program_idls.empty(),
                "IDL for program '{}' not loaded — pass --solana-idl-file",
                OPP_SOLANA_OUTPOST_PROGRAM_NAME);

      sol_outpost_client = std::make_shared<opp_solana_outpost_client>(
         entry->client, program_key, program_idls);
   }

   void deliver_to_sol_outpost(outpost_descriptor& op) {
      ensure_sol_clients();

      std::vector<uint8_t> envelope_bytes(op.outbound_raw_envelope.begin(),
                                          op.outbound_raw_envelope.end());

      auto signature = sol_outpost_client->epoch_in(current_epoch, envelope_bytes);
      ilog("batch_operator: SOL epoch_in sent for outpost {} ({} bytes) sig={}",
           op.id, envelope_bytes.size(), signature);

      // Drain queued outbound attestations on the Solana side. ETH triggers
      // its equivalent (OPP.emitOutboundEnvelope) from inside OPPInbound on
      // consensus; on Solana the counterpart is a separate instruction so
      // the batch operator must invoke it explicitly after epoch_in.
      auto emit_signature = sol_outpost_client->emit_outbound_envelope(current_epoch);
      ilog("batch_operator: SOL emit_outbound_envelope sent for outpost {} sig={}",
           op.id, emit_signature);
   }

   void read_inbound_chain(outpost_descriptor& op) {
      if (op.chain_kind == CHAIN_KIND_ETHEREUM) {
         read_eth_inbound(op);
      } else if (op.chain_kind == CHAIN_KIND_SOLANA) {
         read_sol_inbound(op);
      }
   }

   void read_eth_inbound(outpost_descriptor& op) {
      ensure_eth_clients();

      auto events = opp_client->query_events(
         {"OPPEnvelope"},
         eth::block_tag_t{std::string(eth::block_tag_latest)},
         eth::block_tag_t{std::string(eth::block_tag_latest)});

      ilog("batch_operator: got {} events from ETH for outpost {}", events.size(), op.id);

      std::vector<char> combined_messages;
      uint32_t msg_count = 0;
      for (auto& evt : events) {
         if (evt.event_name == "OPPEnvelope" && !evt.data.empty()) {
            // evt.data is raw ABI-encoded event data; decode via the event ABI
            // to extract the protobuf bytes from the `bytes data` parameter
            auto decoded = evt.decode<fc::variant>();
            if (!decoded.has_value()) {
               elog("batch_operator: failed to ABI-decode OPPEnvelope event: {}", decoded.error().what());
               continue;
            }
            auto& v = decoded.value();
            std::string hex_data;
            if (v.is_object() && v.get_object().contains("data")) {
               hex_data = v["data"].as_string();
            } else if (v.is_string()) {
               hex_data = v.as_string();
            } else {
               elog("batch_operator: unexpected ABI-decoded variant type for OPPEnvelope");
               continue;
            }
            auto proto_bytes = fc::crypto::ethereum::hex_to_bytes(hex_data);

            /**
             * Validate the payload is a well-formed opp::Envelope and that its
             * epoch_index matches the current WIRE epoch. The ETH contract's
             * event log retains OPPEnvelope emissions from prior epochs, so a
             * block-range query for "latest" can surface stale envelopes that
             * would otherwise be delivered to sysio.msgch::deliver and rejected
             * with "envelope epoch_index mismatch". Mirrors the SOL-side guard
             * in read_sol_inbound.
             */
            sysio::opp::Envelope envelope;
            if (!envelope.ParseFromArray(proto_bytes.data(), static_cast<int>(proto_bytes.size()))) {
               wlog("batch_operator: [outpost {}] skipping non-Envelope ETH event payload ({} bytes)",
                    op.id, proto_bytes.size());
               continue;
            }

            if (static_cast<uint32_t>(envelope.epoch_index()) != current_epoch) {
               dlog("batch_operator: [outpost {}] skipping ETH envelope for epoch {} (current {})",
                    op.id, envelope.epoch_index(), current_epoch);
               continue;
            }

            combined_messages.insert(combined_messages.end(),
               reinterpret_cast<const char*>(proto_bytes.data()),
               reinterpret_cast<const char*>(proto_bytes.data() + proto_bytes.size()));
            ++msg_count;
         }
      }

      if (msg_count > 0) {
         op.inbound_raw_messages = std::move(combined_messages);
         op.inbound_msg_count = msg_count;
         op.inbound_chain_hash_hex = fc::sha256::hash(
            op.inbound_raw_messages.data(), op.inbound_raw_messages.size()).str();
         ilog("batch_operator: read {} ETH inbound messages for outpost {}", msg_count, op.id);

         // Emit debugging signal (Outpost ETH -> WIRE direction)
         if (debug_envelope_signal.num_slots() > 0) {
            debug_envelope_signal(opp::debugging::DebugEnvelopeEvent{
               current_epoch,
               opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT,
               operator_account,
               op.inbound_raw_messages});
         }
      }
   }

   void read_sol_inbound(outpost_descriptor& op) {
      auto entry = sol_plug->get_client(sol_client_id);
      if (!entry || !entry->client) return;
      if (sol_program_id.empty()) return;

      try {
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

            // Skip failed transactions — ETH inbound already filters on event
            // success via revert semantics; Solana reports failure out-of-band
            // in `meta.err`. A non-null value means the tx reverted and any
            // emitted "Program data:" is garbage from the partial execution.
            auto& err_field = meta.get_object()["err"];
            if (!err_field.is_null()) continue;

            auto& log_messages = meta.get_object()["logMessages"];
            if (!log_messages.is_array()) continue;

            // Take only the most recent "Program data:" line per transaction.
            // emit_outbound_envelope is a single instruction → at most one
            // sol_log_data payload per invocation. Concatenating every match
            // would mis-count envelopes on transactions that bundle multiple
            // program calls.
            std::optional<std::string> last_b64;
            for (auto& log : log_messages.get_array()) {
               auto log_str = log.as_string();
               auto pos = log_str.find("Program data: ");
               if (pos != std::string::npos) {
                  last_b64 = log_str.substr(pos + 14);
               }
            }
            if (!last_b64) continue;

            auto decoded = fc::base64_decode(*last_b64);
            // Validate as a protobuf Envelope before accepting. Bad bytes
            // would otherwise propagate into sysio.msgch::deliver and poison
            // the epoch's inbound chain.
            sysio::opp::Envelope envelope;
            if (!envelope.ParseFromArray(decoded.data(), decoded.size())) {
               wlog("batch_operator: [outpost {}] skipping non-Envelope program data ({} bytes)",
                    op.id, decoded.size());
               continue;
            }

            if (static_cast<uint32_t>(envelope.epoch_index()) != current_epoch) {
               dlog("batch_operator: [outpost {}] skipping SOL envelope for epoch {} (current {})",
                    op.id, envelope.epoch_index(), current_epoch);
               continue;
            }

            combined_messages.insert(combined_messages.end(), decoded.begin(), decoded.end());
            ++msg_count;
         }

         if (msg_count > 0) {
            op.inbound_raw_messages = std::move(combined_messages);
            op.inbound_msg_count = msg_count;
            op.inbound_chain_hash_hex = fc::sha256::hash(
               op.inbound_raw_messages.data(), op.inbound_raw_messages.size()).str();
            ilog("batch_operator: read {} SOL inbound envelopes for outpost {}", msg_count, op.id);

            // Emit debugging signal (Outpost SOL -> WIRE direction)
            if (debug_envelope_signal.num_slots() > 0) {
               debug_envelope_signal(opp::debugging::DebugEnvelopeEvent{
                  current_epoch,
                  opp::debugging::DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_SOLANA_DEPOT,
                  operator_account,
                  op.inbound_raw_messages});
            }
         }
      } catch (const fc::exception& e) {
         wlog("batch_operator: SOL inbound read failed for outpost {}: {}", op.id, e.to_string());
      } catch (const std::exception& e) {
         wlog("batch_operator: SOL inbound read failed for outpost {}: {}", op.id, e.what());
      }
   }

   void deliver_to_depot(outpost_descriptor& op) {
      if (op.inbound_raw_messages.empty()) {
         ilog("batch_operator: no inbound messages for outpost {}", op.id);
         return;
      }

      if (has_delivered_envelope(op.id, current_epoch)) {
         ilog("batch_operator: already delivered for outpost {} epoch {}, skipping", op.id, current_epoch);
         return;
      }

      // Decode and log envelope contents before delivering
      log_inbound_envelope(op);

      push_action("sysio.msgch", "deliver", operator_account,
                  fc::mutable_variant_object()
                     ("batch_op_name", operator_account.to_string())
                     ("outpost_id", op.id)
                     ("data", op.inbound_raw_messages));

      ilog("batch_operator: delivered {} inbound messages for outpost {}",
           op.inbound_msg_count, op.id);
   }

   void log_inbound_envelope(const outpost_descriptor& op) {
      try {
         auto& raw = op.inbound_raw_messages;
         sysio::opp::Envelope envelope;
         if (!envelope.ParseFromString(std::string(raw.begin(), raw.end()))) {
            wlog("batch_operator: [outpost {}] failed to decode inbound data as Envelope ({} bytes), "
                 "error={}, hex={}",
                 op.id, raw.size(),
                 envelope.InitializationErrorString(),
                 fc::to_hex(raw.data(), std::min(raw.size(), size_t(64))));
            return;
         }

         ilog("batch_operator: [outpost {}] inbound envelope: epoch_index={}, epoch_timestamp={}, "
              "messages={}, start_message_id={} bytes, end_message_id={} bytes",
              op.id,
              envelope.epoch_index(),
              envelope.epoch_timestamp(),
              envelope.messages_size(),
              envelope.start_message_id().size(),
              envelope.end_message_id().size());

         for (int m = 0; m < envelope.messages_size(); ++m) {
            auto& message = envelope.messages(m);
            auto& payload = message.payload();
            int att_count = payload.attestations_size();
            ilog("batch_operator: [outpost {}]   message[{}]: version={}, attestation_count={}",
                 op.id, m, payload.version(), att_count);

            for (int i = 0; i < att_count; ++i) {
               auto& entry = payload.attestations(i);
               auto type_val = entry.type();
               auto type_name = sysio::opp::types::AttestationType_Name(
                  static_cast<sysio::opp::types::AttestationType>(type_val));
               ilog("batch_operator: [outpost {}]     attestation[{}]: type={} ({}) data_size={}",
                    op.id, i,
                    type_name.empty() ? "UNKNOWN" : type_name,
                    type_val,
                    entry.data_size());
            }
         }
      } catch (const std::exception& e) {
         wlog("batch_operator: [outpost {}] envelope decode error: {}", op.id, e.what());
      }
   }

   // Phase 3 (Consensus) removed — now handled inline by sysio.msgch::evalcons

   // -----------------------------------------------------------------------
   //  Helpers
   // -----------------------------------------------------------------------

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

signal<void(const opp::debugging::DebugEnvelopeEvent&)>& batch_operator_plugin::debugging_opp_envelope() {
   return _impl->debug_envelope_signal;
}

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

   // Schedule inbound polling on a separate cron job.
   // Runs independently of the outbound epoch cycle so eth_getLogs
   // doesn't race against the same-block epochIn delivery.
   cron_service::job_schedule inbound_sched;
   inbound_sched.milliseconds = {cron_service::job_schedule::step_value{_impl->epoch_poll_ms}};

   cron_service::job_metadata_t inbound_meta;
   inbound_meta.label = "batch_operator_inbound_poll";
   inbound_meta.one_at_a_time = true;

   _impl->inbound_poll_job_id = cron.add_job(
      inbound_sched,
      [this]() { _impl->poll_inbound(); },
      inbound_meta
   );

   ilog("batch_operator_plugin: scheduled inbound poll (id={}, interval={}ms)",
        _impl->inbound_poll_job_id, _impl->epoch_poll_ms);
}

void batch_operator_plugin::plugin_shutdown() {
   _impl->shutting_down = true;

   auto& cron = app().get_plugin<cron_plugin>();
   if (_impl->epoch_poll_job_id != 0) {
      cron.cancel_job(_impl->epoch_poll_job_id);
   }
   if (_impl->inbound_poll_job_id != 0) {
      cron.cancel_job(_impl->inbound_poll_job_id);
   }

   ilog("batch_operator_plugin: shutdown complete");
}

} // namespace sysio
