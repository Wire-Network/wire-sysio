#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/es/es_client.hpp>
#include <sysio/alert_monitor_plugin/alert_monitor.hpp>
#include <sysio/alert_monitor_plugin/alert_monitor_plugin.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/query_engine_plugin/query_engine_plugin.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h> // fmt::join
#include <magic_enum/magic_enum.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sysio {

namespace {
/// Budget handed to chain_plugin::get_read_only_api. get_info() answers from get_info_db's cache and ignores
/// its deadline; the value only shapes deadlines of HTTP-style calls this plugin never makes.
constexpr fc::microseconds read_only_api_response_budget = fc::seconds(1);
/// While the sync gate keeps pausing documents, the pause is re-announced at most this often, so lagging finality or
/// a host clock running ahead of the chain (either can pause documents indefinitely) stays visible in the log. A
/// halted LIB publishes no irreversible_block, so it is not logged here.
constexpr fc::microseconds pause_report_interval = fc::seconds(60);
/// The warning logged when the startup probe fails: the endpoint, then the client's reason. Not fatal: an endpoint
/// that is down at startup may be up by the first batch, and until it is each batch retries, then is dropped and
/// counted -- the warning makes a wrong URL or a rejected credential visible at once.
constexpr std::string_view probe_failed_format =
   "endpoint {} not reachable at startup; batches retry, then drop and count: {}";
/// The reason a failed probe gives when what it threw is neither an fc nor a std exception.
constexpr std::string_view unknown_probe_failure = "unknown error";
/// How the startup line describes the endpoint after a passed probe.
constexpr std::string_view endpoint_reachable = "reachable";
/// How the startup line describes the endpoint after a failed probe (already warned about).
constexpr std::string_view endpoint_not_reachable = "not reachable yet";
/// The query counters the delivery problem line and the shutdown line carry when queries are configured: the
/// runner's outcomes, its failures by error kind (only the kinds that occurred), and the pipeline's query
/// document admissions.
constexpr std::string_view query_counters_format =
   "; queries_succeeded={} queries_skipped_busy={} queries_failed=[{}] queries_failed_other={} "
   "query_documents_queued={} query_documents_dropped_oversize={} query_documents_dropped_queue_full={}";
/// One entry of query_counters_format's failures by kind: the error kind and its count.
constexpr std::string_view query_failure_count_format = "{}={}";
/// The line logged for a bulk request that did not fully index: the attempts the client spent, its
/// endpoint-sanitized detail, what became of the batch's documents (see delivery_outcome_clause), and the
/// alert_monitor::unlogged_events_clause over failed_batch_events.
constexpr std::string_view failed_batch_format = "bulk request failed after {} attempt(s): {} -- {}{}";
/// What a failed-batch line calls the failed batches its throttle held back.
constexpr std::string_view failed_batch_events = "failed batch(es)";

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

/// Run @p client's one startup probe. nullopt when the endpoint answered as a bulk target should, otherwise the
/// reason it did not: every failure is caught, because an unreachable endpoint is reported, never fatal.
std::optional<std::string> probe_endpoint(fc::network::es::es_client& client) {
   try {
      client.probe();
      return std::nullopt;
   } catch (const fc::exception& e) {
      // The client's message already names the probed endpoint and why it is not reachable.
      return e.top_message();
   } catch (const std::exception& e) {
      return std::string{e.what()};
   } catch (...) {
      return std::string{unknown_probe_failure};
   }
}
} // anonymous namespace

