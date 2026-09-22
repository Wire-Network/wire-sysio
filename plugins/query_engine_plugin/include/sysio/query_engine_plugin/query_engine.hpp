#pragma once
#include <fc/log/logger.hpp>
#include <sysio/query_engine_plugin/query_read_api.hpp>
#include <sysio/query_engine_plugin/query_service.hpp>

#include <magic_enum/magic_enum.hpp>

#include <array>

namespace sysio::query_engine {
/// Observable stages used for deterministic scheduling and lifecycle tests.
enum class query_stage { parse, describe, plan, capture, evaluate };

/// Cumulative engine counters, copied under its mutex. Returned rows count completed execution results.
struct service_stats {
   uint64_t accepted = 0, completed = 0, rejected_busy = 0, timed_out = 0, cancelled = 0;
   uint64_t scanned_rows = 0, returned_rows = 0, capture_bytes = 0, peak_accounted_bytes = 0;
   uint64_t in_flight = 0;
   std::array<uint64_t, magic_enum::enum_count<error_kind>()> failed{};
};

/// Bounded execution shared by C++ callers and optional HTTP ingress. Cancelled callbacks retain
/// admission until drained; no controller reference escapes into a result or a worker-side stage.
class query_engine : public query_service {
public:
   using stage_observer = std::function<void(query_stage)>;
   /// Bind an internally synchronized read API and start the bounded query_task workers.
   query_engine(query_config, std::shared_ptr<query_read_api>, query_budget::now_function = query_budget::clock::now,
                stage_observer = {});
   ~query_engine() override;
   query_engine(const query_engine&) = delete;
   query_engine& operator=(const query_engine&) = delete;
   /// Enqueue a query_task and block until completion. Call from a worker, never a chain executor callback.
   query_result execute(const std::string& query, const std::optional<query_options>& options = std::nullopt) override;
   /// Create a request budget using this engine's immutable caps and monotonic clock.
   std::shared_ptr<query_budget> create_budget(const std::optional<query_options>& options = std::nullopt) const;
   /// Read startup caps; HTTP uses the configured timeout for its own per-call options.
   const query_config& config() const;
   /// Cancel, drain/join workers, and invalidate the source before controller shutdown.
   void stop();
   /// Rebind diagnostics without changing configuration.
   void set_logger(fc::logger);
   /// Copy counters without retaining mutable engine state.
   service_stats stats() const;

private:
   friend class query_http_handler;
   /// HTTP keeps ingress, queue time, execution and serialization in the same budget.
   query_result execute(const std::string& query, std::shared_ptr<query_budget>);
   /// Share throttled diagnostics and SIGHUP logger rebinding with the optional HTTP adapter.
   void report_internal(const std::string& detail);
   struct impl;
   std::shared_ptr<impl> state;
};
} // namespace sysio::query_engine
