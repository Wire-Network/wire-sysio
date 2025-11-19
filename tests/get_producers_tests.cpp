#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio_system_tester.hpp>

#include <fc/variant_object.hpp>

#include <fc/log/logger.hpp>


BOOST_AUTO_TEST_SUITE(get_producers_tests)
using namespace sysio::testing;

// this test verifies the exception case of get_producer, where it is populated by the active schedule of producers
BOOST_AUTO_TEST_CASE_TEMPLATE( get_producers, T, testers ) { try {
      T chain;

      std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
      sysio::chain_apis::read_only plugin(*(chain.control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
      sysio::chain_apis::read_only::get_producers_params params = { .json = true, .lower_bound = "", .limit = 21 };

      auto results = plugin.get_producers(params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(results.more, "");
      BOOST_REQUIRE_EQUAL(results.rows.size(), 1u);
      const auto& row = results.rows[0].get_object();
      BOOST_REQUIRE(row.contains("owner"));
      BOOST_REQUIRE_EQUAL(row["owner"].as_string(), "sysio");
      // check for producer_authority, since it is only set when the producer schedule is used
      BOOST_REQUIRE(row.contains("producer_authority"));


      chain.produce_block();

      chain.create_accounts( {"dan"_n,"sam"_n,"pam"_n} );
      chain.produce_block();
      chain.set_producers( {"dan"_n,"sam"_n,"pam"_n} );
      chain.produce_block();
      chain.produce_block(fc::seconds(1000));
      auto b = chain.produce_block();
      auto index = b->timestamp.slot % config::producer_repetitions;
      chain.produce_blocks(config::producer_repetitions - index - 1); // until the last block of round 1
      chain.produce_blocks(config::producer_repetitions); // round 2
      chain.produce_block(); // round 3

      results = plugin.get_producers(params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(results.rows.size(), 3u);
      auto owners = std::vector<std::string>{"dan", "sam", "pam"};
      auto it     = owners.begin();
      for (const auto& elem : results.rows) {
         BOOST_REQUIRE_EQUAL(elem["owner"].as_string(), *it++);
      }
   } FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
