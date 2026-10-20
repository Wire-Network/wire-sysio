#include <boost/test/unit_test.hpp>
#include "contracts.hpp"
#include "contract_test_support.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;
namespace support = sysio_system::test_support;

/// Exercise native updateauth/linkauth with distinct signer keys, rather than bypassing authorization.
class sysio_andon_tester : public tester {
public:
   static constexpr auto ANDON = "sysio.andon"_n;
   static constexpr auto PANIC = "panic"_n;
   static constexpr auto PULLER = "puller"_n;
   static constexpr auto ALICE = "alice"_n;
   abi_serializer ser;

   sysio_andon_tester() {
      create_accounts({ANDON, PANIC, PULLER, ALICE});
      support::andon::deploy(*this, ser, contracts::andon_wasm(), contracts::andon_abi());
   }

   /// Replace one permission's delegates; updateauth is signed by the account owner.
   void grant(name permission, std::vector<name> delegates) {
      std::sort(delegates.begin(), delegates.end());
      authority auth{1, {}, {}};
      for (auto delegate : delegates) auth.accounts.push_back({{delegate, config::active_name}, 1});
      set_authority(ANDON, permission, auth, config::active_name);
      produce_block();
   }

   /// Link each action independently using the account owner's native authority.
   void configure() {
      grant("pull"_n, {PANIC, PULLER, config::system_account_name});
      grant("clear"_n, {PANIC, config::system_account_name});
      signed_transaction trx;
      for (auto permission : {"pull"_n, "clear"_n})
         trx.actions.emplace_back(std::vector<permission_level>{{ANDON, config::owner_name}},
                                   linkauth{ANDON, ANDON, permission, permission});
      set_transaction_headers(trx);
      trx.sign(get_private_key(ANDON, "owner"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }

   /// Declare an Andon permission while signing with the delegated account's key.
   action_result call(name action_name, name signer, name permission, const std::string& text = "test") {
      return support::push_contract_action_and_produce_block(*this, ANDON, ser, signer, action_name,
         mvo()(action_name == "pull"_n ? "reason" : "note", text), {{ANDON, permission}});
   }

   /// Raw singleton bytes make idempotence checks exact.
   std::vector<char> bytes() { return get_row_by_account(ANDON, ANDON, "cord"_n, "cord"_n); }
   /// Decode the public three-field emergency-stop state.
   fc::variant cord() {
      return ser.binary_to_variant("cord_state", bytes(), abi_serializer::create_yield_function(abi_serializer_max_time));
   }
};

BOOST_AUTO_TEST_SUITE(sysio_andon_tests)

/// Before custom links exist, only the account's governance delegate can call the actions.
BOOST_FIXTURE_TEST_CASE(default_authority_is_governance_only, sysio_andon_tester) {
   BOOST_REQUIRE_NE(success(), call("pull"_n, PANIC, config::active_name));
   BOOST_REQUIRE_NE(success(), call("pull"_n, ALICE, config::active_name));
   BOOST_REQUIRE(bytes().empty());
   BOOST_REQUIRE_EQUAL(success(), call("pull"_n, config::system_account_name, config::active_name));
   BOOST_REQUIRE(cord()["pulled"].as_bool());
   BOOST_REQUIRE_NE(success(), call("clear"_n, PANIC, config::active_name));
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, config::system_account_name, config::active_name));
   BOOST_REQUIRE(!cord()["pulled"].as_bool());
}

/// Native authority enforces separate pull/clear delegates and rejects actor-only declarations.
BOOST_FIXTURE_TEST_CASE(linked_permissions_separate_pull_from_clear, sysio_andon_tester) {
   configure();
   BOOST_REQUIRE_NE(success(), call("pull"_n, ALICE, "pull"_n));
   BOOST_REQUIRE_EQUAL(success(), call("pull"_n, PULLER, "pull"_n));
   BOOST_REQUIRE_NE(success(), call("clear"_n, PULLER, "clear"_n));
   BOOST_REQUIRE_NE(success(), call("clear"_n, PULLER, "pull"_n));
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, PANIC, "clear"_n));
   BOOST_REQUIRE_EQUAL(success(), call("pull"_n, PANIC, "pull"_n));
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, config::system_account_name, "clear"_n));
   BOOST_REQUIRE_NE(success(), support::push_contract_action_and_produce_block(*this, ANDON, ser,
      PANIC, "pull"_n, mvo()("reason", "wrong declared account")));
}

/// Updating native authorities rotates panic keys/accounts and revokes pullers without a contract list.
BOOST_FIXTURE_TEST_CASE(updateauth_rotates_and_revokes_delegates, sysio_andon_tester) {
   configure();
   grant("pull"_n, {ALICE, config::system_account_name});
   grant("clear"_n, {ALICE, config::system_account_name});
   BOOST_REQUIRE_NE(success(), call("pull"_n, PANIC, "pull"_n));
   BOOST_REQUIRE_NE(success(), call("pull"_n, PULLER, "pull"_n));
   BOOST_REQUIRE_EQUAL(success(), call("pull"_n, ALICE, "pull"_n));
   BOOST_REQUIRE_NE(success(), call("clear"_n, PANIC, "clear"_n));
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, ALICE, "clear"_n));
}

/// Repeated requests do not change timestamps or notes, and recorded text is bounded.
BOOST_FIXTURE_TEST_CASE(transitions_are_idempotent_and_text_is_bounded, sysio_andon_tester) {
   configure();
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, PANIC, "clear"_n));
   BOOST_REQUIRE(bytes().empty());
   BOOST_REQUIRE_EQUAL(success(), call("pull"_n, PANIC, "pull"_n, std::string(300, 'p')));
   BOOST_REQUIRE_EQUAL(std::string(256, 'p'), cord()["reason"].as_string());
   BOOST_REQUIRE_EQUAL(3u, cord().get_object().size());
   const auto pulled = bytes();
   BOOST_REQUIRE_EQUAL(success(), call("pull"_n, PULLER, "pull"_n, "ignored"));
   BOOST_REQUIRE(pulled == bytes());
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, PANIC, "clear"_n, std::string(300, 'c')));
   BOOST_REQUIRE(!cord()["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(std::string(256, 'c'), cord()["reason"].as_string());
   const auto cleared = bytes();
   BOOST_REQUIRE_EQUAL(success(), call("clear"_n, PANIC, "clear"_n, "ignored"));
   BOOST_REQUIRE(cleared == bytes());
}
BOOST_AUTO_TEST_SUITE_END()
