#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <thread>
#include <optional>
#include <set>

#include <sysio/services/cron_service.hpp>

using namespace std::literals;
using svc = sysio::services::cron_service;

namespace {
  auto cron_service_factory(
    const std::optional<std::string>& name = std::nullopt,
    const std::optional<std::size_t>& worker_count = std::nullopt
  ) {
    return svc::create(
      svc::options{.name = name.value_or("cron_service_test"), .num_threads = worker_count.value_or(1)}
    );
  }

  // Utility: busy-wait with small sleeps until predicate true or timeout
  template <typename Pred>
  bool wait_until(
    Pred&& p,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds step = std::chrono::milliseconds(2)
  ) {
    auto start = std::chrono::steady_clock::now();
    while (!p()) {
      if (std::chrono::steady_clock::now() - start > timeout) return false;
      std::this_thread::sleep_for(step);
    }
    return true;
  }

  long current_ms_in_minute() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tp_days = floor<days>(now);
    auto day_offset = now - tp_days;
    auto h = duration_cast<hours>(day_offset);
    day_offset -= h;
    auto m = duration_cast<minutes>(day_offset);
    day_offset -= m;
    auto ms = duration_cast<milliseconds>(day_offset);
    return static_cast<long>(ms.count());
  }
}

BOOST_AUTO_TEST_SUITE(cron_service)

  // -----------------------------------------------------------------------
  // Original tests — adapted for new API
  // -----------------------------------------------------------------------

  BOOST_AUTO_TEST_CASE(create_with_options_and_basic_add) try {
    std::atomic_int calls{0};
    auto service = cron_service_factory("cron_service_test_A", 2);

    svc::schedule s;
    s.milliseconds.insert(svc::schedule::step_value{1000}); // every second

    auto id = service->add(
      s,
      [&]() {
        calls.fetch_add(1);
      }
    );
    BOOST_CHECK(id > 0);

    // With wildcard schedule (fires every second at each ms), expect invocations
    bool ok = wait_until(
      [&]() {
        return calls.load() >= 3;
      },
      std::chrono::milliseconds(5000)
    );
    BOOST_CHECK(ok);

    service->cancel(id);

    int snapshot = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // After cancel, calls should not increase significantly
    BOOST_CHECK_LE(calls.load() - snapshot, 1);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(milliseconds_field_precise_triggers_same_second) try {
    std::atomic_int calls{0};
    auto service = cron_service_factory("cron_service_test_ms", 1);

    // Build a schedule for a few ms offsets
    auto now_ms = current_ms_in_minute();
    svc::schedule s;
    s.milliseconds.insert(svc::schedule::exact_value{static_cast<std::uint64_t>((now_ms + 100) % 60000)});
    s.milliseconds.insert(svc::schedule::exact_value{static_cast<std::uint64_t>((now_ms + 200) % 60000)});
    s.milliseconds.insert(svc::schedule::exact_value{static_cast<std::uint64_t>((now_ms + 300) % 60000)});

    auto id = service->add(
      s,
      [&]() {
        calls.fetch_add(1);
      }
    );
    BOOST_REQUIRE(id > 0);

    // We expect three callbacks within a reasonable time
    bool ok = wait_until(
      [&]() {
        return calls.load() >= 3;
      },
      std::chrono::milliseconds(3000)
    );
    BOOST_CHECK(ok);
    service->cancel(id);

    int snapshot = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_CHECK_LE(calls.load() - snapshot, 1);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(cancel_all_stops_multiple_jobs) try {
    std::atomic_int a{0}, b{0};
    auto service = cron_service_factory("cron_service_test_cancel_all", 2);

    svc::schedule s;
    s.milliseconds.insert(svc::schedule::step_value{1000}); // every second
    auto id1 = service->add(
      s,
      [&]() {
        a.fetch_add(1);
      }
    );
    auto id2 = service->add(
      s,
      [&]() {
        b.fetch_add(1);
      }
    );
    BOOST_REQUIRE(id1 != id2);

    bool a_started = wait_until(
      [&]() {
        return a.load() > 0;
      },
      std::chrono::milliseconds(3000)
    );
    bool b_started = wait_until(
      [&]() {
        return b.load() > 0;
      },
      std::chrono::milliseconds(3000)
    );
    BOOST_CHECK(a_started && b_started);

    service->cancel_all();
    int a_snap = a.load();
    int b_snap = b.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_CHECK_LE(a.load() - a_snap, 1);
    BOOST_CHECK_LE(b.load() - b_snap, 1);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(destructor_stops_service_cleanly) try {
    std::atomic_int calls{0};
    {
      auto service = cron_service_factory("cron_service_test_dtor", 1);
      svc::schedule s;
      s.milliseconds.insert(svc::schedule::step_value{1000}); // every second
      service->add(
        s,
        [&]() {
          calls.fetch_add(1);
        }
      );
      bool ok = wait_until(
        [&]() {
          return calls.load() > 0;
        },
        std::chrono::milliseconds(3000)
      );
      BOOST_CHECK(ok);
      // Service goes out of scope here
    }
    // No assertions after, just ensuring no crash/deadlock
    BOOST_CHECK(true);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(multiple_independent_jobs_progress) try {
    std::atomic_int a{0}, b{0};
    auto service = cron_service_factory("cron_service_test_multi", 2);

    svc::schedule fast;
    fast.milliseconds.insert(svc::schedule::step_value{1000}); // every second

    auto id_a = service->add(
      fast,
      [&]() {
        a.fetch_add(1);
      }
    );
    auto id_b = service->add(
      fast,
      [&]() {
        b.fetch_add(1);
      }
    );
    BOOST_REQUIRE(id_a != id_b);

    bool ok_a = wait_until(
      [&]() {
        return a.load() >= 3;
      },
      std::chrono::milliseconds(5000)
    );
    bool ok_b = wait_until(
      [&]() {
        return b.load() >= 3;
      },
      std::chrono::milliseconds(5000)
    );
    BOOST_CHECK(ok_a && ok_b);

    service->cancel(id_a);
    int a_snap = a.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    BOOST_CHECK_LE(std::abs(a_snap - a.load()), 1);

    // B still progresses
    int b_before = b.load();
    bool b_progressed = wait_until(
      [&]() {
        return b.load() >= b_before + 3;
      },
      std::chrono::milliseconds(5000)
    );
    BOOST_CHECK(b_progressed);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(list_filtering) try {
    auto service = cron_service_factory("cron_service_test_list", 1);

    svc::schedule s;
    auto id1 = service->add(s, [](){});
    auto id2 = service->add(s, [](){});
    auto id3 = service->add(s, [](){});

    {
       auto rv = service->list();
       BOOST_CHECK_EQUAL(rv.size(), 3);
    }

    auto ids = service->list(id1, id3);
    BOOST_CHECK_EQUAL(ids.size(), 2);
    BOOST_CHECK(std::ranges::contains(ids, id1));
    BOOST_CHECK(std::ranges::contains(ids, id3));
    BOOST_CHECK(!std::ranges::contains(ids, id2));

    auto all_ids = service->list();
    BOOST_CHECK_EQUAL(all_ids.size(), 3);

  } FC_LOG_AND_RETHROW();

  // -----------------------------------------------------------------------
  // New tests for expanded schedule model
  // -----------------------------------------------------------------------

  BOOST_AUTO_TEST_CASE(test_expand_field_exact) try {
    std::set<svc::schedule::schedule_value> field;
    field.insert(svc::schedule::exact_value{5});
    field.insert(svc::schedule::exact_value{10});
    field.insert(svc::schedule::exact_value{15});

    auto result = svc::schedule::expand_field(field, 0, 59);
    BOOST_CHECK_EQUAL(result.size(), 3);
    BOOST_CHECK(result.contains(5));
    BOOST_CHECK(result.contains(10));
    BOOST_CHECK(result.contains(15));
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_expand_field_step) try {
    std::set<svc::schedule::schedule_value> field;
    field.insert(svc::schedule::step_value{15});

    auto result = svc::schedule::expand_field(field, 0, 59);
    BOOST_CHECK_EQUAL(result.size(), 4);
    BOOST_CHECK(result.contains(0));
    BOOST_CHECK(result.contains(15));
    BOOST_CHECK(result.contains(30));
    BOOST_CHECK(result.contains(45));
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_expand_field_range) try {
    std::set<svc::schedule::schedule_value> field;
    field.insert(svc::schedule::range_value{3, 7});

    auto result = svc::schedule::expand_field(field, 1, 31);
    BOOST_CHECK_EQUAL(result.size(), 5);
    for (std::uint64_t i = 3; i <= 7; ++i)
      BOOST_CHECK(result.contains(i));
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_expand_field_wildcard) try {
    std::set<svc::schedule::schedule_value> field; // empty = wildcard
    auto result = svc::schedule::expand_field(field, 0, 59);
    BOOST_CHECK(result.empty()); // expand_field returns empty for wildcards
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_step_value_schedule) try {
    // step_value{250} for milliseconds should fire at ms 0, 250, 500, 750
    // within each second of each minute.
    std::atomic_int calls{0};
    auto service = cron_service_factory("cron_svc_step", 2);

    svc::schedule s;
    s.milliseconds.insert(svc::schedule::step_value{250});

    auto id = service->add(
      s,
      [&]() {
        calls.fetch_add(1);
      }
    );

    // 4 fires per second => should get many calls within 3 seconds
    bool ok = wait_until(
      [&]() {
        return calls.load() >= 8;
      },
      std::chrono::milliseconds(5000)
    );
    BOOST_CHECK(ok);
    service->cancel(id);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_range_value_schedule) try {
    // range_value for minutes: fire only in minutes 0-2
    // We verify that next_fire_time produces times within that range.
    using namespace std::chrono;

    svc::schedule s;
    s.minutes.insert(svc::schedule::range_value{0, 2});
    s.milliseconds.insert(svc::schedule::exact_value{0});

    auto now = system_clock::now();
    auto triggers = svc::compute_next_n_triggers(s, now, 5);
    BOOST_CHECK_EQUAL(triggers.size(), 5);

    for (const auto& tp : triggers) {
      auto tp_days = floor<days>(tp);
      auto day_offset = tp - tp_days;
      auto h = duration_cast<hours>(day_offset);
      day_offset -= h;
      auto m = duration_cast<minutes>(day_offset);
      auto minute_val = static_cast<unsigned>(m.count());
      BOOST_CHECK_LE(minute_val, 2u);
    }
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_next_fire_time_algorithm) try {
    using namespace std::chrono;

    // Schedule: minute 30, hour 14, ms 0, all other fields wildcard.
    // From any time before 14:30, the next fire should be today at 14:30:00.000.
    // From after 14:30, it should be tomorrow at 14:30:00.000.
    svc::schedule s;
    s.hours.insert(svc::schedule::exact_value{14});
    s.minutes.insert(svc::schedule::exact_value{30});
    s.milliseconds.insert(svc::schedule::exact_value{0});

    // Construct a known time: 2025-06-15 10:00:00.000 UTC
    auto base = sys_days{year{2025} / month{6} / day{15}} + hours{10};

    auto next = svc::next_fire_time(s, base);
    auto tp_days = floor<days>(next);
    year_month_day ymd{tp_days};
    auto day_offset = next - tp_days;
    auto h = duration_cast<hours>(day_offset);
    day_offset -= h;
    auto m = duration_cast<minutes>(day_offset);

    BOOST_CHECK_EQUAL(static_cast<int>(ymd.year()), 2025);
    BOOST_CHECK_EQUAL(static_cast<unsigned>(ymd.month()), 6u);
    BOOST_CHECK_EQUAL(static_cast<unsigned>(ymd.day()), 15u);
    BOOST_CHECK_EQUAL(h.count(), 14);
    BOOST_CHECK_EQUAL(m.count(), 30);

    // Now test from after 14:30 — should be next day
    auto after = sys_days{year{2025} / month{6} / day{15}} + hours{15};
    auto next2 = svc::next_fire_time(s, after);
    auto tp_days2 = floor<days>(next2);
    year_month_day ymd2{tp_days2};
    BOOST_CHECK_EQUAL(static_cast<unsigned>(ymd2.day()), 16u);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_trigger_precomputation) try {
    using namespace std::chrono;

    // Schedule: every hour at minute 0, second 0, ms 0.
    svc::schedule s;
    s.minutes.insert(svc::schedule::exact_value{0});
    s.milliseconds.insert(svc::schedule::exact_value{0});

    auto now = system_clock::now();
    auto triggers = svc::compute_next_n_triggers(s, now, 8);
    BOOST_CHECK_EQUAL(triggers.size(), 8);

    // Triggers should be in chronological order
    for (std::size_t i = 1; i < triggers.size(); ++i) {
      BOOST_CHECK(triggers[i] > triggers[i - 1]);
    }
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_scheduler_wakes_on_new_job) try {
    std::atomic_int calls{0};
    auto service = cron_service_factory("cron_svc_wake", 1);

    // Wait a bit, then add a job and verify it fires.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    svc::schedule s;
    s.milliseconds.insert(svc::schedule::step_value{1000}); // every second
    auto id = service->add(
      s,
      [&]() {
        calls.fetch_add(1);
      }
    );

    bool ok = wait_until(
      [&]() {
        return calls.load() >= 1;
      },
      std::chrono::milliseconds(5000)
    );
    BOOST_CHECK(ok);
    service->cancel(id);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(test_day_of_week_schedule) try {
    using namespace std::chrono;

    // Schedule constrained to Wednesdays (day_of_week = 3), at 12:00:00.000
    svc::schedule s;
    s.day_of_week.insert(svc::schedule::exact_value{3}); // Wednesday
    s.hours.insert(svc::schedule::exact_value{12});
    s.minutes.insert(svc::schedule::exact_value{0});
    s.milliseconds.insert(svc::schedule::exact_value{0});

    // Start from a known Monday: 2025-06-16 is a Monday
    auto base = sys_days{year{2025} / month{6} / day{16}} + hours{10};
    auto next = svc::next_fire_time(s, base);

    auto tp_days = floor<days>(next);
    year_month_day ymd{tp_days};
    weekday wd{tp_days};

    // Should land on Wednesday 2025-06-18
    BOOST_CHECK_EQUAL(static_cast<unsigned>(ymd.day()), 18u);
    BOOST_CHECK_EQUAL(wd.c_encoding(), 3u); // Wednesday
  } FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
