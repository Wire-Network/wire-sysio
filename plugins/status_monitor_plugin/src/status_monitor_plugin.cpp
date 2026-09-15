#include <boost/signals2/connection.hpp>
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
#include <sysio/status_monitor_plugin/status_monitor.hpp>
#include <sysio/status_monitor_plugin/status_monitor_plugin.hpp>
#include <thread>
#include <tuple>
#include <utility>

namespace sysio {

using boost::signals2::scoped_connection;

namespace {
/// Budget handed to chain_plugin::get_read_only_api. get_info() answers from get_info_db's cache and ignores
/// its deadline; the value only shapes deadlines of HTTP-style calls this plugin never makes.
constexpr fc::microseconds read_only_api_response_budget = fc::seconds(1);
/// While the catch-up gate or the snapshot check keeps pausing documents, the pause is re-announced at most this
/// often, so a host clock running ahead of the chain or a persistent snapshot mismatch (either pauses documents
/// indefinitely) stays visible in the log.
constexpr fc::microseconds pause_report_interval = fc::seconds(60);
/// The delivery failure line and the delivery recovery line share this interval: whichever one is reported,
/// the next line of either kind waits this long, so an endpoint alternating between failed and acknowledged
/// batches cannot log a line per irreversible block.
constexpr fc::microseconds failure_report_interval = fc::seconds(60);
/// The line logged for every bulk request that did not fully index: the attempts the client spent, its
/// endpoint-sanitized detail, and what became of the batch's documents (see delivery_outcome_clause).
constexpr std::string_view failed_batch_format = "bulk request failed after {} attempt(s): {} -- {}";

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
/// SIGHUP handlers are dispatched through the executor's read_write queue, and irreversible_block is emitted
/// from block application. The workers see only the pipeline's own state (its atomics, its last-failure
/// string under its mutex) and the es client, whose bulk() runs on the delivery worker and whose cancel() is
/// safe from any thread. No lock is needed here; assert_application_thread() documents and (in debug)
/// checks it.
///
/// The declaration order of the last four members is load-bearing: they are destroyed in reverse --
/// connection, then pipeline, then client, then the read-only handle -- which is the only safe order, since
/// nothing can be queued once the connection is gone, the workers are joined before the client whose bulk()
/// they call, and the client's io thread stops after both. plugin_shutdown() only stops them; it destroys
/// nothing. `log` is declared FIRST for the same reason from the other end: it outlives the pipeline, whose
/// delivery worker logs through it (via report_failed_batch(), the failure reporter) right up until the join
/// inside ~pipeline() completes.
struct status_monitor_plugin::impl {
   fc::logger log;                            ///< the `status_monitor` logger
   std::optional<status_monitor::config> cfg; ///< empty: disabled
   bool catching_up = false;                  ///< the gate is pausing documents
   fc::time_point last_pause_report;          ///< when the pause was last announced
   bool stale_snapshot_reported = false;      ///< a snapshot-mismatch streak is open
   fc::time_point last_stale_report;          ///< when the mismatch was last announced
   bool failing = false;                      ///< a delivery-failure streak is open
   fc::time_point last_failure_report;        ///< when a failure or a recovery was last reported
   status_monitor::pipeline_stats last_stats; ///< counters as last compared
   /// chain_plugin's read-only API (get_info). It references chain_plugin's internals, which outlive it only
   /// because appbase shuts plugins down in reverse startup order: this plugin depends on chain_plugin, so
   /// plugin_shutdown resets the handle before chain_plugin's own shutdown runs.
   std::optional<chain_apis::read_only> read_only_api;
   std::unique_ptr<fc::network::es::es_client> client; ///< created before the pipeline; canceled before it stops
   std::unique_ptr<status_monitor::pipeline> pipeline; ///< the two workers
   std::optional<scoped_connection> irreversible_block_connection;

   /** Cancels the client first, so ~pipeline()'s joins stay bounded however the plugin is destroyed. */
   ~impl();

