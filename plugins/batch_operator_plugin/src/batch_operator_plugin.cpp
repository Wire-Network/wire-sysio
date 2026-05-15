#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>
#include <functional>
#include <optional>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/batch_operator_plugin/depot_ops.hpp>
#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>
#include <sysio/depot/opreg_status.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

namespace sysio {

using namespace chain_apis;
using namespace sysio::opp::types;
namespace eth = fc::network::ethereum;
namespace sol = fc::network::solana;

namespace {
   constexpr auto DELIVERY_TIMEOUT_MS  = 15000;
   constexpr auto EPOCH_POLL_MS        = 15000;
   constexpr auto EPOCH_EDGE_BUFFER_MS = 2500;

   /// Minimum private cron-service thread count even when 0 outposts are
   /// discovered at startup — keeps `epoch_tick` viable so a cold-sync node
   /// that finds outposts later still responds.
   constexpr std::size_t MIN_CRON_THREADS = 5;

   /// my_group sentinel meaning "we are not in any batch-op group".
   constexpr uint8_t GROUP_NONE = 255;

   /// Rolling window size for the outbound-envelope lookup. Covers ~4 epochs
   /// across up to 2 outposts (`DEPOT → OUTPOST` pair each epoch) plus a
   /// slack row, so `read_pending_outbound` always sees the current epoch
   /// without paying for a full-table scan as `outenvelopes` grows.
   constexpr uint32_t OUTBOUND_LOOKUP_WINDOW = 8;

   // ── WIRE contract identifiers (actions, tables, indexes, field names) ──
   // Centralised so a contract rename/refactor shows up as one search hit,
   // and so we can't accidentally query "envelopes" when the contract was
   // renamed to something else.

   namespace msgch {
      constexpr auto account             = "sysio.msgch";
      constexpr auto table_envelopes     = "envelopes";
      constexpr auto table_outenvelopes  = "outenvelopes";
      constexpr auto index_byoutepoch    = "byoutepoch";
      constexpr auto action_deliver      = "deliver";
      constexpr auto action_chkcons      = "chkcons";
      constexpr auto action_bootstrap    = "bootstrap";
      /// Field names on `envelope_entry` / `outbound_envelope` rows.
      namespace field {
         constexpr auto outpost_id     = "outpost_id";
         constexpr auto epoch_index    = "epoch_index";
         constexpr auto status         = "status";
         constexpr auto raw_envelope   = "raw_envelope";
         constexpr auto envelope_hash  = "envelope_hash";
         constexpr auto batch_op_name  = "batch_op_name";
         constexpr auto data           = "data";
      }
   }

   namespace opreg {
      constexpr auto account            = "sysio.opreg";
      constexpr auto table_operators    = "operators";
      /// Field names on `operator_entry` rows. Used by the awareness poll
      /// that gates the relay loop on a SLASHED / TERMINATED status flip.
      namespace field {
         constexpr auto status = "status";
      }
      // `OperatorStatus` enum spellings + the `is_active` decision live
      // in `sysio/depot/opreg_status.hpp` so underwriter_plugin can pull
      // the same source of truth without a cross-plugin dependency.
   }

   namespace epoch {
      constexpr auto account            = "sysio.epoch";
      constexpr auto table_epochstate   = "epochstate";
      constexpr auto table_outposts     = "outposts";
      /// Field names on `epoch_state` singleton.
      namespace field {
         constexpr auto current_epoch_index    = "current_epoch_index";
         constexpr auto current_batch_op_group = "current_batch_op_group";
         constexpr auto is_paused              = "is_paused";
         constexpr auto batch_op_groups        = "batch_op_groups";
         constexpr auto current_epoch_start    = "current_epoch_start";
         constexpr auto next_epoch_start       = "next_epoch_start";
      }
      /// Field names on `outpost_info` rows.
      namespace outpost_field {
         constexpr auto id         = "id";
         constexpr auto chain_kind = "chain_kind";
         constexpr auto chain_id   = "chain_id";
      }
   }
}

// ---------------------------------------------------------------------------
//  Outpost descriptor — one per registered outpost
// ---------------------------------------------------------------------------
struct outpost_descriptor {
   uint64_t  id          = 0;
   ChainKind chain_kind  = CHAIN_KIND_UNKNOWN;
   uint32_t  chain_id    = 0;
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
   uint8_t                  my_group = 255;
   bool                     is_elected = false;
   fc::time_point           epoch_start;
   fc::time_point           next_epoch_start;
   std::vector<chain::name> current_group_members;
   std::vector<outpost_descriptor> outposts;

