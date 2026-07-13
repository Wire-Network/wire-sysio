#pragma once

#include <fc/exception/exception.hpp>
#include <fc/task/deadline.hpp>
#include <fc/time.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace fc::task {

/**
 * Exponential-backoff retry envelope. Caller-agnostic — used by any code
 * that needs "poll until a predicate is satisfied, bounded by a deadline".
 *
 * The same struct drives Solana tx-confirmation polling and Ethereum
 * receipt-confirmation polling, so both chains share one backoff + timeout
 * implementation.
 */
struct retry_options {
   /// Delay before the FIRST retry attempt (i.e. between attempt 1 and 2).
   /// Tune down for latency-sensitive polling (Solana `processed` commitment
   /// resolves in ~400ms), up for expensive remote calls.
   fc::microseconds  initial_backoff = fc::milliseconds(200);

   /// Upper bound on any single sleep between attempts. Backoff grows by
   /// `growth_factor` each iteration up to this cap; prevents a slow remote
   /// from snowballing into minute-long pauses.
   fc::microseconds  max_backoff     = fc::seconds(2);

   /// Total wall-clock budget across ALL attempts (including the first one,
   /// not just subsequent retries). On expiry the helper throws
   /// `fc::timeout_exception`. Matches the outpost_client relative-timeout
   /// idiom — the caller typically plumbs the same deadline through.
   fc::microseconds  total_timeout   = fc::seconds(15);

   /// Multiplier applied to the backoff between attempts. `2.0` = standard
   /// exponential; `1.0` = fixed interval; values between give gentler ramps.
   double            growth_factor   = 2.0;
};

inline constexpr retry_options retry_option_defaults{};

/**
 * Drive `attempt` repeatedly until it returns a non-empty optional, or
 * `opts.total_timeout` elapses. The predicate signals:
 *
 *  - returns `T`          → success; `retry_until` returns it immediately.
 *  - returns `std::nullopt` → transient; sleep and retry.
 *  - throws               → fatal; the exception propagates out of
 *                            `retry_until` without further retries.
 *
 * Backoff starts at `opts.initial_backoff` and grows by `opts.growth_factor`
 * each iteration, capped at `opts.max_backoff`. If the deadline expires
 * before success, `fc::timeout_exception` is thrown with `op_label` in the
 * message for traceability.
 *
 * @param op_label   Short label embedded in the timeout exception (e.g.
 *                   "solana:send_transaction_and_confirm"). Shown in logs.
 * @param opts       Backoff + deadline envelope.
 * @param attempt    Predicate called once per iteration.
 * @return           The successful result of `attempt`.
 * @throws fc::timeout_exception on deadline exhaustion.
 * @throws Any exception raised from inside `attempt`.
 */
template <typename T>
T retry_until(std::string_view                          op_label,
              const retry_options&                      opts,
              const std::function<std::optional<T>()>&  attempt) {
   const auto start_time   = fc::time_point::now();
   const auto deadline_abs = fc::task::clamp_to_current_deadline(start_time + opts.total_timeout);
   const auto budget       = std::max(fc::microseconds(), deadline_abs - start_time);
   auto       backoff      = opts.initial_backoff;

   while (true) {
      if (fc::time_point::now() >= deadline_abs) {
         FC_THROW_EXCEPTION(fc::timeout_exception,
                            "{}: deadline exceeded after {}ms",
                            std::string(op_label),
                            budget.count() / 1000);
      }
      if (auto out = attempt(); out.has_value()) {
         return std::move(*out);
      }
      const auto now = fc::time_point::now();
      if (now >= deadline_abs) {
         FC_THROW_EXCEPTION(fc::timeout_exception,
                            "{}: deadline exceeded after {}ms",
                            std::string(op_label),
                            budget.count() / 1000);
      }
      const auto remaining = deadline_abs - now;
      const auto sleep_for_us = std::min(backoff, remaining);
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_for_us.count()));

      // Grow backoff geometrically, clamped to `max_backoff`.
      const auto next_us = static_cast<int64_t>(
         static_cast<double>(backoff.count()) * opts.growth_factor);
      backoff = std::min(fc::microseconds(next_us), opts.max_backoff);
   }
}

} // namespace fc::task
