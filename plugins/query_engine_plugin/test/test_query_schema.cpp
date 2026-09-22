#include "query_schema.hpp"

/// Documentation examples are subject to the same wire validation as runtime responses.
BOOST_AUTO_TEST_CASE(query_documentation_examples) {
   using namespace sysio::query_engine_plugin::test;
   validate_document(read_document("examples/request.json"), "query-request");
   validate_response(read_document("examples/response.json"));
   validate_response(read_document("examples/error.json"));
}
