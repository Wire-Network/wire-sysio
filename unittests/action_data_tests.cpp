/**
 * Tests that a native action's data holds exactly the fields of its struct.
 *
 * `action::data_as` fails with action_data_and_struct_mismatch when bytes are left after the struct. The native
 * structs end in the optional fields the system contract adds (`memo` on setcode and setabi, `authorized_by` on
 * the permission actions), so those fields are accepted and anything after them is not. Also covers the host's
 * binary_extension, and which actions an account `sysio` without a contract accepts.
 */
#include <sysio/chain/binary_extension.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

namespace {

/// A tail longer than any native struct, well inside the transaction NET limit.
constexpr size_t large_tail_size = 100'000;

/// Bytes appended to an action's data.
bytes tail_of(size_t size, char value = 0) {
   return bytes(size, value);
}

/// `value` packed, as the bytes of an optional field at the end of an action's data.
template <typename T>
bytes packed(const T& value) {
   return fc::raw::pack(value);
}

/// `head` followed by `tail`.
bytes concat(bytes head, const bytes& tail) {
   head.insert(head.end(), tail.begin(), tail.end());
   return head;
}

/// Pushes `act` with `tail` appended to its data, signed by its first authorizer's active key.
template <typename Tester>
transaction_trace_ptr push_with_tail(Tester& chain, action act, const bytes& tail) {
   act.data.insert(act.data.end(), tail.begin(), tail.end());
   signed_transaction trx;
   trx.actions.push_back(std::move(act));
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(trx.actions[0].authorization.at(0).actor, "active"), chain.get_chain_id());
   return chain.push_transaction(trx);
}

struct action_data_tester : validating_tester {
   transaction_trace_ptr push_with_tail(action act, const bytes& tail) {
      return ::push_with_tail(*this, std::move(act), tail);
   }

   /// An action of native struct `T`, authorized by `actor`'s active permission.
   template <typename T>
   static action native_action(name actor, const T& value) {
      return action{vector<permission_level>{{actor, config::active_name}}, value};
   }

   authority active_authority(name account) { return authority(get_public_key(account, "active")); }

   bool has_permission(name account, name permission) {
      return find<permission_object, by_owner>(boost::make_tuple(account, permission)) != nullptr;
   }

   bool has_code(name account) {
      const auto* metadata = control->find_account_metadata(account);
      return metadata != nullptr && metadata->code_hash != digest_type();
   }

   bool has_abi(name account) {
      const auto* metadata = control->find_account_metadata(account);
      return metadata != nullptr && metadata->abi.size() != 0;
   }
};

/// Fails a push with a tail past the end of the struct.
#define CHECK_TRAILING_BYTES_REJECTED(PUSH) \
   BOOST_CHECK_EXCEPTION(PUSH, action_data_and_struct_mismatch, \
                         fc_exception_message_contains("bytes left after the arguments"))

} // namespace

BOOST_AUTO_TEST_SUITE(action_data_tests)

