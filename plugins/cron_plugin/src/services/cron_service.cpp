#include <boost/asio/post.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <chrono>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/log/logger_config.hpp>
#include <ranges>
#include <sysio/services/cron_service.hpp>

namespace sysio::services {

// ---------------------------------------------------------------------------
// expand_field
// ---------------------------------------------------------------------------

std::set<std::uint64_t> cron_service::job_schedule::expand_field(const std::set<job_schedule::schedule_value>& field,
                                                   std::uint64_t min_val,
                                                   std::uint64_t max_val) {
   if (field.empty())
      return {}; // wildcard — caller must treat empty result as "match all"

   std::set<std::uint64_t> result;
   for (const auto& sv : field) {
      std::visit(
         [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, exact_value>) {
               if (v.value >= min_val && v.value <= max_val)
                  result.insert(v.value);
            } else if constexpr (std::is_same_v<T, step_value>) {
               if (v.step == 0)
                  return;
               for (auto i = min_val; i <= max_val; i += v.step)
                  result.insert(i);
            } else if constexpr (std::is_same_v<T, range_value>) {
               auto lo = std::max(v.from, min_val);
               auto hi = std::min(v.to, max_val);
               for (auto i = lo; i <= hi; ++i)
                  result.insert(i);
            }
         },
         sv);
   }
   return result;
}

// ---------------------------------------------------------------------------
// next_fire_time — efficient field-by-field iteration
// ---------------------------------------------------------------------------

namespace {

using schedule_value = cron_service::job_schedule::schedule_value;

// Decomposed wall-clock time used for schedule matching.
struct decomposed_time {
   int year;
   unsigned month;       // 1-12
   unsigned day;         // 1-31
   unsigned weekday;     // 0=Sun ... 6=Sat
   unsigned hour;        // 0-23
   unsigned minute;      // 0-59
   unsigned millisecond; // 0-59999 (ms within the minute)
};

decomposed_time decompose(std::chrono::system_clock::time_point tp) {
   using namespace std::chrono;

   auto tp_days = floor<days>(tp);
   year_month_day ymd{tp_days};
   weekday wd{tp_days};
   auto day_offset = tp - tp_days;

   auto h = duration_cast<hours>(day_offset);
   day_offset -= h;
   auto m = duration_cast<minutes>(day_offset);
   day_offset -= m;
   auto ms_in_minute = duration_cast<milliseconds>(day_offset);

   return {
      static_cast<int>(ymd.year()),
      static_cast<unsigned>(ymd.month()),
      static_cast<unsigned>(ymd.day()),
      wd.c_encoding(), // 0=Sun
      static_cast<unsigned>(h.count()),
      static_cast<unsigned>(m.count()),
      static_cast<unsigned>(ms_in_minute.count()),
   };
}

std::chrono::system_clock::time_point compose(const decomposed_time& dt) {
   using namespace std::chrono;

   auto ymd = year{dt.year} / month{dt.month} / day{dt.day};
   auto tp = sys_days{ymd};
   return tp + hours{dt.hour} + minutes{dt.minute} +
          milliseconds{dt.millisecond};
}

// Return the set of valid values for a field, or all values in [min,max] if
// the field is a wildcard (empty).
std::set<std::uint64_t> field_values(const std::set<schedule_value>& field,
                                     std::uint64_t min_val,
                                     std::uint64_t max_val) {
   auto expanded = cron_service::job_schedule::expand_field(field, min_val, max_val);
   if (expanded.empty()) {
      // wildcard: all values in range
      for (auto i = min_val; i <= max_val; ++i)
         expanded.insert(i);
   }
   return expanded;
}

// How many days in a given year/month.
unsigned days_in_month(int year, unsigned month) {
   using namespace std::chrono;
   auto ymd_last = year_month_day_last{std::chrono::year{year} / std::chrono::month{month} / last};
   return static_cast<unsigned>(ymd_last.day());
}

bool day_of_week_matches(int year, unsigned month, unsigned day_val,
                         const std::set<std::uint64_t>& dow_set) {
   using namespace std::chrono;
   auto ymd = std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day_val};
   if (!ymd.ok())
      return false;
   weekday wd{sys_days{ymd}};
   auto enc = wd.c_encoding(); // 0=Sun
   // day_of_week 7 also means Sunday
   return dow_set.contains(enc) || (enc == 0 && dow_set.contains(7));
}

} // namespace

