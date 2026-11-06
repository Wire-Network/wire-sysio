#include <boost/test/unit_test.hpp>
#include <fc/exception/exception.hpp>
#include <fc/time.hpp>

#include <array>
#include <string>

namespace {
constexpr auto whole_second = "2026-10-23T09:44:37";
constexpr auto compact_second = "20261023T094437";
constexpr auto microseconds_per_millisecond = 1000;

/// Pair an ISO fractional spelling with its truncated microsecond count.
struct fraction_fixture {
   const char* suffix;
   int64_t microseconds;
};

constexpr std::array fractions = {
   fraction_fixture{"", 0},
   fraction_fixture{".", 0},
   fraction_fixture{".5", 500000},
   fraction_fixture{".57", 570000},
   fraction_fixture{".579", 579000},
   fraction_fixture{".5790", 579000},
   fraction_fixture{".57908", 579080},
   fraction_fixture{".579083", 579083},
   fraction_fixture{".5790839", 579083},
   fraction_fixture{".579083999", 579083},
   fraction_fixture{".000000999", 0},
   fraction_fixture{".999999999999999999999999999999", 999999},
};
} // namespace

BOOST_AUTO_TEST_SUITE(time_test_suite)

/// Fractions scale to microseconds, truncate excess digits, and never move the whole second.
BOOST_AUTO_TEST_CASE(iso_fraction_precision) {
   const auto base = fc::time_point_sec::from_iso_string(whole_second);
   for (const auto* second : {whole_second, compact_second}) {
      for (const auto& fixture : fractions) {
         const auto input = std::string(second) + fixture.suffix;
         BOOST_TEST_CONTEXT(input) {
            const auto parsed = fc::time_point::from_iso_string(input);
            BOOST_CHECK_EQUAL((parsed - base.to_time_point()).count(), fixture.microseconds);
            BOOST_CHECK(fc::time_point_sec::from_iso_string(input) == base);
         }
      }
   }
   constexpr std::array invalid_fractions = {".Z", ".579083x", ".579083999x", ".579083Zjunk", ".-1"};
   for (const auto* fraction : invalid_fractions) {
      const auto input = std::string(whole_second) + fraction;
      BOOST_TEST_CONTEXT(input) {
         BOOST_CHECK_THROW(fc::time_point::from_iso_string(input), fc::exception);
      }
   }
}

/// Preserve rejection of the UTC suffix forms rejected by the original parser.
BOOST_AUTO_TEST_CASE(iso_fraction_utc_suffix) {
   constexpr std::array rejected_inputs = {
      "2026-10-23T09:44:37Z",
      "2026-10-23T09:44:37.5Z",
      "2026-10-23T09:44:37.579Z",
      "20261023T094437.5Z",
      "20261023T094437.579Z",
   };
   for (const auto* input : rejected_inputs) {
      BOOST_TEST_CONTEXT(input) {
         BOOST_CHECK_THROW(fc::time_point::from_iso_string(input), fc::exception);
         BOOST_CHECK_THROW(fc::time_point_sec::from_iso_string(input), fc::exception);
      }
   }
}

/// Preserve the three fractions observed to shift genesis forward by many minutes.
BOOST_AUTO_TEST_CASE(iso_genesis_regressions) {
   /// Keep the expected whole second separate from the observed input fraction.
   struct genesis_fixture {
      const char* second;
      const char* fraction;
      int64_t microseconds;
   };
   constexpr std::array fixtures = {
      genesis_fixture{"2026-10-23T09:44:37", ".579083", 579083},
      genesis_fixture{"2026-10-23T09:44:10", ".092330", 92330},
      genesis_fixture{"2026-10-23T21:20:12", ".040645", 40645},
   };
   for (const auto& fixture : fixtures) {
      const auto base = fc::time_point_sec::from_iso_string(fixture.second).to_time_point();
      const auto input = std::string(fixture.second) + fixture.fraction;
      BOOST_TEST_CONTEXT(input) {
         BOOST_CHECK_EQUAL((fc::time_point::from_iso_string(input) - base).count(), fixture.microseconds);
      }
   }
}

/// Serialization retains milliseconds; reparsing loses only the submillisecond remainder.
BOOST_AUTO_TEST_CASE(iso_fraction_round_trip) {
   const auto base = fc::time_point_sec::from_iso_string(whole_second).to_time_point();
   for (const auto& fixture : fractions) {
      const auto input = std::string(whole_second) + fixture.suffix;
      BOOST_TEST_CONTEXT(input) {
         const auto parsed = fc::time_point::from_iso_string(input);
         const auto restored = fc::time_point::from_iso_string(parsed.to_iso_string());
         const auto expected = fixture.microseconds / microseconds_per_millisecond * microseconds_per_millisecond;
         BOOST_CHECK_EQUAL((restored - base).count(), expected);
         BOOST_CHECK_EQUAL((parsed - restored).count(), fixture.microseconds % microseconds_per_millisecond);
         BOOST_CHECK_EQUAL(restored.to_iso_string(), parsed.to_iso_string());
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