BOOST_AUTO_TEST_CASE(data_as_reads_exactly_the_struct) try {
   const deleteauth value{"alice"_n, "perm"_n};
   action act{vector<permission_level>{{"alice"_n, config::active_name}}, value};

   const auto exact = act.data_as<deleteauth>();
   BOOST_CHECK_EQUAL(exact.account, value.account);
   BOOST_CHECK_EQUAL(exact.permission, value.permission);
   BOOST_CHECK(!exact.authorized_by.has_value());

   act.data = concat(act.data, packed("bob"_n));
   const auto extended = act.data_as<deleteauth>();
   BOOST_REQUIRE(extended.authorized_by.has_value());
   BOOST_CHECK_EQUAL(extended.authorized_by.value->to_string(), "bob");

   act.data.push_back(0);
   BOOST_CHECK_EXCEPTION(act.data_as<deleteauth>(), action_data_and_struct_mismatch,
                         fc_exception_message_contains("1 bytes left after the arguments"));

   act.data = concat(fc::raw::pack(value), tail_of(large_tail_size));
   BOOST_CHECK_EXCEPTION(act.data_as<deleteauth>(), action_data_and_struct_mismatch,
                         fc_exception_message_contains("bytes left after the arguments"));

   // A struct with no optional field: any byte after it is left over.
   action create{vector<permission_level>{{config::system_account_name, config::active_name}},
                 newaccount{.creator = config::system_account_name, .name = "alice"_n,
                            .owner = authority{}, .active = authority{}}};
   create.data_as<newaccount>();
   create.data.push_back(0);
   BOOST_CHECK_EXCEPTION(create.data_as<newaccount>(), action_data_and_struct_mismatch,
                         fc_exception_message_contains("1 bytes left after the arguments"));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(binary_extension_round_trip) try {
   const binary_extension<account_name> absent;
   BOOST_CHECK(fc::raw::pack(absent).empty());
   BOOST_CHECK_EQUAL(fc::raw::pack_size(absent), 0u);

   const binary_extension<account_name> present{"alice"_n};
   BOOST_CHECK(fc::raw::pack(present) == fc::raw::pack("alice"_n));
   BOOST_CHECK_EQUAL(fc::raw::pack_size(present), sizeof(uint64_t));

   // Unpacking reads the field only when bytes remain.
   const bytes empty;
   fc::datastream<const char*> empty_ds(empty.data(), empty.size());
   binary_extension<account_name> unpacked{"bob"_n};
   fc::raw::unpack(empty_ds, unpacked);
   BOOST_CHECK(!unpacked.has_value());

   const bytes alice = fc::raw::pack("alice"_n);
   fc::datastream<const char*> alice_ds(alice.data(), alice.size());
   fc::raw::unpack(alice_ds, unpacked);
   BOOST_CHECK(unpacked == present);

   // A partial field does not unpack.
   const bytes partial(3, 1);
   fc::datastream<const char*> partial_ds(partial.data(), partial.size());
   BOOST_CHECK_THROW(fc::raw::unpack(partial_ds, unpacked), fc::exception);

   // An absent field is null in a variant, a present one is its value.
   fc::variant v;
   fc::to_variant(absent, v);
   BOOST_CHECK(v.is_null());
   fc::to_variant(present, v);
   BOOST_CHECK_EQUAL(v.as_string(), "alice");

   binary_extension<account_name> from;
   fc::from_variant(fc::variant("carol"), from);
   BOOST_REQUIRE(from.has_value());
   BOOST_CHECK_EQUAL(from.value->to_string(), "carol");
   fc::from_variant(fc::variant(), from);
   BOOST_CHECK(!from.has_value());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(newaccount_rejects_bytes_after_arguments, action_data_tester) try {
   const auto create = [&](name account) {
      return native_action(config::system_account_name,
                           newaccount{.creator = config::system_account_name, .name = account,
                                      .owner = active_authority(account), .active = active_authority(account)});
   };

   push_with_tail(create("alice"_n), {});
   produce_block();
   BOOST_CHECK(control->find_account("alice"_n) != nullptr);

   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(create("bob"_n), tail_of(1)));
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(create("bob"_n), tail_of(large_tail_size)));
   produce_block();
   BOOST_CHECK(control->find_account("bob"_n) == nullptr);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(setcode_accepts_memo_and_rejects_bytes_after_it, action_data_tester) try {
   create_accounts({"alice"_n, "bob"_n, "carol"_n});
   produce_block();

   const auto set = [&](name account, const std::vector<uint8_t>& wasm) {
      return native_action(account, setcode{.account = account, .vmtype = 0, .vmversion = 0,
                                            .code = bytes(wasm.begin(), wasm.end())});
   };

   push_with_tail(set("alice"_n, test_contracts::asserter_wasm()), {});
   push_with_tail(set("bob"_n, test_contracts::asserter_wasm()), packed(std::string("deploy")));
   produce_block();
   BOOST_CHECK(has_code("alice"_n));
   BOOST_CHECK(has_code("bob"_n));

   const auto with_memo = packed(std::string("deploy"));
   CHECK_TRAILING_BYTES_REJECTED(
      push_with_tail(set("carol"_n, test_contracts::asserter_wasm()), concat(with_memo, tail_of(1))));
   CHECK_TRAILING_BYTES_REJECTED(
      push_with_tail(set("carol"_n, test_contracts::asserter_wasm()), concat(with_memo, tail_of(large_tail_size))));
   // A byte that cannot be read as the memo.
   BOOST_CHECK_THROW(push_with_tail(set("carol"_n, test_contracts::asserter_wasm()), tail_of(1, 0x7f)), fc::exception);
   produce_block();
   BOOST_CHECK(!has_code("carol"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(setabi_rejects_bytes_after_arguments, action_data_tester) try {
   create_accounts({"alice"_n, "bob"_n});
   produce_block();

   const auto set = [&](name account) {
      const std::string abi_json = test_contracts::asserter_abi();
      const auto abi = fc::raw::pack(fc::json::from_string(abi_json).as<abi_def>());
      return native_action(account, setabi{.account = account, .abi = abi});
   };

   push_with_tail(set("alice"_n), {});
   produce_block();
   BOOST_CHECK(has_abi("alice"_n));

   const auto with_memo = packed(std::string("deploy"));
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(set("bob"_n), concat(with_memo, tail_of(1))));
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(set("bob"_n), concat(with_memo, tail_of(large_tail_size))));
   BOOST_CHECK_THROW(push_with_tail(set("bob"_n), tail_of(1, 0x7f)), fc::exception);
   produce_block();
   BOOST_CHECK(!has_abi("bob"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(permission_actions_accept_authorized_by_and_reject_bytes_after_it, action_data_tester) try {
   create_accounts({"alice"_n});
   produce_block();

   const name alice = "alice"_n;
   const auto authorized_by = packed(alice);
   const auto update = [&](name permission) {
      return native_action(alice, updateauth{.account = alice, .permission = permission,
                                             .parent = config::active_name, .auth = active_authority(alice)});
   };
   const auto link = [&](name type, name permission) {
      return native_action(alice, linkauth{alice, config::system_account_name, type, permission});
   };
   const auto unlink = [&](name type) {
      return native_action(alice, unlinkauth{alice, config::system_account_name, type});
   };
   const auto remove = [&](name permission) { return native_action(alice, deleteauth{alice, permission}); };

   // updateauth: exactly the struct, then with authorized_by.
   push_with_tail(update("first"_n), {});
   push_with_tail(update("second"_n), authorized_by);
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(update("third"_n), concat(authorized_by, tail_of(1))));
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(update("third"_n), concat(authorized_by, tail_of(large_tail_size))));
   BOOST_CHECK_THROW(push_with_tail(update("third"_n), tail_of(1)), fc::exception);
   produce_block();
   BOOST_CHECK(has_permission(alice, "second"_n));
   BOOST_CHECK(!has_permission(alice, "third"_n));

   // linkauth.
   push_with_tail(link("one"_n, "first"_n), {});
   push_with_tail(link("two"_n, "second"_n), authorized_by);
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(link("three"_n, "first"_n), concat(authorized_by, tail_of(1))));
   CHECK_TRAILING_BYTES_REJECTED(
      push_with_tail(link("three"_n, "first"_n), concat(authorized_by, tail_of(large_tail_size))));
   BOOST_CHECK_THROW(push_with_tail(link("three"_n, "first"_n), tail_of(1)), fc::exception);
   produce_block();

   // unlinkauth.
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(unlink("one"_n), concat(authorized_by, tail_of(1))));
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(unlink("one"_n), concat(authorized_by, tail_of(large_tail_size))));
   BOOST_CHECK_THROW(push_with_tail(unlink("one"_n), tail_of(1)), fc::exception);
   push_with_tail(unlink("one"_n), {});
   push_with_tail(unlink("two"_n), authorized_by);
   produce_block();

   // deleteauth.
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(remove("first"_n), concat(authorized_by, tail_of(1))));
   CHECK_TRAILING_BYTES_REJECTED(push_with_tail(remove("first"_n), concat(authorized_by, tail_of(large_tail_size))));
   BOOST_CHECK_THROW(push_with_tail(remove("first"_n), tail_of(1)), fc::exception);
   push_with_tail(remove("first"_n), {});
   push_with_tail(remove("second"_n), authorized_by);
   produce_block();
   BOOST_CHECK(!has_permission(alice, "first"_n));
   BOOST_CHECK(!has_permission(alice, "second"_n));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(sysio_without_contract_runs_only_chain_actions) try {
   validating_tester chain({}, nullptr, setup_policy::none);

   // The chain's own onblock runs in every block.
   bool onblock_ran = false;
   {
      boost::signals2::scoped_connection conn = chain.control->applied_transaction().connect(
         [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> x) {
            const auto& trace = std::get<0>(x);
            if (is_onblock(*trace)) {
               BOOST_CHECK_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : std::string{}));
               onblock_ran = true;
            }
         });
      chain.produce_block();
   }
   BOOST_CHECK(onblock_ran);

   // Native actions still run with no contract on sysio.
   chain.create_accounts({"alice"_n});
   chain.produce_block();
   BOOST_CHECK(chain.control->find_account("alice"_n) != nullptr);

   // Nothing else does: no other action name, and no onblock outside the chain's own transaction.
   const auto push = [&](name action_name) {
      return push_with_tail(chain,
                            action{vector<permission_level>{{config::system_account_name, config::active_name}},
                                   config::system_account_name, action_name, bytes{}},
                            {});
   };
   BOOST_CHECK_EXCEPTION(push("nosuchact"_n), action_validate_exception,
                         fc_exception_message_contains("No contract for action nosuchact on account sysio"));
   BOOST_CHECK_EXCEPTION(push(config::onblock_action_name), action_validate_exception,
                         fc_exception_message_contains("No contract for action onblock on account sysio"));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
