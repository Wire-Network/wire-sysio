add_executable( test_alert_monitor_plugin main.cpp test_alert_monitor.cpp )
# version: sysio/chain/app.hpp, included by the case that checks nodeop's non-default-options masking.
target_link_libraries( test_alert_monitor_plugin alert_monitor_plugin sysio_testing sysio_chain_wrap fc-test version )
# The end-to-end query case reuses the query engine's chain fixture read-only: its test directory supplies
# query_fixture.hpp, and unittests/include the test-contract accessors it deploys.
target_include_directories( test_alert_monitor_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/plugins/query_engine_plugin/test
        ${CMAKE_BINARY_DIR}/unittests/include )
# The shipped sample template is compiled and rendered by a test, so the file and the token vocabulary
# cannot drift apart. QUERY_PLUGIN_SOURCE_DIR is what the query engine's fixture headers read their checked-in
# schemas from.
target_compile_definitions( test_alert_monitor_plugin PRIVATE
        ALERT_MONITOR_SAMPLE_TEMPLATE="${CMAKE_SOURCE_DIR}/etc/alert_monitor/document-template.json"
        QUERY_PLUGIN_SOURCE_DIR="${CMAKE_SOURCE_DIR}/plugins/query_engine_plugin" )
# Plugin directories precede unittests in configure order.
if(BUILD_TEST_CONTRACTS)
   cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL add_dependencies test_alert_monitor_plugin test_contracts_project)
endif()
add_p_test(NAME test_alert_monitor_plugin COMMAND plugins/alert_monitor_plugin/test/test_alert_monitor_plugin)
