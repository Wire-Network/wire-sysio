#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/int128.hpp>
#include <fc/io/json.hpp>
#include <fc/slug_name.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>
#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string_view>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/batch_operator_plugin/depot_ops.hpp>
#include <sysio/batch_operator_plugin/outpost_binding.hpp>
#include <sysio/batch_operator_plugin/outpost_epoch_lookup.hpp>
#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>
#include <sysio/depot/opreg_status.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/plugin_interface.hpp>
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
   /// Inbound plus outbound cron entries for each active outpost.
   constexpr std::size_t OPP_CRON_JOBS_PER_OUTPOST = 2;
   /// The plugin-wide epoch polling cron entry.
   constexpr std::size_t EPOCH_TICK_CRON_JOBS = 1;
   /// Exact secondary-index lookups should return at most the matching row.
   constexpr uint32_t EXACT_LOOKUP_LIMIT = 1;

   /// my_group sentinel meaning "we are not in any batch-op group".
   constexpr uint8_t GROUP_NONE = 255;

   // ── WIRE contract identifiers (actions, tables, indexes, field names) ──
   // Centralised so a contract rename/refactor shows up as one search hit,
   // and so we can't accidentally query "envelopes" when the contract was
   // renamed to something else.

   namespace msgch {
      constexpr auto account             = "sysio.msgch";
      constexpr auto table_envelopes     = "envelopes";
      constexpr auto table_outenvelopes  = "outenvelopes";
      constexpr auto action_deliver      = "deliver";
      constexpr auto action_chkcons      = "chkcons";
      constexpr auto action_bootstrap    = "bootstrap";
      /// Field names on `envelope_entry` / `outbound_envelope` rows.
      namespace field {
         constexpr auto chain_code     = "chain_code";
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
      /// Field names on `epoch_state` singleton.
      namespace field {
         constexpr auto current_epoch_index    = "current_epoch_index";
         constexpr auto current_batch_op_group = "current_batch_op_group";
         constexpr auto is_paused              = "is_paused";
         constexpr auto batch_op_groups        = "batch_op_groups";
         constexpr auto current_epoch_start    = "current_epoch_start";
         constexpr auto next_epoch_start       = "next_epoch_start";
      }
   }

   /// v6: chain registry was split out of `sysio.epoch` onto its own
   /// `sysio.chains` contract. The `outposts` table was replaced by the
   /// `chains` KV table, keyed by slug_name (uint64 packed).
   namespace chains {
      constexpr auto account       = "sysio.chains";
      constexpr auto table_chains  = "chains";
      /// Field names on `Chain` row (proto-mirror schema).
      namespace field {
         constexpr auto code              = "code";              // {value: uint64} slug_name
         constexpr auto kind              = "kind";              // ChainKind enum (string spelling)
         constexpr auto external_chain_id = "external_chain_id"; // uint32
         constexpr auto is_depot          = "is_depot";          // bool — the single WIRE-self row
         constexpr auto active            = "active";            // bool
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
   // SVM RPC client id (one Solana cluster serves all SVM programs). The EVM
   // client is selected per outpost by external_chain_id, and each outpost's
   // OPP contract addresses come from its `--batch-outpost` binding — see
   // build_opp_jobs.
   std::string  sol_client_id;
   /// Remote OPP contract bindings from `--batch-outpost`, keyed by packed
   /// chain code (matches `outpost_descriptor::id`).
   std::map<uint64_t, batch_operator_detail::outpost_binding> outpost_bindings;

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
   /// count at plugin_startup and accepts dynamic per-outpost jobs as
   /// `refresh_outposts` observes governance changes. Lifecycle tied to the
   /// plugin's startup/shutdown.
   sysio::services::cron_service_ptr cron_svc;
   std::vector<cron_service::job_id_t> cron_job_ids;
   /// Cron job IDs for a scheduled outpost relay pair.
   struct scheduled_opp_job_ids {
      cron_service::job_id_t outbound = 0;
      cron_service::job_id_t inbound  = 0;
   };
   std::map<uint64_t, scheduled_opp_job_ids> scheduled_opp_jobs;
   std::atomic<bool>                 shutting_down{false};

   /// Sync gate: `channels::irreversible_block` subscription that arms
   /// {@link run_deferred_startup_or_quit} once `controller::is_synced()`
   /// holds — a LIB advance is the only event that can turn the predicate
   /// true. Unsubscribed after arming; otherwise the handle's destructor
   /// releases it (no shutdown unsubscribe needed: posted channel deliveries
   /// are drained without executing after quit, and the slot checks
   /// `shutting_down` first).
   chain::plugin_interface::channels::irreversible_block::channel_type::handle sync_gate_subscription;

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
      read_pending_outbound(uint64_t chain_code, uint32_t epoch_index) override {
         // Exact-match the `(chain_code, epoch_index)` secondary index. A fixed
         // latest-N primary-key scan can hide an active outpost behind unrelated
         // rows when the deployment has many outposts or bursty outbound emits.
         sysio::chain_apis::read_only::get_table_rows_params p;
         p.code        = chain::name(msgch::account);
         p.scope       = msgch::account;
         p.table       = msgch::table_outenvelopes;
         p.find        = batch_operator_detail::byoutepoch_find_bound(chain_code, epoch_index);
         p.index_name  = batch_operator_detail::byoutepoch_index_name;
         p.limit       = EXACT_LOOKUP_LIMIT;
         p.values_only = true;
         auto rows = _impl.read_table(std::move(p));
         for (auto& row : rows.rows) {
            auto     obj         = row.get_object();
            uint64_t row_outpost = obj[msgch::field::chain_code].as_uint64();
            auto     row_epoch   = static_cast<uint32_t>(obj[msgch::field::epoch_index].as_uint64());
            auto     status      = obj[msgch::field::status].as<EnvelopeStatus>();
            if (row_outpost != chain_code || row_epoch != epoch_index
                || status != ENVELOPE_STATUS_PENDING_DELIVERY) continue;

            sysio::outbound_envelope_record rec;
            rec.chain_code        = row_outpost;
            rec.epoch_index       = row_epoch;
            rec.envelope_hash_hex = obj[msgch::field::envelope_hash].as_string();
            auto raw_bytes        = fc::from_hex(obj[msgch::field::raw_envelope].as_string());
            rec.raw_envelope.assign(raw_bytes.begin(), raw_bytes.end());
            return rec;
         }
         return std::nullopt;
      }

      bool has_delivered_envelope(uint64_t chain_code, uint32_t epoch_index) override {
         return _impl.has_delivered_envelope(chain_code, epoch_index);
      }

      void deliver_to_depot(uint64_t chain_code,
                            const std::vector<char>& raw_messages) override {
         _impl.push_action(
            msgch::account, msgch::action_deliver, _impl.operator_account,
            fc::mutable_variant_object()
               (msgch::field::batch_op_name, _impl.operator_account.to_string())
               (msgch::field::chain_code,    chain_code)
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
   bool has_delivered_envelope(uint64_t chain_code, uint32_t epoch_index) {
      // Canonical (outpost, epoch) packing per sysio.opp.common/opp_keys.hpp —
      // chain_code (a slug_name, up to 48 bits) occupies bits 32-79, epoch bits 0-31.
      // The byoutepoch index is uint128; serialize the bound as a decimal string so
      // the JSON round-trip stays lossless past 2^64.
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
      p.find        = batch_operator_detail::byoutepoch_find_bound(chain_code, epoch_index);
      p.index_name  = batch_operator_detail::byoutepoch_index_name;
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
      // v6: `sysio.opreg::operators` is a KV table whose PK is a struct
      // `{account: name}`; the chain_plugin's `lower_bound` / `upper_bound`
      // expects JSON-shaped key bounds for KV tables, not the bare name
      // string the v5 multi_index path accepted. Easiest robust fix: scan
      // all rows and filter in-plugin — the operator count stays bounded
      // by `op_config.max_available_*` (capped at ~100 for the lifetime
      // of this plugin), so a linear scan once per `poll_own_status`
      // period is cheap.
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(opreg::account);
      p.scope       = opreg::account;
      p.table       = opreg::table_operators;
      p.all_rows    = true;
      p.values_only = true;
      auto rows = read_table(std::move(p));
      if (rows.rows.empty()) return;

      auto self = operator_account.to_string();
      fc::variant_object obj;
      bool found = false;
      for (auto& r : rows.rows) {
         auto row_obj = r.get_object();
         auto acct_it = row_obj.find("account");
         if (acct_it != row_obj.end() && acct_it->value().as_string() == self) {
            obj = row_obj;
            found = true;
            break;
         }
      }
      if (!found) return;
      auto status = obj[opreg::field::status].as_string();

      bool was_active = is_active;
      is_active = sysio::depot::opreg_status::compute_is_active(status, was_active);

      if (was_active && !is_active) {
         elog("batch_operator: own status flipped to {} — halting relay loop", status);
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
      // `build_opp_jobs` and `schedule_opp_jobs` are idempotent, so newly
      // active outposts start relaying without a batch-operator restart.
      refresh_outposts();
   }

   // -----------------------------------------------------------------------
   //  Outpost registry
   // -----------------------------------------------------------------------

   void refresh_outposts() {
      // v6: chain registry lives on `sysio.chains::chains` (replaces the
      // removed `sysio.epoch::outposts` table). Each row carries the
      // chain's slug_name + kind + external_chain_id + is_depot + active.
      // Outposts are the non-depot, active rows; the single is_depot=true
      // row is the WIRE chain itself and is skipped.
      //
      // Startup race: in a multi-node cluster the batch-op node replays
      // blocks from the producer asynchronously. There's a brief window
      // where `sysio.chains` exists on the producer but the local node
      // hasn't replayed far enough to see it — `read_table` throws
      // `Account Query Exception (3060002)` / `Contract Table Query
      // Exception (3060003)` during that window. Catch + return; the
      // outer cron tick re-enters every poll interval and self-heals.
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(chains::account);
      p.scope       = chains::account;
      p.table       = chains::table_chains;
      p.all_rows    = true;
      p.values_only = true;
      sysio::chain_apis::read_only::get_table_rows_result rows;
      try {
         rows = read_table(std::move(p));
      } catch (const fc::exception& e) {
         // Transient (cold-start replay, account not yet visible).
         // Don't clear `outposts` — keep the last-known set so jobs
         // built from earlier reads continue to work; the next tick
         // will refresh once the table is reachable.
         static fc::time_point last_warn;
         auto now = fc::time_point::now();
         if (now > last_warn + fc::seconds(30)) {
            wlog("batch_operator: refresh_outposts deferred — sysio.chains read failed: {}",
                 ("e", e.top_message()));
            last_warn = now;
         }
         return;
      }
      outposts.clear();
      for (auto& row : rows.rows) {
         auto obj = row.get_object();
         // The `code` field on the Chain proto is a `slug_name` struct
         // wrapping a uint64 (see slug_name.hpp). The JSON view exposes
         // it as `{value: <uint64>}`. Unpack defensively.
         uint64_t code_val = 0;
         if (auto code_obj = obj.find(chains::field::code); code_obj != obj.end()) {
            if (code_obj->value().is_object()) {
               code_val = code_obj->value().get_object()["value"].as_uint64();
            } else {
               code_val = code_obj->value().as_uint64();
            }
         }
         bool is_depot = obj[chains::field::is_depot].as_bool();
         bool active   = obj[chains::field::active].as_bool();
         if (is_depot || !active) continue;
         outpost_descriptor od;
         od.id         = code_val;  // slug_name uint64 doubles as outpost id
         od.chain_kind = obj[chains::field::kind].as<ChainKind>();
         od.chain_id   = static_cast<uint32_t>(obj[chains::field::external_chain_id].as_uint64());
         outposts.push_back(std::move(od));
      }
      ilog("batch_operator: loaded {} outposts (v6 sysio.chains)", outposts.size());
      prune_inactive_opp_jobs();
      build_opp_jobs();
      schedule_opp_jobs();
   }

   /// Construct an `outpost_opp_job` per registered outpost using the
   /// chain-specific plugin factories. Idempotent: already-built jobs stay.
   /// Called from `refresh_outposts`; scheduling is handled separately so
   /// startup-created jobs and governance-added jobs share the same path.
   void build_opp_jobs() {
      if (!depot_ops_backing) return; // plugin not initialized yet
      for (auto& op : outposts) {
         if (opp_jobs.contains(op.id)) continue;

         std::shared_ptr<sysio::outpost_client> client;
         try {
            if (op.chain_kind == CHAIN_KIND_EVM) {
               // Bind this exact outpost to its own remote identity: the RPC
               // client is the one whose configured chain id matches this
               // row's external_chain_id (never a shared per-kind tuple), and
               // the OPP / OPPInbound contract addresses come from the row's
               // `--batch-outpost` binding. Anything missing => skip the job
               // (fail closed) so an outpost is never relayed through another
               // chain's endpoint.
               auto entry = eth_plug->get_client_by_chain_id(op.chain_id);
               if (!entry) {
                  wlog("batch_operator: no unique --outpost-ethereum-client for chain_id {} "
                       "(outpost {}); skipping until one is configured",
                       op.chain_id, fc::slug_name{op.id}.to_string());
                  continue;
               }
               auto bound = outpost_bindings.find(op.id);
               if (bound == outpost_bindings.end() || bound->second.opp_inbound_addr.empty()) {
                  wlog("batch_operator: outpost {} (EVM) has no {}=<CODE>,<OPP>,<OPPInbound> "
                       "binding; skipping until one is configured",
                       fc::slug_name{op.id}.to_string(), batch_operator_detail::BATCH_OUTPOST_OPTION);
                  continue;
               }
               client = eth_plug->create_outpost_client(entry->id, op.id, op.chain_id,
                                                     bound->second.opp_addr,
                                                     bound->second.opp_inbound_addr);
            } else if (op.chain_kind == CHAIN_KIND_SVM) {
               // SVM: one Solana cluster (RPC client) serves every program, so
               // the shared sol client is correct; the per-outpost identity is
               // the program id from the row's `--batch-outpost` binding.
               auto bound = outpost_bindings.find(op.id);
               if (bound == outpost_bindings.end()) {
                  wlog("batch_operator: outpost {} (SVM) has no {}=<CODE>,<program_id> binding; "
                       "skipping until one is configured",
                       fc::slug_name{op.id}.to_string(), batch_operator_detail::BATCH_OUTPOST_OPTION);
                  continue;
               }
               if (!bound->second.opp_inbound_addr.empty()) {
                  wlog("batch_operator: outpost {} (SVM) binding must not carry an inbound "
                       "address (the single program serves both directions); skipping",
                       fc::slug_name{op.id}.to_string());
                  continue;
               }
               client = sol_plug->create_outpost_client(sol_client_id, op.id, op.chain_id,
                                                     bound->second.opp_addr);
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

   /// Returns true when a chain code is present in the latest active outpost list.
   bool is_current_outpost(uint64_t chain_code) const {
      for (const auto& outpost : outposts) {
         if (outpost.id == chain_code) return true;
      }
      return false;
   }

   /// Forget a cron job ID after the job has been cancelled individually.
   void forget_cron_job_id(cron_service::job_id_t id) {
      cron_job_ids.erase(std::remove(cron_job_ids.begin(), cron_job_ids.end(), id),
                         cron_job_ids.end());
   }

   /// Cancel the inbound/outbound cron entries for one outpost, if they exist.
   void cancel_scheduled_opp_job(uint64_t chain_code) {
      auto sched_it = scheduled_opp_jobs.find(chain_code);
      if (sched_it == scheduled_opp_jobs.end()) return;
      if (cron_svc) {
         cron_svc->cancel(sched_it->second.outbound);
         cron_svc->cancel(sched_it->second.inbound);
      }
      forget_cron_job_id(sched_it->second.outbound);
      forget_cron_job_id(sched_it->second.inbound);
      scheduled_opp_jobs.erase(sched_it);
   }

   /// Remove relay jobs for outposts that are no longer active on `sysio.chains`.
   void prune_inactive_opp_jobs() {
      for (auto it = opp_jobs.begin(); it != opp_jobs.end(); ) {
         if (is_current_outpost(it->first)) {
            ++it;
            continue;
         }
         cancel_scheduled_opp_job(it->first);
         ilog("batch_operator: removed outpost_opp_job for inactive outpost {}", it->first);
         it = opp_jobs.erase(it);
      }
   }

   /// Schedule one cron direction for an outpost relay job.
   cron_service::job_id_t schedule_opp_job_direction(uint64_t chain_code,
                                                     const std::shared_ptr<sysio::outpost_opp_job>& job,
                                                     std::string_view direction,
                                                     void (sysio::outpost_opp_job::*runner)()) {
      sysio::services::cron_service::job_schedule sched;
      sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{epoch_poll_ms}};
      sysio::services::cron_service::job_metadata_t meta;
      meta.label         = std::format("outpost_opp_{}_{}", direction, chain_code);
      meta.one_at_a_time = true;
      auto id = cron_svc->add(sched,
                              [job_wp = std::weak_ptr<sysio::outpost_opp_job>(job), runner]() {
                                 if (auto j = job_wp.lock()) ((*j).*runner)();
                              },
                              meta);
      cron_job_ids.push_back(id);
      ilog("batch_operator_plugin: scheduled {} for {} (id={}, every {}ms)",
           meta.label, job->client().to_string(), id, epoch_poll_ms);
      return id;
   }

   /// Schedule inbound and outbound cron jobs for a single active outpost.
   void schedule_opp_job(uint64_t chain_code, const std::shared_ptr<sysio::outpost_opp_job>& job) {
      if (!cron_svc || shutting_down || scheduled_opp_jobs.contains(chain_code)) return;
      auto outbound_id = schedule_opp_job_direction(chain_code, job, "outbound",
                                                    &sysio::outpost_opp_job::run_outbound);
      auto inbound_id = schedule_opp_job_direction(chain_code, job, "inbound",
                                                   &sysio::outpost_opp_job::run_inbound);
      scheduled_opp_jobs.emplace(chain_code, scheduled_opp_job_ids{
         .outbound = outbound_id,
         .inbound  = inbound_id,
      });
   }

   /// Schedule every built active outpost job that does not already have cron entries.
   void schedule_opp_jobs() {
      for (const auto& [chain_code, job] : opp_jobs) {
         schedule_opp_job(chain_code, job);
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

   // -----------------------------------------------------------------------
   //  Sync-gated startup
   // -----------------------------------------------------------------------

   /// The startup body deferred behind the sync gate: outpost
   /// discovery → private cron_service creation (sized from the discovered
   /// outposts) → epoch_tick scheduling → per-outpost relay jobs. Runs on the
   /// main thread from {@link run_deferred_startup_or_quit} once the node is
   /// synced. Deferral exists because `refresh_outposts` reads `sysio.chains`
   /// LOCALLY: on a cold-booting operator node still replaying toward the
   /// deploy blocks the read throws Account/Contract Query Exceptions
   /// (3060002/3060003) — spurious boot-window errors the gate removes.
   void run_deferred_startup() {
      if (shutting_down) {
         return;
      }

      // Discover outposts before the private cron_service starts. Later refresh
      // ticks add/remove per-outpost cron jobs as the active chain set changes.
      try {
         refresh_outposts();
      } catch (const fc::exception& e) {
         wlog("batch_operator_plugin: initial outpost discovery failed: {}. "
              "Starting with 0 per-outpost jobs; refresh ticks will retry after "
              "the chain has caught up.", e.to_string());
      }

      // Size the pool for startup outposts. Later dynamic outposts are added to
      // the same queued cron service; the minimum keeps epoch_tick viable even
      // when no outposts are known yet.
      const std::size_t outpost_count   = opp_jobs.size();
      const std::size_t required_threads = outpost_count * OPP_CRON_JOBS_PER_OUTPOST + EPOCH_TICK_CRON_JOBS;
      const std::size_t thread_count    = std::max(required_threads, MIN_CRON_THREADS);

      sysio::services::cron_service::options svc_opts;
      svc_opts.name        = "batch_operator";
      svc_opts.num_threads = thread_count;
      svc_opts.autostart   = true;

      cron_svc = sysio::services::cron_service::create(svc_opts);
      ilog("batch_operator_plugin: cron_service started with {} thread(s) ({} outpost(s) discovered)",
           thread_count, outpost_count);

      const auto poll_ms = epoch_poll_ms;

      // epoch_tick — refresh epoch state + election. Keeps `current_epoch`
      // and `within_epoch_window` accurate for every per-outpost job.
      {
         sysio::services::cron_service::job_schedule sched;
         sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{poll_ms}};
         sysio::services::cron_service::job_metadata_t meta;
         meta.label          = "batch_operator_epoch_tick";
         meta.one_at_a_time  = true;
         auto id = cron_svc->add(sched,
                                 [this]() { poll_epoch_state(); },
                                 meta);
         cron_job_ids.push_back(id);
         ilog("batch_operator_plugin: scheduled {} (id={}, every {}ms)", meta.label, id, poll_ms);
      }

      schedule_opp_jobs();
   }

   /// {@link run_deferred_startup} plus the uniform fail-fast policy: the
   /// sync-gate callback is a posted channel delivery, so an escaping
   /// exception would unwind the application executor mid-task with no
   /// diagnosable trace of WHAT failed. Contain it long enough to log, then
   /// shut the node down — an operator daemon whose relay never started must
   /// be supervisor-visible (it has liveness/slashing consequences), not
   /// hidden behind a running process. Expected transient failures
   /// (outpost discovery against a not-yet-deployed registry) are already
   /// absorbed inside {@link run_deferred_startup} and retried by the refresh
   /// ticks; what reaches here is structural (cron_service creation or job
   /// scheduling failed). FC_LOG_AND_DROP deliberately rethrows
   /// boost::interprocess::bad_alloc — chainbase shared-memory exhaustion
   /// stays immediately fatal.
   void run_deferred_startup_or_quit() {
      try {
         run_deferred_startup();
         return;
      } FC_LOG_AND_DROP("batch_operator_plugin: deferred startup failed unexpectedly:");
      elog("batch_operator_plugin: deferred startup failed terminally — shutting down node (fail-fast)");
      app().quit();
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
   opts("batch-sol-client-id", bpo::value<std::string>()->default_value("sol-default"),
        "Solana outpost client ID (RPC connection) for SVM outpost rows");
   // Help text must not contain a " --" sequence (or non-ASCII): the
   // PerformanceHarness plugin-args generator splits nodeop's --help output on
   // " --", so option names referenced below are spelled without the dashes.
   opts(batch_operator_detail::BATCH_OUTPOST_OPTION, bpo::value<std::vector<std::string>>()->multitoken(),
        "Remote OPP contract binding for one active sysio.chains row, repeatable once per "
        "chain code. Spec: CHAIN_CODE,opp_addr[,opp_inbound_addr]. EVM rows require the OPP "
        "and OPPInbound contract addresses (0x-hex); SVM rows require only the outpost "
        "program id (base58). The Ethereum RPC client for a row is selected by matching the "
        "row's external_chain_id against the chain ids of the outpost-ethereum-client specs; "
        "an active row with no binding or no matching client is skipped (fail closed).");
}

void batch_operator_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("batch-operator-account"))
      _impl->operator_account = chain::name(options["batch-operator-account"].as<std::string>());
   _impl->epoch_poll_ms       = options["batch-epoch-poll-ms"].as<uint32_t>();
   _impl->delivery_timeout_ms = options["batch-delivery-timeout-ms"].as<uint32_t>();
   _impl->enabled             = options["batch-enabled"].as<bool>();
   _impl->sol_client_id       = options["batch-sol-client-id"].as<std::string>();
   if (options.count(batch_operator_detail::BATCH_OUTPOST_OPTION)) {
      for (const auto& spec :
           options[batch_operator_detail::BATCH_OUTPOST_OPTION].as<std::vector<std::string>>()) {
         auto [code, binding] = batch_operator_detail::parse_outpost_binding(spec);
         FC_ASSERT(_impl->outpost_bindings.emplace(code, std::move(binding)).second,
                   "Duplicate {} binding for chain code {}",
                   batch_operator_detail::BATCH_OUTPOST_OPTION, fc::slug_name{code}.to_string());
      }
   }

   _impl->chain_plug = &app().get_plugin<chain_plugin>();
   _impl->cron_plug  = &app().get_plugin<cron_plugin>();
   _impl->eth_plug   = &app().get_plugin<outpost_ethereum_client_plugin>();
   _impl->sol_plug   = &app().get_plugin<outpost_solana_client_plugin>();

   // Operator daemons are designed for read-mode = irreversible: the sync gate
   // (controller::is_synced) measures LIB recency and every local table read the
   // relay performs serves the irreversible view. Any other read mode would relay
   // envelopes derived from state that can still fork out. Together with
   // producer_plugin's inverse assert (no producer-name under irreversible
   // read-mode), this also makes co-hosting a producer with an operator daemon
   // impossible by configuration.
   FC_ASSERT(!_impl->enabled ||
                _impl->chain_plug->chain().get_read_mode() == chain::db_read_mode::IRREVERSIBLE,
             "batch_operator_plugin requires read-mode = irreversible");
}

void batch_operator_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("batch_operator_plugin: disabled, skipping startup");
      return;
   }

   ilog("batch_operator_plugin: starting for account {}", _impl->operator_account.to_string());

   // The startup body's outpost discovery reads `sysio.chains` LOCALLY. On a
   // cold-booting operator node those reads see mid-sync (possibly genesis)
   // state and throw spuriously, so the whole body (discovery → cron_service →
   // epoch_tick → relay jobs) is DEFERRED until the node is synced —
   // `controller::is_synced()`: the LAST IRREVERSIBLE block's time within
   // `controller::default_sync_recency_ms` of now (the state the reads
   // actually serve under read-mode = irreversible). The wake-up is the
   // existing `irreversible_block` channel: a LIB advance is the only event
   // that can turn the predicate true, and channel deliveries are posted to
   // the application executor — main thread, AFTER the triggering block fully
   // commits — so the callback may run the startup body directly. There is
   // deliberately no already-synced fast path: operator daemons boot with
   // genesis-stale LIB in every deployment topology (producer co-hosting is
   // impossible by configuration — see the read-mode requirement in
   // plugin_initialize), and a node that somehow is synced at startup is
   // released by the next LIB advance.
   auto& chain = _impl->chain_plug->chain();
   ilog("batch_operator_plugin: waiting for chain sync before outpost discovery "
        "(head {} is {}s behind now; irreversible state is {}s behind)",
        chain.head().block_num(),
        (fc::time_point::now() - chain.head().block_time()).to_seconds(),
        chain.fork_db_has_root()
           ? std::to_string((fc::time_point::now() - chain.fork_db_root().block_time()).to_seconds())
           : "n/a");
   _impl->sync_gate_subscription =
      app().get_channel<chain::plugin_interface::channels::irreversible_block>().subscribe(
         [impl = _impl.get()](const chain::block_signal_params&) {
            if (impl->shutting_down || !impl->chain_plug->chain().is_synced()) {
               return;
            }
            // One-shot consumption: unsubscribe (safe from within the slot) and
            // run the startup body directly — channel deliveries are posted to
            // the application executor, so this already runs on the main thread
            // AFTER the triggering block committed (mid block-application,
            // table reads would observe an incomplete view).
            impl->sync_gate_subscription.unsubscribe();
            ilog("batch_operator_plugin: chain synced — starting deferred startup");
            impl->run_deferred_startup_or_quit();
         });
}

void batch_operator_plugin::plugin_shutdown() {
   _impl->shutting_down = true;
   if (_impl->cron_svc) {
      _impl->cron_svc->cancel_all();
      _impl->cron_svc->stop();
      _impl->cron_svc.reset();
   }
   _impl->scheduled_opp_jobs.clear();
   _impl->cron_job_ids.clear();
   ilog("batch_operator_plugin: shutdown complete");
}

} // namespace sysio
