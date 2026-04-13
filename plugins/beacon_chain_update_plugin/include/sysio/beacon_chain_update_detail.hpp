#pragma once

#include <fc-lite/expected.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <sysio/services/cron_service.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace sysio::beacon_chain_detail {

using cron_job_id_t = services::cron_service::job_id_t;

struct retry_config_t {
   int max_retries{600};
   std::function<cron_job_id_t(std::function<void()>)> schedule;
   std::function<void(cron_job_id_t)> cancel;
   std::function<fc::exception()> on_exhaustion;
};

template <typename Fn, typename... Args>
auto retry(retry_config_t config, Fn fn, Args&&... args)
   -> std::expected<typename std::invoke_result_t<Fn, Args...>::value_type, fc::exception> {
   auto ret = fn(std::forward<Args>(args)...);
   if (ret.has_value())
      return std::move(*ret);

   std::atomic<bool> complete{false};
   std::optional<cron_job_id_t> job_id;
   std::optional<fc::exception> error;

   auto retry_fn = [&, attempt = 0]() mutable {
      if (complete.load(std::memory_order_acquire))
         return;

      try {
         ret = fn(std::forward<Args>(args)...);
         bool exiting = false;
         if (ret.has_value()) {
            complete.store(true, std::memory_order_release);
            exiting = true;
         } else if (++attempt >= config.max_retries) {
            complete.store(true, std::memory_order_release);
            exiting = true;
         }

         if (exiting) {
            config.cancel(*job_id);
         }
      } catch (const fc::exception& e) {
         error = e;
         complete.store(true, std::memory_order_release);
      }
   };

   job_id = config.schedule(retry_fn);

   while (!complete.load(std::memory_order_acquire))
      std::this_thread::yield();

   if (job_id.has_value())
      config.cancel(*job_id);

   if (error.has_value())
      return std::unexpected(std::move(*error));
   if (ret.has_value())
      return std::move(*ret);
   return std::unexpected(config.on_exhaustion());
}

/// The field name in beacon chain queue API responses that holds the estimated processing timestamp (Unix seconds).
inline constexpr auto epa_field = "estimated_processed_at";

/// Extract a named field from an fc::variant object.
/// Returns empty optional if expected_obj is not an object or does not contain expected_field.
std::optional<fc::variant> get_field_from_object(const fc::variant& expected_obj,
                                                  const std::string& expected_field);

/// Extract the queue wait time in seconds (from now) for the given queue branch from a beacon chain queues response.
/// Throws sysio::chain::plugin_config_exception if required fields are absent or malformed.
std::optional<uint64_t> get_queue_length(const fc::variant& queues, const std::string& queue_branch);

/// Convert an APY fraction (e.g. 0.05 for 5%) to basis points (e.g. 500).
/// Uses a small epsilon for floating-point robustness when the result should be a whole number.
inline uint64_t apy_fraction_to_bps(double apr_fraction) {
   if (apr_fraction < 0.0)
      apr_fraction = 0.0;
   return static_cast<uint64_t>(apr_fraction * 10000.0 + 1e-12);
}

} // namespace sysio::beacon_chain_detail
