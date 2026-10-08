#include <fc/log/logger.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/int128.hpp>
#include <fc/io/json.hpp>
#include <fc/slug_name.hpp>
#include <fc/variant_object.hpp>
#include <boost/endian/conversion.hpp>
#include <magic_enum/magic_enum.hpp>
#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <thread>

#include "async_action_completion.hpp"
#include "group_election.hpp"
#include "role_config.hpp"
#include "underwriter.hpp"
#include "yield_cranks.hpp"

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/batch_operator_plugin/depot_ops.hpp>
#include <sysio/batch_operator_plugin/outpost_epoch_lookup.hpp>
#include <sysio/batch_operator_plugin/outpost_opp_job.hpp>
#include <sysio/opp/depot/chains_registry.hpp>
#include <sysio/opp/depot/opreg_status.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/chain/subjective_billing.hpp>
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
   constexpr fc::microseconds DELIVERY_TIMEOUT  = fc::seconds(15);
   constexpr fc::microseconds EPOCH_POLL        = fc::seconds(15);
   constexpr fc::microseconds EPOCH_EDGE_BUFFER = fc::milliseconds(2500);
   /// Minimum spacing between this operator's `sysio.swap::tickyield` pushes per
   /// yield pool (`--batch-yield-tick-interval-ms`).
   constexpr fc::microseconds YIELD_TICK_INTERVAL = fc::minutes(1);

   /// `d` in whole milliseconds, the unit of the `-ms` options and of cron schedules.
   constexpr uint32_t whole_milliseconds(fc::microseconds d) {
      return static_cast<uint32_t>(d.count() / fc::milliseconds(1).count());
   }

   /// Minimum private cron-service thread count even when 0 outposts are
   /// discovered at startup — keeps `epoch_tick` viable so a cold-sync node
   /// that finds outposts later still responds.
   constexpr std::size_t MIN_CRON_THREADS = 5;
   /// Inbound plus outbound cron entries for each active outpost.
   constexpr std::size_t OPP_CRON_JOBS_PER_OUTPOST = 2;
   /// The plugin-wide epoch polling cron entry.
   constexpr std::size_t EPOCH_TICK_CRON_JOBS = 1;
   /// The underwriter's polling cron entry.
   constexpr std::size_t UNDERWRITER_CRON_JOBS = 1;
   /// Exact secondary-index lookups should return at most the matching row.
   constexpr uint32_t EXACT_LOOKUP_LIMIT = 1;
   /// How long past the head block's time a pushed transaction stays valid.
   constexpr fc::microseconds PUSH_EXPIRATION = fc::seconds(30);

   /// Work units of the `sysio.synd::crank` the underwriter pushes every pass.
   constexpr uint32_t UNDERWRITER_CRANK_LIMIT = 16;
   /// Requests one `sysio.bond::prune`, and envelopes one `sysio.synd::pruneenv`, act on.
   constexpr uint32_t UNDERWRITER_PRUNE_LIMIT = 64;
   /// Spacing of the underwriter's prunes.
   constexpr fc::microseconds UNDERWRITER_PRUNE_INTERVAL = fc::minutes(10);
   /// Spacing of the underwriter's rebuild of its outpost clients from `sysio.chains`.
   constexpr fc::microseconds UNDERWRITER_CLIENTS_INTERVAL = fc::minutes(1);
   /// Spacing of the underwriter's crank while no envelope request is in play: a bucket refill or a cleared cord
   /// still lets queued releases move.
   constexpr fc::microseconds UNDERWRITER_IDLE_CRANK_INTERVAL = fc::minutes(5);
   /// What a signer signs at startup to show that its signatures recover to its own key.
   constexpr std::string_view SIGNER_PROBE = "batch_operator_plugin signer probe";

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
      // in `sysio/opp/depot/opreg_status.hpp` so underwriter_plugin can pull
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

   namespace chalg {
      constexpr auto account           = "sysio.chalg";
      constexpr auto table_disputes    = "disputes";
      constexpr auto action_chkdispute = "chkdispute";
      /// Field names on `dispute_entry` rows, plus the `chkdispute` action arg.
      namespace field {
         constexpr auto id         = "id";
         constexpr auto status     = "status";
         constexpr auto dispute_id = "dispute_id";
      }
   }

   /// Chain registry was split out of `sysio.epoch` onto its own
   /// `sysio.chains` contract. The `outposts` table was replaced by the
   /// `chains` KV table, keyed by slug_name (uint64 packed). Field spellings
   /// are shared with underwriter_plugin, which reads the same rows.
   namespace chains = sysio::opp::depot::chains;

   namespace uw = batch_operator_detail::underwriter;

   /// A depot action a role pushes.
   struct pushed_action {
      const char* contract;
      const char* action;
   };

   /// Every action the relay pushes: `<operator>@active` must be allowed to declare each one (`linkauth`).
   constexpr std::array RELAY_ACTIONS{
      pushed_action{msgch::account, msgch::action_deliver},
      pushed_action{msgch::account, msgch::action_chkcons},
      pushed_action{chalg::account, chalg::action_chkdispute},
      pushed_action{batch_operator_detail::swap::account, batch_operator_detail::swap::action_tickyield},
      pushed_action{batch_operator_detail::liq::account, batch_operator_detail::liq::action_queueyield},
   };

   /// Every action the underwriter pushes: its permission must be allowed to declare each one (`linkauth`).
   constexpr std::array UNDERWRITER_ACTIONS{
      pushed_action{uw::bond::account, uw::bond::action_accept},
      pushed_action{uw::bond::account, uw::bond::action_approve},
      pushed_action{uw::bond::account, uw::bond::action_claim},
      pushed_action{uw::bond::account, uw::bond::action_prune},
      pushed_action{uw::synd::account, uw::synd::action_crank},
      pushed_action{uw::synd::account, uw::synd::action_pruneenv},
   };

   /// The contracts the underwriter reads and pushes to; it idles until every one runs code. Not `sysio.andon`:
   /// until that runs code there is no cord to pull.
   constexpr std::array UNDERWRITER_CONTRACTS{uw::bond::account, uw::synd::account,
                                              batch_operator_detail::liq::account};
}

// ---------------------------------------------------------------------------
//  Outpost descriptor — one per registered outpost
// ---------------------------------------------------------------------------
struct outpost_descriptor {
   uint64_t  id          = 0;
   ChainKind chain_kind  = CHAIN_KIND_UNKNOWN;
   uint32_t  chain_id    = 0;
   /// Remote contract identities from the row's nested `outpost` struct. EVM
   /// rows carry both; an SVM row carries `opp_addr` alone, because the single
   /// outpost program serves both directions. Either may be empty while the
   /// remote contract is not yet deployed — `build_opp_jobs` fails closed.
   std::string opp_addr;
   std::string opp_inbound_addr;
};

namespace {
   /// The remote contract identities a relay job or an underwriter client was built for, so a `setoutpost`
   /// redeploy is noticed and the job or client rebuilt.
   struct outpost_deployment {
      std::string opp_addr;
      std::string opp_inbound_addr;

      explicit outpost_deployment(const outpost_descriptor& op)
         : opp_addr(op.opp_addr)
         , opp_inbound_addr(op.opp_inbound_addr) {}

      /// Whether `op` still names this deployment.
      bool matches(const outpost_descriptor& op) const {
         return op.opp_addr == opp_addr && op.opp_inbound_addr == opp_inbound_addr;
      }
   };

   /// An outpost client the underwriter verifies with, and the deployment it was built for.
   struct built_outpost_client {
      std::shared_ptr<sysio::outpost_client> client;
      outpost_deployment                     deployment;
   };

   /// A chain this node cannot serve, and why.
   struct unserviceable_chain {
      std::string code;
      std::string why;
   };

   /// A `sysio.liq` shadow, as the underwriter uses it.
   struct liq_token {
      chain::symbol symbol;           ///< the shadow's symbol: its caps and balances are in it
      fc::slug_name chain_code{};     ///< the outpost whose token it shadows
   };

   /// The authorization a role declares on its depot actions and the signature provider whose key alone satisfies
   /// it, resolved once the node is synced (`impl::resolve_signer`).
   struct signer {
      chain::permission_level            auth;
      fc::crypto::signature_provider_ptr provider;
   };

   /// The descriptor of `chain_code` in `outposts`, or nullptr when it is not there.
   const outpost_descriptor* find_outpost(const std::vector<outpost_descriptor>& outposts, uint64_t chain_code) {
      const auto it = std::ranges::find(outposts, chain_code, &outpost_descriptor::id);
      return it == outposts.end() ? nullptr : &*it;
   }

   /// `find_outpost` by the chain's registry code as a slug.
   const outpost_descriptor* find_outpost(const std::vector<outpost_descriptor>& outposts, fc::slug_name chain_code) {
      return find_outpost(outposts, chain_code.value);
   }

   /// Whether `account` runs code. Call from a read window.
   bool runs_code(const chain::controller& controller, std::string_view account) {
      const chain::account_metadata_object* meta = controller.find_account_metadata(chain::name(account));
      return meta != nullptr && meta->code_hash != chain::digest_type();
   }

   /// "request <id> (<chain> <token> epoch <n>)", or "request <id>" when its statement does not decode.
   std::string describe(const uw::request& r) {
      const std::optional<uw::envelope_statement> s = uw::decode_statement(r.statement);
      if (!s) return std::format("request {}", r.id);
      return std::format("request {} ({} {} epoch {})", r.id, s->chain_code.to_string(), s->token_code.to_string(),
                         s->epoch_index);
   }
}

// ---------------------------------------------------------------------------
//  Implementation
// ---------------------------------------------------------------------------
struct batch_operator_plugin::impl {
   // Configuration
   chain::name  operator_account;
   /// Derived from `operator_account` at plugin_initialize: the relay runs iff
   /// an account was configured. There is no separate enable flag.
   bool         enabled             = false;
   fc::microseconds epoch_poll          = EPOCH_POLL;
   fc::microseconds delivery_timeout    = DELIVERY_TIMEOUT;
   fc::microseconds yield_tick_interval = YIELD_TICK_INTERVAL;

