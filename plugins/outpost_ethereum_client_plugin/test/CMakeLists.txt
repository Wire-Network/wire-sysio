# Ethereum RPC fixtures receive ports from the platform bind registry.
file(GLOB TEST_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(test_outpost_ethereum_client_plugin ${TEST_FILES})
target_link_libraries(test_outpost_ethereum_client_plugin PUBLIC
        outpost_ethereum_client_plugin sysio_testing sysio_chain_wrap)
set_target_properties(test_outpost_ethereum_client_plugin PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..)
find_program(WIRE_TEST_NODE node REQUIRED)
add_test(NAME test_outpost_ethereum_client_plugin
        COMMAND ${WIRE_TEST_NODE} ${CMAKE_SOURCE_DIR}/tests/run_with_bind_config.cjs
                $<TARGET_FILE:test_outpost_ethereum_client_plugin>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
