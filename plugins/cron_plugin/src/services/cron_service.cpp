#include <boost/asio/post.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <ctime>
#include <ranges>
#include <sysio/services/cron_service.hpp>

namespace sysio::services {

namespace {

bool matches_field(const std::set<long>& allowed, long value) {
   if (allowed.empty())
      return true;
   return allowed.contains(value);
}

} // namespace


std::shared_ptr<cron_service> cron_service::create(const std::optional<cron_service::options>& options) {
   cron_service::options opts = options ? *options : cron_service::options{};
   return std::shared_ptr<cron_service>{new cron_service(opts)};
}

cron_service::~cron_service() {
   stop();
}

cron_service::cron_service(const cron_service::options& options)
   : _options(options) {
   ilog("cron_service starting");
   if (options.autostart)
      start();
}
bool cron_service::is_running() const {
   return _running.load();
}

bool cron_service::start() {
   std::scoped_lock lock(_state_mutex);
   if (_running.exchange(true)) {
      wlog("cron_service already started");
      return false;
   }
   _pool.start(_options.num_threads, nullptr /*on_except*/);

   auto now = std::chrono::steady_clock::now();
   auto jobs = _jobs.writeable();
   for (auto& job : jobs.values()) {
      schedule_next(job, now);
   }
   return true;
}

void cron_service::stop() {
   std::scoped_lock lock(_state_mutex);
   ilog("cron_service stop");
   if (!_running.exchange(false)) {
      wlog("cron_service already stopped");
      return;
   }
   cancel_all();
   _pool.stop();
}

cron_service::job_id_t cron_service::add(const cron_schedule& sched, job_fn_t fn,
                                         const std::optional<job_metadata_t>& metadata) {
   auto jobs = _jobs.writeable();
   auto j = std::make_shared<job>();
   j->id = _next_id++;
   j->sched = sched;
   j->metadata = metadata.value_or(job_metadata_t{});
   j->fn = std::move(fn);
   j->timer = std::make_unique<boost::asio::steady_timer>(_pool.get_executor());

   jobs.try_emplace(j->id, j);

   if (is_running()) {
      auto now = std::chrono::steady_clock::now();
      schedule_next(j, now);
   }

   return j->id;
}

void cron_service::update_metadata(job_id_t id, const job_metadata_t& metadata) {

   auto jobs = _jobs.writeable();
   FC_ASSERT_FMT(jobs.contains(id), "cron_service::update_metadata() no job with id {}", id);

   jobs.get(id)->metadata = metadata;
}

std::vector<cron_service::job_id_t> cron_service::list(const std::vector<job_query_t>& queries) {
   auto jobs = _jobs.readable().values();
   if (!queries.empty())
      jobs = jobs | std::views::filter([&](auto& j) {
         return std::any_of(queries.begin(), queries.end(), [&](auto& query_var) {
            if (std::holds_alternative<std::string>(query_var)) {
               auto& query = std::get<std::string>(query_var);
               return j->label() == query || std::ranges::contains(j->tags(),query);
            }
            return j->id == std::get<job_id_t>(query_var);
         });
      }) | std::ranges::to<std::vector>();

   return jobs | std::views::transform([](auto& j) { return j->id; }) | std::ranges::to<std::vector>();
}

void cron_service::cancel(job_id_t id) {
   std::shared_ptr<job> j;
   {
      auto wv = _jobs.writeable();
      auto it = wv.map().find(id);
      if (it == wv.map().end())
         return;
      j = std::move(it->second);
      j->cancelled = true;
      wv.map().erase(it);
   }
   if (j && j->timer) {
      try {
         j->timer->cancel();
      } catch (const std::exception& e) {
         wlogf("cron_service::cancel() timer->cancel() threw: {}", e.what());
      }
   }
}

void cron_service::cancel_all() {
   std::vector<std::shared_ptr<job>> to_cancel;
   {
      auto wv = _jobs.writeable();
      for (auto& val : wv.map() | std::views::values) {
         val->cancelled = true;
         to_cancel.push_back(val);
      }
      wv.clear();
   }
   for (auto& j : to_cancel) {
      if (j->timer) {
         dlogf("Cancelling job id {}", j->id);
         try {
            j->timer->cancel();
         } catch (const std::exception& e) {
            wlogf("cron_service::cancel() timer->cancel() threw: {}", e.what());
         }
      }
   }
}

std::chrono::steady_clock::time_point cron_service::next_fire_time(const cron_schedule& sched,
                                                                   std::chrono::steady_clock::time_point now) {
   using clock = std::chrono::steady_clock;
   using namespace std::chrono;

   auto tp = now;

   // Split into seconds and milliseconds components
   auto ms_since_epoch = duration_cast<milliseconds>(tp.time_since_epoch());
   auto sec_since_epoch = duration_cast<seconds>(ms_since_epoch);
   auto ms_in_sec = duration_cast<milliseconds>(ms_since_epoch - sec_since_epoch);

   // Helper to check all non-ms fields for a given whole-second time_point
   auto matches_non_ms = [&](const clock::time_point& t) -> bool {
      auto ms_epoch = duration_cast<milliseconds>(t.time_since_epoch());
      auto ms = ms_epoch.count() % 1000;
      auto sec = duration_cast<seconds>(ms_epoch).count() % 60;
      auto min = duration_cast<minutes>(ms_epoch).count() % 60;
      auto hour = duration_cast<hours>(ms_epoch).count() % 24;

      return matches_field(sched.seconds, sec) && matches_field(sched.minutes, min) &&
             matches_field(sched.hours, hour);
   };

   // First consider current second: if non-ms fields match, see if a future millisecond in this second matches
   clock::time_point sec_tp{sec_since_epoch};
   // if (matches_non_ms(sec_tp))
   //    return sec_tp + seconds(1);
   if (matches_non_ms(sec_tp)) {
      if (sched.milliseconds.empty()) {
         // Wildcard ms means trigger at the next millisecond >= now
         return tp + milliseconds(1); // ensure we don't fire in the past; minimal drift
      }

      int cur_ms = static_cast<int>(ms_in_sec.count());
      for (int ms : sched.milliseconds) {
         if (ms >= cur_ms) {
            return sec_tp + milliseconds(ms);
         }
      }
   }

   // Otherwise, find the next whole second that matches the non-ms fields
   // Iterate by seconds up to a reasonable bound (e.g., 400 days) to avoid infinite loops on invalid schedules
   constexpr int max_seconds_scan = 400 * 24 * 60 * 60; // ~400 days
   auto scan_tp = sec_tp + seconds(1);
   for (int i = 0; i < max_seconds_scan; ++i, scan_tp += seconds(1)) {
      if (matches_non_ms(scan_tp)) {
         //return scan_tp; // trigger at start of that second
         if (sched.milliseconds.empty()) {
            return scan_tp; // trigger at start of that second
         }
         // Earliest millisecond in that second
         return scan_tp + milliseconds(*sched.milliseconds.begin());
      }
   }

   // If nothing found, just push far into the future to avoid tight reschedule loops
   return now + hours(24);
}

void cron_service::schedule_next(std::shared_ptr<job> job_ref, std::chrono::steady_clock::time_point ref) {
   if (!is_running() || job_ref->cancelled)
      return;

   auto next_tp = next_fire_time(job_ref->sched, ref);
   auto now = std::chrono::steady_clock::now();
   if (next_tp < now)
      next_tp = now;

   auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next_tp - now);
   if (delay.count() < 0)
      delay = std::chrono::milliseconds(0);

