/**
 * @file test_solana_cluster_identity_metrics.cpp
 * @brief Tests bounded, credential-free Solana cluster identity telemetry.
 */

#include "../src/metrics.hpp"

#include <boost/test/unit_test.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <string>

using namespace fc::network::solana;
using sysio::metrics::catalog_type;

BOOST_AUTO_TEST_SUITE(prometheus_solana_cluster_identity_metrics)

BOOST_AUTO_TEST_CASE(exports_state_counters_and_bounded_operation_labels) {
   catalog_type catalog;
   solana_cluster_identity_snapshot snapshot{
      .client_id = "client-a",
      .sanitized_endpoint = "https://rpc.example:443",
      .mode = solana_cluster_identity_mode::pinned,
      .status = solana_cluster_identity_status::mismatch,
      .reason = solana_cluster_identity_reason::identity_mismatch,
      .verification_age = fc::seconds(2),
      .verification_attempts = 4,
      .verification_successes = 3,
      .verification_mismatches = 1,
      .blocked_operations =
         {
                              {solana_cluster_identity_operation::signing, 2},
                              },
   };

   catalog.update({snapshot});
   const auto output = catalog.report();

   BOOST_CHECK(output.find("nodeop_solana_cluster_identity{client_id=\"client-a\",mode=\"pinned\","
                           "reason=\"identity_mismatch\",status=\"mismatch\"} 1") != std::string::npos);
   BOOST_CHECK(output.find("nodeop_solana_cluster_identity_verification_age_seconds{client_id=\"client-a\"} 2") !=
               std::string::npos);
   BOOST_CHECK(output.find("nodeop_solana_cluster_identity_verification_attempts_total{client_id=\"client-a\"} 4") !=
               std::string::npos);
   BOOST_CHECK(output.find("operation=\"signing\"} 2") != std::string::npos);
   BOOST_CHECK(output.find("rpc.example") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(replaces_state_series_and_does_not_double_count_snapshots) {
   catalog_type catalog;
   solana_cluster_identity_snapshot snapshot{
      .client_id = "client-a",
      .mode = solana_cluster_identity_mode::unpinned,
      .status = solana_cluster_identity_status::unpinned,
      .reason = solana_cluster_identity_reason::missing_expected_identity,
      .verification_attempts = 2,
   };

   catalog.update({snapshot});
   catalog.update({snapshot});
   auto output = catalog.report();
   BOOST_CHECK(output.find("nodeop_solana_cluster_identity_verification_attempts_total{client_id=\"client-a\"} 2") !=
               std::string::npos);

   snapshot.mode = solana_cluster_identity_mode::pinned;
   snapshot.status = solana_cluster_identity_status::verified;
   snapshot.reason = solana_cluster_identity_reason::none;
   snapshot.verification_attempts = 3;
   catalog.update({snapshot});
   output = catalog.report();

   BOOST_CHECK(output.find("mode=\"unpinned\"") == std::string::npos);
   BOOST_CHECK(output.find("status=\"verified\"") != std::string::npos);
   BOOST_CHECK(output.find("nodeop_solana_cluster_identity_verification_attempts_total{client_id=\"client-a\"} 3") !=
               std::string::npos);

   catalog.update(std::vector<solana_cluster_identity_snapshot>{});
   output = catalog.report();
   BOOST_CHECK(output.find("client_id=\"client-a\"") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
