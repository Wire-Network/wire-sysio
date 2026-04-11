add_executable(test_beacon_chain_update_plugin
        main.cpp
        test_beacon_chain_update_plugin.cpp
)
target_link_libraries(test_beacon_chain_update_plugin beacon_chain_update_plugin sysio_testing sysio_chain_wrap)
add_test(NAME test_beacon_chain_update_plugin COMMAND plugins/beacon_chain_update_plugin/test/test_beacon_chain_update_plugin WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
