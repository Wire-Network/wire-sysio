
#include <boost/test/unit_test.hpp>
#include <sysio/producer_plugin/block_timing_util.hpp>
#include <fc/mock_time.hpp>

namespace fc {
std::ostream& boost_test_print_type(std::ostream& os, const time_point& t) { return os << t.to_iso_string(); }
std::ostream& boost_test_print_type(std::ostream& os, const std::optional<time_point>& t) { return os << (t ? t->to_iso_string() : "null"); }
} // namespace fc

static_assert(sysio::chain::config::block_interval_ms == 500);

constexpr auto       block_interval = fc::microseconds(sysio::chain::config::block_interval_us);
constexpr auto       cpu_effort_us  = 400000;
constexpr auto       cpu_effort     = fc::microseconds(cpu_effort_us);
constexpr auto       production_round_1st_block_slot = 100 * sysio::chain::config::producer_repetitions;


BOOST_AUTO_TEST_SUITE(block_timing_util)

BOOST_AUTO_TEST_CASE(test_production_round_block_start_time) {
   const fc::time_point production_round_1st_block_time =
      sysio::chain::block_timestamp_type(production_round_1st_block_slot).to_time_point();
   auto expected_start_time = production_round_1st_block_time - block_interval;
   for (int i = 0; i < sysio::chain::config::producer_repetitions;
        ++i, expected_start_time = expected_start_time + cpu_effort) {
      auto block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + i);
      BOOST_CHECK_EQUAL(sysio::block_timing_util::production_round_block_start_time(cpu_effort, block_time), expected_start_time);
   }
}