/// Every member is touched only on the application thread: plugin_initialize/startup/shutdown run there,
/// SIGHUP handlers are dispatched through the executor's read_write queue, and irreversible_block channel
/// deliveries are posted to it. The workers see only the pipeline's own state (its counters and last-failure
/// detail, under its mutex) and the es client, whose bulk() runs on the delivery worker and whose cancel() is
/// safe from any thread; the query callers see only the runner's own state, the query service, and the
/// pipeline's submit_document(). No lock is needed here; assert_application_thread() documents and (in debug)
/// checks it. The exception is failed_batch_throttle, which only report_failed_batch() touches, on the delivery
/// worker.
///
/// The declaration order of the last five members is load-bearing: they are destroyed in reverse --
/// subscription, then the query runner, then pipeline, then client, then the read-only handle -- which is the only
/// safe order, since nothing can be queued or dispatched once the subscription is gone, the query callers are
/// joined before the pipeline they submit into, the workers are joined before the client whose bulk() they call,
/// and the client's io thread stops after both. plugin_shutdown() and ~impl() only stop them, both through stop();
/// neither destroys anything. `log` is declared FIRST for the same reason from the other end: it outlives the
/// pipeline, whose delivery worker logs through it (via report_failed_batch(), the failure reporter) right up until
/// the join inside ~pipeline() completes.
struct alert_monitor_plugin::impl {
   fc::logger log;                           ///< the `alert_monitor` logger
   std::optional<alert_monitor::config> cfg; ///< empty: disabled
   chain_plugin* chain_plug = nullptr;       ///< set at startup; outlives this plugin's shutdown
   bool catching_up = false;                 ///< the gate is pausing documents
   fc::time_point last_pause_report;         ///< when the pause was last announced
   /// LIB of the last submitted snapshot; a delivery that finds no newer LIB submits nothing
   chain::block_num_type last_submitted_lib = 0;
   bool failing = false;                     ///< a delivery-failure streak is open
   fc::time_point last_failure_report;       ///< when a failure or a recovery was last reported
   alert_monitor::pipeline_stats last_stats; ///< counters as last compared
   /// Throttle of the failed-batch lines. Delivery worker only, like report_failed_batch(); declared before the
   /// pipeline so it outlives the worker.
   alert_monitor::failure_throttle failed_batch_throttle;
   /// chain_plugin's read-only API (get_info). It references chain_plugin's internals, which outlive it only
   /// because appbase shuts plugins down in reverse startup order: this plugin depends on chain_plugin, so
   /// plugin_shutdown resets the handle before chain_plugin's own shutdown runs.
   std::optional<chain_apis::read_only> read_only_api;
   std::unique_ptr<fc::network::es::es_client> client; ///< created before the pipeline; canceled before it stops
   std::unique_ptr<alert_monitor::pipeline> pipeline;  ///< the two workers
   /// The query callers; set at startup only when queries are configured. Joined before the pipeline (see above).
   std::unique_ptr<alert_monitor::query_runner> queries;
   /// Declared last, so it is released first (see above).
   chain::plugin_interface::channels::irreversible_block::channel_type::handle irreversible_block_subscription;

   /** stop(), for a plugin destroyed without plugin_shutdown() (a directly constructed plugin, as in the tests). */
   ~impl();

   /**
    * Stop everything in the one order that keeps every join bounded: unsubscribe, so no channel delivery -- even
    * one already queued -- submits or dispatches again; join the query callers, each bounded by query_deadline,
    * before the pipeline they submit into stops; cancel the client, so an in-flight request or backoff returns at
    * once instead of holding the pipeline's join for the whole retry budget against a dead endpoint; stop the
    * pipeline. Destroys nothing: the runner, the pipeline and the client go when impl does, in the reverse
    * declaration order this sequence follows. Idempotent, and a no-op for every member never started.
    */
   void stop();

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
    * thread -- the same arrangement every plugin that logs off the application thread uses -- and
    * failed_batch_throttle. It only logs and counts, so it does not throw, as pipeline::failure_reporter requires.
    */
   void report_failed_batch(const fc::network::es::es_bulk_result& result, uint32_t doc_count);
   /** The query_counters_format clause over the runner's counters and @p documents; empty without queries. */
   std::string query_counters_clause(const alert_monitor::pipeline_stats& documents) const;
};

alert_monitor_plugin::impl::~impl() {
   stop();
}

void alert_monitor_plugin::impl::stop() {
   irreversible_block_subscription.unsubscribe();
   if (queries)
      queries->shutdown();
   if (client)
      client->cancel();
   if (pipeline)
      pipeline->shutdown();
}

void alert_monitor_plugin::impl::assert_application_thread() const {
   // get_main_thread_id() is captured when the executor is constructed and refreshed by exec(); nodeop
   // constructs, initializes, starts, and runs the application on one thread, so it holds at every call site.
   assert(std::this_thread::get_id() == appbase::app().executor().get_main_thread_id());
}

void alert_monitor_plugin::impl::on_irreversible_block() {
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
      // Copied before the snapshot takes `info` by move.
      const alert_monitor::query_trigger trigger{.lib_num = info.last_irreversible_block_num,
                                                 .lib_id = info.last_irreversible_block_id,
                                                 .lib_time = info.last_irreversible_block_time};
      // A full render queue is counted by the pipeline and surfaces through the next report_progress().
      pipeline->submit(alert_monitor::status_snapshot{std::move(info), now});
      // Behind the same sync gate and the same once-per-LIB check as the status document.
      if (queries)
         queries->dispatch(trigger);
   }
   FC_LOG_AND_DROP("status snapshot or query dispatch dropped");
}

