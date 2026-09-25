add_executable(test_query_engine_plugin main.cpp test_query_language.cpp test_query_values.cpp test_query_rpc.cpp
   test_query_integration.cpp test_query_lifecycle.cpp test_query_execute.cpp test_query_schema.cpp)
target_link_libraries(test_query_engine_plugin query_engine_plugin sysio_testing sysio_chain_wrap fc-test)
target_include_directories(test_query_engine_plugin PRIVATE ${CMAKE_BINARY_DIR}/unittests/include)
target_compile_definitions(test_query_engine_plugin PRIVATE QUERY_PLUGIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
# Plugin directories precede unittests in configure order.
if(BUILD_TEST_CONTRACTS)
   cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL add_dependencies test_query_engine_plugin test_contracts_project)
endif()
add_p_test(NAME test_query_engine_plugin COMMAND plugins/query_engine_plugin/test/test_query_engine_plugin)

add_executable(test_query_engine_plugin_http main.cpp test_query_http.cpp)
target_link_libraries(test_query_engine_plugin_http query_engine_plugin chain_api_plugin producer_plugin version sysio_testing sysio_chain_wrap fc-test)
target_include_directories(test_query_engine_plugin_http PRIVATE ${CMAKE_BINARY_DIR}/unittests/include)
target_compile_definitions(test_query_engine_plugin_http PRIVATE QUERY_PLUGIN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
if(BUILD_TEST_CONTRACTS)
   cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL add_dependencies test_query_engine_plugin_http test_contracts_project)
endif()
add_np_test(NAME test_query_engine_plugin_http COMMAND plugins/query_engine_plugin/test/test_query_engine_plugin_http)