   /// `account@permission` the underwriter bonds and claims as (`batch-underwriter-account`).
   chain::permission_level underwriter_auth;
   /// Derived from `underwriter_auth` at plugin_initialize: the underwriter runs iff an account was configured,
   /// with or without the relay.
   bool                    underwriter_enabled = false;
   /// `batch-underwriter-max-exposure`, by shadow symbol code: the most the underwriter may have bonded at once.
   std::map<chain::symbol_code, chain::asset> underwriter_caps;

   /// The relay's and the underwriter's signers, resolved by `resolve_signers` once the node is synced.
   std::optional<signer> relay_signer;
   std::optional<signer> underwriter_signer;

   /// What the underwriter reads in a read window every pass (`refresh_underwriter_chain_view`), acting on the last
   /// reading: whether the contracts it needs run code (`UNDERWRITER_CONTRACTS`), whether `sysio.andon` does, the head
   /// block's time, and whether its key still satisfies its permission.
   std::atomic<bool>           underwriter_contracts_deployed{false};
   std::atomic<bool>           andon_deployed{false};
   std::atomic<fc::time_point> underwriter_head_time{};
   std::atomic<bool>           underwriter_signer_valid{true};
   /// Whether the relay's key still satisfies `<operator>@active`, read with the yield contracts' presence.
   std::atomic<bool>           relay_signer_valid{true};

   // Underwriter state, see `underwriter_tick`. Touched only from the underwriter's cron job.
   std::map<fc::slug_name, built_outpost_client> underwriter_clients;   ///< by outpost chain code
   fc::time_point                           underwriter_clients_at;   ///< last rebuild of `underwriter_clients`
   fc::time_point                           underwriter_pruned_at;    ///< last prune
   fc::time_point                           underwriter_cranked_at;   ///< last crank
   /// The lowest `sysio.bond::requests` id the next pass reads (`underwriter::next_scan_start`); 0 reads them all.
   uw::request_id_t                         underwriter_scan_from = 0;
   /// Every request the underwriter bonded, until its request row is pruned (`underwriter::remember_bonded`).
   std::set<uw::request_id_t>               underwriter_bonded;
   /// Which forfeit stops bonding (`underwriter::forfeit_watch`).
   uw::forfeit_watch                        underwriter_forfeits;

   // Yield cranks -- see `crank_yield`.
   /// Whether `sysio.swap` and `sysio.liq` run code, refreshed off the read-only
   /// executor queue every poll (a chainbase read belongs in a read window). The
   /// cranks act on the last reading rather than wait for a fresh one: one poll
   /// of lag is nothing against a yield horizon, and it keeps the cron thread
   /// off chainbase.
   std::atomic<bool>                    yield_contracts_deployed{false};
   batch_operator_detail::crank_spacing yield_tick_spacing;

   // Epoch state tracked across polls
   uint32_t                 current_epoch = 0;
   /// This operator's standing for `current_epoch`. Retained across polls
   /// because the election is decided in `parse_epoch_state` but reported in
   /// `do_poll_epoch_state`.
   batch_operator_detail::group_election election;
   fc::time_point           epoch_start;
   fc::time_point           next_epoch_start;
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

   /// Private cron_service owned by this plugin, created by `run_deferred_startup` and sized from the outposts
   /// discovered then. It runs the relay's epoch_tick and per-outpost jobs, which `refresh_outposts` adds and
   /// removes as governance changes the active set, and the underwriter's poll. Stopped at plugin_shutdown.
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
            msgch::account, msgch::action_deliver, *_impl.relay_signer,
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
      bool     is_elected()         const override { return _impl.election.is_elected; }
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
   /// A built relay job together with the remote contract identities it was
   /// built from. Addresses now come from `sysio.chains`, where governance can
   /// change them under a running node via `setoutpost`; keeping the pair here
   /// lets `prune_stale_opp_jobs` notice a redeploy and rebuild, instead of
   /// leaving the job relaying to an address the outpost has moved off.
   struct built_opp_job {
      std::shared_ptr<sysio::outpost_opp_job> job;
      outpost_deployment                      deployment;
   };
   std::map<uint64_t, built_opp_job>                              opp_jobs;

   // -----------------------------------------------------------------------
   //  Table read helper
   // -----------------------------------------------------------------------

   /// Thin delegate to `chain_plugin::read_table_rows`, which posts the scan onto the app executor's read_only queue
   /// so chainbase iteration runs during the controller's read window instead of racing with block apply.
   sysio::chain_apis::read_only::get_table_rows_result
   read_table(sysio::chain_apis::read_only::get_table_rows_params p) {
      return chain_plug->read_table_rows(std::move(p), delivery_timeout, "batch_operator", shutting_down);
   }

   /// `read_table`, but nullopt when the read failed (logged by chain_plugin), so a caller that must not act on a
   /// partial view can tell a failure from an empty table.
   std::optional<sysio::chain_apis::read_only::get_table_rows_result>
   read_table_checked(sysio::chain_apis::read_only::get_table_rows_params p) {
      return chain_plug->read_table_rows_checked(std::move(p), delivery_timeout,
                                                 chain_plug->get_abi_serializer_max_time(), "batch_operator",
                                                 shutting_down);
   }

   /// Every row of `code::table`, values only, or nullopt when the read failed. `scope` filters only a table whose
   /// first key is `scope`.
   std::optional<fc::variants> read_all_rows(std::string_view code, std::string_view scope, std::string_view table) {
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(code);
      p.scope       = std::string(scope);
      p.table       = std::string(table);
      p.all_rows    = true;
      p.values_only = true;
      std::optional<sysio::chain_apis::read_only::get_table_rows_result> rows = read_table_checked(std::move(p));
      if (!rows) return std::nullopt;
      return std::move(rows->rows);
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
      if (!relay_signer_valid) report_stale_signer(*relay_signer, "the relay");

      // chkcons advances the epoch on consensus. Only the elected operator
      // should push it — the contract verifies authorization regardless,
      // but pushing from every batch op wastes trx slots.
      if (election.is_elected) {
         try {
            push_action(msgch::account, msgch::action_chkcons, *relay_signer,
                        fc::mutable_variant_object());
         } catch (const fc::exception& e) {
            dlog("batch_operator: chkcons: {}", e.to_string());
         }
      }

      // Tally any OPEN envelope dispute. Deliberately NOT gated on `is_elected`
      // — see crank_open_disputes.
      try {
         crank_open_disputes();
      } FC_LOG_AND_DROP();

      // Sell queued yield and queue reported yield. Not gated on `is_elected`
      // either — see crank_yield. Idle until both contracts are deployed.
      refresh_yield_contract_presence();
      if (yield_contracts_deployed) {
         try {
            crank_yield();
         } FC_LOG_AND_DROP();
      }
   }

   /**
    * Crank `sysio.chalg::chkdispute` for every OPEN envelope dispute.
    *
    * `sysio.chalg::opendispute` sends `sysio.epoch::pause`, and `chkdispute` is
    * the ONLY action that tallies the Tier-1 votes, dispatches the winning
    * envelope and lifts that pause. Nothing on chain drives it: its sibling
    * `chkuwchal` needs no cadence because `sysio.uwrit::chklocks` pokes it from
    * every `sysio.epoch::advance` — which works precisely because an
    * underwriter challenge does NOT pause the chain. An envelope dispute halts
    * `advance` itself, so no inline poke can reach it. Without this crank a
    * dispute stays OPEN after Tier-1 has already reached quorum, and epoch
    * advancement is paused indefinitely.
    *
    * Not gated on `is_elected` (unlike the `chkcons` push above): the elected
    * operator may be the very one that is offline or delivered the
    * non-canonical envelope — often the reason the dispute exists. Disputes are
    * rare, so pushing from every ACTIVE operator costs effectively nothing, and
    * `chkdispute` asserts the dispute is still OPEN, which makes a redundant
    * push a cheap no-op.
    *
    * The scan is a full-table filter rather than a `byepoch` index lookup: the
    * table retains RESOLVED rows as the audit trail, but disputes are rare and
    * `poll_own_status` already scans a comparably-sized table each tick. If
    * disputes ever become frequent, bound this by `current_epoch` via the
    * `byepoch` secondary index.
    */
   void crank_open_disputes() {
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(chalg::account);
      p.scope       = chalg::account;
      p.table       = chalg::table_disputes;
      p.all_rows    = true;
      p.values_only = true;
      p.filter      = [](const fc::variant& row) {
         const auto& obj = row.get_object();
         auto status_it  = obj.find(chalg::field::status);
         return status_it != obj.end() &&
                status_it->value().as<DisputeStatus>() == DISPUTE_STATUS_OPEN;
      };
      auto rows = read_table(std::move(p));

      for (const auto& r : rows.rows) {
         const auto& obj = r.get_object();
         auto id_it      = obj.find(chalg::field::id);
         if (id_it == obj.end()) continue;
         const uint64_t dispute_id = id_it->value().as_uint64();

         try {
            push_action(chalg::account, chalg::action_chkdispute, *relay_signer,
                        fc::mutable_variant_object()(chalg::field::dispute_id, dispute_id));
         } catch (const fc::exception& e) {
            // Expected-transient: the dispute resolved between the scan and the push, or
            // another operator's crank won the race.
            dlog("batch_operator: chkdispute({}): {}", dispute_id, e.to_string());
         }
      }
   }

   /**
    * Refresh `yield_contracts_deployed` from a read window. `sysio.swap` and
    * `sysio.liq` are the two contracts the yield cranks read, and a depot whose
    * bootstrap has not deployed them (or never will) must not pay a failed table
    * read -- an `elog` per poll -- for tables that legitimately do not exist.
    */
   void refresh_yield_contract_presence() {
      // `this` outlives every queued task: appbase drains the executor before it
      // destroys plugins (the same guarantee chain_plugin::read_table_rows_checked
      // relies on for its own posted scans).
      app().executor().post(appbase::priority::low, appbase::exec_queue::read_only, [this] {
         using namespace batch_operator_detail;
         if (shutting_down) return;
         const auto& controller = chain_plug->chain();
         const bool  deployed   = runs_code(controller, swap::account) && runs_code(controller, liq::account);
         note_signer_validity(controller, relay_signer, relay_signer_valid);
         if (yield_contracts_deployed.exchange(deployed) != deployed) {
            ilog("batch_operator: yield cranks {}: {} and {} {}",
                 deployed ? "active" : "idle", swap::account, liq::account,
                 deployed ? "are deployed" : "are not both deployed");
         }
      });
   }

