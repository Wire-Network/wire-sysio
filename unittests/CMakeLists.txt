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

# snapshot_attest_fixture.hpp includes the snapshot-attestation constants owned by the sysio.system contract.
target_include_directories(unit_test PRIVATE ${CMAKE_SOURCE_DIR}/contracts/sysio.system/include)
# Share the frozen crypto vectors with test_fc without source-relative includes.
target_include_directories(unit_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/libfc/test)

# No pkg-config cflags; keep flags minimal & portable
target_compile_options(unit_test PUBLIC -DDISABLE_SYSLIB_SERIALIZE)

if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
  target_compile_definitions(unit_test PRIVATE
    "NATIVE_CONTRACTS_DIR=\"${CMAKE_BINARY_DIR}/contracts\"")
endif()

# Link and include libsodium from vcpkg

### MARK TEST SUITES FOR EXECUTION ###
# Boost.Test arguments of every unit_test entry: a detailed report, and each test unit's start and end, messages,
# warnings and failures logged; passed checks are not.
set(UNIT_TEST_ARGS --report_level=detailed --log_level=test_suite --color_output)
# Suites that run no WASM, so that the runtime does not matter to them: one entry each, <suite>_unit_test.
string(CONCAT UNIT_TEST_NO_RUNTIME_SUITES
    "^(be_key_codec|blake3_encoder|block_header|block_header_state|block_log_extraction|block_timestamp|"
    "chainbase_fast_path|crypto_golden|ed25519|finality_core|finality_misc|int128|merkle_tree|name|"
    "protocol_feature_digest|resource_limits|softfloat_golden|softfloat_thread_local|sysvmoc_code_cache|"
    "sysvmoc_signal_handler|thread_stack_size|transaction_dedup|wasm_jit_serialization)_tests?$")
# Suites whose checks depend on wall-clock time: transaction deadlines and interrupts through the platform timer, and
# work timed against a budget. They run alone, so other tests' load does not move those times. The first list runs no
# WASM, like the suites above.
set(UNIT_TEST_NO_RUNTIME_TIMED_SUITES "^(authority_checker|platform_timer)_tests$")
set(UNIT_TEST_TIMED_SUITES
    "^(abi|bls_primitives|checktime|crypto_primitives|ed25519_intrinsic|receiver_cpu_billing|sysvmoc_interrupt)_tests$")
boost_test_discover_tests(unit_test
        NAME @NAME@_unit_test
        NAME_STRIP "s$" "_test$"
        INCLUDE "${UNIT_TEST_NO_RUNTIME_SUITES}"
        ARGS ${UNIT_TEST_ARGS})
# RUN_SERIAL: ctest starts these only when no other test is running, and starts no other test until they end.
boost_test_discover_tests(unit_test
        NAME @NAME@_unit_test
        NAME_STRIP "s$" "_test$"
        INCLUDE "${UNIT_TEST_NO_RUNTIME_TIMED_SUITES}"
        ARGS ${UNIT_TEST_ARGS}
        PROPERTIES RUN_SERIAL TRUE)
# One entry per suite per runtime, <suite without _tests>_unit_test_<runtime>, discovered from the built executable.
# To run unit_test with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose".
# COST starts the longest suites first, even on a fresh run before ctest has measured them.
boost_test_discover_tests(unit_test
        NAME @NAME@_unit_test_@RUNTIME@
        NAME_STRIP "s$" "_test$"
        RUNTIMES ${UNIT_TEST_RUNTIMES}
        EXCLUDE "${UNIT_TEST_NO_RUNTIME_SUITES}|${UNIT_TEST_NO_RUNTIME_TIMED_SUITES}|${UNIT_TEST_TIMED_SUITES}"
        ARGS ${UNIT_TEST_ARGS}
        COST_BY_NAME "^(api_part[1-3]|wasm_part[1-3]|wasm_config_part[1-5])$" 5000
             "^(snapshot_part[12]|finality)$" 4000
             "^delay$" 3000)
# The timed suites that run WASM, once per runtime, each alone.
boost_test_discover_tests(unit_test
        NAME @NAME@_unit_test_@RUNTIME@
        NAME_STRIP "s$" "_test$"
        RUNTIMES ${UNIT_TEST_RUNTIMES}
        INCLUDE "${UNIT_TEST_TIMED_SUITES}"
        ARGS ${UNIT_TEST_ARGS}
        PROPERTIES RUN_SERIAL TRUE)
