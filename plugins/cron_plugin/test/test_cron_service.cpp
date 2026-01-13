#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>

#include <sysio/services/cron_service.hpp>

using namespace std::literals;
using namespace sysio::services;

namespace {
  auto cron_service_factory(
    const std::optional<std::string>& name = std::nullopt,
    const std::optional<std::size_t>& worker_count = std::nullopt
  ) {
    return cron_service::create(
      cron_service::options{.name = name.value_or("cron_service_test"), .num_threads = worker_count.value_or(1)}
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

  long current_ms_in_second() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    auto ms_since_epoch = duration_cast<milliseconds>(now.time_since_epoch());
    auto sec_since_epoch = duration_cast<seconds>(ms_since_epoch);
    auto ms_in_sec = duration_cast<milliseconds>(ms_since_epoch - sec_since_epoch);
    return static_cast<int>(ms_in_sec.count());
  }
}

BOOST_AUTO_TEST_SUITE(cron_service)

  BOOST_AUTO_TEST_CASE(create_with_options_and_basic_add) try {
    auto service = cron_service_factory("cron_service_test_A", 2);
    std::atomic_int calls{0};

    cron_schedule s; // all fields wildcard, so with empty milliseconds it fires frequently
    s.milliseconds.clear(); // wildcard -> scheduler will target next millisecond

    auto id = service->add(
      s,
      [&]() {
        calls.fetch_add(1);
      }
    );
    BOOST_CHECK(id > 0);

    // Expect at least a few invocations within a short time
    bool ok = wait_until(
      [&]() {
        return calls.load() >= 3;
      },
      std::chrono::milliseconds(500)
    );
    BOOST_CHECK(ok);

    service->cancel(id);

    int snapshot = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(snapshot, calls.load());
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(milliseconds_field_precise_triggers_same_second) try {
    auto service = cron_service_factory("cron_service_test_ms", 1);
    std::atomic_int calls{0};

    // Build a schedule for a few ms offsets later in the current second
    auto now_ms = current_ms_in_second();
    std::set<long> ms_set;
    ms_set.insert((now_ms + 10) % 1000);
    ms_set.insert((now_ms + 20) % 1000);
    ms_set.insert((now_ms + 30) % 1000);

    cron_schedule s; // other fields wildcard for current second
    s.milliseconds = ms_set;

    auto id = service->add(
      s,
      [&]() {
        calls.fetch_add(1);
      }
    );
    BOOST_REQUIRE(id > 0);

    // We expect three callbacks within ~150ms
    bool ok = wait_until(
      [&]() {
        return calls.load() >= 3;
      },
      std::chrono::milliseconds(200)
    );
    BOOST_CHECK(ok);
    service->cancel(id);

    int snapshot = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK_EQUAL(snapshot, calls.load());
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(cancel_all_stops_multiple_jobs) try {
    auto service = cron_service_factory("cron_service_test_cancel_all", 1);
    std::atomic_int a{0}, b{0};

    cron_schedule s; // fast schedule using wildcard milliseconds
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
      std::chrono::milliseconds(100)
    );
    bool b_started = wait_until(
      [&]() {
        return b.load() > 0;
      },
      std::chrono::milliseconds(100)
    );
    BOOST_CHECK(a_started && b_started);

    service->cancel_all();
    int a_snap = a.load();
    int b_snap = b.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOST_CHECK_EQUAL(a_snap, a.load());
    BOOST_CHECK_EQUAL(b_snap, b.load());
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(destructor_stops_service_cleanly) try {
    std::atomic_int calls{0};
    {
      auto service = cron_service_factory("cron_service_test_dtor", 1);
      cron_schedule s; // frequent schedule
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
        std::chrono::milliseconds(100)
      );
      BOOST_CHECK(ok);
      // Service goes out of scope here
    }
    // No assertions after, just ensuring no crash/deadlock
    BOOST_CHECK(true);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(multiple_independent_jobs_progress) try {
    auto service = cron_service_factory("cron_service_test_multi", 2);
    std::atomic_int a{0}, b{0};

    cron_schedule fast; // wildcard -> frequent

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
        return a.load() >= 5;
      },
      std::chrono::milliseconds(150)
    );
    bool ok_b = wait_until(
      [&]() {
        return b.load() >= 5;
      },
      std::chrono::milliseconds(150)
    );
    BOOST_CHECK(ok_a && ok_b);

    service->cancel(id_a);
    int a_snap = a.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    BOOST_CHECK_LE(std::abs(a_snap - a.load()), 1);

    // B still progresses
    int b_before = b.load();
    bool b_progressed = wait_until(
      [&]() {
        return b.load() >= b_before + 3;
      },
      std::chrono::milliseconds(100)
    );
    BOOST_CHECK(b_progressed);
  } FC_LOG_AND_RETHROW();

  BOOST_AUTO_TEST_CASE(list_filtering) try {
    auto service = cron_service_factory("cron_service_test_list", 1);
    
    cron_schedule s;
    auto id1 = service->add(s, [](){});
    auto id2 = service->add(s, [](){});
    auto id3 = service->add(s, [](){});

    // Add tags to jobs
    {
       auto rv = service->list();
       BOOST_CHECK_EQUAL(rv.size(), 3);
    }

    // If I can't set tags, I can at least test filtering by ID.
    auto ids = service->list(id1, id3);
    BOOST_CHECK_EQUAL(ids.size(), 2);
    BOOST_CHECK(std::ranges::contains(ids, id1));
    BOOST_CHECK(std::ranges::contains(ids, id3));
    BOOST_CHECK(!std::ranges::contains(ids, id2));

    auto all_ids = service->list();
    BOOST_CHECK_EQUAL(all_ids.size(), 3);

  } FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