cron_service::time_point
cron_service::next_fire_time(const job_schedule& sched, time_point after) {
   using namespace std::chrono;

   // Advance by 1ms so we find a time strictly after `after`.
   auto candidate_tp = after + milliseconds{1};
   auto dt = decompose(candidate_tp);

   auto months_set  = field_values(sched.month, 1, 12);
   auto hours_set   = field_values(sched.hours, 0, 23);
   auto minutes_set = field_values(sched.minutes, 0, 59);
   // Wildcard milliseconds defaults to {0} (once per second) rather than
   // materializing all 60 000 values.
   auto ms_set = cron_service::job_schedule::expand_field(sched.milliseconds, 0, 59999);
   if (ms_set.empty())
      ms_set.insert(0);
   auto dow_set     = field_values(sched.day_of_week, 0, 7);

   // Cap at 4 years of scanning to prevent infinite loops.
   int start_year = dt.year;
   int max_year   = start_year + 4;

   for (int y = dt.year; y <= max_year; ++y) {
      unsigned month_start = (y == dt.year) ? dt.month : 1;
      for (unsigned mo : months_set) {
         if (y == dt.year && mo < month_start)
            continue;

         auto dom_set = field_values(sched.day_of_month, 1, days_in_month(y, mo));

         unsigned day_start = (y == dt.year && mo == dt.month) ? dt.day : 1;
         for (unsigned d : dom_set) {
            if (y == dt.year && mo == dt.month && d < day_start)
               continue;

            // Validate day_of_week constraint
            if (!day_of_week_matches(y, mo, d, dow_set))
               continue;

            unsigned hour_start = (y == dt.year && mo == dt.month && d == dt.day) ? dt.hour : 0;
            for (unsigned h : hours_set) {
               if (y == dt.year && mo == dt.month && d == dt.day && h < hour_start)
                  continue;

               unsigned min_start = (y == dt.year && mo == dt.month && d == dt.day && h == dt.hour)
                                       ? dt.minute : 0;
               for (unsigned mi : minutes_set) {
                  if (y == dt.year && mo == dt.month && d == dt.day && h == dt.hour && mi < min_start)
                     continue;

                  unsigned ms_start =
                     (y == dt.year && mo == dt.month && d == dt.day &&
                      h == dt.hour && mi == dt.minute) ? dt.millisecond : 0;

                  for (unsigned ms : ms_set) {
                     if (ms < ms_start)
                        continue;

                     decomposed_time result{y, mo, d, 0, h, mi, ms};
                     auto tp = compose(result);
                     if (tp > after)
                        return tp;
                  }
               }
            }
         }
      }
      // Reset dt fields for subsequent years so we scan from the start.
      dt = {y + 1, 1, 1, 0, 0, 0, 0};
   }

   // Fallback: push far into the future.
   return after + hours{24};
}

