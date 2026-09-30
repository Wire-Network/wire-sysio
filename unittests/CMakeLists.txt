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

# Runtimes the unit_test and wasm_spec_test entries run on. Native-module tests require .so files built by
# add_native_contract (needs CDT).
set(UNIT_TEST_RUNTIMES ${SYSIO_WASM_RUNTIMES})
if(NOT BUILD_SYSTEM_CONTRACTS)
  list(REMOVE_ITEM UNIT_TEST_RUNTIMES native-module)
endif()

if(NOT DISABLE_WASM_SPEC_TESTS)
  add_subdirectory(wasm-spec-tests/generated-tests)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_contracts.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/test_contracts.hpp ESCAPE_QUOTES)

add_subdirectory(snapshots)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/snapshots.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/snapshots.hpp ESCAPE_QUOTES)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/deep-mind/deep-mind.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/deep-mind.hpp ESCAPE_QUOTES)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test-data/test-data.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/test-data.hpp ESCAPE_QUOTES)

add_custom_command(
  OUTPUT protocol_feature_digest_tests.cpp
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/gen_protocol_feature_digest_tests.py ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol_feature_manager.cpp > protocol_feature_digest_tests.cpp
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol_feature_manager.cpp ${CMAKE_CURRENT_SOURCE_DIR}/gen_protocol_feature_digest_tests.py
)

# === Use vcpkg libsodium ===
message(STATUS "Using libsodium from vcpkg")

### BUILD UNIT TEST EXECUTABLE ###
file(GLOB UNIT_TESTS CONFIGURE_DEPENDS "*.cpp") # find all unit test suites
unittest_target(
  unit_test
  SOURCE_FILES
  ${UNIT_TESTS}
  protocol_feature_digest_tests.cpp
) # build unit tests as one executable

# No pkg-config cflags; keep flags minimal & portable
target_compile_options(unit_test PUBLIC -DDISABLE_SYSLIB_SERIALIZE)

if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
  target_compile_definitions(unit_test PRIVATE
    "NATIVE_CONTRACTS_DIR=\"${CMAKE_BINARY_DIR}/contracts\"")
endif()

# Link and include libsodium from vcpkg

### MARK TEST SUITES FOR EXECUTION ###
add_p_test(NAME protocol_feature_digest_unit_test COMMAND unit_test --run_test=protocol_feature_digest_tests --report_level=detailed --log_level=all --color_output)
# One entry per suite per runtime, <suite without _tests>_unit_test_<runtime>, discovered from the built executable.
# To run unit_test with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose".
# COST starts the longest suites first, even on a fresh run before ctest has measured them.
boost_test_discover_tests(unit_test
        NAME @NAME@_unit_test_@RUNTIME@
        NAME_STRIP "s$" "_test$"
        RUNTIMES ${UNIT_TEST_RUNTIMES}
        EXCLUDE "^protocol_feature_digest_tests$" # registered above: it does not depend on the runtime
        ARGS --report_level=detailed --log_level=all --color_output
        COST_BY_NAME "^(api_part[1-3]|checktime|wasm_part[1-3]|wasm_config_part[1-5])$" 5000
             "^(snapshot_part[12]|finality)$" 4000
             "^delay$" 3000)
if("sys-vm-oc" IN_LIST UNIT_TEST_RUNTIMES)
  # The wasm_part suites also run on sys-vm-oc without fsgsbase instructions.
  boost_test_discover_tests(unit_test
          NAME @NAME@_unit_test_@RUNTIME@-nofsgs
          NAME_STRIP "s$" "_test$"
          RUNTIMES sys-vm-oc
          INCLUDE "^wasm_part"
          ARGS --report_level=detailed --log_level=all --color_output
          PROPERTIES ENVIRONMENT SYSIO_DISABLE_FSGSBASE=1 COST 5000)
endif()
set(ctest_tests "'_unit_test_|^protocol_feature_digest_unit_test' -LE wasm_spec_tests -j8") # every unit_test entry

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
