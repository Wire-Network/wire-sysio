
set(TARGET_NAME cranker-example)

file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp)

chain_target(${TARGET_NAME} SOURCE_FILES ${SRC_FILES})

if (ENABLE_TESTS)
    find_program(WIRE_TEST_NODE node REQUIRED)
    find_program(WIRE_TEST_PYTHON python3 REQUIRED)
    configure_file(test/startup_recovery.py startup_recovery.py COPYONLY)
    add_test(NAME cranker_startup_recovery
        COMMAND ${WIRE_TEST_NODE} ${CMAKE_SOURCE_DIR}/tests/run_with_bind_config.cjs
            ${WIRE_TEST_PYTHON} ${CMAKE_CURRENT_BINARY_DIR}/startup_recovery.py
            --binary $<TARGET_FILE:cranker-example>
            --source ${CMAKE_SOURCE_DIR}
            --output-root ${CMAKE_CURRENT_BINARY_DIR}/test-output
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(cranker_startup_recovery PROPERTIES TIMEOUT 120)
endif ()
