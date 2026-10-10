/// Pure-logic unit tests for the shared `sysio.chains::chains` row view
/// consumed by `batch_operator_plugin`.
///
/// The chain read itself happens through `chain_plugin::read_table_rows`
/// (integration territory; covered by the flow tests in `wire-tools-ts`).
/// What is pinned here is the part a refactor can silently break: the field
/// spellings the relay decodes rows with.

#include <boost/test/unit_test.hpp>

#include <sysio/opp/depot/chains_registry.hpp>

#include <string>

namespace c = sysio::opp::depot::chains;

BOOST_AUTO_TEST_SUITE(chains_registry_tests)

/// Spelling regression guard — these must match `chain_row` and its nested
/// `outpost_addrs` struct in `contracts/sysio.chains`. A contract-side rename
/// that misses this header would otherwise surface as the relay quietly
/// reading empty addresses and skipping every chain.
BOOST_AUTO_TEST_CASE(field_spellings_match_the_contract_row) {
   BOOST_REQUIRE_EQUAL(std::string{"sysio.chains"},      std::string{c::account});
   BOOST_REQUIRE_EQUAL(std::string{"chains"},            std::string{c::table_chains});
   BOOST_REQUIRE_EQUAL(std::string{"code"},              std::string{c::field::code});
   BOOST_REQUIRE_EQUAL(std::string{"kind"},              std::string{c::field::kind});
   BOOST_REQUIRE_EQUAL(std::string{"external_chain_id"}, std::string{c::field::external_chain_id});
   BOOST_REQUIRE_EQUAL(std::string{"is_depot"},          std::string{c::field::is_depot});
   BOOST_REQUIRE_EQUAL(std::string{"active"},            std::string{c::field::active});
   BOOST_REQUIRE_EQUAL(std::string{"outpost"},           std::string{c::field::outpost});

   BOOST_REQUIRE_EQUAL(std::string{"opp_addr"},
                       std::string{c::field::outpost_addr::opp_addr});
   BOOST_REQUIRE_EQUAL(std::string{"opp_inbound_addr"},
                       std::string{c::field::outpost_addr::opp_inbound_addr});
}

BOOST_AUTO_TEST_SUITE_END()
