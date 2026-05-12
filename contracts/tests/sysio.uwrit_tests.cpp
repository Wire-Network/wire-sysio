#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>

#include <fc/variant_object.hpp>
#include <fc-lite/crypto/chain_types.hpp>

#include "contracts.hpp"

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;
using namespace fc::crypto;

using mvo = fc::mutable_variant_object;

class sysio_uwrit_tester : public tester {
public:
   static constexpr auto UWRIT_ACCOUNT = "sysio.uwrit"_n;
   static constexpr auto MSGCH_ACCOUNT = "sysio.msgch"_n;
   static constexpr auto OPREG_ACCOUNT = "sysio.opreg"_n;
   static constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

   sysio_uwrit_tester() {
      produce_blocks(2);

      create_accounts({
         UWRIT_ACCOUNT, MSGCH_ACCOUNT, OPREG_ACCOUNT, CHALG_ACCOUNT,
         "sysio.epoch"_n, "uwrit.a"_n, "uwrit.b"_n
      });
      produce_blocks(2);

      set_code(UWRIT_ACCOUNT, contracts::uwrit_wasm());
      set_abi(UWRIT_ACCOUNT, contracts::uwrit_abi().data());
      set_privileged(UWRIT_ACCOUNT);

      produce_blocks();

      const auto* accnt = control->find_account_metadata(UWRIT_ACCOUNT);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   action_result push_uwrit_action(name signer, name action_name, const variant_object& data) {
      string action_type_name = abi_ser.get_action_type(action_name);

      action act;
      act.account = UWRIT_ACCOUNT;
      act.name = action_name;
      act.data = abi_ser.variant_to_binary(
         action_type_name, data,
         abi_serializer::create_yield_function(abi_serializer_max_time)
      );
      act.authorization = vector<permission_level>{{signer, config::active_name}};

      signed_transaction trx;
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      trx.sign(get_private_key(signer, "active"), control->get_chain_id());
      try {
         push_transaction(trx);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result setconfig(uint32_t fee_bps = 10) {
      return push_uwrit_action(UWRIT_ACCOUNT, "setconfig"_n, mvo()
         ("fee_bps", fee_bps)
      );
   }

   /// Read uwconfig singleton row.
   fc::variant get_uwconfig() {
      auto data = get_row_by_account(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwconfig"_n, "uwconfig"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "uw_config", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Read a uwreq by id.
   fc::variant get_uwreq(uint64_t id) {
      auto data = get_row_by_id(UWRIT_ACCOUNT, UWRIT_ACCOUNT, "uwreqs"_n, id);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "uw_request_t", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   abi_serializer abi_ser;
};

// ---- Tests ----

BOOST_AUTO_TEST_SUITE(sysio_uwrit_tests)

BOOST_FIXTURE_TEST_CASE(setconfig_basic, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setconfig(25));

   auto cfg = get_uwconfig();
   BOOST_REQUIRE_EQUAL(25, cfg["fee_bps"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setconfig_rejects_excessive_fee, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: fee_bps cannot exceed 10000 (100%)"),
      setconfig(10001)
   );
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(createuwreq_requires_msgch_auth, sysio_uwrit_tester) { try {
   // createuwreq must be invoked by sysio.msgch (inline action). A direct
   // call from another account (uwrit.a here) is rejected.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "createuwreq"_n, mvo()
      ("attestation_id", 1)
      ("type", sysio::opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REQUEST)
      ("outpost_id", 1)
      ("data", std::vector<char>{})
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(release_requires_msgch_or_self_auth, sysio_uwrit_tester) { try {
   // release accepts sysio.msgch (SWAP_REMIT dispatch path) or sysio.uwrit
   // (expirelock self-inline path) auth. Anything else is rejected.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "release"_n, mvo()
      ("uwreq_id", 1)
   ).find("release requires sysio.msgch or sysio.uwrit authority") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(expirelock_missing_uwreq, sysio_uwrit_tester) { try {
   // Permissionless caller — but the uwreq doesn't exist, so we expect a
   // not-found assertion rather than an auth failure.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq not found"),
      push_uwrit_action("uwrit.a"_n, "expirelock"_n, mvo()
         ("uwreq_id", 999)
      )
   );
} FC_LOG_AND_RETHROW() }

// ── rcrdcommit (Task 3: per-leg COMMIT arrival recorder) ──

BOOST_FIXTURE_TEST_CASE(rcrdcommit_requires_msgch_auth, sysio_uwrit_tester) { try {
   // rcrdcommit is invoked inline from sysio.msgch on UNDERWRITE_INTENT_COMMIT
   // dispatch. A direct call from another account is rejected.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "rcrdcommit"_n, mvo()
      ("uwreq_id",   1)
      ("underwriter", "uwrit.a")
      ("outpost_id",  1)
      ("from_chain",  ChainKind::CHAIN_KIND_ETHEREUM)
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(rcrdcommit_rejects_unknown_uwreq, sysio_uwrit_tester) { try {
   // msgch-signed but the uwreq doesn't exist — should report not-found.
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq not found"),
      push_uwrit_action(MSGCH_ACCOUNT, "rcrdcommit"_n, mvo()
         ("uwreq_id",   42)
         ("underwriter", "uwrit.a")
         ("outpost_id",  1)
         ("from_chain",  ChainKind::CHAIN_KIND_ETHEREUM)
      )
   );
} FC_LOG_AND_RETHROW() }

// ── rcrdreject (Task 3: explicit underwriter intent rejection) ──

BOOST_FIXTURE_TEST_CASE(rcrdreject_requires_msgch_auth, sysio_uwrit_tester) { try {
   // Like rcrdcommit, rcrdreject is dispatched inline from sysio.msgch only.
   BOOST_REQUIRE(push_uwrit_action("uwrit.a"_n, "rcrdreject"_n, mvo()
      ("uwreq_id",    1)
      ("underwriter", "uwrit.a")
      ("reason",      "rejected by underwriter")
   ).find("missing authority of sysio.msgch") != std::string::npos);
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(rcrdreject_rejects_unknown_uwreq, sysio_uwrit_tester) { try {
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq not found"),
      push_uwrit_action(MSGCH_ACCOUNT, "rcrdreject"_n, mvo()
         ("uwreq_id",    77)
         ("underwriter", "uwrit.a")
         ("reason",      "n/a")
      )
   );
} FC_LOG_AND_RETHROW() }

// ── release (Task 3: settle UWREQ + opreg::releaselock fan-out) ──

BOOST_FIXTURE_TEST_CASE(release_rejects_unknown_uwreq, sysio_uwrit_tester) { try {
   // msgch is one of the two valid auth holders for release; verifies the
   // not-found check fires after auth passes (not blocked by auth).
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: uwreq not found"),
      push_uwrit_action(MSGCH_ACCOUNT, "release"_n, mvo()
         ("uwreq_id", 9999)
      )
   );
} FC_LOG_AND_RETHROW() }

// ── sumlocks (Task 3: read-only per-(underwriter, chain, token) lock total) ──

BOOST_FIXTURE_TEST_CASE(sumlocks_zero_for_unbonded_underwriter, sysio_uwrit_tester) { try {
   // Read-only action with no preconditions: an underwriter that has never
   // entered a race holds zero locks on every (chain, token_kind). The action
   // returns 0; with no exception the call is considered successful.
   BOOST_REQUIRE_EQUAL(success(),
      push_uwrit_action("uwrit.a"_n, "sumlocks"_n, mvo()
         ("underwriter", "uwrit.a")
         ("chain",       ChainKind::CHAIN_KIND_ETHEREUM)
         ("token_kind",  TokenKind::TOKEN_KIND_ETH)
      )
   );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
