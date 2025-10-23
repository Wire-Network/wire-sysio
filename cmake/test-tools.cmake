include(CMakeParseArguments)

macro(unittest_target TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCE_FILES" ${ARGN})
  message(NOTICE "Creating unittest target (${TARGET}) with sources: ${ARG_SOURCE_FILES}")
  add_executable(${TARGET} ${ARG_SOURCE_FILES})

  target_link_libraries(
    ${TARGET}
    sysio_chain_wrap
    state_history
    chainbase
    sysio_testing
    fc
    custom_appbase
    abieos
    libsodium::libsodium
  )

  target_include_directories(${TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/libraries/testing/include
    ${CMAKE_BINARY_DIR}/contracts
    ${CMAKE_SOURCE_DIR}/contracts
    ${CMAKE_SOURCE_DIR}/unittests
    ${CMAKE_SOURCE_DIR}/unittests/system-test-contracts
    ${CMAKE_BINARY_DIR}/unittests/system-test-contracts
    ${CMAKE_BINARY_DIR}/unittests/include
    ${CMAKE_SOURCE_DIR}/plugins/http_plugin/include
    ${CMAKE_SOURCE_DIR}/plugins/chain_interface/include)

endmacro()

macro(unittest_tests_add TARGET)
  cmake_parse_arguments(ARG "" "" "UNITTEST_FILES" ${ARGN})
  foreach(TEST_SUITE ${ARG_UNITTEST_FILES}) # create an independent target for each test suite
    # GET TEST SUITE NAME
    execute_process(
      COMMAND
      bash -c
      "grep -E 'BOOST_AUTO_TEST_SUITE\\s*[(]' ${TEST_SUITE} | grep -vE '//.*BOOST_AUTO_TEST_SUITE\\s*[(]' | cut -d ')' -f 1 | cut -d '(' -f 2"
      OUTPUT_VARIABLE SUITE_NAME
      OUTPUT_STRIP_TRAILING_WHITESPACE) # get the test suite name from the *.cpp file

    # IF NOT EMPTY, ADD TESTS
    if(NOT "" STREQUAL "${SUITE_NAME}") # ignore empty lines
      # TRIM TEST SUITE NAME
      execute_process(
        COMMAND bash -c "echo ${SUITE_NAME} | sed -e 's/s$//' | sed -e 's/_test$//'"
        OUTPUT_VARIABLE TRIMMED_SUITE_NAME
        OUTPUT_STRIP_TRAILING_WHITESPACE) # trim "_test" or "_tests" from the end of ${SUITE_NAME}

      # to run ${TARGET} with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose"
      foreach(RUNTIME ${SYSIO_WASM_RUNTIMES})
        add_test(NAME ${TRIMMED_SUITE_NAME}_${TARGET}_${RUNTIME} COMMAND ${TARGET} --run_test=${SUITE_NAME} --report_level=detailed --color_output -- --${RUNTIME})
      endforeach()

    endif()
  endforeach()
endmacro()