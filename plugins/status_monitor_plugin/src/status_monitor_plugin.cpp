#include <cassert>
#include <cstdint>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/es/es_client.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h> // fmt::join
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/status_monitor_plugin/status_monitor.hpp>
#include <sysio/status_monitor_plugin/status_monitor_plugin.hpp>
#include <thread>
#include <utility>

namespace sysio {

namespace {
/// Budget handed to chain_plugin::get_read_only_api. get_info() answers from get_info_db's cache and ignores
/// its deadline; the value only shapes deadlines of HTTP-style calls this plugin never makes.
constexpr fc::microseconds read_only_api_response_budget = fc::seconds(1);
/// While the sync gate keeps pausing documents, the pause is re-announced at most this often, so lagging finality or
/// a host clock running ahead of the chain (either can pause documents indefinitely) stays visible in the log. A
/// halted LIB publishes no irreversible_block, so it is not logged here.
constexpr fc::microseconds pause_report_interval = fc::seconds(60);
/// The delivery failure line and the delivery recovery line share this interval: whichever one is reported,
/// the next line of either kind waits this long, so an endpoint alternating between failed and acknowledged
/// batches cannot log a line per irreversible block. The per-batch failure line uses it on its own clock.
constexpr fc::microseconds failure_report_interval = fc::seconds(60);
/// The line logged for a bulk request that did not fully index: the attempts the client spent, its
/// endpoint-sanitized detail, and what became of the batch's documents (see delivery_outcome_clause).
constexpr std::string_view failed_batch_format = "bulk request failed after {} attempt(s): {} -- {}";
/// failed_batch_format plus the number of failed batches not logged since the previous line.
constexpr std::string_view failed_batch_with_unlogged_format =
   "bulk request failed after {} attempt(s): {} -- {} ({} more failed batch(es) since the last line)";

/// What became of one failed batch's documents: a partial batch had only its rejected documents refused (the
/// rest did index), a canceled batch was abandoned at shutdown, and every other outcome loses the whole batch
/// once the client's retries are exhausted.
std::string delivery_outcome_clause(const fc::network::es::es_bulk_result& result, uint32_t doc_count) {
   using status = fc::network::es::es_bulk_result::status;
   switch (result.outcome) {
   case status::partial:
      return fmt::format("{} of {} document(s) rejected", result.failed_docs, doc_count);
   case status::canceled:
      return fmt::format("{} document(s) canceled", doc_count);
   // `indexed` never reaches the reporter; `rejected` and `unavailable` lose the batch outright.
   case status::indexed:
   case status::rejected:
   case status::unavailable:
      break;
   }
   return fmt::format("{} document(s) dropped", doc_count);
}
} // anonymous namespace

/// Every member is touched only on the application thread: plugin_initialize/startup/shutdown run there,
/// SIGHUP handlers are dispatched through the executor's read_write queue, and irreversible_block channel
/// deliveries are posted to it. The workers see only the pipeline's own state (its counters and last-failure
/// detail, under its mutex) and the es client, whose bulk() runs on the delivery worker and whose cancel() is
/// safe from any thread. No lock is needed here; assert_application_thread() documents and (in debug)
/// checks it. The exception is report_failed_batch()'s two throttle members, which only the delivery worker
/// touches.
///
/// The declaration order of the last four members is load-bearing: they are destroyed in reverse --
/// subscription, then pipeline, then client, then the read-only handle -- which is the only safe order, since
/// nothing can be queued once the subscription is gone, the workers are joined before the client whose bulk()
/// they call, and the client's io thread stops after both. plugin_shutdown() only stops them; it destroys
/// nothing. `log` is declared FIRST for the same reason from the other end: it outlives the pipeline, whose
/// delivery worker logs through it (via report_failed_batch(), the failure reporter) right up until the join
/// inside ~pipeline() completes.
struct status_monitor_plugin::impl {
   fc::logger log;                            ///< the `status_monitor` logger
   std::optional<status_monitor::config> cfg; ///< empty: disabled
   chain_plugin* chain_plug = nullptr;        ///< set at startup; outlives this plugin's shutdown
   bool catching_up = false;                  ///< the gate is pausing documents
   fc::time_point last_pause_report;          ///< when the pause was last announced
   /// LIB of the last submitted snapshot; a delivery that finds no newer LIB submits nothing
   chain::block_num_type last_submitted_lib = 0;
   bool failing = false;                      ///< a delivery-failure streak is open
   fc::time_point last_failure_report;        ///< when a failure or a recovery was last reported
   status_monitor::pipeline_stats last_stats; ///< counters as last compared
   /// When a failed batch was last logged. Delivery worker only, like report_failed_batch(); declared before the
   /// pipeline so it outlives the worker.
   fc::time_point last_failed_batch_report;
   /// Failed batches not logged since last_failed_batch_report. Delivery worker only.
   uint64_t unlogged_failed_batches = 0;
   /// chain_plugin's read-only API (get_info). It references chain_plugin's internals, which outlive it only
   /// because appbase shuts plugins down in reverse startup order: this plugin depends on chain_plugin, so
   /// plugin_shutdown resets the handle before chain_plugin's own shutdown runs.
   std::optional<chain_apis::read_only> read_only_api;
   std::unique_ptr<fc::network::es::es_client> client; ///< created before the pipeline; canceled before it stops
   std::unique_ptr<status_monitor::pipeline> pipeline; ///< the two workers
   /// Declared last, so it is released first (see above).
   chain::plugin_interface::channels::irreversible_block::channel_type::handle irreversible_block_subscription;

