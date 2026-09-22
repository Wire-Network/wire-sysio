# Build through the repository's CDT ExternalProject, or reuse checked-in fixture artifacts.
if(BUILD_TEST_CONTRACTS)
   add_contract(query_fixture query_fixture query_fixture.cpp)
else()
   configure_file(query_fixture.wasm query_fixture.wasm COPYONLY)
   configure_file(query_fixture.abi query_fixture.abi COPYONLY)
endif()
