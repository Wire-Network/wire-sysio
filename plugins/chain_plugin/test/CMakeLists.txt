add_executable( test_chain_plugin
        test_account_query_db.cpp
        test_block_channels.cpp
        test_trx_retry_db.cpp
        test_trx_finality_status_processing.cpp
        plugin_config_test.cpp
        main.cpp
        )
target_link_libraries( test_chain_plugin chain_plugin sysio_testing sysio_chain_wrap version )
target_include_directories(test_chain_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/unittests
        ${CMAKE_BINARY_DIR}/unittests/include)
# These cases open HTTP listeners and need the shared port lease; keep ordinary discovery for the others.
set(snapshot_endpoint_tests
        chain_plugin_accepts_attested_auto_fetched_snapshot
        chain_plugin_rejects_auto_fetched_snapshot_without_enabled_config
        chain_plugin_rejects_auto_fetched_snapshot_at_unscheduled_height)
list(JOIN snapshot_endpoint_tests "|" snapshot_endpoint_test_pattern)

boost_test_discover_tests(test_chain_plugin
        NAME test_chain_plugin_@UNIT@
        EXCLUDE "^(${snapshot_endpoint_test_pattern})$"
        ARGS --report_level=detailed --color_output)

foreach(snapshot_endpoint_test IN LISTS snapshot_endpoint_tests)
    add_p_test(NAME test_chain_plugin_${snapshot_endpoint_test} PORT_LEASE
            COMMAND test_chain_plugin --run_test=${snapshot_endpoint_test}
                    --report_level=detailed --color_output)
endforeach()
