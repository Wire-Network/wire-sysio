#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fc-lite/threadsafe_map.hpp>
#include <functional>
#include <memory>
#include <set>
#include <ranges>
#include <boost/asio/thread_pool.hpp>
#include <thread>
#include <unordered_map>
#include <variant>
#include <algorithm>
#include <concepts>

namespace sysio::services {

class cron_service;
using cron_service_ptr = std::shared_ptr<cron_service>;

class cron_service {
public:
   using clock_t    = std::chrono::system_clock;
   using time_point = clock_t::time_point;
   using job_id_t   = std::uint64_t;
   using job_query_t = std::variant<job_id_t, std::string>;
   using job_fn_t   = std::function<void()>;

   /// Number of upcoming trigger times to pre-compute per job.
   static constexpr std::uint64_t schedule_trigger_count = 8;

   // -----------------------------------------------------------------------
   // Schedule
   // -----------------------------------------------------------------------

   /**
    * Represents a cron-like schedule.  Empty set for any field means wildcard
    * (match all valid values for that field).
    *
    * Field ranges:
    *   milliseconds : 0..59999     (milliseconds within the minute)
    *   minutes      : 0..59
    *   hours        : 0..23
    *   day_of_month : 1..31
    *   month        : 1..12
    *   day_of_week  : 0..7   (0 and 7 both represent Sunday)
    */
   struct schedule {
      /// An exact match for a single value (e.g. "30" in the minutes field).
      struct exact_value {
         std::uint64_t value;
         auto operator<=>(const exact_value&) const = default;
         bool operator==(const exact_value&) const = default;
      };

      /// A step expression representing "every N" (e.g. "*/15" in the minutes field).
      struct step_value {
         std::uint64_t step;
         auto operator<=>(const step_value&) const = default;
         bool operator==(const step_value&) const = default;
      };

      /// A range expression (e.g. "1-5" in the day_of_week field).
      struct range_value {
         std::uint64_t from;
         std::uint64_t to;
         auto operator<=>(const range_value&) const = default;
         bool operator==(const range_value&) const = default;
      };

      using schedule_value = std::variant<exact_value, step_value, range_value>;


      // -----------------------------------------------------------------------
      // Schedule value types
      // -----------------------------------------------------------------------

      /// Expand a set of schedule_values into the concrete uint64_t values they
      /// represent within [min_val, max_val].  An empty input set means "wildcard"
      /// and the caller should treat it as "match all".
      static std::set<std::uint64_t> expand_field(const std::set<schedule_value>& field,
                                                  std::uint64_t min_val,
                                                  std::uint64_t max_val);


      std::set<schedule_value> milliseconds; // 0-59999
      std::set<schedule_value> minutes;      // 0-59
      std::set<schedule_value> hours;        // 0-23
      std::set<schedule_value> day_of_month; // 1-31
      std::set<schedule_value> month;        // 1-12
      std::set<schedule_value> day_of_week;  // 0-7; 0 and 7 = Sunday
   };

   // -----------------------------------------------------------------------
   // Other nested types
   // -----------------------------------------------------------------------

   template <typename T>
   static constexpr bool is_job_query_v = std::convertible_to<T, job_query_t>;

   struct options {
      std::string name{"cron_service"};
      std::size_t num_threads{1};
      bool autostart{true};
   };

   struct job_metadata_t {
      // If true, each schedule runs at most one invocation at a time. If an
      // invocation is still running when the next trigger occurs, that trigger
      // is skipped.
      bool one_at_a_time{false};
      std::vector<std::string> tags{};
      std::string label{""};
   };

   struct job {
      cron_service::job_id_t id;
      schedule sched;
      job_metadata_t metadata;
      job_fn_t fn;

      const std::vector<std::string>& tags() const { return metadata.tags; };
      const std::string& label() const { return metadata.label; };

      std::atomic_int running{0};
      std::atomic_bool cancelled{false};

      // Pre-computed upcoming trigger times (chronological order).
      std::vector<time_point> upcoming_triggers;
      std::mutex triggers_mutex;
   };

   // -----------------------------------------------------------------------
   // Public API
   // -----------------------------------------------------------------------

   static cron_service_ptr create(const std::optional<cron_service::options>& options = std::nullopt);

   virtual ~cron_service();

   /**
    * Add a new scheduled job.
    *
    * @param sched job_schedule
    * @param fn job function to execute
    * @param metadata optional job metadata
    * @return job_id_t
    */
   job_id_t add(const schedule& sched, job_fn_t fn,
                const std::optional<job_metadata_t>& metadata = std::nullopt);

   /**
    * Update job metadata
    *
    * @param id to update
    * @param metadata to set
    */
   void update_metadata(job_id_t id, const job_metadata_t& metadata);

   /**
    * Get matching jobs by id or tag
    *
    * @param queries to search for
    * @return job_id_t vector
    */
   std::vector<job_id_t> list(const std::vector<job_query_t>& queries);

   /**
    * Get matching jobs by ID or tag.
    *
    * @tparam Args variadic template parameter for job queries (job_id_t or std::string tag)
    * @param args job_query_t...
    * @return job_id_t vector
    */
   template <typename... Args>
   requires (is_job_query_v<Args> && ...)
   std::vector<job_id_t> list(Args&&... args) {
      std::vector<job_query_t> queries = {args...};
      return list(queries);
   }

   // Cancel a single job or all jobs. Safe to call concurrently with callbacks.
   void cancel(job_id_t id);

   void cancel_all();

   explicit cron_service(const options& options);

   bool is_running() const;

   bool start();

   void stop();

   // Compute the next fire time strictly after `after`.
   static time_point next_fire_time(const schedule& sched, time_point after);

   // Compute the next `n` trigger times starting strictly after `from`.
   static std::vector<time_point> compute_next_n_triggers(const schedule& sched,
                                                          time_point from, std::size_t n);

private:
   // Scheduler thread
   void start_scheduler();
   void stop_scheduler();
   void scheduler_loop();
   void dispatch_ready_jobs();
   void replenish_triggers();
   void wake_scheduler();
   std::optional<time_point> compute_earliest_trigger();

   const cron_service::options _options;

   std::unique_ptr<boost::asio::thread_pool> _pool;

   fc::threadsafe_map<job_id_t, std::shared_ptr<job>,
                      std::unordered_map<job_id_t, std::shared_ptr<job>>> _jobs;
   std::atomic<job_id_t> _next_id{1};
   std::atomic_bool _running{false};
   std::mutex _state_mutex{};

   // Scheduler thread infrastructure
   std::thread _scheduler_thread;
   std::mutex _scheduler_mutex;
   std::condition_variable _scheduler_cv;
   bool _scheduler_shutdown{false};
   bool _schedule_requested{false};
};

} // namespace sysio::services
