#include <boost/test/unit_test.hpp>

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain_plugin/abi_serializer_cache.hpp>
#include <sysio/chain_plugin/captured_abis.hpp>
#include <sysio/testing/tester.hpp>

#include <test_contracts.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using sysio::chain_apis::captured_abi_serializers;
using sysio::chain_apis::captured_abis;

namespace {

/// Time generous enough that no build in these tests runs out of it.
constexpr fc::microseconds ample_time = fc::seconds(30);

/// The asserter ABI with its `condition` field renamed to `field`, so its bytes are ones no other test builds.
std::string asserter_abi_with_field(std::string_view field) {
   std::string abi = test_contracts::asserter_abi();
   const size_t at = abi.find("condition");
   BOOST_REQUIRE(at != std::string::npos);
   abi.replace(at, std::string_view("condition").size(), field);
   return abi;
}

/// True when `captured` holds exactly the packed ABI `account` has on `chain` now.
bool holds_current_abi(const captured_abis::packed_abi_ptr& captured, const controller& chain,
                       const account_name& account) {
   const account_metadata_object* metadata = chain.find_account_metadata(account);
   return captured && metadata != nullptr &&
          std::ranges::equal(*captured, std::span<const char>(metadata->abi.data(), metadata->abi.size()));
}

/// An action of `account` with no data and no authorization; enough to name the account.
action naming(const account_name& account) {
   return action(std::vector<permission_level>{}, account, "noop"_n, bytes{});
}

/// Pushes asserter's procassert and returns its trace.
transaction_trace_ptr push_procassert(validating_tester& chain) {
   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                            "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("asserter"_n, "active"), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}

/// Serializer builds the shared cache has started so far.
uint64_t shared_builds() {
   return chain_apis::shared_abi_serializer_cache().builds();
}

} // namespace

BOOST_AUTO_TEST_SUITE(captured_abis_tests)

/// A block's capture holds a copy of the ABI of each account its actions name, context-free actions included, and
/// none for an account without an ABI or one that does not exist.
BOOST_FIXTURE_TEST_CASE(capture_of_block_copies_named_accounts, validating_tester) try {
   create_accounts({"asserter"_n, "cfaacct"_n, "noabi"_n});
   set_abi("asserter"_n, test_contracts::asserter_abi());
   set_abi("cfaacct"_n, asserter_abi_with_field("cfa_probe"));
   produce_block();

   signed_transaction trx;
   trx.actions              = {naming("asserter"_n), naming("noabi"_n), naming("nosuchacct"_n), naming("asserter"_n)};
   trx.context_free_actions = {naming("cfaacct"_n)};
   signed_block block;
   block.transactions.emplace_back(packed_transaction(trx));

   const captured_abis abis = captured_abis::capture(*control, block);
   BOOST_CHECK_EQUAL(abis.size(), 4u);
   BOOST_CHECK(holds_current_abi(abis.find("asserter"_n), *control, "asserter"_n));
   BOOST_CHECK(holds_current_abi(abis.find("cfaacct"_n), *control, "cfaacct"_n));
   BOOST_CHECK(!abis.find("noabi"_n));
   BOOST_CHECK(!abis.find("nosuchacct"_n));
   BOOST_CHECK(!abis.find("sysio"_n)); // not named
} FC_LOG_AND_RETHROW()

/// A trace's capture holds a copy of the ABI of each account its action traces name; the copy does not follow a later
/// change to the ABI.
BOOST_FIXTURE_TEST_CASE(capture_of_trace_copies_named_accounts, validating_tester) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();

   const transaction_trace_ptr trace = push_procassert(*this);
   const captured_abis         abis  = captured_abis::capture(*control, *trace);
   BOOST_CHECK_EQUAL(abis.size(), 1u);
   BOOST_REQUIRE(holds_current_abi(abis.find("asserter"_n), *control, "asserter"_n));

   set_abi("asserter"_n, asserter_abi_with_field("later_probe"));
   produce_block();
   BOOST_CHECK(!holds_current_abi(abis.find("asserter"_n), *control, "asserter"_n));
} FC_LOG_AND_RETHROW()

