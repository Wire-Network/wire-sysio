#include <sysio/chain/block_header.hpp>
#include <boost/test/unit_test.hpp>

using namespace sysio::chain;

BOOST_AUTO_TEST_SUITE(block_header_tests)

// test for block header default values
BOOST_AUTO_TEST_CASE(block_header_default_values_test)
{
   block_header header;
   BOOST_REQUIRE_EQUAL( header.qc_claim.block_num, 0u );
   BOOST_REQUIRE_EQUAL( header.qc_claim.is_strong_qc, false );
   BOOST_REQUIRE( !header.new_finalizer_policy_diff );
   BOOST_REQUIRE( !header.new_proposer_policy_diff );
   BOOST_REQUIRE( header.finality_mroot == digest_type{} );
}

// test for block header with qc_claim values
BOOST_AUTO_TEST_CASE(block_header_qc_claim_test)
{
   block_header       header;
   constexpr uint32_t last_qc_block_num {10};
   constexpr bool     is_strong_qc {true};

   header.qc_claim = qc_claim_t{last_qc_block_num, is_strong_qc};

   BOOST_REQUIRE_EQUAL( header.qc_claim.block_num, last_qc_block_num );
   BOOST_REQUIRE_EQUAL( header.qc_claim.is_strong_qc, is_strong_qc );
}

// test for block header with finality fields
BOOST_AUTO_TEST_CASE(block_header_finality_fields_test)
{
   block_header       header;
   constexpr uint32_t last_qc_block_num {10};
   constexpr bool     is_strong_qc {true};

   std::vector<finalizer_authority> finalizers { {"test description", 50, fc::crypto::bls::public_key{"PUB_BLS_qVbh4IjYZpRGo8U_0spBUM-u-r_G0fMo4MzLZRsKWmm5uyeQTp74YFaMN9IDWPoVVT5rj_Tw1gvps6K9_OZ6sabkJJzug3uGfjA6qiaLbLh5Fnafwv-nVgzzzBlU2kwRrcHc8Q" }} };
   auto fin_policy = std::make_shared<finalizer_policy>();
   finalizer_policy_diff new_fin_policy_diff = fin_policy->create_diff(finalizer_policy{.generation = 1, .threshold = 100, .finalizers = finalizers});

   constexpr uint32_t block_timestamp = 200;
   proposer_policy_diff new_prop_policy_diff = proposer_policy_diff{.version = 1, .proposal_time = block_timestamp_type{block_timestamp}, .producer_auth_diff = {}};

   header.qc_claim = qc_claim_t{last_qc_block_num, is_strong_qc};
   header.new_finalizer_policy_diff = new_fin_policy_diff;
   header.new_proposer_policy_diff = new_prop_policy_diff;

   BOOST_REQUIRE_EQUAL( header.qc_claim.block_num, last_qc_block_num );
   BOOST_REQUIRE_EQUAL( header.qc_claim.is_strong_qc, is_strong_qc );

   BOOST_REQUIRE( !!header.new_finalizer_policy_diff );
   BOOST_REQUIRE_EQUAL(header.new_finalizer_policy_diff->generation, 1u);
   BOOST_REQUIRE_EQUAL(header.new_finalizer_policy_diff->threshold, 100u);
   BOOST_REQUIRE_EQUAL(header.new_finalizer_policy_diff->finalizers_diff.insert_indexes[0].second.description, "test description");
   BOOST_REQUIRE_EQUAL(header.new_finalizer_policy_diff->finalizers_diff.insert_indexes[0].second.weight, 50u);
   BOOST_REQUIRE_EQUAL(header.new_finalizer_policy_diff->finalizers_diff.insert_indexes[0].second.public_key.to_string(), "PUB_BLS_qVbh4IjYZpRGo8U_0spBUM-u-r_G0fMo4MzLZRsKWmm5uyeQTp74YFaMN9IDWPoVVT5rj_Tw1gvps6K9_OZ6sabkJJzug3uGfjA6qiaLbLh5Fnafwv-nVgzzzBlU2kwRrcHc8Q");

   BOOST_REQUIRE( !!header.new_proposer_policy_diff );
   fc::time_point t = (fc::time_point)(header.new_proposer_policy_diff->proposal_time);
   BOOST_REQUIRE_EQUAL(t.time_since_epoch().to_seconds(), config::block_timestamp_epoch/1000+(block_timestamp/(1000/config::block_interval_ms)));
}

BOOST_AUTO_TEST_SUITE_END()
