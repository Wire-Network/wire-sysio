#include <sysio/operator_plugin/services/cron_service.hpp>

#include <boost/asio/post.hpp>

#include <ctime>

namespace sysio::operator_plugin::services {

  namespace {

    fc::logger& logger() {
      static fc::logger log{ "cron_service" };
      return log;
    }

    bool matches_field(const std::set<int>& allowed, int value) {
      if (allowed.empty()) return true;
      return allowed.contains(value);
    }

    std::tm to_local_tm(std::time_t tt) {
      std::tm out{};
#if defined(_WIN32)
      localtime_s(&out, &tt);
#else
      localtime_r(&tt, &out);
#endif
      return out;
    }

  } // anon


  std::shared_ptr<cron_service> cron_service::create(const std::optional<cron_service::options>& options) {
    cron_service::options opts = options ? *options : cron_service::options{};
    return std::shared_ptr<cron_service>{new cron_service(opts)};
  }

  cron_service::~cron_service() {
    stop();
  }

  cron_service::cron_service(const cron_service::options& options) : _options(options) {
    logger().log(FC_LOG_MESSAGE(info,"cron_service starting"));
    start();
  }

  void cron_service::start() {
    if (_running) {
      logger().log(FC_LOG_MESSAGE(warn,"cron_service already started"));
      return;
    }
    _pool.start(_options.num_threads, nullptr /*on_except*/);
    _running = true;
  }

  void cron_service::stop() {
    logger().log(FC_LOG_MESSAGE(info,"cron_service stop"));
    if (!_running) {
      logger().log(FC_LOG_MESSAGE(warn,"cron_service already stopped"));
      return;
    }
    cancel_all();
    _pool.stop();
    _running = false;
  }

  cron_service::job_id cron_service::add(const cron_schedule& sched, job_fn fn, const std::optional<job_options>& opts) {
    auto j = std::make_shared<job>();
    j->id = _next_id++;
    j->sched = sched;
    j->opts = opts.value_or(job_options{});
    j->fn = std::move(fn);
    j->timer = std::make_unique<boost::asio::steady_timer>(_pool.get_executor());

    {
      std::lock_guard<std::mutex> lg(_jobs_mx);
      _jobs.emplace(j->id, j);
    }

    auto now = std::chrono::system_clock::now();
    schedule_next(j, now);

    return j->id;
  }

  void cron_service::cancel(job_id id) {
    std::shared_ptr<job> j;
    {
      std::lock_guard<std::mutex> lg(_jobs_mx);
      auto it = _jobs.find(id);
      if (it == _jobs.end()) return;
      j = it->second;
      _jobs.erase(it);
    }
    if (j) {
      j->cancelled = true;
      if (j->timer) {
        try {
          j->timer->cancel();
        } catch (const std::exception& e) {
          logger().log(FC_LOG_MESSAGE(warn,
            "cron_service::cancel() timer->cancel() threw: ${what}",
            ("what", e.what())
          ));
        }
      }
    }
  }

  void cron_service::cancel_all() {
    std::vector<std::shared_ptr<job>> to_cancel;
    {
      std::lock_guard<std::mutex> lg(_jobs_mx);
      for (auto& kv : _jobs) to_cancel.push_back(kv.second);
      _jobs.clear();
    }
    for (auto& j : to_cancel) {
      j->cancelled = true;
      if (j->timer) {
        try {
          j->timer->cancel();
        } catch (const std::exception& e) {
          logger().log(FC_LOG_MESSAGE(warn,
            "cron_service::cancel() timer->cancel() threw: ${what}",
            ("what", e.what())
          ));
        }
      }
    }
  }

