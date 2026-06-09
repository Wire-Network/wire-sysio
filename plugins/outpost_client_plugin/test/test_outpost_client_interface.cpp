#include <format>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <sysio/outpost_client/outpost_client.hpp>

using sysio::opp::types::CHAIN_KIND_EVM;
using sysio::opp::types::CHAIN_KIND_SVM;

namespace {

/// Minimal subclass exercising the SPI purely for format/getter semantics.
/// No recording — higher-fidelity mocks live in batch_operator_plugin/test,
/// where they're consumed by the outpost_opp_job tests.
class minimal_outpost_client : public sysio::outpost_client {
public:
   minimal_outpost_client(sysio::opp::types::ChainKind kind, uint64_t id, uint32_t cid)
      : _kind(kind), _outpost_id(id), _chain_id(cid) {}

   sysio::opp::types::ChainKind chain_kind() const override { return _kind; }
   uint64_t                     chain_code() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   std::string                  to_string()  const override {
      return std::format("{}:{}:{}",
                         _outpost_id,
                         sysio::opp::types::ChainKind_Name(_kind),
                         _chain_id);
   }
   std::string deliver_outbound_envelope(uint32_t,
                                         const std::vector<char>&,
                                         fc::microseconds) override { return {}; }
   std::vector<char> read_inbound_envelope(uint32_t, fc::microseconds) override { return {}; }
   std::string uw_commit(uint64_t,
                         const std::vector<char>&,
                         fc::microseconds) override { return {}; }

private:
   sysio::opp::types::ChainKind _kind;
   uint64_t                     _outpost_id;
   uint32_t                     _chain_id;
};

} // namespace

BOOST_AUTO_TEST_SUITE(outpost_client_interface_tests)

BOOST_AUTO_TEST_CASE(eth_anvil_to_string) {
   minimal_outpost_client c{CHAIN_KIND_EVM, /*chain_code=*/0, /*chain_id=*/31337};
   BOOST_CHECK_EQUAL(c.chain_kind(), CHAIN_KIND_EVM);
   BOOST_CHECK_EQUAL(c.chain_code(), 0u);
   BOOST_CHECK_EQUAL(c.chain_id(), 31337u);
   BOOST_CHECK_EQUAL(c.to_string(), "0:CHAIN_KIND_EVM:31337");
}

BOOST_AUTO_TEST_CASE(eth_mainnet_to_string) {
   minimal_outpost_client c{CHAIN_KIND_EVM, /*chain_code=*/3, /*chain_id=*/1};
   BOOST_CHECK_EQUAL(c.to_string(), "3:CHAIN_KIND_EVM:1");
}

BOOST_AUTO_TEST_CASE(sol_to_string_no_numeric_chain_id) {
   minimal_outpost_client c{CHAIN_KIND_SVM, /*chain_code=*/1, /*chain_id=*/0};
   BOOST_CHECK_EQUAL(c.chain_kind(), CHAIN_KIND_SVM);
   BOOST_CHECK_EQUAL(c.to_string(), "1:CHAIN_KIND_SVM:0");
}

BOOST_AUTO_TEST_SUITE_END()