   // Operator awareness — set by `poll_own_status()` from sysio.opreg::operators.
   // SLASHED / TERMINATED operators MUST stop relaying: continued deliveries
   // would be wasted CPU on the WIRE chain (msgch's deliver action will reject
   // since the operator no longer holds bond) AND a TERMINATED operator's
   // bond is already remitted, so any continued participation is misleading.
   bool                     is_active = true;

   // Plugin references
   chain_plugin*                     chain_plug = nullptr;
   cron_plugin*                      cron_plug  = nullptr;
   outpost_ethereum_client_plugin*   eth_plug   = nullptr;
   outpost_solana_client_plugin*     sol_plug   = nullptr;

   // Debugging signal — emitted when an OPP envelope is computed.
   // Only fires when num_slots() > 0 (i.e. external_debugging_plugin is connected).
   signal<void(const opp::debugging::DebugEnvelopeEvent&)> debug_envelope_signal;

   /// Private cron_service owned by this plugin. Sized from the outpost
   /// count at plugin_startup so per-outpost jobs run in parallel without
   /// fighting over the shared cron_plugin pool. Lifecycle tied to the
   /// plugin's startup/shutdown.
   sysio::services::cron_service_ptr cron_svc;
   std::vector<cron_service::job_id_t> cron_job_ids;
   std::atomic<bool>                 shutting_down{false};

   // -----------------------------------------------------------------------
   //  Chain-agnostic orchestration layer
   // -----------------------------------------------------------------------

   /// Concrete `sysio::depot_ops` backed by this impl. Lets an
   /// `outpost_opp_job` reach WIRE's read_table / push_action /
   /// debug_envelope_signal without knowing it's embedded in
   /// `batch_operator_plugin`.
   class depot_ops_impl_t : public sysio::depot_ops {
      impl& _impl;
   public:
      explicit depot_ops_impl_t(impl& i) : _impl(i) {}

      std::optional<sysio::outbound_envelope_record>
      read_pending_outbound(uint64_t outpost_id, uint32_t epoch_index) override {
         // Reverse-iterate the latest `OUTBOUND_LOOKUP_WINDOW` rows. The primary
         // key is auto-incrementing `id`, so reverse + small window gives the
         // most recent epochs' envelopes without scanning the whole table (which
         // grows unbounded until cleanup). Filtering by (outpost_id, epoch_index)
         // after the fact is O(window), not O(rows).
         sysio::chain_apis::read_only::get_table_rows_params p;
         p.code        = chain::name(msgch::account);
         p.scope       = msgch::account;
         p.table       = msgch::table_outenvelopes;
         p.reverse     = true;
         p.limit       = OUTBOUND_LOOKUP_WINDOW;
         p.values_only = true;
         auto rows = _impl.read_table(std::move(p));
         for (auto& row : rows.rows) {
            auto     obj         = row.get_object();
            uint64_t row_outpost = obj[msgch::field::outpost_id].as_uint64();
            auto     row_epoch   = static_cast<uint32_t>(obj[msgch::field::epoch_index].as_uint64());
            auto     status      = obj[msgch::field::status].as<EnvelopeStatus>();
            if (row_outpost != outpost_id || row_epoch != epoch_index
                || status != ENVELOPE_STATUS_PENDING_DELIVERY) continue;

            sysio::outbound_envelope_record rec;
            rec.outpost_id        = row_outpost;
            rec.epoch_index       = row_epoch;
            rec.envelope_hash_hex = obj[msgch::field::envelope_hash].as_string();
            auto raw_bytes        = fc::from_hex(obj[msgch::field::raw_envelope].as_string());
            rec.raw_envelope.assign(raw_bytes.begin(), raw_bytes.end());
            return rec;
         }
         return std::nullopt;
      }

      bool has_delivered_envelope(uint64_t outpost_id, uint32_t epoch_index) override {
         return _impl.has_delivered_envelope(outpost_id, epoch_index);
      }

      void deliver_to_depot(uint64_t outpost_id,
                            const std::vector<char>& raw_messages) override {
         _impl.push_action(
            msgch::account, msgch::action_deliver, _impl.operator_account,
            fc::mutable_variant_object()
               (msgch::field::batch_op_name, _impl.operator_account.to_string())
               (msgch::field::outpost_id,    outpost_id)
               (msgch::field::data,          raw_messages));
      }

      void emit_debug_envelope(sysio::opp::debugging::DebugEnvelopeEvent event) override {
         if (_impl.debug_envelope_signal.num_slots() == 0) return;
         // The orchestrating job emits with `name{}` for the batch_op_name
         // slot because it doesn't know the operator identity; fill it in
         // before publishing to slots.
         std::get<2>(event) = _impl.operator_account;
         _impl.debug_envelope_signal(event);
      }