   job_ref->timer->expires_after(delay);
   auto job_id = job_ref->id;
   job_ref->timer->async_wait([this, job_id](const boost::system::error_code& ec) {
      auto jobs = _jobs.readable();
      if (!jobs.contains(job_id)) {
         ilogf("Job id ({}) is no longer valid", job_id);
         return;
      }

      auto job_work = jobs.get(job_id);
      if (ec || job_work->cancelled)
         return; // cancelled or timer error

      // Decide whether to run the job
      bool should_run = true;
      if (job_work->metadata.one_at_a_time) {
         int expected = 0;
         // Only run if currently not running
         should_run = job_work->running.compare_exchange_strong(expected, 1);
      } else {
         job_work->running.fetch_add(1);
      }

      if (should_run) {
         // Execute the job on the pool executor; then decrement running and reschedule
         boost::asio::post(_pool.get_executor(), [this, job_work, job_id] {
            if (job_work->cancelled) {
               job_work->running.fetch_sub(1);
               return;
            }
            try {
               job_work->fn();
            } catch (const fc::exception& e) {
               elogf("JOB_ID({}) FAILED: {}", job_id, e.to_detail_string());
            } catch (const boost::exception& e) {
               elogf("JOB_ID({}) FAILED: {}", job_id,boost::diagnostic_information(e));
            } catch (const std::runtime_error& e) {
               elogf("JOB_ID({}) FAILED: {}", job_id,e.what());
            } catch (const std::exception& e) {
               elogf("JOB_ID({}) FAILED: {}", job_id,e.what());
            } catch (...) {
               elog("unknown exception");
            }
            job_work->running.fetch_sub(1);

            // Reschedule from completion time
            schedule_next(job_work, std::chrono::steady_clock::now());
         });
      } else {
         // Skipped due to one_at_a_time; just reschedule from now
         schedule_next(job_work, std::chrono::steady_clock::now());
      }
   });
}

} // namespace sysio::services