std::vector<cron_service::time_point>
cron_service::compute_next_n_triggers(const job_schedule& sched,
                                      time_point from, std::size_t n) {
   std::vector<time_point> triggers;
   triggers.reserve(n);
   auto cursor = from;
   for (std::size_t i = 0; i < n; ++i) {
      cursor = next_fire_time(sched, cursor);
      triggers.push_back(cursor);
   }
   return triggers;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::shared_ptr<cron_service>
cron_service::create(const std::optional<cron_service::options>& options) {
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
   _pool = std::make_unique<boost::asio::thread_pool>(_options.num_threads);

   // Compute initial triggers for any pre-existing jobs.
   {
      auto now = clock_t::now();
      auto jobs = _jobs.writeable();
      for (auto& [id, j] : jobs.map()) {
         std::scoped_lock tlock(j->triggers_mutex);
         j->upcoming_triggers = compute_next_n_triggers(
            j->schedule, now, schedule_trigger_count);
      }
   }

   start_scheduler();
   return true;
}

void cron_service::stop() {
   std::scoped_lock lock(_state_mutex);
   ilog("cron_service stop");
   if (!_running.exchange(false)) {
      wlog("cron_service already stopped");
      return;
   }
   stop_scheduler();
   cancel_all();
   if (_pool) {
      _pool->stop();
      _pool->join();
      _pool.reset();
   }
}

// ---------------------------------------------------------------------------
// Scheduler thread
// ---------------------------------------------------------------------------

void cron_service::start_scheduler() {
   {
      std::scoped_lock lock(_scheduler_mutex);
      _scheduler_shutdown = false;
      _schedule_requested = false;
   }
   _scheduler_thread = std::thread([this]() {
      fc::set_thread_name("cron-sched");
      scheduler_loop();
   });
}

void cron_service::stop_scheduler() {
   {
      std::scoped_lock lock(_scheduler_mutex);
      _scheduler_shutdown = true;
   }
   _scheduler_cv.notify_one();
   if (_scheduler_thread.joinable())
      _scheduler_thread.join();
}

void cron_service::scheduler_loop() {
   while (true) {
      std::unique_lock lock(_scheduler_mutex);

      auto earliest = compute_earliest_trigger();

      if (earliest.has_value()) {
         _scheduler_cv.wait_until(lock, *earliest, [this] {
            return _scheduler_shutdown || _schedule_requested;
         });
      } else {
         _scheduler_cv.wait(lock, [this] {
            return _scheduler_shutdown || _schedule_requested;
         });
      }

      if (_scheduler_shutdown)
         break;

      _schedule_requested = false;
      lock.unlock();

      dispatch_ready_jobs();
      replenish_triggers();
   }
}

std::optional<cron_service::time_point> cron_service::compute_earliest_trigger() {
   std::optional<time_point> earliest;
   auto jobs = _jobs.readable();
   for (const auto& [id, j] : jobs.map()) {
      if (j->cancelled)
         continue;
      std::scoped_lock tlock(j->triggers_mutex);
      if (!j->upcoming_triggers.empty()) {
         auto first = j->upcoming_triggers.front();
         if (!earliest || first < *earliest)
            earliest = first;
      }
   }
   return earliest;
}

void cron_service::dispatch_ready_jobs() {
   auto now = clock_t::now();
   auto jobs = _jobs.readable();
   for (const auto& [id, j] : jobs.map()) {
      if (j->cancelled)
         continue;

      std::scoped_lock tlock(j->triggers_mutex);
      while (!j->upcoming_triggers.empty() && j->upcoming_triggers.front() <= now) {
         j->upcoming_triggers.erase(j->upcoming_triggers.begin());

         auto job_work = j;
         auto job_id = j->id;
         boost::asio::post(*_pool, [job_work, job_id] {
            if (job_work->cancelled)
               return;

            bool should_run = true;
            if (job_work->metadata.one_at_a_time) {
               int expected = 0;
               should_run = job_work->running.compare_exchange_strong(expected, 1);
            } else {
               job_work->running.fetch_add(1);
            }

            if (!should_run)
               return;

            try {
               job_work->fn();
            } catch (const fc::exception& e) {
               elog("JOB_ID({}) FAILED: {}", job_id, e.to_detail_string());
            } catch (const boost::exception& e) {
               elog("JOB_ID({}) FAILED: {}", job_id, boost::diagnostic_information(e));
            } catch (const std::runtime_error& e) {
               elog("JOB_ID({}) FAILED: {}", job_id, e.what());
            } catch (const std::exception& e) {
               elog("JOB_ID({}) FAILED: {}", job_id, e.what());
            } catch (...) {
               elog("JOB_ID({}) unknown exception", job_id);
            }
            job_work->running.fetch_sub(1);
         });
      }
   }
}

void cron_service::replenish_triggers() {
   auto now = clock_t::now();
   auto jobs = _jobs.readable();
   for (const auto& [id, j] : jobs.map()) {
      if (j->cancelled)
         continue;
      std::scoped_lock tlock(j->triggers_mutex);
      if (j->upcoming_triggers.size() < schedule_trigger_count) {
         auto from = j->upcoming_triggers.empty() ? now : j->upcoming_triggers.back();
         auto additional = compute_next_n_triggers(
            j->schedule, from,
            schedule_trigger_count - j->upcoming_triggers.size());
         j->upcoming_triggers.insert(
            j->upcoming_triggers.end(),
            additional.begin(), additional.end());
      }
   }
}

void cron_service::wake_scheduler() {
   {
      std::scoped_lock lock(_scheduler_mutex);
      _schedule_requested = true;
   }
   _scheduler_cv.notify_one();
}

// ---------------------------------------------------------------------------
// Job management
// ---------------------------------------------------------------------------

cron_service::job_id_t cron_service::add(const job_schedule& sched, job_fn_t fn,
                                         const std::optional<job_metadata_t>& metadata) {
   auto j = std::make_shared<job>();
   j->id = _next_id++;
   j->schedule = sched;
   j->metadata = metadata.value_or(job_metadata_t{});
   j->fn = std::move(fn);

   if (is_running()) {
      auto now = clock_t::now();
      std::scoped_lock tlock(j->triggers_mutex);
      j->upcoming_triggers = compute_next_n_triggers(
         sched, now, schedule_trigger_count);
   }

   {
      auto jobs = _jobs.writeable();
      jobs.try_emplace(j->id, j);
   }

   if (is_running())
      wake_scheduler();

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
               return j->label() == query || std::ranges::contains(j->tags(), query);
            }
            return j->id == std::get<job_id_t>(query_var);
         });
      }) | std::ranges::to<std::vector>();

   return jobs | std::views::transform([](auto& j) { return j->id; }) |
          std::ranges::to<std::vector>();
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
   if (is_running())
      wake_scheduler();
}

void cron_service::cancel_all() {
   auto wv = _jobs.writeable();
   for (auto& [_, j] : wv.map()) {
      j->cancelled = true;
   }
   wv.clear();
}

} // namespace sysio::services