      bool     within_epoch_window() const override { return _impl.within_epoch_window(); }
      bool     is_elected()         const override { return _impl.is_elected; }
      uint32_t current_epoch()      const override { return _impl.current_epoch; }
      bool     is_epoch_boundary_past() const override {
         // `next_epoch_start` is cached on `_impl` from
         // `sysio.epoch::epochstate`. Empty time_point = no cache yet
         // (pre-bootstrap) → conservative `false`. Otherwise compare
         // against wall clock.
         if (_impl.next_epoch_start == fc::time_point()) return false;
         return fc::time_point::now() >= _impl.next_epoch_start;
      }
   };

   std::unique_ptr<depot_ops_impl_t>                              depot_ops_backing{
      std::make_unique<depot_ops_impl_t>(*this)};
   std::map<uint64_t, std::shared_ptr<sysio::outpost_opp_job>>    opp_jobs;

   // -----------------------------------------------------------------------
   //  Table read helper
   // -----------------------------------------------------------------------

   /// Thin delegate to `chain_plugin::read_table_rows`, which posts the scan onto the app executor's read_only queue
   /// so chainbase iteration runs during the controller's read window instead of racing with block apply.
   sysio::chain_apis::read_only::get_table_rows_result
   read_table(sysio::chain_apis::read_only::get_table_rows_params p) {
      return chain_plug->read_table_rows(std::move(p), fc::milliseconds(delivery_timeout_ms),
                                         "batch_operator", shutting_down);
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
      // bad_cast. Wrap the composite key under the `byoutepoch` field to
      // satisfy the codec. Use `find=` rather than equal lower/upper bounds
      // because chain_plugin's secondary-index iteration breaks on
      // `sk >= ub_sv` — `find` mode appends a null byte to the upper bound
      // so the secondary key compares strictly less than it.
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(msgch::account);
      p.scope       = msgch::account;
      p.table       = msgch::table_envelopes;
      p.find        = std::format("{{\"{}\":{}}}", msgch::index_byoutepoch, key);
      p.index_name  = msgch::index_byoutepoch;
      p.values_only = true;
      p.filter      = [op_account](const fc::variant& row) {
         return chain::name(row[msgch::field::batch_op_name].as_string()) == op_account;
      };
      auto rows = read_table(std::move(p));
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
      // Awareness: refresh own status from the depot's bond ledger. SLASHED
      // / TERMINATED operators short-circuit the relay loop below.
      try {
         poll_own_status();
      } FC_LOG_AND_DROP();
      if (!is_active) return;

      // chkcons advances the epoch on consensus. Only the elected operator
      // should push it — the contract verifies authorization regardless,
      // but pushing from every batch op wastes trx slots.
      if (is_elected) {
         try {
            push_action(msgch::account, msgch::action_chkcons, operator_account,
                        fc::mutable_variant_object());
         } catch (const fc::exception& e) {
            dlog("batch_operator: chkcons: {}", e.to_string());
         }
      }
   }

   /**
    * Refresh `is_active` from `sysio.opreg::operators[operator_account]`.
    *
    * Reads the row's `status` field and sets `is_active` per:
    *   * OPERATOR_STATUS_ACTIVE      -> true  (relay loop runs normally)
    *   * OPERATOR_STATUS_SLASHED     -> false (halt; operator forfeit bond)
    *   * OPERATOR_STATUS_TERMINATED  -> false (halt; bond remitted, slot freed)
    *   * other / row missing         -> retain previous value (don't toggle on
    *                                    transient table-read failure)
    *
    * Logs once per status transition so cluster operators can see the flip
    * in the batch-op log without grep'ing every poll.
    */
   void poll_own_status() {
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(opreg::account);
      p.scope       = opreg::account;
      p.table       = opreg::table_operators;
      p.lower_bound = operator_account.to_string();
      p.upper_bound = operator_account.to_string();
      p.limit       = 1;
      p.values_only = true;
      auto rows = read_table(std::move(p));
      if (rows.rows.empty()) return;

      auto obj    = rows.rows[0].get_object();
      auto status = obj[opreg::field::status].as_string();

      bool was_active = is_active;
      is_active = sysio::depot::opreg_status::compute_is_active(status, was_active);

      if (was_active && !is_active) {
         elog("batch_operator: own status flipped to {} — halting relay loop", ("s", status));
      } else if (!was_active && is_active) {
         ilog("batch_operator: own status flipped to ACTIVE — resuming relay loop");
      }
   }