   /** Cancels the client first, so ~pipeline()'s joins stay bounded however the plugin is destroyed. */
   ~impl();

   /** Debug check of the single-thread invariant stated on this struct. */
   void assert_application_thread() const;
   /** Channel delivery: report counter changes, then queue a get_info snapshot if LIB advanced since the last. */
   void on_irreversible_block();
   /** Compare the pipeline's counters with the last comparison and log a failing streak or a recovery. */
   void report_progress(fc::time_point now);
   /**
    * Log a bulk request that did not fully index: the first one, then at most one per failure_report_interval
    * carrying the count of failed batches in between. Unlike every other member function this runs on the
    * pipeline's delivery worker and touches only the logger, which handle_sighup() re-binds on the application
    * thread -- the same arrangement every plugin that logs off the application thread uses -- and its own two
    * throttle members. It only logs and counts, so it does not throw, as pipeline::failure_reporter requires.
    */
   void report_failed_batch(const fc::network::es::es_bulk_result& result, uint32_t doc_count);
};

status_monitor_plugin::impl::~impl() {
   // Covers destruction on a path that never ran plugin_shutdown() (a directly constructed plugin, as in the
   // tests): a delivery worker inside es_client::bulk() would otherwise hold ~pipeline()'s join for the whole
   // retry budget against a dead endpoint.
   if (client)
      client->cancel();
}

void status_monitor_plugin::impl::assert_application_thread() const {
   // get_main_thread_id() is captured when the executor is constructed and refreshed by exec(); nodeop
   // constructs, initializes, starts, and runs the application on one thread, so it holds at every call site.
   assert(std::this_thread::get_id() == appbase::app().executor().get_main_thread_id());
}

void status_monitor_plugin::impl::on_irreversible_block() {
   assert_application_thread();
   try {
      const fc::time_point now = fc::time_point::now();
      // Before the gate, so failures and drops accumulated while paused are still reported at LIB cadence; this
      // delivery's own submit outcome is reported on the next one.
      report_progress(now);
      const auto& chain = chain_plug->chain();
      if (!chain.is_synced()) {
         // Syncing, finality lagging, or a host clock running ahead of the chain; the last two can last
         // indefinitely, so the pause is re-announced every pause_report_interval.
         if (!catching_up || now - last_pause_report >= pause_report_interval) {
            catching_up = true;
            last_pause_report = now;
            if (chain.fork_db_has_root()) {
               const auto lib = chain.fork_db_root();
               fc_wlog(log,
                       "irreversible block {} is {} s behind wall clock; status documents paused "
                       "(node syncing, finality lagging, or the host clock is ahead of the chain)",
                       lib.block_num(), (now - lib.block_time()).to_seconds());
            }
         }
         return;
      }
      if (catching_up) {
         catching_up = false;
         fc_ilog(log, "irreversible block is current; resuming status documents");
      }
      auto info = read_only_api->get_info({}, fc::time_point::maximum());
      // Deliveries queued behind a multi-block LIB advance all find get_info at the newest LIB; only the first
      // submits, so the burst yields one document.
      if (info.last_irreversible_block_num <= last_submitted_lib)
         return;
      last_submitted_lib = info.last_irreversible_block_num;
      // A full render queue is counted by the pipeline and surfaces through the next report_progress().
      pipeline->submit(status_monitor::status_snapshot{std::move(info), now});
   }
   FC_LOG_AND_DROP("status snapshot dropped");
}

void status_monitor_plugin::impl::report_progress(fc::time_point now) {
   const auto current = pipeline->stats();
   // Both report kinds are gated on the one timestamp, so an endpoint alternating between failed and
   // acknowledged batches -- every irreversible block opening a fresh streak -- cannot log a line per block:
   // whichever line is emitted silences both for failure_report_interval. last_failure_report starts at the
   // epoch, so the first report after a quiet period is always due. A streak ends only when its recovery line
   // is logged, so a recovery inside the interval is reported on the first acknowledged batch after the interval.
   const bool report_due = now - last_failure_report >= failure_report_interval;
   switch (status_monitor::classify_progress(last_stats, current, failing)) {
   case status_monitor::progress::failing:
      failing = true;
      if (report_due) {
         last_failure_report = now;
         fc_wlog(log,
                 "delivery problems -- batches_failed={} documents_failed={} "
                 "snapshots_dropped_queue_full={} documents_dropped_queue_full={} documents_dropped_oversize={} "
                 "render_failures={} documents_indexed={}; last failure: {}",
                 current.batches_failed, current.documents_failed, current.snapshots_dropped_queue_full,
                 current.documents_dropped_queue_full, current.documents_dropped_oversize, current.render_failures,
                 current.documents_indexed, pipeline->last_failure());
      }
      break;
   case status_monitor::progress::recovered:
      if (report_due) {
         failing = false;
         last_failure_report = now;
         fc_ilog(log, "delivery recovered (documents_indexed={} batches_failed={})", current.documents_indexed,
                 current.batches_failed);
      }
      break;
   case status_monitor::progress::steady:
      break;
   }
   last_stats = current;
}

void status_monitor_plugin::impl::report_failed_batch(const fc::network::es::es_bulk_result& result,
                                                      uint32_t doc_count) {
   // Throttled so an endpoint that fails every batch cannot log a line per block; the summary in
   // report_progress() still carries the counters and the last detail.
   const fc::time_point now = fc::time_point::now();
   if (now - last_failed_batch_report < failure_report_interval) {
      ++unlogged_failed_batches;
      return;
   }
   last_failed_batch_report = now;
   const uint64_t unlogged = std::exchange(unlogged_failed_batches, 0);
   if (unlogged == 0) {
      fc_wlog(log, failed_batch_format, result.attempts, result.detail, delivery_outcome_clause(result, doc_count));
   } else {
      fc_wlog(log, failed_batch_with_unlogged_format, result.attempts, result.detail,
              delivery_outcome_clause(result, doc_count), unlogged);
   }
}

status_monitor_plugin::status_monitor_plugin()
   : _impl(std::make_unique<impl>()) {}

status_monitor_plugin::~status_monitor_plugin() = default;

void status_monitor_plugin::set_program_options(options_description&, options_description& cfg) {
   status_monitor::add_options(cfg);
}

void status_monitor_plugin::plugin_initialize(const variables_map& options) {
   try {
      _impl->cfg = status_monitor::parse_config(options);
      handle_sighup(); // bind the diagnostic logger, the convention every plugin follows at the end of initialize
   }
   FC_LOG_AND_RETHROW()
}

void status_monitor_plugin::plugin_startup() {
   if (!_impl->cfg) {
      fc_ilog(_impl->log, "no --{} provided, disabled", status_monitor::option::target_url);
      return;
   }
   try {
      auto& chain_plug = app().get_plugin<chain_plugin>();
      _impl->chain_plug = &chain_plug;
      _impl->read_only_api.emplace(chain_plug.get_read_only_api(read_only_api_response_budget));
      _impl->client = std::make_unique<fc::network::es::es_client>(_impl->cfg->delivery, _impl->cfg->transport);
      // Before anything is started: an endpoint that cannot be reached now would silently drop every document,
      // so it fails startup instead. One attempt, no retry -- the retry budget is for a running node.
      try {
         _impl->client->probe();
      } catch (const fc::exception& e) {
         // The client's own message already names the probed endpoint and why it is not reachable; wrapping it
         // in a second sentence of the same shape only says it twice. The outer FC_LOG_AND_RETHROW still logs
         // the exception's detail.
         fc_elog(_impl->log, "{}", e.top_message());
         throw;
      }
      _impl->pipeline = std::make_unique<status_monitor::pipeline>(
         *_impl->cfg, _impl->client->action_line(),
         [client = _impl->client.get()](std::string body, uint32_t doc_count) {
            return client->bulk(std::move(body), doc_count);
         },
         [impl = _impl.get()](const fc::network::es::es_bulk_result& result, uint32_t doc_count) {
            impl->report_failed_batch(result, doc_count);
         });
      // A channel delivery runs on the application thread after the block commits. The payload is unused: the
      // document comes from get_info(), which already holds the newest LIB when the delivery runs.
      _impl->irreversible_block_subscription =
         app().get_channel<chain::plugin_interface::channels::irreversible_block>().subscribe(
            [impl = _impl.get()](const chain::plugin_interface::channels::block_params&) {
               impl->on_irreversible_block();
            });
      fc_ilog(_impl->log,
              "endpoint {} reachable; writing one get_info document per LIB advance to index '{}' "
              "(template tokens: {}; max_items_per_task={}, max_pending_documents={})",
              _impl->client->endpoint(), _impl->cfg->delivery.index,
              fmt::join(_impl->cfg->document_template.tokens(), status_monitor::token_list_separator),
              _impl->cfg->max_items_per_task, _impl->cfg->max_pending_documents);
   }
   FC_LOG_AND_RETHROW()
}

void status_monitor_plugin::plugin_shutdown() {
   // Unsubscribe first so no delivery, even one already queued, submits again; cancel the client so an in-flight
   // request or backoff returns at once (the join below is then bounded); stop the workers. Nothing is destroyed
   // here: the pipeline and the client go when impl does, in the reverse declaration order this sequence follows.
   _impl->irreversible_block_subscription.unsubscribe();
   if (_impl->client)
      _impl->client->cancel();
   if (_impl->pipeline) {
      _impl->pipeline->shutdown();
      const auto final_stats = _impl->pipeline->stats();
      fc_ilog(_impl->log,
              "shutdown -- documents_indexed={} documents_failed={} batches_indexed={} "
              "batches_failed={} snapshots_dropped_queue_full={} documents_dropped_queue_full={}",
              final_stats.documents_indexed, final_stats.documents_failed, final_stats.batches_indexed,
              final_stats.batches_failed, final_stats.snapshots_dropped_queue_full,
              final_stats.documents_dropped_queue_full);
   }
   // The read-only handle reaches into chain_plugin, which shuts down after this plugin, so it is released
   // here rather than at destruction.
   _impl->read_only_api.reset();
}

void status_monitor_plugin::handle_sighup() {
   fc::logger::update(std::string{status_monitor::logger_name}, _impl->log);
}

} // namespace sysio