/// Captures of an account whose ABI has not changed share one copy while any of them holds it, and nothing else keeps
/// a copy alive. A changed ABI is copied again; earlier captures keep the copy they took.
BOOST_FIXTURE_TEST_CASE(captures_share_unchanged_copies, validating_tester) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();
   const transaction_trace_ptr trace = push_procassert(*this);

   {
      const captured_abis sole = captured_abis::capture(*control, *trace);
      BOOST_REQUIRE(sole.find("asserter"_n));
      BOOST_CHECK_EQUAL(sole.find("asserter"_n).use_count(), 1);
   }

   const captured_abis first  = captured_abis::capture(*control, *trace);
   const captured_abis second = captured_abis::capture(*control, *trace);
   BOOST_REQUIRE(first.find("asserter"_n));
   BOOST_CHECK(second.find("asserter"_n) == first.find("asserter"_n));
   signed_transaction trx;
   trx.actions = {naming("asserter"_n)};
   signed_block block;
   block.transactions.emplace_back(packed_transaction(trx));
   BOOST_CHECK(captured_abis::capture(*control, block).find("asserter"_n) == first.find("asserter"_n));

   set_abi("asserter"_n, asserter_abi_with_field("share_probe"));
   produce_block();
   const captured_abis changed = captured_abis::capture(*control, *trace);
   BOOST_REQUIRE(changed.find("asserter"_n));
   BOOST_CHECK(changed.find("asserter"_n) != first.find("asserter"_n));
   BOOST_CHECK(holds_current_abi(changed.find("asserter"_n), *control, "asserter"_n));
   BOOST_CHECK(!holds_current_abi(first.find("asserter"_n), *control, "asserter"_n));
   BOOST_CHECK(captured_abis::capture(*control, *trace).find("asserter"_n) == changed.find("asserter"_n));
} FC_LOG_AND_RETHROW()

/// The resolver of captured_abi_serializers gives each captured account the shared cache's serializer for its copied
/// ABI, looking a copy up once, and none to an account that was not captured or has no ABI.
BOOST_FIXTURE_TEST_CASE(serializers_resolve_captured_accounts, validating_tester) try {
   create_accounts({"asserter"_n, "noabi"_n});
   set_abi("asserter"_n, asserter_abi_with_field("resolve_probe"));
   produce_block();

   signed_transaction trx;
   trx.actions = {naming("asserter"_n), naming("noabi"_n)};
   signed_block block;
   block.transactions.emplace_back(packed_transaction(trx));
   const captured_abis abis = captured_abis::capture(*control, block);

   const uint64_t           builds = shared_builds();
   captured_abi_serializers serializers(ample_time, fc::time_point::now() + ample_time);
   const auto               resolver = serializers.resolver_for(abis);
   const auto               asserter = resolver("asserter"_n);
   BOOST_REQUIRE(asserter);
   BOOST_CHECK(asserter->get().is_struct("procassert"));
   BOOST_CHECK(&asserter->get() == chain_apis::shared_abi_serializer_cache().get(*abis.find("asserter"_n),
                                                                                 ample_time).get());
   BOOST_CHECK(&resolver("asserter"_n)->get() == &asserter->get());
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);

   BOOST_CHECK(!resolver("noabi"_n));
   BOOST_CHECK(!resolver("sysio"_n));
   BOOST_CHECK(serializers.find(nullptr) == nullptr);
} FC_LOG_AND_RETHROW()

/// An ABI whose serializer fails to build, or cannot be built before the deadline, resolves to none.
BOOST_FIXTURE_TEST_CASE(serializers_resolve_failed_builds_to_none, validating_tester) try {
   create_accounts({"asserter"_n, "invalid"_n});
   set_abi("asserter"_n, asserter_abi_with_field("deadline_probe"));
   std::string invalid_abi = test_contracts::asserter_abi();
   const size_t at = invalid_abi.find("int8");
   BOOST_REQUIRE(at != std::string::npos);
   invalid_abi.replace(at, std::string_view("int8").size(), "xxxx");
   set_abi("invalid"_n, invalid_abi);
   produce_block();

   signed_transaction trx;
   trx.actions = {naming("asserter"_n), naming("invalid"_n)};
   signed_block block;
   block.transactions.emplace_back(packed_transaction(trx));
   const captured_abis abis = captured_abis::capture(*control, block);
   BOOST_REQUIRE(abis.find("asserter"_n));
   BOOST_REQUIRE(abis.find("invalid"_n));

   captured_abi_serializers in_time(ample_time, fc::time_point::now() + ample_time);
   BOOST_CHECK(!in_time.resolver_for(abis)("invalid"_n));

   const uint64_t           builds = shared_builds();
   captured_abi_serializers late(ample_time, fc::time_point::now() - fc::seconds(1));
   BOOST_CHECK(!late.resolver_for(abis)("asserter"_n));
   BOOST_CHECK(!late.resolver_for(abis)("asserter"_n));
   BOOST_CHECK_EQUAL(shared_builds(), builds);

   captured_abi_serializers later(ample_time, fc::time_point::now() + ample_time);
   BOOST_CHECK(later.resolver_for(abis)("asserter"_n));
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
