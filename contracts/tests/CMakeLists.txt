#list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/libraries/testing")
#list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/lib/cmake")

# find_package(sysio REQUIRED)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/contracts.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/contracts.hpp)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
# UNIT TESTING ###
include(CTest) # eliminates DartConfiguration.tcl errors at test runtime
enable_testing()

# build unit test executable
file(GLOB UNIT_TESTS "*.cpp" "*.hpp") # find all unit test suites
set(CONTRACT_UNIT_TEST_EXE contracts_unit_test)
unittest_target(
  ${CONTRACT_UNIT_TEST_EXE}
  SOURCE_FILES
  ${UNIT_TESTS}
) # build unit tests as one executable

# add_sysio_test_executable(${CONTRACT_UNIT_TEST_EXE} ${UNIT_TESTS}) # build unit tests as one executable

# mark test suites for execution
foreach(TEST_SUITE ${UNIT_TESTS}) # create an independent target for each test suite
  execute_process(
    COMMAND
    bash -c
    "grep -E 'BOOST_AUTO_TEST_SUITE\\s*[(]' ${TEST_SUITE} | grep -vE '//.*BOOST_AUTO_TEST_SUITE\\s*[(]' | cut -d ')' -f 1 | cut -d '(' -f 2"
    OUTPUT_VARIABLE SUITE_NAME
    OUTPUT_STRIP_TRAILING_WHITESPACE) # get the test suite name from the *.cpp file
  if(NOT "" STREQUAL "${SUITE_NAME}") # ignore empty lines
    execute_process(
      COMMAND bash -c "echo ${SUITE_NAME} | sed -e 's/s$//' | sed -e 's/_test$//'"
      OUTPUT_VARIABLE TRIMMED_SUITE_NAME
      OUTPUT_STRIP_TRAILING_WHITESPACE) # trim "_test" or "_tests" from the end of ${SUITE_NAME}
    # to run unit_test with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose"
    foreach(RUNTIME ${SYSIO_WASM_RUNTIMES})
      add_test(NAME ${TRIMMED_SUITE_NAME}_unit_test_${RUNTIME} COMMAND unit_test --run_test=${SUITE_NAME} --report_level=detailed --color_output -- --${RUNTIME})
      # build list of tests to run during coverage testing
      #      if(ctest_tests)
      #        string(APPEND ctest_tests "|")
      #      endif()
      #      string(APPEND ctest_tests ${TRIMMED_SUITE_NAME}_unit_test_$RUNTIME)
    endforeach()
    #    add_test(
    #      NAME ${TRIMMED_SUITE_NAME}_unit_test
    #      COMMAND ${CONTRACT_UNIT_TEST_EXE} --run_test=${SUITE_NAME} --report_level=detailed
    #      --color_output)
  endif()
endforeach(TEST_SUITE)
