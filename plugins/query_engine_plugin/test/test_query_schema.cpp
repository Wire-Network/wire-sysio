#include "query_schema.hpp"

#include <functional>

namespace {
using namespace sysio::query_engine;
using namespace sysio::query_engine::test;

constexpr auto request_schema = "query-request";
constexpr auto response_schema = "query-response";

/// The paged response example with its `result` changed by `mutate`.
std::string paged_response(const std::function<void(fc::mutable_variant_object&)>& mutate) {
   const auto document = fc::json::from_string(read_document("examples/response-paged.json"));
   fc::mutable_variant_object result(document[response_field::result].get_object());
   mutate(result);
   fc::mutable_variant_object envelope(document.get_object());
   envelope.set(response_field::result, fc::variant_object(result));
   return fc::json::to_string(fc::variant_object(envelope), fc::time_point::maximum());
}

/// The paged response example with one `page` member replaced (or removed when `value` is absent).
std::string paged_response_with(const std::string& member, const std::optional<fc::variant>& value) {
   return paged_response([&](fc::mutable_variant_object& result) {
      fc::mutable_variant_object page(result[response_field::page].get_object());
      if (value)
         page.set(member, *value);
      else
         page.erase(member);
      result.set(response_field::page, fc::variant_object(page));
   });
}
} // namespace

/// Documentation examples are subject to the same wire validation as runtime responses.
BOOST_AUTO_TEST_CASE(query_documentation_examples) {
   validate_document(read_document("examples/request.json"), request_schema);
   validate_document(read_document("examples/request-paged.json"), request_schema);
   validate_response(read_document("examples/response.json"));
   validate_response(read_document("examples/response-paged.json"));
   validate_response(read_document("examples/error.json"));
}

/// The request schema admits the paging members only as bounded JSON integers.
BOOST_AUTO_TEST_CASE(query_request_schema_paging_members) {
   for (const auto* members :
        {R"(,"limit":0)", R"(,"limit":9007199254740991)", R"(,"offset":0)", R"(,"offset":9007199254740991)",
         R"(,"timeout_ms":1)", R"(,"limit":2,"offset":2,"timeout_ms":500)"})
      BOOST_CHECK_MESSAGE(!schema_violation(request_body(wide_select_sql, members), request_schema), members);
   for (const auto* members : {R"(,"limit":-1)", R"(,"limit":1.5)", R"(,"limit":"2")", R"(,"limit":null)",
                               R"(,"limit":9007199254740992)", R"(,"offset":-1)", R"(,"offset":true)",
                               R"(,"timeout_ms":0)", R"(,"timeout_ms":9007199254740992)", R"(,"owner":"sample")"})
      BOOST_CHECK_MESSAGE(schema_violation(request_body(wide_select_sql, members), request_schema), members);
}

/// The response schema requires the complete, strictly shaped page object.
BOOST_AUTO_TEST_CASE(query_response_schema_page_member) {
   BOOST_CHECK(!schema_violation(paged_response_with("limit", fc::variant()), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("has_more", std::nullopt), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("has_more", fc::variant("true")), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("total_rows", fc::variant(uint64_t{7})), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("offset", fc::variant("-1")), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("limit", fc::variant("02")), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("limit", fc::variant(uint64_t{2})), response_schema));
   BOOST_CHECK(schema_violation(paged_response_with("cursor", fc::variant("next")), response_schema));
   BOOST_CHECK(schema_violation(
      paged_response([](fc::mutable_variant_object& result) { result.erase(response_field::page); }), response_schema));
}
