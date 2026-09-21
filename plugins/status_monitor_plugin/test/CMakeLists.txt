add_executable( test_status_monitor_plugin main.cpp test_status_monitor.cpp )
target_link_libraries( test_status_monitor_plugin status_monitor_plugin sysio_testing sysio_chain_wrap fc-test )
# The shipped sample template is compiled and rendered by a test, so the file and the token vocabulary
# cannot drift apart.
target_compile_definitions( test_status_monitor_plugin PRIVATE
        STATUS_MONITOR_SAMPLE_TEMPLATE="${CMAKE_SOURCE_DIR}/etc/status_monitor/document-template.json" )
add_p_test(NAME test_status_monitor_plugin COMMAND plugins/status_monitor_plugin/test/test_status_monitor_plugin)