BOOST_AUTO_TEST_CASE(test_calculate_block_deadline) {
   using namespace sysio::block_timing_util;
   const fc::time_point production_round_1st_block_time =
      sysio::chain::block_timestamp_type(production_round_1st_block_slot).to_time_point();

   {
      // Scenario 1:
      // In producing mode, the deadline of a block will be ahead of its block_time from 100, 200, 300, ...ms,
      // depending on the its index to the starting block of a production round. These deadlines are referred
      // as optimized deadlines.
      fc::mock_time_traits::set_now(production_round_1st_block_time - block_interval + fc::milliseconds(10));
      for (int i = 0; i < sysio::chain::config::producer_repetitions; ++i) {
         auto block_time        = sysio::chain::block_timestamp_type(production_round_1st_block_slot + i);
         auto expected_deadline = block_time.to_time_point() - fc::milliseconds((i + 1) * 100);
         BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, block_time),
                           expected_deadline);
         fc::mock_time_traits::set_now(expected_deadline);
      }
   }
   {
      // Scenario 2:
      // In producing mode and it is already too late to meet the optimized deadlines,
      // the returned deadline can never be later than the next block deadline.
      // now is not taken into account, but leaving set_now in to verify it has no affect.

      auto second_block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + 1);
      fc::mock_time_traits::set_now(second_block_time.to_time_point() - fc::milliseconds(200));
      auto second_block_hard_deadline = second_block_time.to_time_point() - fc::milliseconds(200);
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, second_block_time),
                        second_block_hard_deadline);
      // use previous deadline as now
      fc::mock_time_traits::set_now(second_block_hard_deadline);
      auto third_block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + 2);
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, third_block_time),
                        third_block_time.to_time_point() - fc::milliseconds(300));

      // use previous deadline as now
      fc::mock_time_traits::set_now(third_block_time.to_time_point() - fc::milliseconds(300));
      auto forth_block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + 3);
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, forth_block_time),
                        forth_block_time.to_time_point() - fc::milliseconds(400));

      ///////////////////////////////////////////////////////////////////////////////////////////////////

      auto seventh_block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + 6);
      fc::mock_time_traits::set_now(seventh_block_time.to_time_point() - fc::milliseconds(500));

      // 7th block where cpu effort is 100ms less per block
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, seventh_block_time),
                        seventh_block_time.to_time_point() - fc::milliseconds(700));

      // use previous deadline as now
      fc::mock_time_traits::set_now(seventh_block_time.to_time_point() - fc::milliseconds(100));
      auto eighth_block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + 7);

      // 8th block where cpu effort is 100ms less per block
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, eighth_block_time),
                        eighth_block_time.to_time_point() - fc::milliseconds(800));

      // use previous deadline as now
      fc::mock_time_traits::set_now(eighth_block_time.to_time_point() - fc::milliseconds(200));
      auto ninth_block_time = sysio::chain::block_timestamp_type(production_round_1st_block_slot + 8);

      // 9th block where cpu effort is 100ms less per block
      BOOST_CHECK_EQUAL(calculate_producing_block_deadline(cpu_effort, ninth_block_time),
                        ninth_block_time.to_time_point() - fc::milliseconds(900));
   }
   {
      // Scenario: time has passed for block to be produced
      // Ask for a deadline for current block but its deadline has already passed
      auto default_cpu_effort = fc::microseconds(block_interval.count() - (450000/12)); // 462,500
      // with the default_cpu_effort, the first block deadline would be 29.9625 (29.500 + 0.4625)
      fc::mock_time_traits::set_now(fc::time_point::from_iso_string("2025-10-29T13:40:29.963")); // past first block interval
      auto first_block_time = sysio::chain::block_timestamp_type(fc::time_point::from_iso_string("2025-10-29T13:40:30.000"));
      // pass the first block deadline
      fc::time_point block_deadline = calculate_producing_block_deadline(default_cpu_effort, first_block_time);
      // not equal since block_deadline is 29.962500
      BOOST_CHECK_NE(block_deadline, fc::time_point::from_iso_string("2025-10-29T13:40:29.962"));
      BOOST_CHECK_EQUAL(block_deadline.to_iso_string(), fc::time_point::from_iso_string("2025-10-29T13:40:29.962").to_iso_string()); // truncates

      // second block has passed
      fc::mock_time_traits::set_now(fc::time_point::from_iso_string("2025-10-29T13:40:30.426")); // past second block interval
      auto second_block_time = sysio::chain::block_timestamp_type(fc::time_point::from_iso_string("2025-10-29T13:40:30.500"));
      // pass the second block deadline
      block_deadline = calculate_producing_block_deadline(default_cpu_effort, second_block_time);
      // 29.962500 + (450000/12) = 30.425
      BOOST_CHECK_EQUAL(block_deadline, fc::time_point::from_iso_string("2025-10-29T13:40:30.425"));
   }
   {
      // Real scenario that caused multiple start blocks
      auto default_cpu_effort = fc::microseconds(block_interval.count() - (450000/12)); // 462,500
      auto block_time = sysio::chain::block_timestamp_type(fc::time_point::from_iso_string("2025-10-31T13:37:35.000"));
      fc::time_point block_deadline = calculate_producing_block_deadline(default_cpu_effort, block_time);
      BOOST_CHECK_EQUAL(block_deadline.to_iso_string(), fc::time_point::from_iso_string("2025-10-31T13:37:34.587").to_iso_string());
   }
}