void alert_monitor_plugin::impl::report_progress(fc::time_point now) {
   const auto current = pipeline->stats();
   // Both report kinds are gated on the one timestamp, so an endpoint alternating between failed and
   // acknowledged batches -- every irreversible block opening a fresh streak -- cannot log a line per block:
   // whichever line is emitted silences both for failure_report_interval. last_failure_report starts at the
   // epoch, so the first report after a quiet period is always due. A streak ends only when its recovery line
   // is logged, so a recovery inside the interval is reported on the first acknowledged batch after the interval.
   const bool report_due = now - last_failure_report >= alert_monitor::failure_report_interval;
   switch (alert_monitor::classify_progress(last_stats, current, failing)) {
   case alert_monitor::progress::failing:
      failing = true;
      if (report_due) {
         last_failure_report = now;
         fc_wlog(log,
                 "delivery problems -- batches_failed={} documents_failed={} "
                 "snapshots_dropped_queue_full={} documents_dropped_queue_full={} documents_dropped_oversize={} "
                 "render_failures={} documents_indexed={}{}; last failure: {}",
                 current.batches_failed, current.documents_failed, current.snapshots_dropped_queue_full,
                 current.documents_dropped_queue_full, current.documents_dropped_oversize, current.render_failures,
                 current.documents_indexed, query_counters_clause(current), pipeline->last_failure());
      }
      break;
   case alert_monitor::progress::recovered:
      if (report_due) {
         failing = false;
         last_failure_report = now;
         fc_ilog(log, "delivery recovered (documents_indexed={} batches_failed={})", current.documents_indexed,
                 current.batches_failed);
      }
      break;
   case alert_monitor::progress::steady:
      break;
   }
   last_stats = current;
}

void alert_monitor_plugin::impl::report_failed_batch(const fc::network::es::es_bulk_result& result,
                                                     uint32_t doc_count) {
   // Throttled so an endpoint that fails every batch cannot log a line per block; the summary in
   // report_progress() still carries the counters and the last detail.
   const std::optional<uint64_t> unlogged = failed_batch_throttle.admit(fc::time_point::now());
   if (!unlogged)
      return;
   fc_wlog(log, failed_batch_format, result.attempts, result.detail, delivery_outcome_clause(result, doc_count),
           alert_monitor::unlogged_events_clause(*unlogged, failed_batch_events));
}

std::string alert_monitor_plugin::impl::query_counters_clause(const alert_monitor::pipeline_stats& documents) const {
   if (!queries)
      return {};
   const alert_monitor::query_stats counters = queries->stats();
   std::vector<std::string> failed_by_kind;
   for (std::size_t slot = 0; slot < counters.failed.size(); ++slot) {
      if (counters.failed[slot] > 0) {
         failed_by_kind.push_back(fmt::format(
            query_failure_count_format, magic_enum::enum_name(magic_enum::enum_value<query_engine::error_kind>(slot)),
            counters.failed[slot]));
      }
   }
   return fmt::format(query_counters_format, counters.succeeded, counters.skipped_busy,
                      fmt::join(failed_by_kind, alert_monitor::token_list_separator), counters.failed_other,
                      documents.query_documents_queued, documents.query_documents_dropped_oversize,
                      documents.query_documents_dropped_queue_full);
}

alert_monitor_plugin::alert_monitor_plugin()
   : _impl(std::make_unique<impl>()) {}

alert_monitor_plugin::~alert_monitor_plugin() = default;

void alert_monitor_plugin::set_program_options(options_description&, options_description& cfg) {
   alert_monitor::add_options(cfg);
}

void alert_monitor_plugin::plugin_initialize(const variables_map& options) {
   try {
      _impl->cfg = alert_monitor::parse_config(options, app().config_dir());
      handle_sighup(); // bind the diagnostic logger, the convention every plugin follows at the end of initialize
   }
   FC_LOG_AND_RETHROW()
}

