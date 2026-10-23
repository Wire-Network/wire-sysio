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
        ${CMAKE_SOURCE_DIR}/unittests
        ${CMAKE_BINARY_DIR}/unittests/include)
boost_test_discover_tests(test_chain_plugin
        NAME test_chain_plugin_@UNIT@
        ARGS --report_level=detailed --color_output)
