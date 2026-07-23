#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>

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

   // Resolver half of the two-phase lookup: the account has exactly one ABI version, recorded at
   // effective_global_seq 0, so the handler's cache key becomes (account, 0) regardless of the
   // action's global_seq.
   abi_data_handler::abi_seq_resolver_fn make_resolver(chain::name account) {
      return [account](chain::name a, uint64_t) -> std::optional<uint64_t> {
         if (a == account) return uint64_t{0};
         return std::nullopt;
      };
   }

   // Fetcher half: returns the packed ABI bytes for the (account, 0) version the resolver names.
   abi_data_handler::abi_blob_fetcher_fn make_fetcher(chain::name account, std::vector<char> abi_bytes) {
      return [account, bytes = std::move(abi_bytes)](chain::name a, uint64_t effective_seq) -> std::optional<std::vector<char>> {
         if (a == account && effective_seq == 0) return bytes;
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

      abi_data_handler handler(exception_handler{}, make_resolver("alice"_n), make_fetcher("alice"_n, pack_abi(abi)));

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

      abi_data_handler handler(exception_handler{}, make_resolver("alice"_n), make_fetcher("alice"_n, pack_abi(abi)));

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

      abi_data_handler handler(exception_handler{}, make_resolver("alice"_n), make_fetcher("alice"_n, pack_abi(abi)));

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
         make_resolver("alice"_n),
         make_fetcher("alice"_n, pack_abi(abi))
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

      abi_data_handler handler(exception_handler{}, make_resolver("alice"_n), make_fetcher("alice"_n, pack_abi(abi)));

      fc::variant expected = fc::mutable_variant_object()
            ("a", 0)
            ("b", 1)
            ("c", 2)
            ("d", 3);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   namespace {
      // Shared two-type ABI for the streaming/variant lock-step cases below: action type "foo"
      // (four varuint32 fields) with action_result type "foor" (three varuint32 fields).
      chain::abi_def make_foo_abi_with_result() {
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
         return abi;
      }

      action_trace_v0 make_foo_action(chain::bytes data, chain::bytes return_value) {
         action_trace_v0 action;
         action.global_sequence = 0;
         action.receiver        = "alice"_n;
         action.account         = "alice"_n;
         action.action          = "foo"_n;
         action.data            = std::move(data);
         action.return_value    = std::move(return_value);
         return action;
      }
   }

   // Streaming serialize_to_json_stream must emit byte-equivalent JSON to the variant path
   // when the same ABI is supplied.  Drive the streamer into a wrapped object so the result
   // can be parsed back as a variant and compared field-by-field.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_with_return_value_parity)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06});

      abi_data_handler handler(exception_handler{}, make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      auto [params_v, return_v] = handler.serialize_to_variant(action_trace_t);

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      fc::variant streamed = fc::json::from_string(out);
      const auto& streamed_obj = streamed.get_object();
      BOOST_REQUIRE(streamed_obj.contains("params"));
      BOOST_REQUIRE(streamed_obj.contains("return_data"));

      BOOST_TEST(to_kv(params_v) == to_kv(streamed_obj["params"]), boost::test_tools::per_element());
      BOOST_REQUIRE(return_v.has_value());
      BOOST_TEST(to_kv(*return_v) == to_kv(streamed_obj["return_data"]), boost::test_tools::per_element());
   }

   // No ABI resolvable: the streaming path must emit zero key/value pairs and leave the writer
   // in a balanced, key/value-pair-empty state so the surrounding object closes cleanly.
   BOOST_AUTO_TEST_CASE(streaming_no_abi_emits_nothing)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06});

      abi_data_handler handler(exception_handler{});

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      BOOST_TEST(out == "{}");
   }

   // Variant-pipeline shape on params decode failure: decode() short-circuits with
   // decode_status::failed, and serialize_to_variant maps anything but ok to the legacy empty
   // tuple.  The failure is still surfaced through except_handler.
   BOOST_AUTO_TEST_CASE(variant_basic_abi_params_throws_yields_empty)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02} /* truncated */, {0x04, 0x05, 0x06});

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; },
                               make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(log_called);
      BOOST_TEST(std::get<0>(actual).is_null());
      BOOST_REQUIRE(!std::get<1>(actual).has_value());
   }

   // Variant-pipeline shape on return_value decode failure: decode() keeps the decoded params in
   // decode_result (the get_actions path emits them alongside decode_error), but the legacy
   // tuple wrapper drops the whole action on any failure -- both fields come back empty.
   BOOST_AUTO_TEST_CASE(variant_basic_abi_return_data_throws_yields_empty)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02, 0x03}, {0x04, 0x05} /* truncated */);

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; },
                               make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(log_called);
      BOOST_TEST(std::get<0>(actual).is_null());
      BOOST_REQUIRE(!std::get<1>(actual).has_value());
   }

   // Streaming lock-step with the variant pipeline on params decode failure: truncated params
   // bytes throw after "params" is half-written; the handler rewinds every byte it wrote and
   // logs, emitting nothing -- the same empty shape serialize_to_variant produces.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_params_throws_emits_nothing)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02} /* truncated */, {0x04, 0x05, 0x06});

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; },
                               make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      BOOST_TEST(log_called);
      BOOST_TEST(out == "{}");
   }

   // Streaming lock-step with the variant pipeline on return_value decode failure: params were
   // already emitted when the return_data decode throws, so the rewind must roll back params as
   // well -- the variant path returns the legacy empty shape and the two paths must match.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_return_data_throws_emits_nothing)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02, 0x03}, {0x04, 0x05} /* truncated */);

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; },
                               make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      BOOST_TEST(log_called);
      BOOST_TEST(out == "{}");
   }

   // Truncated action data triggers an exception inside binary_to_json_stream after "params" has
   // already been written.  The handler must rewind, log via except_handler, and leave the writer
   // observably empty (no half-written "params": fragment) so the surrounding object closes cleanly.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_insufficient_data_rolls_back)
   {
      std::variant<action_trace_v0> action_trace_t =
         make_foo_action({0x00, 0x01, 0x02} /* truncated */, {});

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
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; },
                               make_resolver("alice"_n), make_fetcher("alice"_n, pack_abi(abi)));

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      BOOST_TEST(log_called);
      BOOST_TEST(out == "{}");
   }


BOOST_AUTO_TEST_SUITE_END()
