#pragma once

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <chrono>

#include <boost/asio/steady_timer.hpp>

#include <sysio/chain/thread_utils.hpp>

namespace sysio::operator_plugin::services {

  // Represents a cron-like schedule. Empty set for any field means wildcard (match all values).
  struct cron_schedule {
    // Ranges:
    //  - seconds:       0..59
    //  - minutes:       0..59
    //  - hours:         0..23
    //  - day_of_month:  1..31
    //  - month:         1..12
    //  - day_of_week:   0..6 (0 = Sunday)
    //  - milliseconds:  0..999 (additional member beyond typical cron)
    std::set<int> seconds;
    std::set<int> minutes;
    std::set<int> hours;
    std::set<int> day_of_month;
    std::set<int> month;
    std::set<int> day_of_week;
    std::set<int> milliseconds; // additional member to support millisecond resolution
  };


  class cron_service {
    public:

      using job_id = uint64_t;
      using job_fn = std::function<void()>;

      struct options {
        std::string name{"cron_service"};
        std::size_t num_threads{4};
      };

      struct job_options {
        // If true, each schedule runs at most one invocation at a time. If an invocation is still
        // running when the next trigger occurs, that trigger is skipped.
        bool one_at_a_time{false};
      };


      struct job {
        cron_service::job_id id;
        cron_schedule sched;
        job_options opts;
        job_fn fn;
        std::atomic<int> running{0};
        std::atomic<bool> cancelled{false};
        std::unique_ptr<boost::asio::steady_timer> timer; // lives on _pool io_context
      };


      static std::shared_ptr<cron_service> create(const std::optional<cron_service::options>& options = std::nullopt);

      virtual ~cron_service();

      // Register a new job. Returns a handle (job_id) that can be used to cancel later.
      job_id add(const cron_schedule& sched, job_fn fn, const std::optional<job_options>& opts = std::nullopt);

      // Cancel a single job or all jobs. Safe to call concurrently with callbacks.
      void cancel(job_id id);

      void cancel_all();

    protected:

      explicit cron_service(const options& options);

    private:

      const cron_service::options _options;

      void start();

      void stop();

      // Compute the next fire time at or after now.
      static std::chrono::system_clock::time_point next_fire_time(
        const cron_schedule& sched,
        std::chrono::system_clock::time_point now
      );

      void schedule_next(std::shared_ptr<job> j, std::chrono::system_clock::time_point ref);

    private:


      sysio::chain::named_thread_pool<cron_service> _pool;

      std::mutex _jobs_mx;
      std::unordered_map<job_id, std::shared_ptr<job>> _jobs;
      std::atomic<job_id> _next_id{1};
      bool _running = false;
  };

} // namespace sysio