void alert_monitor_plugin::plugin_startup() {
   if (!_impl->cfg) {
      fc_ilog(_impl->log, "no --{} provided, disabled", alert_monitor::option::target_url);
      return;
   }
   try {
      // A configuration check, so it runs before anything is probed or built: a missing query engine fails startup
      // at once. In startup, not initialize: the engine may initialize after this plugin (plugin option order).
      query_engine_plugin* query_plugin = nullptr;
      if (!_impl->cfg->queries.empty()) {
         query_plugin = app().find_plugin<query_engine_plugin>();
         SYS_ASSERT(query_plugin && query_plugin->get_state() != appbase::abstract_plugin::registered,
                    chain::plugin_config_exception, "--{} needs plugin = sysio::query_engine_plugin",
                    alert_monitor::option::query);
      }
      auto& chain_plug = app().get_plugin<chain_plugin>();
      _impl->chain_plug = &chain_plug;
      _impl->read_only_api.emplace(chain_plug.get_read_only_api(read_only_api_response_budget));
      _impl->client = std::make_unique<fc::network::es::es_client>(_impl->cfg->delivery, _impl->cfg->transport);
      // Before anything is started, once, and never fatal: an endpoint that is down now may be up by the first
      // batch, and until it is each batch retries, then is dropped and counted. One warning makes a wrong URL or a
      // rejected credential visible at once. One attempt, no retry -- the retry budget is for a running node.
      // Configuration errors were already thrown by parse_config() and the query engine check above.
      const std::optional<std::string> probe_failure = probe_endpoint(*_impl->client);
      if (probe_failure)
         fc_wlog(_impl->log, probe_failed_format, _impl->client->endpoint(), *probe_failure);
      _impl->pipeline = std::make_unique<alert_monitor::pipeline>(
         *_impl->cfg, _impl->client->action_line(),
         [client = _impl->client.get()](std::string body, uint32_t doc_count) {
            return client->bulk(std::move(body), doc_count);
         },
         [impl = _impl.get()](const fc::network::es::es_bulk_result& result, uint32_t doc_count) {
            impl->report_failed_batch(result, doc_count);
         });
      if (query_plugin) {
         // Built over the pipeline it submits into. The engine's service is looked up per execution, so an engine
         // that starts after this plugin is picked up then.
         _impl->queries = std::make_unique<alert_monitor::query_runner>(
            *_impl->cfg, *_impl->pipeline, [query_plugin] { return query_plugin->get_query_service(); }, _impl->log);
      }
      // A channel delivery runs on the application thread after the block commits. The payload is unused: the
      // document comes from get_info(), which already holds the newest LIB when the delivery runs.
      _impl->irreversible_block_subscription =
         app().get_channel<chain::plugin_interface::channels::irreversible_block>().subscribe(
            [impl = _impl.get()](const chain::plugin_interface::channels::block_params&) {
               impl->on_irreversible_block();
            });
      // Query names only: a query's text is never logged.
      fc_ilog(_impl->log,
              "endpoint {} {}; writing one get_info document per LIB advance to index '{}' "
              "(template tokens: {}; max_items_per_task={}, max_pending_documents={}; queries: [{}])",
              _impl->client->endpoint(), probe_failure ? endpoint_not_reachable : endpoint_reachable,
              _impl->cfg->delivery.index,
              fmt::join(_impl->cfg->document_template.tokens(), alert_monitor::token_list_separator),
              _impl->cfg->max_items_per_task, _impl->cfg->max_pending_documents,
              fmt::join(_impl->cfg->queries | std::views::transform(&alert_monitor::query_entry::name),
                        alert_monitor::token_list_separator));
   }
   FC_LOG_AND_RETHROW()
}

void alert_monitor_plugin::plugin_shutdown() {
   _impl->stop();
   if (_impl->pipeline) {
      const auto final_stats = _impl->pipeline->stats();
      fc_ilog(_impl->log,
              "shutdown -- documents_indexed={} documents_failed={} batches_indexed={} "
              "batches_failed={} snapshots_dropped_queue_full={} documents_dropped_queue_full={}{}",
              final_stats.documents_indexed, final_stats.documents_failed, final_stats.batches_indexed,
              final_stats.batches_failed, final_stats.snapshots_dropped_queue_full,
              final_stats.documents_dropped_queue_full, _impl->query_counters_clause(final_stats));
   }
   // The read-only handle reaches into chain_plugin, which shuts down after this plugin, so it is released
   // here rather than at destruction.
   _impl->read_only_api.reset();
}

void alert_monitor_plugin::handle_sighup() {
   fc::logger::update(std::string{alert_monitor::logger_name}, _impl->log);
   if (_impl->queries)
      _impl->queries->set_logger(_impl->log);
}

} // namespace sysio