  std::chrono::system_clock::time_point cron_service::next_fire_time(
    const cron_schedule& sched,
    std::chrono::system_clock::time_point now
  ) {
    using clock = std::chrono::system_clock;
    using namespace std::chrono;

    auto tp = now;

    // Split into seconds and milliseconds components
    auto ms_since_epoch = duration_cast<milliseconds>(tp.time_since_epoch());
    auto sec_since_epoch = duration_cast<seconds>(ms_since_epoch);
    auto ms_in_sec = duration_cast<milliseconds>(ms_since_epoch - sec_since_epoch);

    // Helper to check all non-ms fields for a given whole-second time_point
    auto matches_non_ms = [&](const clock::time_point& t)-> bool {
      auto tt = clock::to_time_t(t);
      std::tm tm = to_local_tm(tt);
      int sec = tm.tm_sec;
      int min = tm.tm_min;
      int hour = tm.tm_hour;
      int mday = tm.tm_mday;
      int mon = tm.tm_mon + 1; // 0-11 -> 1-12
      int wday = tm.tm_wday; // 0-6, 0=Sunday
      return matches_field(sched.seconds, sec) && matches_field(sched.minutes, min) && matches_field(sched.hours, hour)
        && matches_field(sched.day_of_month, mday) && matches_field(sched.month, mon) && matches_field(
          sched.day_of_week,
          wday
        );
    };

    // First consider current second: if non-ms fields match, see if a future millisecond in this second matches
    clock::time_point sec_tp{sec_since_epoch};
    if (matches_non_ms(sec_tp)) {
      if (sched.milliseconds.empty()) {
        // Wildcard ms means trigger at the next millisecond >= now
        return tp + milliseconds(1); // ensure we don't fire in the past; minimal drift
      } else {
        int cur_ms = static_cast<int>(ms_in_sec.count());
        for (int ms : sched.milliseconds) {
          if (ms >= cur_ms) {
            return sec_tp + milliseconds(ms);
          }
        }
      }
    }

    // Otherwise, find the next whole second that matches the non-ms fields
    // Iterate by seconds up to a reasonable bound (e.g., 400 days) to avoid infinite loops on invalid schedules
    const int max_seconds_scan = 400 * 24 * 60 * 60; // ~400 days
    auto scan_tp = sec_tp + seconds(1);
    for (int i = 0; i < max_seconds_scan; ++i, scan_tp += seconds(1)) {
      if (matches_non_ms(scan_tp)) {
        if (sched.milliseconds.empty()) {
          return scan_tp; // trigger at start of that second
        } else {
          // Earliest millisecond in that second
          return scan_tp + milliseconds(*sched.milliseconds.begin());
        }
      }
    }

    // If nothing found, just push far into the future to avoid tight reschedule loops
    return now + hours(24);
  }

  void cron_service::schedule_next(std::shared_ptr<job> j, std::chrono::system_clock::time_point ref) {
    if (j->cancelled) return;

    auto next_tp = next_fire_time(j->sched, ref);
    auto now = std::chrono::system_clock::now();
    if (next_tp < now) next_tp = now;

    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next_tp - now);
    if (delay.count() < 0) delay = std::chrono::milliseconds(0);

    j->timer->expires_after(delay);
    j->timer->async_wait(
      [this, j](const boost::system::error_code& ec) {
        if (ec || j->cancelled) return; // cancelled or timer error

        // Decide whether to run the job
        bool should_run = true;
        if (j->opts.one_at_a_time) {
          int expected = 0;
          // Only run if currently not running
          should_run = j->running.compare_exchange_strong(expected, 1);
        } else {
          j->running.fetch_add(1);
        }

        if (should_run) {
          // Execute the job on the pool executor; then decrement running and reschedule
          boost::asio::post(
            _pool.get_executor(),
            [this, j] {
              try {
                j->fn();
              } catch (...) {
                // Swallow to keep scheduler alive. Production code may log.
              }
              j->running.fetch_sub(1);
              // Reschedule from completion time
              schedule_next(j, std::chrono::system_clock::now());
            }
          );
        } else {
          // Skipped due to one_at_a_time; just reschedule from now
          schedule_next(j, std::chrono::system_clock::now());
        }
      }
    );
  }

} // namespace sysio