if("sys-vm-oc" IN_LIST UNIT_TEST_RUNTIMES)
  # The wasm_part suites also run on sys-vm-oc without fsgsbase instructions.
  boost_test_discover_tests(unit_test
          NAME @NAME@_unit_test_@RUNTIME@-nofsgs
          NAME_STRIP "s$" "_test$"
          RUNTIMES sys-vm-oc
          INCLUDE "^wasm_part"
          ARGS ${UNIT_TEST_ARGS}
          PROPERTIES ENVIRONMENT SYSIO_DISABLE_FSGSBASE=1 COST 5000)
endif()
# The KV suites also run with the std::map KV engine under the chain. A validating_tester's validator stays on chainbase
# and compares block ids, so a difference between the engines fails there only if it changes a block. kv_store_tests
# holds both engines itself, compares their integrity hashes, and needs neither.
boost_test_discover_tests(unit_test
        NAME @NAME@_unit_test_@RUNTIME@-kv-map
        NAME_STRIP "s$" "_test$"
        RUNTIMES sys-vm
        INCLUDE "^(deep_mind_tests|kv_tests|kv_(api|comprehensive|intrinsic_probe|limits|token)_tests|payer_choice_test|ram_tests|snapshot_part[12]_tests|test_state_history)$"
        ARGS ${UNIT_TEST_ARGS}
        PROPERTIES ENVIRONMENT SYSIO_TEST_KV_ENGINE=map)
# This one test, not its cluster suite: it is what undoes several blocks through the controller on the map engine.
add_p_test(NAME fork_switch_undoes_kv_writes_unit_test_sys-vm-kv-map
           COMMAND unit_test --run_test=forked_tests_savanna/fork_switch_undoes_kv_writes
                   ${UNIT_TEST_ARGS} -- --sys-vm
           ENVIRONMENT SYSIO_TEST_KV_ENGINE=map)
# The tests the KV docs cite have to exist. The name carries _unit_test_ so that every run of the unit tests includes it.
add_test(NAME kv_doc_links_unit_test_check
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_kv_doc_test_links.py --unit-test $<TARGET_FILE:unit_test>
                 ${CMAKE_SOURCE_DIR}/docs/kv-behavior-spec.md ${CMAKE_SOURCE_DIR}/docs/kv-security-audit.md)
# Include every unit_test entry, including the runtime-independent suites.
set(ctest_tests "_unit_test(_|$)")

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
  # setup target; VERBATIM hands each argument to its command unchanged, the test regex's `$` included
  add_custom_target(${Coverage_NAME}
    # cleanup lcov
    COMMAND ${LCOV_PATH} --directory . --zerocounters
    # run tests
    COMMAND ./tools/ctestwrapper.sh -R ${ctest_tests} -LE wasm_spec_tests -j8
    COMMAND ${LCOV_PATH} --directory . --capture --gcov-tool ${CMAKE_SOURCE_DIR}/tools/llvm-gcov.sh
            --output-file ${Coverage_NAME}.info
    COMMAND ${LCOV_PATH} -remove ${Coverage_NAME}.info */boost/* /usr/lib/* /usr/include/* */externals/* */libfc/*
            */wasm-jit/* --output-file ${Coverage_NAME}_filtered.info
    COMMAND ${GENHTML_PATH} -o ${Coverage_NAME} ${PROJECT_BINARY_DIR}/${Coverage_NAME}_filtered.info
    # outside CI, remove the intermediate files: sh runs `$0 -E remove $@` with the cmake and file arguments after it
    COMMAND sh -c "if [ \"$CI\" != true ]; then \"$0\" -E remove \"$@\"; fi" ${CMAKE_COMMAND}
            ${Coverage_NAME}.base ${Coverage_NAME}.info ${Coverage_NAME}_filtered.info ${Coverage_NAME}.total
            ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned
            ${PROJECT_BINARY_DIR}/${Coverage_NAME}_filtered.info.cleaned
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    COMMENT "Resetting code coverage counters to zero. Processing code coverage counters and generating report. Report published in ./${Coverage_NAME}"
    VERBATIM
  )
  # show info where to find the report
  add_custom_command(TARGET ${Coverage_NAME} POST_BUILD
    COMMAND ;
    COMMENT "Open ./${Coverage_NAME}/index.html in your browser to view the coverage report."
  )
endif()
