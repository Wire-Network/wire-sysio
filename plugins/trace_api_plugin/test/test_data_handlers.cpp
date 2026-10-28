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

   // serialize_to_json_stream's output, wrapped in the object the get_block stream writes it into.
   std::string serialized_json(abi_data_handler& handler, const std::variant<action_trace_v0>& action) {
      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }
      return out;
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

      BOOST_TEST(serialized_json(handler, action_trace_t) == "{}");
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

         BOOST_TEST(serialized_json(handler, action_trace_t) == "{}");
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

         BOOST_TEST(serialized_json(handler, action_trace_t) == "{}");
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

      BOOST_TEST(serialized_json(handler, action_trace_t) == R"json({"params":{"a":0,"b":1,"c":2,"d":3}})json");
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

      BOOST_TEST(serialized_json(handler, action_trace_t) == R"json({"params":{"a":0,"b":1,"c":2,"d":3},"return_data":{"e":4,"f":5,"g":6}})json");
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

      BOOST_TEST(serialized_json(handler, action_trace_t) == "{}");
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

      BOOST_TEST(serialized_json(handler, action_trace_t) == "{}");
      BOOST_TEST(log_called);
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

      BOOST_TEST(serialized_json(handler, action_trace_t) == R"json({"params":{"a":0,"b":1,"c":2,"d":3}})json");
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

   // Params decode failure: truncated params bytes throw after "params" is half-written; the handler
   // rewinds every byte it wrote and logs, emitting nothing.
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

   // Return_value decode failure: params were already emitted when the return_data decode throws,
   // and emission is all-or-nothing, so the rewind rolls back params as well.
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

   namespace {
      // decode_to_json_stream's output, wrapped in the object the get_actions stream writes it into.
      std::string streamed_decode_json(abi_data_handler& handler, const action_trace_v0& action) {
         std::string out;
         {
            fc::json_writer w(out);
            w.begin_object();
            handler.decode_to_json_stream(action, w);
            w.end_object();
            BOOST_REQUIRE(w.balanced());
         }
         return out;
      }

      // An action type with a single bytes field, so a payload of any size can be decoded.
      chain::abi_def make_blob_abi() {
         auto abi = chain::abi_def ( {},
            {
               { "blob", "", { {"payload", "bytes"} } }
            },
            {
               { "foo"_n, "blob", ""}
            },
            {}, {}, {}
         );
         abi.version = "sysio::abi/1.";
         return abi;
      }

      struct budget_abort {};
   }

   // get_actions' decoder: params and return_data both decoded.
   BOOST_AUTO_TEST_CASE(decode_stream_with_return_value)
   {
      const action_trace_v0 action = make_foo_action({0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06});
      abi_data_handler handler(exception_handler{}, make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      BOOST_TEST(streamed_decode_json(handler, action) == R"json({"params":{"a":0,"b":1,"c":2,"d":3},"return_data":{"e":4,"f":5,"g":6}})json");
   }

   // A params failure drops params and reports the exception in decode_error.
   BOOST_AUTO_TEST_CASE(decode_stream_params_failure)
   {
      const action_trace_v0 action = make_foo_action({0x00, 0x01, 0x02} /* truncated */, {0x04, 0x05, 0x06});
      size_t log_calls = 0;
      abi_data_handler handler([&log_calls](const exception_with_context&){ ++log_calls; },
                               make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      const fc::variant streamed = fc::json::from_string(streamed_decode_json(handler, action));
      const auto& obj = streamed.get_object();
      BOOST_TEST(obj.size() == 1u);
      BOOST_REQUIRE(obj.contains("decode_error"));
      const std::string error = obj["decode_error"].as_string();
      BOOST_TEST(error.starts_with("3015013 unpack_exception"));
      BOOST_TEST(error.find("unable to unpack field 'd' of struct 'foo'") != std::string::npos);
      BOOST_TEST(log_calls == 1u);
   }

   // A return_value failure keeps the decoded params and reports decode_error in place of return_data.
   BOOST_AUTO_TEST_CASE(decode_stream_return_value_failure)
   {
      const action_trace_v0 action = make_foo_action({0x00, 0x01, 0x02, 0x03}, {0x04, 0x05} /* truncated */);
      size_t log_calls = 0;
      abi_data_handler handler([&log_calls](const exception_with_context&){ ++log_calls; },
                               make_resolver("alice"_n),
                               make_fetcher("alice"_n, pack_abi(make_foo_abi_with_result())));

      const std::string streamed = streamed_decode_json(handler, action);
      BOOST_TEST(streamed.starts_with(
         R"json({"params":{"a":0,"b":1,"c":2,"d":3},"decode_error":"return_value decode failed: 3015013 unpack_exception)json"));
      BOOST_TEST(streamed.find("unable to unpack field 'g' of struct 'foor'") != std::string::npos);
      BOOST_TEST(log_calls == 1u);
   }

   // No ABI: nothing to decode, nothing emitted.
   BOOST_AUTO_TEST_CASE(decode_stream_without_abi_emits_nothing)
   {
      const action_trace_v0 action = make_foo_action({0x00, 0x01, 0x02, 0x03}, {});
      abi_data_handler handler(exception_handler{});

      BOOST_TEST(streamed_decode_json(handler, action) == "{}");
   }

   // A response-budget abort inside the params decode is not a decode failure: it leaves the writer, it is not
   // reported as decode_error, and it is not logged.
   BOOST_AUTO_TEST_CASE(decode_stream_does_not_absorb_budget_abort)
   {
      constexpr size_t payload_size = 4096;  // 8 KiB of hex once decoded
      constexpr size_t budget       = 1024;
      constexpr size_t guard_stride = 64;
      const action_trace_v0 action = make_foo_action(fc::raw::pack(chain::bytes(payload_size, 'x')), {});
      size_t log_calls = 0;
      abi_data_handler handler([&log_calls](const exception_with_context&){ ++log_calls; },
                               make_resolver("alice"_n), make_fetcher("alice"_n, pack_abi(make_blob_abi())));

      std::string out;
      fc::json_writer w(out, [](size_t size) { if (size > budget) throw budget_abort{}; }, guard_stride);
      w.begin_object();
      BOOST_CHECK_THROW(handler.decode_to_json_stream(action, w), budget_abort);
      BOOST_TEST(log_calls == 0u);
      BOOST_TEST(out.find("decode_error") == std::string::npos);
   }


BOOST_AUTO_TEST_SUITE_END()