   /** Debug check of the single-thread invariant stated on this struct. */
   void assert_application_thread() const;
   /** Slot body: report counter changes, then queue the get_info snapshot for the newly irreversible block. */
   void on_irreversible_block(const chain::block_signal_params& params);
   /** Compare the pipeline's counters with the last comparison and log a failing streak or a recovery. */
   void report_progress(fc::time_point now);
   /**
    * Log one line for a bulk request that did not fully index, so no batch is lost silently. Unlike every
    * other member function this runs on the pipeline's delivery worker and touches only the logger, which
    * handle_sighup() re-binds on the application thread -- the same arrangement every plugin that logs off
    * the application thread uses. It only logs, so it does not throw, as pipeline::failure_reporter requires.
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

void status_monitor_plugin::impl::on_irreversible_block(const chain::block_signal_params& params) {
   assert_application_thread();
   const auto& block = std::get<0>(params);
   const auto& lib_id = std::get<1>(params);
   try {
      const fc::time_point block_time = block->timestamp;
      const fc::time_point now = fc::time_point::now();
      // Before the gates, so failures and drops accumulated while paused or suppressed are still reported at
      // block cadence; this block's own submit outcome is reported on the next call.
      report_progress(now);
      if (!status_monitor::is_current(block_time, now)) {
         // Catching up (sync or replay), or a host clock running ahead of the chain -- the latter pauses
         // documents for as long as it lasts, so the pause is re-announced every pause_report_interval.
         if (!catching_up || now - last_pause_report >= pause_report_interval) {
            catching_up = true;
            last_pause_report = now;
            fc_wlog(log,
                    "irreversible block {} is {} s behind wall clock; status documents "
                    "paused (node catching up, or the host clock is ahead of the chain; see max_current_block_age)",
                    lib_id.str(), (now - block_time).to_seconds());
         }
         return;
      }
      if (catching_up) {
         catching_up = false;
         fc_ilog(log, "caught up; resuming status documents");
      }
      auto info = read_only_api->get_info({}, fc::time_point::maximum());
      if (!status_monitor::snapshot_is_for(info, lib_id)) {
         // chain_plugin's slot on this signal (connected first) refreshes get_info_db for this LIB before this
         // slot runs; a mismatch means that ordering changed or get_info_db's lazy refresh ran instead (see
         // snapshot_is_for), and the document would carry a stale LIB. A persistent mismatch is an outage, so
         // it is re-announced every pause_report_interval like the catch-up pause.
         if (!stale_snapshot_reported || now - last_stale_report >= pause_report_interval) {
            stale_snapshot_reported = true;
            last_stale_report = now;
            fc_elog(log, "get_info snapshot is not for irreversible block {}; status documents suppressed until it is",
                    lib_id.str());
         }
         return;
      }
      if (stale_snapshot_reported) {
         stale_snapshot_reported = false;
         fc_ilog(log, "get_info snapshot matches the irreversible block again; resuming status documents");
      }
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
   // epoch, so the first report after a quiet period is always due. The streak flag follows the pipeline's
   // state unconditionally; only the log line is rate-limited.
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
      failing = false;
      if (report_due) {
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
   // The one line this plugin logs per batch, and only for a batch that failed: the per-block data path stays
   // log-free. The rate-limited summary in report_progress() still carries the counters and the last detail.
   fc_wlog(log, failed_batch_format, result.attempts, result.detail, delivery_outcome_clause(result, doc_count));
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
      _impl->read_only_api.emplace(chain_plug.get_read_only_api(read_only_api_response_budget));
      _impl->client = std::make_unique<fc::network::es::es_client>(_impl->cfg->delivery);
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
      _impl->irreversible_block_connection.emplace(chain_plug.chain().irreversible_block().connect(
         [impl = _impl.get()](const chain::block_signal_params& params) { impl->on_irreversible_block(params); }));
      fc_ilog(_impl->log,
              "endpoint {} reachable; writing one get_info document per irreversible block to index '{}' "
              "(template tokens: {}; max_items_per_task={}, max_pending_documents={})",
              _impl->client->endpoint(), _impl->cfg->delivery.index,
              fmt::join(_impl->cfg->document_template.tokens(), status_monitor::token_list_separator),
              _impl->cfg->max_items_per_task, _impl->cfg->max_pending_documents);
   }
   FC_LOG_AND_RETHROW()
}

void status_monitor_plugin::plugin_shutdown() {
   // Disconnect first so nothing new is queued; cancel the client so an in-flight request or backoff returns
   // at once (the join below is then bounded); stop the workers. Nothing is destroyed here: the pipeline and
   // the client go when impl does, in the reverse declaration order this sequence follows.
   _impl->irreversible_block_connection.reset();
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
