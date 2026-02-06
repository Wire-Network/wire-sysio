#pragma once

#include <atomic>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <fc-lite/threadsafe_map.hpp>
#include <functional>
#include <memory>
#include <set>
#include <ranges>
#include <sysio/chain/thread_utils.hpp>
#include <unordered_map>
#include <variant>
#include <algorithm>
#include <concepts>

namespace sysio::services {

class cron_service;
using cron_service_ptr = std::shared_ptr<cron_service>;

/**
 * Represents a cron-like schedule. Empty set for any field means wildcard (match all values).
 *
 * Ranges:
 * - milliseconds:  0..999 (additional member beyond typical cron)
 * - seconds:       0..59
 * - minutes:       0..59
 * - hours:         0..23
 */
struct cron_schedule {
   std::set<long> milliseconds; // additional member to support millisecond resolution
   std::set<long> seconds;
   std::set<long> minutes;
   std::set<long> hours;
};

class cron_service;
using cron_service_ptr = std::shared_ptr<cron_service>;

class cron_service {
public:
   using job_id_t = std::uint64_t;
   using job_query_t = std::variant<job_id_t, std::string>;
   using job_fn_t = std::function<void()>;

   template <typename T>
   static constexpr bool is_job_query_v = std::convertible_to<T, job_query_t>;

   struct options {
      std::string name{"cron_service"};
      std::size_t num_threads{1};
      bool autostart{true};
   };

   struct job_metadata_t {
      // If true, each schedule runs at most one invocation at a time. If an invocation is still
      // running when the next trigger occurs, that trigger is skipped.
      bool one_at_a_time{false};
      std::vector<std::string> tags{};
      std::string label{""};
   };
   
   struct job {
      cron_service::job_id_t id;
      cron_schedule sched;
      job_metadata_t metadata;
      job_fn_t fn;

      const std::vector<std::string>& tags() const { return metadata.tags; };
      const std::string& label() const { return metadata.label; };

      std::atomic_int running{0};
      std::atomic_bool cancelled{false};
      std::unique_ptr<boost::asio::steady_timer> timer{nullptr}; // lives on _pool io_context
   };


   static cron_service_ptr create(const std::optional<cron_service::options>& options = std::nullopt);

   virtual ~cron_service();

   /**
    * Add a new scheduled job.
    *
    * @param sched cron_schedule
    * @param fn job function to execute
    * @param metadata optional job metadata
    * @return job_id_t
    */
   job_id_t add(const cron_schedule& sched, job_fn_t fn, const std::optional<job_metadata_t>& metadata = std::nullopt);

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

private:
   // Compute the next fire time at or after now.
   static std::chrono::steady_clock::time_point next_fire_time(const cron_schedule& sched,
                                                               std::chrono::steady_clock::time_point now);

   void schedule_next(std::shared_ptr<job> job_ref, std::chrono::steady_clock::time_point ref);

   const cron_service::options _options;

   sysio::chain::named_thread_pool<cron_service> _pool;

   fc::threadsafe_map<job_id_t, std::shared_ptr<job>, std::unordered_map<job_id_t, std::shared_ptr<job>>> _jobs;
   std::atomic<job_id_t> _next_id{1};
   std::atomic_bool _running{false};
   std::mutex _state_mutex{};
};

} // namespace sysio::cron_plugin::services