   /**
    * Parse epoch state into local fields.
    * Returns {true, epoch_index} on success, {false, 0} if state is unavailable.
    */
   std::pair<bool, uint32_t> parse_epoch_state() {
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(epoch::account);
      p.scope       = epoch::account;
      p.table       = epoch::table_epochstate;
      p.limit       = 1;
      p.values_only = true;
      auto state_rows = read_table(std::move(p));
      if (state_rows.rows.empty()) return {false, 0};

      auto obj = state_rows.rows[0].get_object();
      uint32_t epoch_index = static_cast<uint32_t>(obj[epoch::field::current_epoch_index].as_uint64());
      uint8_t  cur_group   = static_cast<uint8_t>(obj[epoch::field::current_batch_op_group].as_uint64());
      bool     paused      = obj[epoch::field::is_paused].as_bool();

      if (paused) {
         if (is_elected) {
            ilog("batch_operator: epoch paused, suspending");
            is_elected = false;
         }
         return {false, 0};
      }

      // Determine group assignment
      my_group = GROUP_NONE;
      current_group_members.clear();
      auto groups_arr = obj[epoch::field::batch_op_groups].get_array(); // copy, not reference

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
      fc::from_variant(obj[epoch::field::current_epoch_start], epoch_start);
      fc::from_variant(obj[epoch::field::next_epoch_start],    next_epoch_start);

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
            push_action(msgch::account, msgch::action_bootstrap, operator_account,
                        fc::mutable_variant_object());
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
         // Keep current_epoch fresh even when not elected: per-outpost jobs
         // consult it via depot_ops, and they bail on !is_elected anyway.
         current_epoch = epoch_index;
         return;
      }

      if (epoch_index != current_epoch) {
         ilog("batch_operator: ELECTED for epoch {} (group {}, {} members)",
              epoch_index, my_group, current_group_members.size());
      }
      current_epoch = epoch_index;
      // Refresh the outpost list so governance-added outposts become visible.
      // Job scheduling is fixed at plugin_startup; a genuine governance change
      // still requires a batch_operator restart to size the cron pool.
      refresh_outposts();
   }

   // -----------------------------------------------------------------------
   //  Outpost registry
   // -----------------------------------------------------------------------

