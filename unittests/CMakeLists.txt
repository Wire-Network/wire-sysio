### Build contracts with cdt if available ###
include(ExternalProject)

if(DONT_SKIP_TESTS)
  add_compile_definitions(RUN_FAILING_TESTS=TRUE)
endif()

if(BUILD_TEST_CONTRACTS)
  set(SYSIO_WASM_OLD_BEHAVIOR "Off")

  message(STATUS "Building contracts in directory `./unittests/system-test-contracts/`")
  contracts_target(
    system_test_contracts_project
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/system-test-contracts
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/system-test-contracts
  )

  message(STATUS "Building contracts in directory `./unittests/test-contracts/`")
  contracts_target(
    test_contracts_project
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test-contracts
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-contracts
  )
else()
  message(STATUS "Not building contracts in directory `./unittests/system-test-contracts/`")
  add_subdirectory(system-test-contracts)
  message(STATUS "Not building contracts in directory `./unittests/test-contracts/`")
  add_subdirectory(test-contracts)
endif()

if(NOT DISABLE_WASM_SPEC_TESTS)
  add_subdirectory(wasm-spec-tests/generated-tests)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_contracts.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/test_contracts.hpp ESCAPE_QUOTES)

add_subdirectory(snapshots)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/snapshots.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/snapshots.hpp ESCAPE_QUOTES)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/deep-mind/deep-mind.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/deep-mind.hpp ESCAPE_QUOTES)

add_custom_command(
  OUTPUT protocol_feature_digest_tests.cpp
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/gen_protocol_feature_digest_tests.py ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol_feature_manager.cpp > protocol_feature_digest_tests.cpp
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol_feature_manager.cpp ${CMAKE_CURRENT_SOURCE_DIR}/gen_protocol_feature_digest_tests.py
)

# === Use vcpkg libsodium ===
message(STATUS "Using libsodium from vcpkg")

### BUILD UNIT TEST EXECUTABLE ###
file(GLOB UNIT_TESTS "*.cpp") # find all unit test suites
unittest_target(
  unit_test
  SOURCE_FILES
  ${UNIT_TESTS}
  protocol_feature_digest_tests.cpp
) # build unit tests as one executable

# No pkg-config cflags; keep flags minimal & portable
target_compile_options(unit_test PUBLIC -DDISABLE_SYSLIB_SERIALIZE)

# Link and include libsodium from vcpkg

### MARK TEST SUITES FOR EXECUTION ###
add_test(NAME protocol_feature_digest_unit_test COMMAND unit_test --run_test=protocol_feature_digest_tests --report_level=detailed --color_output)
set(ctest_tests "protocol_feature_digest_tests")
foreach(TEST_SUITE ${UNIT_TESTS}) # create an independent target for each test suite
  execute_process(COMMAND sh -c "grep -E 'BOOST_AUTO_TEST_SUITE\\s*[(]' '${TEST_SUITE}' | grep -vE '//.*BOOST_AUTO_TEST_SUITE\\s*[(]' | cut -d ')' -f 1 | cut -d '(' -f 2" OUTPUT_VARIABLE SUITE_NAMES OUTPUT_STRIP_TRAILING_WHITESPACE) # get the test suite name from the *.cpp file
  string(REPLACE "\n" ";" SUITE_NAMES "${SUITE_NAMES}")
  foreach(SUITE_NAME IN LISTS SUITE_NAMES)
    if(NOT "" STREQUAL "${SUITE_NAME}") # ignore empty lines
      execute_process(COMMAND sh -c "echo ${SUITE_NAME} | sed -e 's/s$//' | sed -e 's/_test$//'" OUTPUT_VARIABLE TRIMMED_SUITE_NAME OUTPUT_STRIP_TRAILING_WHITESPACE) # trim "_test" or "_tests" from the end of ${SUITE_NAME}
      # to run unit_test with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose"
      foreach(RUNTIME ${SYSIO_WASM_RUNTIMES})
        add_test(NAME ${TRIMMED_SUITE_NAME}_unit_test_${RUNTIME} COMMAND unit_test --run_test=${SUITE_NAME} --report_level=detailed --color_output -- --${RUNTIME})
        # build list of tests to run during coverage testing
        if(ctest_tests)
          string(APPEND ctest_tests "|")
        endif()
        string(APPEND ctest_tests ${TRIMMED_SUITE_NAME}_unit_test_$RUNTIME)
      endforeach()
    endif()
  endforeach()