   /**
    * Crank the two permissionless yield actions nothing on chain schedules:
    * `sysio.swap::tickyield` for every yield pool whose reservoir has a queued
    * balance, and `sysio.liq::queueyield` for every shadow with yield pending from
    * an outpost `LIQ_YIELD` report. Like `crank_open_disputes`, NOT gated on
    * `is_elected`: the elected operator may be the one that is offline, and every
    * push is a cheap no-op once its work is done (`tickyield` returns when nothing
    * is queued or the clip is below its floor; `queueyield` when nothing is
    * pending). `yield_tick_spacing` bounds this operator to one tick per pool per
    * `batch-yield-tick-interval-ms`: a reservoir drains over a horizon of hours
    * while every ACTIVE operator polls every few seconds.
    */
   void crank_yield() {
      crank_yield_ticks();
      crank_pending_yield();
   }

   /// `tickyield` for every tickable pool whose reservoir has something queued.
   void crank_yield_ticks() {
      using namespace batch_operator_detail;
      // The pools: only a yield pool with its tick parameters set survives the
      // action's own checks; the pool's symbol code is the row's kv key, so the
      // rows are read with their keys.
      sysio::chain_apis::read_only::get_table_rows_params pools;
      pools.code     = chain::name(swap::account);
      pools.scope    = swap::account;
      pools.table    = swap::table_stat;
      pools.all_rows = true;
      pools.filter   = [](const fc::variant& row) {
         const auto value = row_value(row.get_object());
         return value && is_tickable_pool(*value);
      };
      auto pool_rows = read_table(std::move(pools));
      if (pool_rows.rows.empty()) return;

      // The reservoirs: what is queued to sell, keyed by the same symbol code.
      sysio::chain_apis::read_only::get_table_rows_params reservoirs;
      reservoirs.code     = chain::name(swap::account);
      reservoirs.scope    = swap::account;
      reservoirs.table    = swap::table_reservoirs;
      reservoirs.all_rows = true;
      std::map<uint64_t, int64_t> queued_by_pool;
      for (const auto& r : read_table(std::move(reservoirs)).rows) {
         const auto& row   = r.get_object();
         const auto  code  = row_symbol_code(row);
         const auto  value = row_value(row);
         if (!code || !value) continue;
         auto balance = value->find(swap::field::balance);
         if (balance == value->end() || !balance->value().is_object()) continue;
         queued_by_pool[code->value] = asset_amount(balance->value().get_object(), swap::field::quantity);
      }

      const auto now      = fc::time_point::now();
      for (const auto& r : pool_rows.rows) {
         const auto code = row_symbol_code(r.get_object());
         if (!code) continue;
         const auto queued = queued_by_pool.find(code->value);
         if (queued == queued_by_pool.end() || queued->second <= 0) continue;
         const auto pool = symbol_code_name(*code);
         if (!yield_tick_spacing.due(pool, now, yield_tick_interval)) continue;
         try {
            push_action(swap::account, swap::action_tickyield, *relay_signer,
                        fc::mutable_variant_object()(swap::field::pair_token, *code));
            yield_tick_spacing.mark(pool, now);
         } catch (const fc::exception& e) {
            // Expected-transient: another operator's tick sold the clip first, or the
            // pool's parameters changed between the scan and the push.
            dlog("batch_operator: tickyield({}): {}", pool, e.to_string());
         }
      }
   }