   void refresh_outposts() {
      outposts.clear();
      // `all_rows` walks every row in one call. The outpost count should stay tiny (one per external chain), but
      // don't bake in a scan cap that would stall the batch operator if governance adds more than the default bound.
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(epoch::account);
      p.scope       = epoch::account;
      p.table       = epoch::table_outposts;
      p.all_rows    = true;
      p.values_only = true;
      auto rows = read_table(std::move(p));
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         outpost_descriptor od;
         od.id         = obj[epoch::outpost_field::id].as_uint64();
         od.chain_kind = obj[epoch::outpost_field::chain_kind].as<ChainKind>();
         od.chain_id   = static_cast<uint32_t>(obj[epoch::outpost_field::chain_id].as_uint64());
         outposts.push_back(std::move(od));
      }
      ilog("batch_operator: loaded {} outposts", outposts.size());
      build_opp_jobs();
   }

   /// Construct an `outpost_opp_job` per registered outpost using the
   /// chain-specific plugin factories. Idempotent: already-built jobs stay.
   /// Called from `refresh_outposts`; jobs only run if USE_OUTPOST_OPP_JOB
   /// is on (guarded at each use site).
   void build_opp_jobs() {
      if (!depot_ops_backing) return; // plugin not initialized yet
      for (auto& op : outposts) {
         if (opp_jobs.contains(op.id)) continue;

         std::shared_ptr<sysio::outpost_client> client;
         try {
            if (op.chain_kind == CHAIN_KIND_ETHEREUM) {
               client = eth_plug->create_outpost_client(eth_client_id, op.id, op.chain_id,
                                                     eth_opp_addr, eth_opp_inbound_addr);
            } else if (op.chain_kind == CHAIN_KIND_SOLANA) {
               client = sol_plug->create_outpost_client(sol_client_id, op.id, op.chain_id,
                                                     sol_program_id);
            } else {
               wlog("batch_operator: outpost {} has unsupported chain_kind, skipping job build",
                    op.id);
               continue;
            }
         } catch (const fc::exception& e) {
            wlog("batch_operator: failed to build outpost_client for outpost {}: {}",
                 op.id, e.to_string());
            continue;
         }

         auto job = std::make_shared<sysio::outpost_opp_job>(
            client, *depot_ops_backing, fc::milliseconds(delivery_timeout_ms));
         opp_jobs.emplace(op.id, std::move(job));
         ilog("batch_operator: built outpost_opp_job for {}", client->to_string());
      }
   }

   /// Returns true if we are within the safe operating window for this epoch.
   /// Blocks operations only in the narrow buffer zones at epoch boundaries.
   /// Once past next_epoch_start, operations are allowed (epoch is overdue).
   bool within_epoch_window() const {
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

   // Discover outposts once so we can size the private cron_service thread
   // pool to match. A governance change that adds or removes an outpost
   // requires a batch operator restart — the set is fixed here.
   try {
      _impl->refresh_outposts();
   } catch (const fc::exception& e) {
      wlog("batch_operator_plugin: initial outpost discovery failed: {}. "
           "Starting with 0 per-outpost jobs; restart batch_operator after "
           "the chain has caught up.", e.to_string());
   }

   // Size the pool: two jobs per outpost (outbound + inbound) plus one
   // epoch_tick. A minimum floor of 3 keeps the service viable even when
   // no outposts are known yet (so epoch_tick can still run).
   const std::size_t outpost_count   = _impl->opp_jobs.size();
   const std::size_t required_threads = outpost_count * 2 + 1;
   const std::size_t thread_count    = std::max(required_threads, MIN_CRON_THREADS);

   sysio::services::cron_service::options svc_opts;
   svc_opts.name        = "batch_operator";
   svc_opts.num_threads = thread_count;
   svc_opts.autostart   = true;

   _impl->cron_svc = sysio::services::cron_service::create(svc_opts);
   ilog("batch_operator_plugin: cron_service started with {} thread(s) ({} outpost(s) discovered)",
        thread_count, outpost_count);

   const auto poll_ms = _impl->epoch_poll_ms;

   // epoch_tick — refresh epoch state + election. Keeps `current_epoch`
   // and `within_epoch_window` accurate for every per-outpost job.
   {
      sysio::services::cron_service::job_schedule sched;
      sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{poll_ms}};
      sysio::services::cron_service::job_metadata_t meta;
      meta.label          = "batch_operator_epoch_tick";
      meta.one_at_a_time  = true;
      auto id = _impl->cron_svc->add(sched,
                                     [impl = _impl.get()]() { impl->poll_epoch_state(); },
                                     meta);
      _impl->cron_job_ids.push_back(id);
      ilog("batch_operator_plugin: scheduled {} (id={}, every {}ms)", meta.label, id, poll_ms);
   }

   // Per-outpost outbound + inbound. Each pair targets the same
   // outpost_opp_job instance; the job's internal mutex serializes them
   // when they run on different worker threads concurrently.
   for (auto& [outpost_id, job] : _impl->opp_jobs) {
      const auto identifier = job->client().to_string();
      {
         sysio::services::cron_service::job_schedule sched;
         sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{poll_ms}};
         sysio::services::cron_service::job_metadata_t meta;
         meta.label         = std::format("outpost_opp_outbound_{}", outpost_id);
         meta.one_at_a_time = true;
         auto id = _impl->cron_svc->add(sched,
                                        [job_wp = std::weak_ptr(job)]() {
                                           if (auto j = job_wp.lock()) j->run_outbound();
                                        },
                                        meta);
         _impl->cron_job_ids.push_back(id);
         ilog("batch_operator_plugin: scheduled {} for {} (id={}, every {}ms)",
              meta.label, identifier, id, poll_ms);
      }
      {
         sysio::services::cron_service::job_schedule sched;
         sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{poll_ms}};
         sysio::services::cron_service::job_metadata_t meta;
         meta.label         = std::format("outpost_opp_inbound_{}", outpost_id);
         meta.one_at_a_time = true;
         auto id = _impl->cron_svc->add(sched,
                                        [job_wp = std::weak_ptr(job)]() {
                                           if (auto j = job_wp.lock()) j->run_inbound();
                                        },
                                        meta);
         _impl->cron_job_ids.push_back(id);
         ilog("batch_operator_plugin: scheduled {} for {} (id={}, every {}ms)",
              meta.label, identifier, id, poll_ms);
      }
   }
}

void batch_operator_plugin::plugin_shutdown() {
   _impl->shutting_down = true;
   if (_impl->cron_svc) {
      _impl->cron_svc->cancel_all();
      _impl->cron_svc->stop();
      _impl->cron_svc.reset();
   }
   _impl->cron_job_ids.clear();
   ilog("batch_operator_plugin: shutdown complete");
}

} // namespace sysio
