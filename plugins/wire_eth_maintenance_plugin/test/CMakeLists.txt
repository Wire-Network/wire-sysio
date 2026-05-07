add_executable(test_wire_eth_maintenance_plugin
        main.cpp
        test_wire_eth_maintenance_plugin.cpp
)
target_link_libraries(test_wire_eth_maintenance_plugin wire_eth_maintenance_plugin sysio_testing sysio_chain_wrap)
add_test(NAME test_wire_eth_maintenance_plugin COMMAND plugins/wire_eth_maintenance_plugin/test/test_wire_eth_maintenance_plugin WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