BOOST_AUTO_TEST_CASE(test_calculate_producer_wake_up_time) {
   using namespace sysio;
   using namespace sysio::chain;
   using namespace sysio::chain::literals;
   using namespace sysio::block_timing_util;

   // use full cpu effort for most of these tests since calculate_producing_block_deadline is tested above
   constexpr fc::microseconds full_cpu_effort = fc::microseconds{sysio::chain::config::block_interval_us};

   { // no producers
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, chain::block_timestamp_type{}, {}, {}), std::optional<fc::time_point>{});
   }
   { // producers not in active_schedule
      std::set<chain::account_name>          producers{"p1"_n, "p2"_n};
      std::vector<chain::producer_authority> active_schedule{{"active1"_n}, {"active2"_n}};
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, chain::block_timestamp_type{}, producers, active_schedule), std::optional<fc::time_point>{});
   }
   { // Only one producer in active_schedule, we should produce every block
      std::set<chain::account_name>          producers{"p1"_n, "p2"_n};
      std::vector<chain::producer_authority> active_schedule{{"p1"_n}};
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * sysio::chain::config::producer_repetitions - 1;
      for (uint32_t i = 0; i < static_cast<uint32_t>(config::producer_repetitions * active_schedule.size() * 3); ++i) { // 3 rounds to test boundaries
         block_timestamp_type block_timestamp(prod_round_1st_block_slot + i);
         auto block_time = block_timestamp.to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{block_time});
      }
   }
   { // We have all producers in active_schedule configured, we should produce every block
      std::set<chain::account_name>          producers{"p1"_n, "p2"_n, "p3"_n};
      std::vector<chain::producer_authority> active_schedule{{"p1"_n}, {"p2"_n}};
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * sysio::chain::config::producer_repetitions - 1;
      for (uint32_t i = 0; i < static_cast<uint32_t>(config::producer_repetitions * active_schedule.size() * 3); ++i) { // 3 rounds to test boundaries
         block_timestamp_type block_timestamp(prod_round_1st_block_slot + i);
         auto block_time = block_timestamp.to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{block_time});
      }
   }
   { // We have all producers in active_schedule of 21 (plus a couple of extra producers configured), we should produce every block
      std::set<account_name> producers = {
         "inita"_n, "initb"_n, "initc"_n, "initd"_n, "inite"_n, "initf"_n, "initg"_n, "p1"_n,
         "inith"_n, "initi"_n, "initj"_n, "initk"_n, "initl"_n, "initm"_n, "initn"_n,
         "inito"_n, "initp"_n, "initq"_n, "initr"_n, "inits"_n, "initt"_n, "initu"_n, "p2"_n
      };
      std::vector<chain::producer_authority> active_schedule{
         {"inita"_n}, {"initb"_n}, {"initc"_n}, {"initd"_n}, {"inite"_n}, {"initf"_n}, {"initg"_n},
         {"inith"_n}, {"initi"_n}, {"initj"_n}, {"initk"_n}, {"initl"_n}, {"initm"_n}, {"initn"_n},
         {"inito"_n}, {"initp"_n}, {"initq"_n}, {"initr"_n}, {"inits"_n}, {"initt"_n}, {"initu"_n}
      };
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * sysio::chain::config::producer_repetitions - 1;
      for (uint32_t i = 0; i < static_cast<uint32_t>(config::producer_repetitions * active_schedule.size() * 3); ++i) { // 3 rounds to test boundaries
         block_timestamp_type block_timestamp(prod_round_1st_block_slot + i);
         auto block_time = block_timestamp.to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{block_time});
      }
   }
   { // Tests for when we only have a subset of all active producers, we do not produce all blocks, only produce blocks for our round
      std::vector<chain::producer_authority> active_schedule{ // 21
         {"inita"_n}, {"initb"_n}, {"initc"_n}, {"initd"_n}, {"inite"_n}, {"initf"_n}, {"initg"_n},
         {"inith"_n}, {"initi"_n}, {"initj"_n}, {"initk"_n}, {"initl"_n}, {"initm"_n}, {"initn"_n},
         {"inito"_n}, {"initp"_n}, {"initq"_n}, {"initr"_n}, {"inits"_n}, {"initt"_n}, {"initu"_n}
      };
      const uint32_t prod_round_1st_block_slot = 100 * active_schedule.size() * sysio::chain::config::producer_repetitions - 1;

      // initb is second in the schedule, so it will produce config::producer_repetitions after start, verify its block times
      std::set<account_name> producers = { "initb"_n };
      block_timestamp_type block_timestamp(prod_round_1st_block_slot);
      auto expected_block_time = block_timestamp_type(prod_round_1st_block_slot + config::producer_repetitions).to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-1}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions-1}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions-2}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions-3}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      // current which gives same expected
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});
      expected_block_time += fc::microseconds(config::block_interval_us);
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot+config::producer_repetitions+1}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});

      // inita is first in the schedule, prod_round_1st_block_slot is block time of the first block, so will return the next block time as that is when current should be produced
      producers = std::set<account_name>{ "inita"_n };
      block_timestamp = block_timestamp_type{prod_round_1st_block_slot};
      expected_block_time = block_timestamp.to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-1}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-2}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp_type{block_timestamp.slot-3}, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // same
      for (size_t i = 0; i < config::producer_repetitions; ++i) {
         expected_block_time = block_timestamp_type(prod_round_1st_block_slot+i).to_time_point();
         BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});
         block_timestamp = block_timestamp.next();
      }
      expected_block_time = block_timestamp.to_time_point();
      BOOST_CHECK_NE(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time}); // end of round, so not the next

      // initc is third in the schedule, verify its wake-up time is as expected
      producers = std::set<account_name>{ "initc"_n };
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot);
      // expect 2*producer_repetitions since we expect wake-up time to be after the first two rounds
      expected_block_time = block_timestamp_type(prod_round_1st_block_slot + 2*config::producer_repetitions).to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});

      // inith, initk - configured for 2 of the 21 producers. inith is 8th in schedule, initk is 11th in schedule
      producers = std::set<account_name>{ "inith"_n, "initk"_n };
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot);
      // expect to produce after 7 rounds since inith is 8th
      expected_block_time = block_timestamp_type(prod_round_1st_block_slot + 7*config::producer_repetitions).to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});
      // give it a time after inith otherwise would return inith time
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot + 8*config::producer_repetitions); // after inith round
      // expect to produce after 10 rounds since inith is 11th
      expected_block_time = block_timestamp_type(prod_round_1st_block_slot + 10*config::producer_repetitions).to_time_point();
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(full_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});

      // cpu_effort at 50%, initc
      constexpr fc::microseconds half_cpu_effort = fc::microseconds{sysio::chain::config::block_interval_us / 2u};
      producers = std::set<account_name>{ "initc"_n };
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot);
      expected_block_time = block_timestamp_type(prod_round_1st_block_slot + 2*config::producer_repetitions).to_time_point();
      // first in round is not affected by cpu effort
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(half_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot + 2*config::producer_repetitions + 1);
      // second in round is 50% sooner
      expected_block_time = block_timestamp.to_time_point();
      expected_block_time -= fc::microseconds(half_cpu_effort);
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(half_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});
      // third in round is 2*50% sooner
      block_timestamp = block_timestamp_type(prod_round_1st_block_slot + 2*config::producer_repetitions + 2);
      // second in round is 50% sooner
      expected_block_time = block_timestamp.to_time_point();
      expected_block_time -= fc::microseconds(2*half_cpu_effort.count());
      BOOST_CHECK_EQUAL(calculate_producer_wake_up_time(half_cpu_effort, 2, block_timestamp, producers, active_schedule), std::optional<fc::time_point>{expected_block_time});
   }
   { // actual example that caused multiple start blocks
      std::set<account_name> producers = {
              "inita"_n, "initb"_n, "initc"_n, "initd"_n, "inite"_n, "initf"_n, "initg"_n, "p1"_n,
              "inith"_n, "initi"_n, "initj"_n, "initk"_n, "initl"_n, "initm"_n, "initn"_n,
              "inito"_n, "initp"_n, "initq"_n, "initr"_n, "inits"_n, "initt"_n, "initu"_n, "p2"_n
      };
      std::vector<chain::producer_authority> active_schedule{
              {"inita"_n}, {"initb"_n}, {"initc"_n}, {"initd"_n}, {"inite"_n}, {"initf"_n}, {"initg"_n},
              {"inith"_n}, {"initi"_n}, {"initj"_n}, {"initk"_n}, {"initl"_n}, {"initm"_n}, {"initn"_n},
              {"inito"_n}, {"initp"_n}, {"initq"_n}, {"initr"_n}, {"inits"_n}, {"initt"_n}, {"initu"_n}
      };
      auto default_cpu_effort = fc::microseconds(block_interval.count() - (450000/12)); // 462,500
      auto wake_time = calculate_producer_wake_up_time(default_cpu_effort, 106022362, sysio::chain::block_timestamp_type(fc::time_point::from_iso_string("2025-10-31T16:06:41.000")),
                                                       producers, active_schedule);
      BOOST_REQUIRE(!!wake_time);
      BOOST_CHECK_EQUAL(wake_time->to_iso_string(), fc::time_point::from_iso_string("2025-10-31T16:06:40.587").to_iso_string());
   }

}

BOOST_AUTO_TEST_SUITE_END()