   /// `queueyield` for every shadow with yield pending from an outpost report.
   void crank_pending_yield() {
      using namespace batch_operator_detail;
      sysio::chain_apis::read_only::get_table_rows_params pending;
      pending.code     = chain::name(liq::account);
      pending.scope    = liq::account;
      pending.table    = liq::table_liqpending;
      pending.all_rows = true;
      for (const auto& r : read_table(std::move(pending)).rows) {
         const auto& row   = r.get_object();
         const auto  code  = row_symbol_code(row);
         const auto  value = row_value(row);
         if (!code || !value || asset_amount(*value, liq::field::quantity) <= 0) continue;
         try {
            push_action(liq::account, liq::action_queueyield, *relay_signer,
                        fc::mutable_variant_object()(liq::field::sym, *code));
         } catch (const fc::exception& e) {
            // Expected-transient: another operator queued it first. Persistent while
            // the shadow has no yield pool yet (`regliqpool` still to come).
            dlog("batch_operator: queueyield({}): {}", symbol_code_name(*code), e.to_string());
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
      // `sysio.opreg::operators` is a KV table whose PK is a struct
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
      is_active = sysio::opp::depot::opreg_status::compute_is_active(status, was_active);

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
         if (election.is_elected) {
            ilog("batch_operator: epoch paused, suspending");
            election.is_elected = false;
         }
         return {false, 0};
      }

      // Determine group assignment
      election = batch_operator_detail::evaluate_group_election(
         cur_group, obj[epoch::field::batch_op_groups].get_array(), operator_account);

      // Parse epoch timing
      fc::from_variant(obj[epoch::field::current_epoch_start], epoch_start);
      fc::from_variant(obj[epoch::field::next_epoch_start],    next_epoch_start);

      if (election.is_elected) {
         ilog("batch_operator: current_epoch={}", epoch_index);
      }

      return {true, epoch_index};
   }

   void do_poll_epoch_state() {
      auto [ok, epoch_index] = parse_epoch_state();
      if (!ok) return;

      // The genesis advance (epoch 0 -> 1) is a SYSTEM responsibility, not a
      // batch operator's: `sysio.msgch::bootstrap` gates on
      // `require_auth(get_self())`, so only the holder of `sysio.msgch`'s own
      // authority (the genesis / bootstrap process) can trigger it. A batch
      // operator signs with its own account authority, so an operator-side push
      // fails unconditionally with `missing authority of sysio.msgch` before the
      // epoch check — it never advanced anything, it only produced boot-window
      // error-log noise. From epoch 1 onward batch operators still TRIGGER
      // advancement — the elected operator's poll cranks `msgch::chkcons`
      // (see poll_epoch_state), which sends the inline `sysio.epoch::advance`
      // (again under msgch's own authority) once the per-outpost consensus
      // recorded during `evalcons` dispatch and the epoch time gate are met —
      // but they never AUTHORIZE bootstrap or advance directly.

      if (!election.is_elected) {
         if (epoch_index != current_epoch) {
            ilog("{}", batch_operator_detail::not_elected_message(epoch_index, election));
         }
         // Keep current_epoch fresh even when not elected: per-outpost jobs
         // consult it via depot_ops, and they bail on !is_elected anyway.
         current_epoch = epoch_index;
         return;
      }

      if (epoch_index != current_epoch) {
         ilog("batch_operator: ELECTED for epoch {} (group {}, {} members)",
              epoch_index, election.my_group, election.current_group_members.size());
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

   /// The active outposts on `sysio.chains::chains`, or nullopt when the table cannot be read.
   ///
   /// Each row carries the chain's slug_name + kind + external_chain_id + is_depot + active. Outposts are the
   /// non-depot, active rows; the single is_depot=true row is the WIRE chain itself and is skipped.
   ///
   /// Startup race: in a multi-node cluster the batch-op node replays blocks from the producer asynchronously.
   /// There's a brief window where `sysio.chains` exists on the producer but the local node hasn't replayed far
   /// enough to see it, and the read fails (logged by chain_plugin). The caller keeps its last-known set and retries
   /// on its next tick.
   std::optional<std::vector<outpost_descriptor>> read_active_outposts() {
      const std::optional<fc::variants> rows = read_all_rows(chains::account, chains::account, chains::table_chains);
      if (!rows) {
         wlog("batch_operator: outpost refresh deferred: sysio.chains could not be read");
         return std::nullopt;
      }
      std::vector<outpost_descriptor> active;
      for (const fc::variant& row : *rows) {
         const fc::variant_object& obj = row.get_object();
         // `code` is a `slug_name`. fc::slug_name's own from_variant reads the
         // decoded slug string ("" for zero) and the transitional
         // `{value: <uint64>}` object, so the shape is not probed here.
         uint64_t code_val = 0;
         if (auto code_obj = obj.find(chains::field::code); code_obj != obj.end()) {
            code_val = code_obj->value().as<fc::slug_name>().value;
         }
         bool is_depot = obj[chains::field::is_depot].as_bool();
         bool is_active = obj[chains::field::active].as_bool();
         if (is_depot || !is_active) continue;
         outpost_descriptor od;
         od.id         = code_val;  // slug_name uint64 doubles as outpost id
         od.chain_kind = obj[chains::field::kind].as<ChainKind>();
         od.chain_id   = static_cast<uint32_t>(obj[chains::field::external_chain_id].as_uint64());
         // The remote contract identities live in a nested struct on the row.
         // Absent (pre-upgrade row) reads as empty, which fails closed in
         // `make_outpost_client` exactly like a row governance has not configured yet.
         if (auto out_it = obj.find(chains::field::outpost);
             out_it != obj.end() && out_it->value().is_object()) {
            const fc::variant_object& out_obj = out_it->value().get_object();
            if (auto a = out_obj.find(chains::field::outpost_addr::opp_addr); a != out_obj.end())
               od.opp_addr = a->value().as_string();
            if (auto a = out_obj.find(chains::field::outpost_addr::opp_inbound_addr); a != out_obj.end())
               od.opp_inbound_addr = a->value().as_string();
         }
         active.push_back(std::move(od));
      }
      return active;
   }

   void refresh_outposts() {
      std::optional<std::vector<outpost_descriptor>> active = read_active_outposts();
      if (!active) return;   // keep the last-known set; jobs built from it continue to work
      outposts = std::move(*active);
      ilog("batch_operator: loaded {} outposts (sysio.chains)", outposts.size());
      prune_stale_opp_jobs();
      build_opp_jobs();
      schedule_opp_jobs();
   }

   /// An outpost client, or why there is none.
   struct client_build {
      std::shared_ptr<sysio::outpost_client> client;
      /// Why this node cannot serve the chain at all; empty when `client` is set, or when the row only lacks
      /// its remote contract address so far.
      std::string                            unserviceable;
   };

   /// Build the outpost client for `op` with the chain-specific plugin factories, for `role` (which Solana IDL
   /// declarations it validates at boot).
   ///
   /// A missing RPC client, an unsupported chain kind or a factory refusal is local configuration only the
   /// operator can fix, reported as `unserviceable`. Missing CONTRACT ADDRESSES are governance state on the row,
   /// fixable with `sysio.chains::setoutpost` without touching any node, so the chain is skipped (no client, no
   /// reason) and picked up on a later refresh.
   client_build make_outpost_client(const outpost_descriptor& op, solana_outpost_role role) {
      const std::string code_str = fc::slug_name{op.id}.to_string();
      try {
         if (op.chain_kind == CHAIN_KIND_EVM) {
            // Bind this exact outpost to its own remote identity: the RPC
            // client is the one registered under this chain's OWN code, and
            // the OPP / OPPInbound contract addresses come from the row
            // itself. `create_outpost_client` additionally asserts the
            // client's verified eth_chainId equals the row's
            // external_chain_id, so a client registered under the wrong code
            // is caught rather than relayed through.
            if (!eth_plug->get_client(code_str)) {
               return {.unserviceable = "no Ethereum RPC client is registered under this chain code"};
            }
            if (op.opp_addr.empty() || op.opp_inbound_addr.empty()) {
               wlog("batch_operator: outpost {} (EVM) has no OPP/OPPInbound address on its "
                    "sysio.chains row; skipping until sysio.chains::setoutpost supplies both",
                    code_str);
               return {};
            }
            return {.client = eth_plug->create_outpost_client(code_str, op.id, op.chain_id,
                                                              op.opp_addr, op.opp_inbound_addr)};
         }
         if (op.chain_kind == CHAIN_KIND_SVM) {
            // SVM: the RPC client is likewise registered under the chain's
            // own code. The per-outpost identity is the program id on the
            // row; `sysio.chains` already rejects an SVM row that carries a
            // separate inbound address, so only presence is checked here.
            if (!sol_plug->get_client(code_str)) {
               return {.unserviceable = "no Solana RPC client is registered under this chain code"};
            }
            if (op.opp_addr.empty()) {
               wlog("batch_operator: outpost {} (SVM) has no program id on its sysio.chains "
                    "row; skipping until sysio.chains::setoutpost supplies one",
                    code_str);
               return {};
            }
            return {.client = sol_plug->create_outpost_client(code_str, op.id, op.chain_id, op.opp_addr, role)};
         }
         // A chain kind this build does not know how to serve is just as
         // unserviceable as a missing client, and equally unfixable
         // on-chain: the operator needs a newer nodeop.
         return {.unserviceable = std::format("chain kind {} is not supported by this build",
                                              ChainKind_Name(op.chain_kind))};
      } catch (const fc::exception& e) {
         // The factory throws on a client that cannot serve this row at all,
         // most importantly when the client registered under this chain code
         // reports a different eth_chainId than the row's external_chain_id.
         // That is a misconfiguration only the operator can fix, so it is
         // unserviceable on the same terms as a missing client; swallowing it
         // would leave the node silently unable to do its job on that chain.
         return {.unserviceable = std::format("outpost client could not be built: {}", e.top_message())};
      } catch (const std::exception& e) {
         // The same for a standard exception, such as a lookup of an ABI file that was never loaded.
         return {.unserviceable = std::format("outpost client could not be built: {}", e.what())};
      }
   }

   /// Build, with `wrap(op, client)`, what `built` lacks for each chain in `active`, the clients made for `role`.
   /// Returns the chains this node cannot serve; a chain whose row has no remote address yet is skipped until
   /// `setoutpost` supplies one.
   template <typename Key, typename Built, typename Wrap>
   std::vector<unserviceable_chain> build_missing(std::map<Key, Built>& built,
                                                  const std::vector<outpost_descriptor>& active,
                                                  solana_outpost_role role, Wrap&& wrap) {
      std::vector<unserviceable_chain> unserviceable;
      for (const outpost_descriptor& op : active) {
         const Key key{op.id};
         if (built.contains(key)) continue;
         client_build made = make_outpost_client(op, role);
         if (!made.unserviceable.empty()) {
            unserviceable.push_back({fc::slug_name{op.id}.to_string(), std::move(made.unserviceable)});
            continue;
         }
         if (!made.client) continue;   // no remote address on the row yet
         built.emplace(key, wrap(op, made.client));
      }
      return unserviceable;
   }

   /// Drop what `built` holds for a chain that left `active`, or whose deployment moved (`setoutpost`), calling
   /// `on_drop(chain_code, why)` for each, so the next `build_missing` builds it again for the new address.
   template <typename Key, typename Built, typename OnDrop>
   static void drop_stale(std::map<Key, Built>& built, const std::vector<outpost_descriptor>& active,
                          OnDrop&& on_drop) {
      for (auto it = built.begin(); it != built.end();) {
         const outpost_descriptor* current = find_outpost(active, it->first);
         if (current != nullptr && it->second.deployment.matches(*current)) {
            ++it;
            continue;
         }
         on_drop(it->first, current == nullptr ? "inactive" : "redeployed");
         it = built.erase(it);
      }
   }

   /// Name the chains this node cannot serve and shut it down, so the failure is visible to a supervisor
   /// instead of hiding behind a running process. Only the operator can fix it (it is local config).
   void quit_unserviceable(const std::vector<unserviceable_chain>& unserviceable, size_t active_count,
                           std::string_view why_every_chain) {
      for (const auto& [code, why] : unserviceable) {
         elog("batch_operator: cannot serve active chain {}: {}", code, why);
      }
      elog("batch_operator: cannot serve {} of {} active chain(s), shutting down node ({})",
           unserviceable.size(), active_count, why_every_chain);
      app().quit();
   }

   /// Construct an `outpost_opp_job` per registered outpost. Idempotent: already-built jobs stay.
   /// Called from `refresh_outposts`; scheduling is handled separately so
   /// startup-created jobs and governance-added jobs share the same path.
   ///
   /// An elected group fans the epoch cycle out across EVERY active chain, so a
   /// batch operator that cannot reach one of them cannot do its job — it would
   /// simply withhold deliveries for that chain and drag its group below
   /// consensus. An unserviceable chain is therefore FATAL, not skippable; this
   /// runs after the sync gate, where `sysio.chains` is actually readable.
   void build_opp_jobs() {
      if (!depot_ops_backing) return; // plugin not initialized yet
      const std::vector<unserviceable_chain> unserviceable = build_missing(
         opp_jobs, outposts, solana_outpost_role::batch_operator,
         [&](const outpost_descriptor& op, const std::shared_ptr<sysio::outpost_client>& client) {
            std::shared_ptr<sysio::outpost_opp_job> job =
               std::make_shared<sysio::outpost_opp_job>(client, *depot_ops_backing, delivery_timeout);
            ilog("batch_operator: built outpost_opp_job for {}", client->to_string());
            return built_opp_job{std::move(job), outpost_deployment{op}};
         });
      if (!unserviceable.empty()) {
         quit_unserviceable(unserviceable, outposts.size(), "an elected group must deliver on every active chain");
      }
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

   /// Drop relay jobs that no longer match `sysio.chains`: the chain went
   /// inactive, or governance moved its remote deployment with `setoutpost`.
   /// `build_opp_jobs` rebuilds what is dropped here on the same refresh, so an
   /// address change costs one tick rather than an operator restart — without
   /// it a redeployed outpost would keep receiving deliveries at a dead
   /// address, and inbound reads would keep polling the old contract.
   void prune_stale_opp_jobs() {
      drop_stale(opp_jobs, outposts, [&](uint64_t chain_code, std::string_view why) {
         cancel_scheduled_opp_job(chain_code);
         ilog("batch_operator: removed outpost_opp_job for {} outpost {}", why, fc::slug_name{chain_code}.to_string());
      });
   }

   /// Schedule one cron direction for an outpost relay job.
   cron_service::job_id_t schedule_opp_job_direction(uint64_t chain_code,
                                                     const std::shared_ptr<sysio::outpost_opp_job>& job,
                                                     std::string_view direction,
                                                     void (sysio::outpost_opp_job::*runner)()) {
      sysio::services::cron_service::job_schedule sched;
      sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{whole_milliseconds(epoch_poll)}};
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
           meta.label, job->client().to_string(), id, whole_milliseconds(epoch_poll));
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
      for (const auto& [chain_code, built] : opp_jobs) {
         schedule_opp_job(chain_code, built.job);
      }
   }

   /// Returns true if we are within the safe operating window for this epoch.
   /// Blocks operations only in the narrow buffer zones at epoch boundaries.
   /// Once past next_epoch_start, operations are allowed (epoch is overdue).
   bool within_epoch_window() const {
      const fc::time_point now = fc::time_point::now();
      if (now < epoch_start + EPOCH_EDGE_BUFFER) return false;  // too close to epoch start
      // Only block in the narrow window BEFORE next_epoch_start.
      // Once past next_epoch_start, the epoch is overdue — allow operations.
      if (next_epoch_start != fc::time_point() &&
          now > next_epoch_start - EPOCH_EDGE_BUFFER &&
          now < next_epoch_start) return false;
      return true;
   }



   // -----------------------------------------------------------------------
   //  Signers
   // -----------------------------------------------------------------------

   /// The signer of `role`, declaring `level`: the one operator-configured WIRE signature provider whose key alone
   /// satisfies `level` on chain. Nullopt, logged, when there is none, when there is more than one (the choice would
   /// fall to provider order), when its signatures do not recover to its key, or when `level` may not declare one
   /// of `actions`. Call on the main thread, after the sync gate.
   std::optional<signer> resolve_signer(std::string_view role, const chain::permission_level& level,
                                        std::span<const pushed_action> actions) {
      const chain::controller& chain  = chain_plug->chain();
      bool                     linked = true;
      for (const auto& [contract, action] : actions) {
         if (permission_satisfies_link(chain, level, chain::name(contract), chain::name(action))) continue;
         elog("batch_operator: {} declares {}@{}, which may not authorize {}::{}: link that action to it",
              role, level.actor.to_string(), level.permission.to_string(), contract, action);
         linked = false;
      }
      signature_provider_manager_plugin& sig_plug = app().get_plugin<signature_provider_manager_plugin>();
      const batch_operator_detail::signer_choice<fc::crypto::signature_provider_ptr> choice =
         batch_operator_detail::choose_signer(
         sig_plug.query_providers(std::nullopt, fc::crypto::chain_kind_wire),
         [&](const fc::crypto::signature_provider_ptr& provider) {
            return sig_plug.is_operator_configured_provider(provider->key_name) &&
                   key_alone_satisfies(chain, level, provider->public_key);
         });
      if (!choice.chosen) {
         elog("batch_operator: {} needs exactly one configured WIRE signature provider whose key alone satisfies "
              "{}@{}, found {}", role, level.actor.to_string(), level.permission.to_string(), choice.matches);
         return std::nullopt;
      }
      if (!signs_for_its_key(role, **choice.chosen) || !linked) return std::nullopt;
      return signer{.auth = level, .provider = *choice.chosen};
   }

   /// Whether `provider`'s signature over a probe digest recovers to its own key, which is how the chain checks a
   /// transaction signature. Logs why not.
   static bool signs_for_its_key(std::string_view role, const fc::crypto::signature_provider_t& provider) {
      const fc::sha256 probe = fc::sha256::hash(SIGNER_PROBE.data(), SIGNER_PROBE.size());
      try {
         if (fc::crypto::public_key::recover(provider.sign(probe), probe) == provider.public_key) return true;
         elog("batch_operator: {}'s signature provider {} makes signatures that do not recover to its key", role,
              provider.key_name);
      } catch (const fc::exception& e) {
         elog("batch_operator: {}'s signature provider {} cannot sign: {}", role, provider.key_name, e.top_message());
      } catch (const std::exception& e) {
         elog("batch_operator: {}'s signature provider {} cannot sign: {}", role, provider.key_name, e.what());
      }
      return false;
   }

   /// Resolve the signer of every enabled role. False when one cannot be resolved, after both are reported.
   bool resolve_signers() {
      bool resolved = true;
      if (enabled) {
         relay_signer = resolve_signer("the relay", {operator_account, chain::config::active_name}, RELAY_ACTIONS);
         resolved     = relay_signer.has_value() && resolved;
      }
      if (underwriter_enabled) {
         underwriter_signer = resolve_signer("the underwriter", underwriter_auth, UNDERWRITER_ACTIONS);
         resolved           = underwriter_signer.has_value() && resolved;
      }
      return resolved;
   }

   // -----------------------------------------------------------------------
   //  Underwriter
   // -----------------------------------------------------------------------

   /**
    * One pass of the underwriter, run by its own cron job: bond each OPEN `sysio.synd` envelope request whose
    * envelope the outpost has confirmed, crank `sysio.synd`, then approve, claim and prune what it bonded.
    * `underwriter::plan_actions` makes every decision; this reads its inputs and pushes its actions, all signed by
    * `underwriter_signer`.
    *
    * Bonding is what releases an envelope's syndications, so a request is bonded only when its outpost's own record
    * of the envelope it emitted for the statement's epoch carries the statement's digest, within the token's exposure
    * cap and the account's balance, while the andon cord is clear, and while no bond of the underwriter's has been
    * ruled INVALID since the node started. A pass whose table reads fail does nothing: planning from part of the state
    * could exceed a cap or miss a freeze. What it cannot act on waits for a later pass and is logged on every pass
    * it holds; deduplicating those lines is left to log tooling.
    */
   void underwriter_tick() {
      if (shutting_down || !underwriter_enabled) return;
      try {
         run_underwriter_pass();
      } FC_LOG_AND_DROP();
   }

   /// The body of `underwriter_tick`.
   void run_underwriter_pass() {
      refresh_underwriter_chain_view();
      if (!underwriter_contracts_deployed) return;
      if (!underwriter_signer_valid) {   // nothing it pushes could be authorized
         report_stale_signer(*underwriter_signer, "the underwriter");
         return;
      }
      // Chain time, which the contracts judge windows by; the wall clock only spaces this node's own housekeeping.
      const fc::time_point head_time = underwriter_head_time.load();
      if (head_time == fc::time_point()) return;
      const fc::time_point now = fc::time_point::now();
      if (now - underwriter_clients_at >= UNDERWRITER_CLIENTS_INTERVAL) {
         underwriter_clients_at = now;
         underwriter_refresh_clients();
      }

      const std::optional<std::map<fc::slug_name, liq_token>> tokens = read_liq_tokens();
      if (!tokens) return;
      std::optional<uw::plan_inputs> in = read_underwriter_state(*tokens, head_time);
      if (!in || shutting_down) return;
      uw::remember_bonded(underwriter_bonded, in->bonds, in->requests);
      if (underwriter_forfeits.note(uw::forfeited_requests(in->requests, underwriter_bonded))) {
         elog("batch_operator: underwriter stops bonding until the node is restarted: request {} was ruled INVALID "
              "with our bond on it", *underwriter_forfeits.halted_by);
      }
      in->bonded = underwriter_bonded;
      in->halted = underwriter_forfeits.halted_by.has_value();
      if (!in->frozen && !in->halted) in->emitted = read_emitted_digests(in->requests);
      const uw::plan plan = uw::plan_actions(*in);
      report_plan(*in, plan);
      push_plan(plan, uw::synd_has_work(in->requests), now);
      underwriter_scan_from = uw::next_scan_start(underwriter_scan_from, in->requests, in->bonds, underwriter_bonded);

      if (now - underwriter_pruned_at >= UNDERWRITER_PRUNE_INTERVAL) {
         underwriter_pruned_at = now;
         underwriter_prune(*tokens);
      }
   }

   /// Refresh what the underwriter reads in a read window: the contracts' presence, the head block's time and its
   /// key's validity, as `refresh_yield_contract_presence` does for the yield cranks.
   void refresh_underwriter_chain_view() {
      app().executor().post(appbase::priority::low, appbase::exec_queue::read_only, [this] {
         if (shutting_down) return;
         const chain::controller& controller = chain_plug->chain();
         andon_deployed                      = runs_code(controller, uw::andon::account);
         underwriter_head_time               = controller.head().block_time();
         note_signer_validity(controller, underwriter_signer, underwriter_signer_valid);
         const bool deployed = std::ranges::all_of(UNDERWRITER_CONTRACTS, [&](const char* account) {
            return runs_code(controller, account);
         });
         if (underwriter_contracts_deployed.exchange(deployed) != deployed) {
            std::string accounts;
            for (const char* account : UNDERWRITER_CONTRACTS) {
               accounts += std::format("{}{}", accounts.empty() ? "" : ", ", account);
            }
            ilog("batch_operator: underwriter {}: {} {}", deployed ? "active" : "idle", accounts,
                 deployed ? "all run code" : "do not all run code yet");
         }
      });
   }

   /// Refresh `valid` from whether `s`'s key still satisfies its authorization on chain. A rotated key needs a
   /// restart: signers are chosen once, at startup. Call from a read window.
   static void note_signer_validity(const chain::controller& chain, const std::optional<signer>& s,
                                    std::atomic<bool>& valid) {
      if (s) valid = key_alone_satisfies(chain, s->auth, s->provider->public_key);
   }

   /// Log that `s`'s key no longer satisfies its authorization.
   static void report_stale_signer(const signer& s, std::string_view role) {
      elog("batch_operator: {}'s key no longer satisfies {}@{}; restart the node with the new key", role,
           s.auth.actor.to_string(), s.auth.permission.to_string());
   }

   /// Push one action as the underwriter. True when the transaction was accepted; a refusal is logged by the push
   /// callback.
   bool underwriter_push(const char* contract, const char* action, const fc::variant_object& data) {
      if (shutting_down) return false;
      try {
         return push_action(contract, action, *underwriter_signer, data);
      } catch (const fc::exception& e) {
         wlog("batch_operator: underwriter {}::{}: {}", contract, action, e.to_string());
      } catch (const std::exception& e) {
         wlog("batch_operator: underwriter {}::{}: {}", contract, action, e.what());
      }
      return false;
   }

   /// Push what the plan decided: accepts first, so their releases start this pass, then the crank, approves and
   /// claims. A push the irreversible view does not show yet is planned, and refused, again next pass. `synd_busy`
   /// says `sysio.synd` has an envelope request in play or an outcome to acknowledge.
   void push_plan(const uw::plan& plan, bool synd_busy, fc::time_point now) {
      for (const uw::accept_action& accept : plan.accepts) {
         // Accepted by this node only: it can still fail where it lands, so the bond is known from its row.
         if (underwriter_push(uw::bond::account, uw::bond::action_accept,
                              fc::mutable_variant_object()
                                 (uw::bond::field::underwriter, underwriter_auth.actor)
                                 (uw::bond::field::request_id,  accept.request_id)
                                 (uw::bond::field::amount,      accept.amount.get_amount()))) {
            ilog("batch_operator: underwriter pushed an accept of request {} ({})", accept.request_id,
                 accept.amount.to_string());
         }
      }
      // One crank moves a bounded amount of work. Every pass while synd is busy; otherwise now and then, for what a
      // bucket refill or a cleared cord lets through.
      if (synd_busy || now - underwriter_cranked_at >= UNDERWRITER_IDLE_CRANK_INTERVAL) {
         underwriter_cranked_at = now;
         underwriter_push(uw::synd::account, uw::synd::action_crank,
                          fc::mutable_variant_object()(uw::synd::field::limit, UNDERWRITER_CRANK_LIMIT));
      }
      for (const uw::request_id_t id : plan.approves) {
         underwriter_push(uw::bond::account, uw::bond::action_approve,
                          fc::mutable_variant_object()(uw::bond::field::request_id, id));
      }
      for (const uw::request_id_t id : plan.claims) {
         underwriter_push(uw::bond::account, uw::bond::action_claim,
                          fc::mutable_variant_object()
                             (uw::bond::field::request_id, id)
                             (uw::bond::field::account,    underwriter_auth.actor));
      }
   }

   /// Report what the plan leaves to people: a freeze, a halt, challenged and forfeited requests, and requests left
   /// unbonded.
   void report_plan(const uw::plan_inputs& in, const uw::plan& plan) {
      std::map<uw::request_id_t, const uw::request*> by_id;
      for (const uw::request& r : in.requests) by_id.emplace(r.id, &r);
      const auto name_of = [&](uw::request_id_t id) {
         const auto r = by_id.find(id);
         return r == by_id.end() ? std::format("request {}", id) : describe(*r->second);
      };
      if (in.frozen) {
         wlog("batch_operator: underwriter paused: the sysio.andon cord is pulled, so nothing is bonded, approved or "
              "claimed until it clears");
      }
      if (underwriter_forfeits.halted_by) {
         elog("batch_operator: underwriter bonds nothing until the node is restarted: {} was ruled INVALID with our "
              "bond on it", name_of(*underwriter_forfeits.halted_by));
      }
      for (const uw::request_id_t id : plan.held) {
         elog("batch_operator: underwriter {} is challenged (HELD) with our bond on it: sysio must rule it",
              name_of(id));
      }
      for (const uw::request_id_t id : plan.blocked) {
         elog("batch_operator: underwriter {} was challenged (HELD) before it was bonded: its pair releases nothing "
              "until sysio rules it", name_of(id));
      }
      for (const uw::request_id_t id : plan.forfeited) {
         elog("batch_operator: underwriter {} was ruled INVALID: our bond on it is forfeited", name_of(id));
      }
      for (const uw::waiting_request& waiting : plan.waiting) {
         if (waiting.reason == uw::wait_reason::CONTRADICTED) {
            report_contradiction(in, waiting.request_id);
            continue;
         }
         wlog("batch_operator: underwriter left {} unbonded: {}", name_of(waiting.request_id),
              magic_enum::enum_name(waiting.reason));
      }
   }

   /// Report a request whose statement names a digest its outpost did not record for that epoch: the depot accepted
   /// an envelope the outpost never emitted, or the statement misstates it. It is never bonded.
   void report_contradiction(const uw::plan_inputs& in, uw::request_id_t request_id) {
      const auto r = std::ranges::find(in.requests, request_id, &uw::request::id);
      if (r == in.requests.end()) return;
      const std::optional<uw::envelope_statement> s = uw::decode_statement(r->statement);
      if (!s) return;
      const auto emitted = in.emitted.find({s->chain_code, s->epoch_index});
      if (emitted == in.emitted.end()) return;
      elog("batch_operator: underwriter: {} states digest {}, but outpost {} recorded {} for epoch {}; not bonded",
           describe(*r), s->digest.str(), s->chain_code.to_string(), emitted->second.str(), s->epoch_index);
   }

   /// `sysio.bond::prune` from the first request, and `sysio.synd::pruneenv` for every shadow's pair. Both are
   /// permissionless and act on at most UNDERWRITER_PRUNE_LIMIT rows.
   void underwriter_prune(const std::map<fc::slug_name, liq_token>& tokens) {
      underwriter_push(uw::bond::account, uw::bond::action_prune,
                       fc::mutable_variant_object()
                          (uw::bond::field::from_id, 0)
                          (uw::bond::field::limit,   UNDERWRITER_PRUNE_LIMIT));
      for (const auto& [token_code, token] : tokens) {
         underwriter_push(uw::synd::account, uw::synd::action_pruneenv,
                          fc::mutable_variant_object()
                             (uw::synd::field::chain_code, token.chain_code)
                             (uw::synd::field::token_code, token_code)
                             (uw::synd::field::limit,      UNDERWRITER_PRUNE_LIMIT));
      }
   }

   /// Keep one outpost client per active chain for verification, rebuilding one whose deployment moved. A chain this
   /// node cannot serve is reported and its requests wait; the other chains go on.
   void underwriter_refresh_clients() {
      try {
         const std::optional<std::vector<outpost_descriptor>> active = read_active_outposts();
         if (!active) return;
         drop_stale(underwriter_clients, *active, [](fc::slug_name chain_code, std::string_view why) {
            ilog("batch_operator: underwriter dropped its client for {} outpost {}", why, chain_code.to_string());
         });
         const std::vector<unserviceable_chain> unserviceable = build_missing(
            underwriter_clients, *active, solana_outpost_role::underwriter,
            [](const outpost_descriptor& op, const std::shared_ptr<sysio::outpost_client>& client) {
               return built_outpost_client{client, outpost_deployment{op}};
            });
         for (const unserviceable_chain& chain : unserviceable) {
            elog("batch_operator: underwriter cannot verify chain {}, its requests wait: {}", chain.code, chain.why);
         }
      } catch (const fc::exception& e) {
         wlog("batch_operator: underwriter could not refresh its outpost clients: {}", e.top_message());
      } catch (const std::exception& e) {
         wlog("batch_operator: underwriter could not refresh its outpost clients: {}", e.what());
      }
   }

   /// Everything a pass decides from but the verification, or nullopt when a read failed (logged by chain_plugin).
   std::optional<uw::plan_inputs> read_underwriter_state(const std::map<fc::slug_name, liq_token>& tokens,
                                                         fc::time_point now) {
      uw::plan_inputs in;
      in.now        = now;
      std::optional<std::vector<uw::request>> requests = read_bond_requests(underwriter_scan_from, symbols_of(tokens));
      if (!requests) return std::nullopt;
      in.requests = std::move(*requests);
      std::optional<std::map<uw::request_id_t, uw::bond_position>> bonds = read_own_bonds(in.requests);
      if (!bonds) return std::nullopt;
      in.bonds = std::move(*bonds);
      in.caps  = caps_by_token(tokens);
      std::optional<std::map<fc::slug_name, chain::asset>> balances = read_own_balances(tokens);
      if (!balances) return std::nullopt;
      in.balances                     = std::move(*balances);
      const std::optional<bool> frozen = read_cord_pulled();
      if (!frozen) return std::nullopt;
      in.frozen = *frozen;
      return in;
   }

   /// The symbol of every token a request can be bonded in: each `sysio.liq` shadow, and WIRE.
   static uw::token_symbols symbols_of(const std::map<fc::slug_name, liq_token>& tokens) {
      uw::token_symbols out{{uw::wire::token_code, uw::wire::asset_symbol}};
      for (const auto& [token_code, token] : tokens) out.emplace(token_code, token.symbol);
      return out;
   }

   /// Every `sysio.bond::requests` row from id `from` that decodes with `symbols`, or nullopt when the read failed.
   std::optional<std::vector<uw::request>> read_bond_requests(uw::request_id_t from, const uw::token_symbols& symbols) {
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(uw::bond::account);
      p.scope       = uw::bond::account;
      p.table       = uw::bond::table_requests;
      p.lower_bound = fc::json::to_string(fc::mutable_variant_object()(uw::bond::field::id, from),
                                          fc::json::yield_function_t{});
      p.all_rows    = true;
      p.values_only = true;
      std::optional<sysio::chain_apis::read_only::get_table_rows_result> read = read_table_checked(std::move(p));
      if (!read) return std::nullopt;
      std::vector<uw::request> out;
      size_t                   undecodable = 0;
      for (const fc::variant& r : read->rows) {
         std::optional<uw::request> decoded = uw::decode_request(r.get_object(), symbols);
         if (decoded) out.push_back(std::move(*decoded));
         else ++undecodable;
      }
      if (undecodable > 0) {
         elog("batch_operator: underwriter skips {} sysio.bond::requests row(s) that do not decode", undecodable);
      }
      return out;
   }

   /// The underwriter's `sysio.bond::bonds` rows by request id, each in its request's token, or nullopt when the read
   /// failed or one of them does not decode or names no request among `requests`: a bond it cannot read or attribute
   /// to a token must not drop out of its exposure.
   std::optional<std::map<uw::request_id_t, uw::bond_position>>
   read_own_bonds(const std::vector<uw::request>& requests) {
      const std::optional<fc::variants> rows =
         read_all_rows(uw::bond::account, uw::bond::account, uw::bond::table_bonds);
      if (!rows) return std::nullopt;
      std::map<uw::request_id_t, uw::bond_position> out;
      try {
         for (const fc::variant& r : *rows) {
            const std::optional<uw::bond_position> b =
               uw::decode_bond(r.get_object(), underwriter_auth.actor, requests);
            if (b) out.emplace(b->request_id, *b);
         }
      } catch (const fc::exception& e) {
         elog("batch_operator: underwriter skips this pass: {}", e.top_message());
         underwriter_scan_from = 0;   // read every request next pass, in case the scan window was what missed one
         return std::nullopt;
      }
      return out;
   }

   /// Every `sysio.liq` shadow by registry token code, or nullopt when the read failed.
   std::optional<std::map<fc::slug_name, liq_token>> read_liq_tokens() {
      namespace liq   = batch_operator_detail::liq;
      const std::optional<fc::variants> rows = read_all_rows(liq::account, liq::account, liq::table_stat);
      if (!rows) return std::nullopt;
      std::map<fc::slug_name, liq_token> out;
      size_t                             undecodable = 0;
      for (const fc::variant& r : *rows) {
         try {
            const fc::variant_object& row = r.get_object();
            out.emplace(row[liq::field::token_code].as<fc::slug_name>(),
                        liq_token{
                           .symbol     = chain::asset::from_string(row[liq::field::supply].as_string()).get_symbol(),
                           .chain_code = row[liq::field::chain_code].as<fc::slug_name>(),
                        });
         } catch (const fc::exception&) {
            ++undecodable;
         }
      }
      if (undecodable > 0) {
         elog("batch_operator: underwriter skips {} sysio.liq::stat row(s) that do not decode", undecodable);
      }
      return out;
   }

   /// The configured exposure caps keyed by registry token code. A cap whose precision differs from its shadow's is
   /// not applied, so that token's requests wait with no cap until the configuration is fixed; a cap that names no
   /// shadow is reported.
   std::map<fc::slug_name, chain::asset> caps_by_token(const std::map<fc::slug_name, liq_token>& tokens) {
      std::map<fc::slug_name, chain::asset> out;
      std::set<chain::symbol_code>          named;   // the caps that name a shadow
      for (const auto& [token_code, token] : tokens) {
         const auto cap = underwriter_caps.find(token.symbol.to_symbol_code());
         if (cap == underwriter_caps.end()) continue;
         named.insert(cap->first);
         if (cap->second.get_symbol() != token.symbol) {
            elog("batch_operator: batch-underwriter-max-exposure {} does not match the shadow's symbol {}; not "
                 "underwriting it", cap->second.to_string(), token.symbol.to_string());
            continue;
         }
         out.emplace(token_code, cap->second);
      }
      for (const auto& [code, cap] : underwriter_caps) {
         if (named.contains(code)) continue;
         wlog("batch_operator: batch-underwriter-max-exposure {} names no sysio.liq shadow", cap.to_string());
      }
      return out;
   }

   /// The underwriter's `sysio.liq` balances by registry token code, or nullopt when the read failed.
   std::optional<std::map<fc::slug_name, chain::asset>>
   read_own_balances(const std::map<fc::slug_name, liq_token>& tokens) {
      namespace liq = batch_operator_detail::liq;
      const std::optional<fc::variants> rows =
         read_all_rows(liq::account, underwriter_auth.actor.to_string(), liq::table_accounts);
      if (!rows) return std::nullopt;
      std::map<chain::symbol_code, chain::asset> by_symbol;
      for (const fc::variant& r : *rows) {
         try {
            const chain::asset held = chain::asset::from_string(r.get_object()[liq::field::balance].as_string());
            by_symbol.insert_or_assign(held.get_symbol().to_symbol_code(), held);
         } catch (const fc::exception&) {
            // A row that does not decode is no balance to bond from.
         }
      }
      std::map<fc::slug_name, chain::asset> out;
      for (const auto& [token_code, token] : tokens) {
         const auto held = by_symbol.find(token.symbol.to_symbol_code());
         if (held != by_symbol.end() && held->second.get_symbol() == token.symbol && held->second.get_amount() > 0) {
            out.emplace(token_code, held->second);
         }
      }
      return out;
   }

   /// Whether the `sysio.andon` cord is pulled, or nullopt when that cannot be read. Clear until `sysio.andon` runs
   /// code, and while it holds no cord row.
   std::optional<bool> read_cord_pulled() {
      if (!andon_deployed) return false;
      sysio::chain_apis::read_only::get_table_rows_params p;
      p.code        = chain::name(uw::andon::account);
      p.scope       = uw::andon::account;
      p.table       = uw::andon::table_cord;
      p.limit       = 1;
      p.values_only = true;
      const std::optional<sysio::chain_apis::read_only::get_table_rows_result> rows = read_table_checked(std::move(p));
      if (!rows) return std::nullopt;
      if (rows->rows.empty()) return false;
      try {
         return rows->rows.front().get_object()[uw::andon::field::pulled].as_bool();
      } catch (const fc::exception&) {
         elog("batch_operator: underwriter pauses: the sysio.andon cord does not decode");
         return std::nullopt;
      }
   }

   /// The digest each outpost recorded for the envelope it emitted, for every (chain, epoch) an OPEN envelope request
   /// names, read at finality. A chain whose read fails is reported and leaves only its own requests waiting.
   std::map<uw::envelope_key, fc::sha256> read_emitted_digests(const std::vector<uw::request>& requests) {
      return uw::collect_emitted(
         uw::wanted_envelopes(requests),
         [&](fc::slug_name chain_code, uint32_t epoch) -> std::optional<fc::sha256> {
            if (shutting_down) return std::nullopt;
            const auto client = underwriter_clients.find(chain_code);
            FC_ASSERT(client != underwriter_clients.end(), "this node has no client for it");
            return client->second.client->read_emitted_envelope_digest(epoch, delivery_timeout);
         },
         [](fc::slug_name chain_code, const std::string& why) { report_unreadable(chain_code, why); });
   }

   /// Report that `chain_code`'s outpost could not be read this pass, and why.
   static void report_unreadable(fc::slug_name chain_code, std::string_view why) {
      wlog("batch_operator: underwriter cannot read outpost {}, its requests wait: {}", chain_code.to_string(), why);
   }

   // -----------------------------------------------------------------------
   //  Helpers
   // -----------------------------------------------------------------------

   /// An unsigned depot transaction and the digest its signature covers.
   struct unsigned_trx {
      chain::signed_transaction trx;
      chain::digest_type        digest;
   };

   /// `contract::action_name` declaring `auth`, unsigned, against the current head. Reads chain state (the ABI, the
   /// head): call on the main thread or in a read window. Nullopt, logged, when it cannot be built.
   std::optional<unsigned_trx> build_unsigned(const std::string& contract, const std::string& action_name,
                                              const chain::permission_level& auth, const fc::variant_object& data) {
      const chain::controller& chain = chain_plug->chain();
      try {
         auto resolver = make_resolver(chain, delivery_timeout, throw_on_yield::no);
         std::optional<chain::abi_serializer> abis_opt = resolver(chain::name(contract));
         if (!abis_opt) {
            elog("batch_operator: no ABI found for {}", contract);
            return std::nullopt;
         }
         const chain::type_name action_type = abis_opt->get_action_type(chain::name(action_name));
         chain::bytes action_data = abis_opt->variant_to_binary(
            action_type, fc::variant(data), chain::abi_serializer::create_yield_function(delivery_timeout));
         unsigned_trx out;
         out.trx.actions.emplace_back(std::vector<chain::permission_level>{auth}, chain::name(contract),
                                      chain::name(action_name), std::move(action_data));
         out.trx.set_reference_block(chain.head().id());
         out.trx.expiration = fc::time_point_sec(chain.head().block_time() + PUSH_EXPIRATION);
         out.digest         = out.trx.sig_digest(chain.get_chain_id(), out.trx.context_free_data);
         return out;
      } catch (const fc::exception& e) {
         elog("batch_operator: cannot build {}::{}: {}", contract, action_name, e.to_string());
      } catch (const std::exception& e) {
         elog("batch_operator: cannot build {}::{}: {}", contract, action_name, e.what());
      }
      return std::nullopt;
   }

   /// Submit a signed transaction through `read_write::push_transaction`, completing `completion`. Resolves ABIs:
   /// call on the main thread.
   void submit(const std::shared_ptr<batch_operator_detail::async_action_completion>& completion,
               const std::string& contract, const std::string& action_name, const fc::variant& packed_var) {
      try {
         std::shared_ptr<read_write> rw =
            std::make_shared<read_write>(chain_plug->get_read_write_api(delivery_timeout));
         rw->push_transaction(
            packed_var.get_object(),
            batch_operator_detail::create_push_action_callback(rw, completion, contract, action_name));
      } catch (const fc::exception& e) {
         completion->complete([&] { elog(batch_operator_detail::push_action_log::failure, contract, action_name,
                                         e.to_string()); });
      } catch (const std::exception& e) {
         completion->complete([&] { elog(batch_operator_detail::push_action_log::failure, contract, action_name,
                                         e.what()); });
      }
   }

   /// Whether `future` is ready by `deadline`.
   template <typename T>
   static bool ready_by(const std::future<T>& future, fc::time_point deadline) {
      const fc::microseconds left = std::max(deadline - fc::time_point::now(), fc::microseconds(0));
      return future.wait_for(std::chrono::microseconds(left.count())) == std::future_status::ready;
   }

   /// Build, sign and submit a depot action, declaring `by.auth` and signed by `by.provider`. True when the node
   /// accepted the transaction within `delivery_timeout`; a refusal is logged by the push callback.
   ///
   /// Called from cron threads, which may not read chain state: the build runs in a read window and the submission
   /// on the main thread, while the signature, possibly a remote signer, stays here. The waits do not cancel
   /// anything: each posted task owns what it uses, so returning on a timeout is safe.
   bool push_action(const std::string& contract,
                    const std::string& action_name,
                    const signer& by,
                    const fc::variant_object& data) {
      if (shutting_down) return false;
      const fc::time_point deadline = fc::time_point::now() + delivery_timeout;
      const bool           on_main  = std::this_thread::get_id() == app().executor().get_main_thread_id();

      std::optional<unsigned_trx> trx;
      if (on_main) {
         trx = build_unsigned(contract, action_name, by.auth, data);
      } else {
         std::shared_ptr<std::promise<std::optional<unsigned_trx>>> built =
            std::make_shared<std::promise<std::optional<unsigned_trx>>>();
         std::future<std::optional<unsigned_trx>> built_future = built->get_future();
         // Shutdown is checked again when the task runs, since chain state may be going away by then.
         app().executor().post(appbase::priority::medium, appbase::exec_queue::read_only,
                               [this, built, contract, action_name, auth = by.auth, data = fc::variant_object(data)] {
                                  if (shutting_down) {
                                     built->set_value(std::nullopt);
                                     return;
                                  }
                                  built->set_value(build_unsigned(contract, action_name, auth, data));
                               });
         if (!ready_by(built_future, deadline)) {
            elog("batch_operator: push {}::{} timed out before signing", contract, action_name);
            return false;
         }
         trx = built_future.get();
      }
      if (!trx) return false;
      trx->trx.signatures.push_back(by.provider->sign(trx->digest));

      fc::variant packed_var;
      chain::to_variant(
         chain::packed_transaction(std::move(trx->trx), chain::packed_transaction::compression_type::none),
         packed_var);
      std::shared_ptr<batch_operator_detail::async_action_completion> completion =
         std::make_shared<batch_operator_detail::async_action_completion>();
      std::future<void> future = completion->get_future();
      if (on_main) {
         submit(completion, contract, action_name, packed_var);
      } else {
         app().executor().post(appbase::priority::medium, appbase::exec_queue::read_write,
                               [this, completion, contract, action_name, packed_var = std::move(packed_var)] {
                                  submit(completion, contract, action_name, packed_var);
                               });
      }
      if (!ready_by(future, deadline)) {
         elog("batch_operator: push {}::{} timed out", contract, action_name);
         return false;
      }
      return completion->succeeded();
   }

   // -----------------------------------------------------------------------
   //  Sync-gated startup
   // -----------------------------------------------------------------------

   /// The startup body deferred behind the sync gate: signer resolution, the relay's outpost discovery, private
   /// cron_service creation (sized from the discovered outposts), the relay's epoch_tick and the underwriter's poll,
   /// then the per-outpost relay jobs. Runs on the main thread from {@link run_deferred_startup_or_quit} once the
   /// node is synced. Deferral exists because both read chain state LOCALLY (each role's authority and links,
   /// `sysio.chains`): on a cold-booting operator node still replaying toward the deploy blocks those reads fail
   /// spuriously.
   void run_deferred_startup() {
      if (shutting_down) {
         return;
      }

      // Each role pushes as its own account. A role whose key cannot sign for it, or whose permission may not
      // authorize its actions, would only fail every push, so stop here instead, before anything is scheduled.
      if (!resolve_signers()) {
         elog("batch_operator_plugin: a configured role cannot sign its actions, shutting down node (fail-fast)");
         app().quit();
         return;
      }

      // Discover outposts before the private cron_service starts. Later refresh
      // ticks add/remove per-outpost cron jobs as the active chain set changes.
      if (enabled) {
         try {
            refresh_outposts();
         } catch (const fc::exception& e) {
            wlog("batch_operator_plugin: initial outpost discovery failed: {}. "
                 "Starting with 0 per-outpost jobs; refresh ticks will retry after "
                 "the chain has caught up.", e.to_string());
         }
      }

      // Size the pool for startup outposts. Later dynamic outposts are added to
      // the same queued cron service; the minimum keeps the polls viable even
      // when no outposts are known yet.
      const std::size_t outpost_count    = opp_jobs.size();
      const std::size_t relay_jobs =
         enabled ? outpost_count * OPP_CRON_JOBS_PER_OUTPOST + EPOCH_TICK_CRON_JOBS : 0;
      const std::size_t underwriter_jobs = underwriter_enabled ? UNDERWRITER_CRON_JOBS : 0;
      const std::size_t thread_count     = std::max(relay_jobs + underwriter_jobs, MIN_CRON_THREADS);

      sysio::services::cron_service::options svc_opts;
      svc_opts.name        = "batch_operator";
      svc_opts.num_threads = thread_count;
      svc_opts.autostart   = true;

      cron_svc = sysio::services::cron_service::create(svc_opts);
      ilog("batch_operator_plugin: cron_service started with {} thread(s) ({} outpost(s) discovered)",
           thread_count, outpost_count);

      const uint32_t poll_ms = whole_milliseconds(epoch_poll);
      auto schedule_poll = [&](std::string label, std::function<void()> poll) {
         sysio::services::cron_service::job_schedule sched;
         sched.milliseconds = {sysio::services::cron_service::job_schedule::step_value{poll_ms}};
         sysio::services::cron_service::job_metadata_t meta;
         meta.label          = std::move(label);
         meta.one_at_a_time  = true;
         sysio::services::cron_service::job_id_t id = cron_svc->add(sched, std::move(poll), meta);
         cron_job_ids.push_back(id);
         ilog("batch_operator_plugin: scheduled {} (id={}, every {}ms)", meta.label, id, poll_ms);
      };

      // epoch_tick: refresh epoch state + election. Keeps `current_epoch`
      // and `within_epoch_window` accurate for every per-outpost job.
      if (enabled) schedule_poll("batch_operator_epoch_tick", [this]() { poll_epoch_state(); });
      if (underwriter_enabled) schedule_poll("batch_operator_underwriter", [this]() { underwriter_tick(); });

      if (enabled) schedule_opp_jobs();
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
   // Presence of this option IS the enable switch (mirrors producer_plugin's
   // producer-name): the relay runs when an account is configured. Keying off
   // the account rather than the plugin being listed matters because
   // external_debugging_plugin declares this plugin as a dependency, which
   // would otherwise silently promote a debug node to a batch operator.
   opts("batch-operator-account", bpo::value<std::string>(),
        "WIRE account name for this batch operator. Configuring an account enables the relay.");
   opts("batch-epoch-poll-ms", bpo::value<uint32_t>()->default_value(whole_milliseconds(EPOCH_POLL)),
        "How often to check epoch state (ms)");
   // SIZING RULE for batch-delivery-timeout-ms: it bounds the WHOLE outbound
   // delivery, and an Ethereum delivery is now one transaction PER CHUNK
   // (ETHEREUM_MAX_CHUNK_BYTES = 8192; Solana chunks at 672). Size it as
   //     total chunks x (target block time + confirmation margin)
   // for the largest envelope the outpost is expected to carry. At the 32768
   // byte platform envelope cap that is 4 Ethereum chunks, so the 15000 ms
   // default covers a 1 s block time with wide margin. Undersizing is not fatal:
   // the relay resumes from the on-chain per-operator high-water mark on the
   // next cron tick, so a truncated tick converges rather than restarting from
   // chunk zero. SHARED KNOB: this same value also bounds the depot
   // push_action completion wait (see push_action above); post-WIRE-331 a
   // timeout there is safe (the async_action_completion callback retains its
   // own state), but raising this option lengthens BOTH waits, so raise it for
   // chunking deliberately rather than as a blanket tuning step.
   // Help text below must stay ASCII with no " --" sequence (PerformanceHarness
   // splits nodeop --help output on that token).
   opts("batch-delivery-timeout-ms", bpo::value<uint32_t>()->default_value(whole_milliseconds(DELIVERY_TIMEOUT)),
        "Max time to wait for chain delivery confirmation (ms)");
   opts("batch-yield-tick-interval-ms",
        bpo::value<uint32_t>()->default_value(whole_milliseconds(YIELD_TICK_INTERVAL)),
        "Minimum spacing between this operator's sysio.swap::tickyield pushes per yield pool (ms)");
   // Same presence-is-the-switch rule as batch-operator-account, independently of it: a node may run the
   // underwriter, the relay, or both.
   opts("batch-underwriter-account", bpo::value<std::string>(),
        "WIRE account the underwriter bonds sysio.synd envelope requests from, as account or "
        "account@permission (default active). Configuring it enables the underwriter, with or without the relay. "
        "It signs with the one configured WIRE signature provider whose key alone satisfies that permission.");
   opts("batch-underwriter-max-exposure", bpo::value<std::vector<std::string>>()->composing(),
        "Most the underwriter may have bonded at once in one shadow token, as an asset such as "
        "100.000000000 LIQETH. Repeat for each token; requests in a token without one are not bonded.");
}

void batch_operator_plugin::plugin_initialize(const variables_map& options) {
   if (options.count("batch-operator-account"))
      _impl->operator_account = chain::name(options["batch-operator-account"].as<std::string>());
   _impl->epoch_poll          = fc::milliseconds(options["batch-epoch-poll-ms"].as<uint32_t>());
   _impl->delivery_timeout    = fc::milliseconds(options["batch-delivery-timeout-ms"].as<uint32_t>());
   _impl->yield_tick_interval = fc::milliseconds(options["batch-yield-tick-interval-ms"].as<uint32_t>());
   _impl->enabled             = _impl->operator_account.good();
   if (options.count("batch-underwriter-account")) {
      const std::string& spec = options["batch-underwriter-account"].as<std::string>();
      try {
         _impl->underwriter_auth = batch_operator_detail::parse_permission_level(spec);
      } FC_RETHROW_EXCEPTIONS(error, "invalid batch-underwriter-account '{}'", spec)
   }
   _impl->underwriter_enabled = _impl->underwriter_auth.actor.good();
   if (options.count("batch-underwriter-max-exposure")) {
      try {
         _impl->underwriter_caps = batch_operator_detail::parse_exposure_caps(
            options["batch-underwriter-max-exposure"].as<std::vector<std::string>>());
      } FC_RETHROW_EXCEPTIONS(error, "invalid batch-underwriter-max-exposure")
   }
   if (_impl->underwriter_enabled && _impl->underwriter_caps.empty()) {
      wlog("batch_operator_plugin: batch-underwriter-account without batch-underwriter-max-exposure bonds nothing");
   }
   if (!_impl->underwriter_enabled && !_impl->underwriter_caps.empty()) {
      wlog("batch_operator_plugin: batch-underwriter-max-exposure has no effect without batch-underwriter-account");
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
   FC_ASSERT(!(_impl->enabled || _impl->underwriter_enabled) ||
                _impl->chain_plug->chain().get_read_mode() == chain::db_read_mode::IRREVERSIBLE,
             "batch_operator_plugin requires read-mode = irreversible");

   // A failed push is billed subjectively to its authorizer, and both roles push on a schedule that loses some
   // pushes (a race another operator won, a dispute already resolved, an action the irreversible view does not show
   // done yet). Billed, an account would soon have every transaction refused by its own node, so the node exempts
   // the accounts it signs as.
   chain::subjective_billing& billing = _impl->chain_plug->chain().get_mutable_subjective_billing();
   for (const chain::name account : {_impl->operator_account, _impl->underwriter_auth.actor}) {
      if (!account.good()) continue;
      billing.disable_account(account);
      ilog("batch_operator_plugin: {} is exempt from subjective CPU billing on this node", account.to_string());
   }
}

void batch_operator_plugin::plugin_startup() {
   if (!_impl->enabled && !_impl->underwriter_enabled) {
      ilog("batch_operator_plugin: neither batch-operator-account nor batch-underwriter-account configured, "
           "skipping startup");
      return;
   }

   if (_impl->enabled)
      ilog("batch_operator_plugin: starting the relay for account {}", _impl->operator_account.to_string());
   if (_impl->underwriter_enabled)
      ilog("batch_operator_plugin: starting the underwriter for {}@{}", _impl->underwriter_auth.actor.to_string(),
           _impl->underwriter_auth.permission.to_string());

   // The startup body's signer resolution and outpost discovery read chain
   // state LOCALLY. On a cold-booting operator node those reads see mid-sync
   // (possibly genesis) state and fail spuriously, so the whole body (signers,
   // discovery, cron_service, the polls, relay jobs) is DEFERRED until the
   // node is synced, per
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
   // released by the next LIB advance. That advance is also the relay's WORK
   // SUPPLY — everything here acts on newly-finalized state — so a finality
   // stall that spans startup (the classic lost-wakeup case) is one with
   // nothing to relay: the first finalized block re-arms the gate and
   // creates the first actionable state at the same instant.
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
         [impl = _impl.get()](const chain::plugin_interface::channels::block_params&) {
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