endforeach(TEST_SUITE)
set(ctest_tests "'${ctest_tests}' -j8") # surround test list string in apostrophies

# The following tests are known to take the longest, bump up their cost (priority) so that they'll run first
# even on fresh first time test runs before ctest auto-detects costs
foreach(RUNTIME ${SYSIO_WASM_RUNTIMES})
  set_tests_properties(api_unit_test_${RUNTIME} PROPERTIES COST 5000)
  set_tests_properties(wasm_config_part1_unit_test_${RUNTIME} PROPERTIES COST 5000)
  set_tests_properties(wasm_config_part4_unit_test_${RUNTIME} PROPERTIES COST 5000)
  set_tests_properties(wasm_config_part6_unit_test_${RUNTIME} PROPERTIES COST 3000)
  set_tests_properties(wasm_config_part7_unit_test_${RUNTIME} PROPERTIES COST 5000)
  set_tests_properties(wasm_unit_test_${RUNTIME} PROPERTIES COST 4000)
  set_tests_properties(delay_unit_test_${RUNTIME} PROPERTIES COST 3000)
endforeach()

### COVERAGE TESTING ###
if(ENABLE_COVERAGE_TESTING)
  set(Coverage_NAME ${PROJECT_NAME}_ut_coverage)
  # check for dependencies
  if(NOT LCOV_PATH)
    message(FATAL_ERROR "lcov not found! Aborting...")
  endif()
  if(NOT LLVMCOV_PATH)
    message(FATAL_ERROR "llvm-cov not found! Aborting...")
  endif()
  if(NOT GENHTML_PATH)
    message(FATAL_ERROR "genhtml not found! Aborting...")
  endif()
  # tests to skip during coverage testing
  set(ctest_exclude_tests '') # no spaces allowed within tests list
  # setup target
  add_custom_target(${Coverage_NAME}
    # cleanup lcov
    COMMAND ${LCOV_PATH} --directory . --zerocounters
    # run tests
    COMMAND ./tools/ctestwrapper.sh -R ${ctest_tests} -E ${ctest_exclude_tests}
    COMMAND ${LCOV_PATH} --directory . --capture --gcov-tool ${CMAKE_SOURCE_DIR}/tools/llvm-gcov.sh --output-file ${Coverage_NAME}.info
    COMMAND ${LCOV_PATH} -remove ${Coverage_NAME}.info '*/boost/*' '/usr/lib/*' '/usr/include/*' '*/externals/*' '*/libfc/*' '*/wasm-jit/*' --output-file ${Coverage_NAME}_filtered.info
    COMMAND ${GENHTML_PATH} -o ${Coverage_NAME} ${PROJECT_BINARY_DIR}/${Coverage_NAME}_filtered.info
    COMMAND if [ "$CI" != "true" ]\; then ${CMAKE_COMMAND} -E remove ${Coverage_NAME}.base ${Coverage_NAME}.info ${Coverage_NAME}_filtered.info ${Coverage_NAME}.total ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned ${PROJECT_BINARY_DIR}/${Coverage_NAME}_filtered.info.cleaned\; fi
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    COMMENT "Resetting code coverage counters to zero. Processing code coverage counters and generating report. Report published in ./${Coverage_NAME}"
  )
  # show info where to find the report
  add_custom_command(TARGET ${Coverage_NAME} POST_BUILD
    COMMAND ;
    COMMENT "Open ./${Coverage_NAME}/index.html in your browser to view the coverage report."
  )
endif()
