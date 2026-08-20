/// Pure-logic unit tests for the shared `sysio.chains::chains` row view
/// consumed by `batch_operator_plugin` and `underwriter_plugin`.
///
/// The chain read itself happens through `chain_plugin::read_table_rows`
/// (integration territory; covered by the flow tests in `wire-tools-ts`).
/// What is pinned here is the part a refactor can silently break: the field
/// spellings the two daemons decode rows with, and the single-program rule
/// that lets one code path serve both EVM and SVM deployment shapes.

#include <boost/test/unit_test.hpp>

#include <sysio/depot/chains_registry.hpp>

#include <string>

namespace c = sysio::depot::chains;

BOOST_AUTO_TEST_SUITE(chains_registry_tests)

/// EVM: each role names its own contract, so the role field wins.
BOOST_AUTO_TEST_CASE(role_specific_address_is_used_when_present) {
   constexpr auto opp    = "0x5FbDB2315678afecb367f032d93F642f64180aa3";
   constexpr auto opreg  = "0x9fE46736679d2D9a65F0992F2272dE9f3c7fa6e0";
   BOOST_REQUIRE_EQUAL(std::string{opreg}, c::resolve_role_addr(opreg, opp));
}

/// SVM: one program serves every role, so `sysio.chains` requires the role
/// fields to be empty and a reader falls back to `opp_addr`.
BOOST_AUTO_TEST_CASE(empty_role_address_falls_back_to_opp_addr) {
   constexpr auto program = "So11111111111111111111111111111111111111112";
   BOOST_REQUIRE_EQUAL(std::string{program}, c::resolve_role_addr("", program));
}

/// A row registered before its remote contracts were deployed carries neither,
/// and must resolve to empty so the caller can fail closed rather than sign to
/// the zero address.
BOOST_AUTO_TEST_CASE(unconfigured_row_resolves_to_empty) {
   BOOST_REQUIRE_EQUAL(std::string{}, c::resolve_role_addr("", ""));
}

/// Spelling regression guard — these must match `chain_row` and its nested
/// `outpost_addrs` struct in `contracts/sysio.chains`. A contract-side rename
/// that misses this header would otherwise surface as both daemons quietly
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
   BOOST_REQUIRE_EQUAL(std::string{"operator_registry_addr"},
                       std::string{c::field::outpost_addr::operator_registry_addr});
   BOOST_REQUIRE_EQUAL(std::string{"source_deposit_addr"},
                       std::string{c::field::outpost_addr::source_deposit_addr});
}

BOOST_AUTO_TEST_SUITE_END()
