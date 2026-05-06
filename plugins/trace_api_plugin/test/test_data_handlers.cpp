#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>

#include <sysio/trace_api/abi_data_handler.hpp>

#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

BOOST_AUTO_TEST_SUITE(abi_data_handler_tests)
   BOOST_AUTO_TEST_CASE(empty_data)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {}, {}
      };
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
         auto action = action_trace_v0 {
            0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {}
         };
         std::variant<action_trace_v0> action_trace_t = action;
         abi_data_handler handler(exception_handler{});

         auto expected = fc::variant();
         auto actual = handler.serialize_to_variant(action_trace_t);

         BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
         BOOST_REQUIRE(!std::get<1>(actual));
      }

      // With return_value
      {
         auto action = action_trace_v0 {
            0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06, 0x07}
         };
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
      auto action = action_trace_v0 {
            0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {}
      };

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

      abi_data_handler handler(exception_handler{});
      handler.add_abi("alice"_n, std::move(abi));

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
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06}
      };

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

      abi_data_handler handler(exception_handler{});
      handler.add_abi("alice"_n, std::move(abi));

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
      auto action = action_trace_v0 {
            0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06, 0x07}
      };

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

      abi_data_handler handler(exception_handler{});
      handler.add_abi("alice"_n, std::move(abi));

      auto expected = fc::variant();

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   BOOST_AUTO_TEST_CASE(basic_abi_insufficient_data)
   {
      auto action = action_trace_v0 {
            0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02}, {}
      };

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
      abi_data_handler handler([&log_called](const exception_with_context& ){log_called = true;});
      handler.add_abi("alice"_n, std::move(abi));

      auto expected = fc::variant();

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_TEST(log_called);
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   // If no ABI provided for return type then do not attempt to decode it
   BOOST_AUTO_TEST_CASE(basic_abi_no_return_abi_when_return_value_provided)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06}
      };

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

      abi_data_handler handler(exception_handler{});
      handler.add_abi("alice"_n, std::move(abi));

      fc::variant expected = fc::mutable_variant_object()
            ("a", 0)
            ("b", 1)
            ("c", 2)
            ("d", 3);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(to_kv(expected) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual));
   }

   // Streaming serialize_to_json_stream must emit byte-equivalent JSON to the variant path
   // when the same ABI is supplied.  Drive the streamer into a wrapped object so the result
   // can be parsed back as a variant and compared field-by-field.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_with_return_value_parity)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06}
      };
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

      abi_data_handler handler(exception_handler{});
      handler.add_abi("alice"_n, std::move(abi));

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

   // No ABI registered: the streaming path must emit zero key/value pairs and leave the writer
   // in a balanced, key/value-pair-empty state so the surrounding object closes cleanly.
   BOOST_AUTO_TEST_CASE(streaming_no_abi_emits_nothing)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05, 0x06}
      };
      std::variant<action_trace_v0> action_trace_t = action;

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

   // Per-field independence (variant path, params throws / return_data succeeds): truncated params
   // bytes raise inside binary_to_variant; serialize_to_variant logs and falls through to attempt
   // return_data with the same yield.  return_data decodes cleanly, so the result is
   // {null_variant, ret_data}: call sites that check `!params.is_null()` skip params and emit
   // just return_data.  Pre-PR (single try wrapping both) the catch swallowed both fields.
   BOOST_AUTO_TEST_CASE(variant_basic_abi_params_throws_return_data_succeeds)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02}, {0x04, 0x05, 0x06}
      };
      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } },
            { "foor", "", { {"e", "varuint32"}, {"f", "varuint32"}, {"g", "varuint32"} } }
         },
         { { "foo"_n, "foo", ""} },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";
      abi.action_results = { std::vector<chain::action_result_def>{ chain::action_result_def{ "foo"_n, "foor"} } };

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; });
      handler.add_abi("alice"_n, std::move(abi));

      fc::variant expected_return = fc::mutable_variant_object()
            ("e", 4)("f", 5)("g", 6);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(log_called);
      BOOST_TEST(std::get<0>(actual).is_null());
      BOOST_REQUIRE(std::get<1>(actual).has_value());
      BOOST_TEST(to_kv(expected_return) == to_kv(*std::get<1>(actual)), boost::test_tools::per_element());
   }

   // Per-field independence (variant path, params succeeds / return_data throws): params decodes
   // cleanly, return_data bytes are truncated.  serialize_to_variant logs the return_data failure
   // and returns the params-decoded variant alongside an empty optional.  Pre-PR the catch on
   // return_data discarded the already-successful params; the per-field rewrite preserves it.
   BOOST_AUTO_TEST_CASE(variant_basic_abi_params_succeeds_return_data_throws)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05}
      };
      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } },
            { "foor", "", { {"e", "varuint32"}, {"f", "varuint32"}, {"g", "varuint32"} } }
         },
         { { "foo"_n, "foo", ""} },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";
      abi.action_results = { std::vector<chain::action_result_def>{ chain::action_result_def{ "foo"_n, "foor"} } };

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; });
      handler.add_abi("alice"_n, std::move(abi));

      fc::variant expected_params = fc::mutable_variant_object()
            ("a", 0)("b", 1)("c", 2)("d", 3);

      auto actual = handler.serialize_to_variant(action_trace_t);

      BOOST_TEST(log_called);
      BOOST_TEST(to_kv(expected_params) == to_kv(std::get<0>(actual)), boost::test_tools::per_element());
      BOOST_REQUIRE(!std::get<1>(actual).has_value());
   }

   // Per-field independence (streaming path, params throws / return_data succeeds): truncated
   // params bytes throw after "params" is half-written; the params catch rewinds and logs but
   // does NOT short-circuit (the throw is not abi_recursion_depth_exception), so return_data is
   // attempted with a fresh checkpoint and decodes successfully.  Output contains return_data only.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_params_throws_return_data_succeeds)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02}, {0x04, 0x05, 0x06}
      };
      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } },
            { "foor", "", { {"e", "varuint32"}, {"f", "varuint32"}, {"g", "varuint32"} } }
         },
         { { "foo"_n, "foo", ""} },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";
      abi.action_results = { std::vector<chain::action_result_def>{ chain::action_result_def{ "foo"_n, "foor"} } };

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; });
      handler.add_abi("alice"_n, std::move(abi));

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      BOOST_TEST(log_called);
      fc::variant streamed = fc::json::from_string(out);
      const auto& streamed_obj = streamed.get_object();
      BOOST_TEST(!streamed_obj.contains("params"));
      BOOST_REQUIRE(streamed_obj.contains("return_data"));

      fc::variant expected_return = fc::mutable_variant_object()
            ("e", 4)("f", 5)("g", 6);
      BOOST_TEST(to_kv(expected_return) == to_kv(streamed_obj["return_data"]), boost::test_tools::per_element());
   }

   // Per-field independence (streaming path, params succeeds / return_data throws): params is
   // emitted cleanly; the return_data decode throws on truncated bytes; the return_data catch
   // rewinds only its own tokens and logs.  Output contains params only; the writer stays
   // balanced and the surrounding object closes correctly.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_params_succeeds_return_data_throws)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02, 0x03}, {0x04, 0x05}
      };
      std::variant<action_trace_v0> action_trace_t = action;

      auto abi = chain::abi_def ( {},
         {
            { "foo", "", { {"a", "varuint32"}, {"b", "varuint32"}, {"c", "varuint32"}, {"d", "varuint32"} } },
            { "foor", "", { {"e", "varuint32"}, {"f", "varuint32"}, {"g", "varuint32"} } }
         },
         { { "foo"_n, "foo", ""} },
         {}, {}, {}
      );
      abi.version = "sysio::abi/1.";
      abi.action_results = { std::vector<chain::action_result_def>{ chain::action_result_def{ "foo"_n, "foor"} } };

      bool log_called = false;
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; });
      handler.add_abi("alice"_n, std::move(abi));

      std::string out;
      {
         fc::json_writer w(out);
         w.begin_object();
         handler.serialize_to_json_stream(action_trace_t, w);
         w.end_object();
         BOOST_REQUIRE(w.balanced());
      }

      BOOST_TEST(log_called);
      fc::variant streamed = fc::json::from_string(out);
      const auto& streamed_obj = streamed.get_object();
      BOOST_REQUIRE(streamed_obj.contains("params"));
      BOOST_TEST(!streamed_obj.contains("return_data"));

      fc::variant expected_params = fc::mutable_variant_object()
            ("a", 0)("b", 1)("c", 2)("d", 3);
      BOOST_TEST(to_kv(expected_params) == to_kv(streamed_obj["params"]), boost::test_tools::per_element());
   }

   // Truncated action data triggers an exception inside binary_to_json_stream after "params" has
   // already been written.  The handler must rewind, log via except_handler, and leave the writer
   // observably empty (no half-written "params": fragment) so the surrounding object closes cleanly.
   BOOST_AUTO_TEST_CASE(streaming_basic_abi_insufficient_data_rolls_back)
   {
      auto action = action_trace_v0 {
         0, "alice"_n, "alice"_n, "foo"_n, {}, {0x00, 0x01, 0x02}, {}
      };
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
      abi_data_handler handler([&log_called](const exception_with_context&){ log_called = true; });
      handler.add_abi("alice"_n, std::move(abi));

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
