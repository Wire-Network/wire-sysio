#include <boost/test/unit_test.hpp>

#include <sysio/trace_api/abi_data_handler.hpp>

#include <sysio/trace_api/test_common.hpp>
#include <fc/io/raw.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {
   // Pack an abi_def into raw bytes that abi_data_handler can unpack
   std::vector<char> pack_abi(const chain::abi_def& abi) {
      return fc::raw::pack(abi);
   }

   // Build a lookup_fn that returns packed ABI bytes for a given account.
   // effective_global_seq is fixed at 0 for test purposes -- the handler's
   // cache key becomes (account, 0) regardless of the action's global_seq.
   abi_data_handler::abi_lookup_fn make_lookup(chain::name account, std::vector<char> abi_bytes) {
      return [account, bytes = std::move(abi_bytes)](chain::name a, uint64_t) -> std::optional<abi_data_handler::lookup_entry> {
         if (a == account) return abi_data_handler::lookup_entry{0, bytes};
         return std::nullopt;
      };
   }
}

BOOST_AUTO_TEST_SUITE(abi_data_handler_tests)
   BOOST_AUTO_TEST_CASE(empty_data)
   {
      action_trace_v0 action;
      action.global_sequence = 0;
      action.receiver = "alice"_n;
      action.account  = "alice"_n;
      action.action   = "foo"_n;
      std::variant<action_trace_v0> action_trace_t = action;
      abi_data_handler handler(exception_handler{});

      auto expected = fc::variant();
      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   BOOST_AUTO_TEST_CASE(no_abi)
   {
      // Without return_value
      {
         action_trace_v0 action;
         action.global_sequence = 0;
         action.receiver = "alice"_n;
         action.account  = "alice"_n;
         action.action   = "foo"_n;
         action.data     = {0x00, 0x01, 0x02, 0x03};
         std::variant<action_trace_v0> action_trace_t = action;
         abi_data_handler handler(exception_handler{});

         auto expected = fc::variant();
         auto actual = handler.serialize_to_variant(action_trace_t);

         BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
         BOOST_REQUIRE(!std::get<1>(actual));
      }

      // With return_value
      {
         action_trace_v0 action;
         action.global_sequence = 0;
         action.receiver      = "alice"_n;
         action.account       = "alice"_n;
         action.action        = "foo"_n;
         action.data          = {0x00, 0x01, 0x02, 0x03};
         action.return_value  = {0x04, 0x05, 0x06, 0x07};
         std::variant<action_trace_v0> action_trace_t = action;
         abi_data_handler handler(exception_handler{});

         auto expected = fc::variant();
         auto actual = handler.serialize_to_variant(action_trace_t);

         BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
         BOOST_REQUIRE(!std::get<1>(actual));
      }
   }

   BOOST_AUTO_TEST_CASE(basic_abi)
   {
      action_trace_v0 action;
      action.global_sequence = 0;
      action.receiver = "alice"_n;
      action.account  = "alice"_n;
      action.action   = "foo"_n;
      action.data     = {0x00, 0x01, 0x02, 0x03};

      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } }
         },
         {
            { "foo"_n, "foo", ""}
         },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";

      abi_data_handler handler(exception_handler{}, make_lookup("alice"_n, pack_abi(abi)));

      fc::variant expected = fc::mutable_variant_object()
         ("a", 0)
         ("b", 1)
         ("c", 2)
         ("d", 3);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   BOOST_AUTO_TEST_CASE(basic_abi_with_return_value)
   {
      action_trace_v0 action;
      action.global_sequence = 0;
      action.receiver      = "alice"_n;
      action.account       = "alice"_n;
      action.action        = "foo"_n;
      action.data          = {0x00, 0x01, 0x02, 0x03};
      action.return_value  = {0x04, 0x05, 0x06};

      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } },
            { "foor", "", { {"e", "varuint32"}, {"f", "varuint32"}, {"g", "varuint32"} } }
         },
         {
            { "foo"_n, "foo", ""}
         },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";
      abi.action_results = { std::vector<chain::action_result_def>{ chain::action_result_def{ "foo"_n, "foor"} } };

      abi_data_handler handler(exception_handler{}, make_lookup("alice"_n, pack_abi(abi)));

      fc::variant expected = fc::mutable_variant_object()
            ("a", 0)
            ("b", 1)
            ("c", 2)
            ("d", 3);
      fc::variant expected_return = fc::mutable_variant_object()
            ("e", 4)
            ("f", 5)
            ("g", 6);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(std::get<1>(actual));
      BOOST_TEST(to_kv(expected_return) == to_kv(*std::get<1>(actual)), boost::test_tools::per_element());
   }

   BOOST_AUTO_TEST_CASE(basic_abi_wrong_type)
   {
      action_trace_v0 action;
      action.global_sequence = 0;
      action.receiver      = "alice"_n;
      action.account       = "alice"_n;
      action.action        = "foo"_n;
      action.data          = {0x00, 0x01, 0x02, 0x03};
      action.return_value  = {0x04, 0x05, 0x06, 0x07};

      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } }
         },
         {
            { "bar"_n, "foo", ""}
         },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";

      abi_data_handler handler(exception_handler{}, make_lookup("alice"_n, pack_abi(abi)));

      auto expected = fc::variant();

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   BOOST_AUTO_TEST_CASE(basic_abi_insufficient_data)
   {
      action_trace_v0 action;
      action.global_sequence = 0;
      action.receiver = "alice"_n;
      action.account  = "alice"_n;
      action.action   = "foo"_n;
      action.data     = {0x00, 0x01, 0x02};

      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } }
         },
         {
            { "foo"_n, "foo", ""}
         },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";

      bool log_called = false;
      abi_data_handler handler(
         [&log_called](const exception_with_context& ){log_called = true;},
         make_lookup("alice"_n, pack_abi(abi))
      );

      auto expected = fc::variant();

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_TEST(log_called);
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   // If no ABI provided for return type then do not attempt to decode it
   BOOST_AUTO_TEST_CASE(basic_abi_no_return_abi_when_return_value_provided)
   {
      action_trace_v0 action;
      action.global_sequence = 0;
      action.receiver      = "alice"_n;
      action.account       = "alice"_n;
      action.action        = "foo"_n;
      action.data          = {0x00, 0x01, 0x02, 0x03};
      action.return_value  = {0x04, 0x05, 0x06};

      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } },
         },
         {
            { "foo"_n, "foo", ""}
         },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";

      abi_data_handler handler(exception_handler{}, make_lookup("alice"_n, pack_abi(abi)));

      fc::variant expected = fc::mutable_variant_object()
            ("a", 0)
            ("b", 1)
            ("c", 2)
            ("d", 3);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }


BOOST_AUTO_TEST_SUITE_END()
