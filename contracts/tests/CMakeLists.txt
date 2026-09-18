#list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/libraries/testing")
#list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/lib/cmake")

# find_package(sysio REQUIRED)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/contracts.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/contracts.hpp)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_SOURCE_DIR}/contracts/sysio.system/include)
include_directories(${CMAKE_SOURCE_DIR}/contracts/sysio.opp.common/include)
include_directories(${CMAKE_SOURCE_DIR}/contracts/sysio.councl/include)
# UNIT TESTING ###
include(CTest) # eliminates DartConfiguration.tcl errors at test runtime
enable_testing()

# build unit test executable
file(GLOB UNIT_TESTS CONFIGURE_DEPENDS "*.cpp" "*.hpp") # find all unit test suites
set(CONTRACT_UNIT_TEST_EXE contracts_unit_test)
set(CONTRACT_UNIT_TEST_TIMEOUT_SECONDS 2700)
unittest_target(
        ${CONTRACT_UNIT_TEST_EXE}
        SOURCE_FILES
        ${UNIT_TESTS}
) # build unit tests as one executable

target_link_libraries(${CONTRACT_UNIT_TEST_EXE}
        PRIVATE
        fc-lite
        opp
)
target_include_directories(${CONTRACT_UNIT_TEST_EXE} PRIVATE
        ${CMAKE_SOURCE_DIR}/libraries/opp/test/include)

# Register every top-level Boost suite as a separate CTest process. The
# processes share this executable but remain independently selectable and may
# run concurrently under ctest -j.
set(CONTRACT_SUITE_MACRO_PATTERN
  "BOOST_(AUTO|FIXTURE|DATA)_TEST_SUITE[ \t\r\n]*\\([ \t\r\n]*([A-Za-z_][A-Za-z_0-9]*)[ \t\r\n]*[,)]")
function(add_contract_suite SUITE_NAME SOURCE_FILE)
  cmake_parse_arguments(SUITE "" "" "ACCOUNTS" ${ARGN})
  if(SUITE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unexpected arguments for contract suite ${SUITE_NAME}: ${SUITE_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
    message(FATAL_ERROR "Contract suite source is missing: ${SOURCE_FILE}")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}" _suite_source_text)
  string(REGEX MATCH
    "BOOST_(AUTO|FIXTURE|DATA)_TEST_SUITE[ \t\r\n]*\\([ \t\r\n]*${SUITE_NAME}[ \t\r\n]*[,)]"
    _suite_declaration
    "${_suite_source_text}")
  if(NOT _suite_declaration)
    message(FATAL_ERROR "Contract suite ${SUITE_NAME} is not declared in ${SOURCE_FILE}")
  endif()
  get_property(_registered_suites GLOBAL PROPERTY CONTRACT_REGISTERED_SUITES)
  if(SUITE_NAME IN_LIST _registered_suites)
    message(FATAL_ERROR "Contract suite ${SUITE_NAME} is registered twice")
  endif()
  set_property(GLOBAL APPEND PROPERTY CONTRACT_REGISTERED_SUITES ${SUITE_NAME})
  set(_ctest_name "contract.${SUITE_NAME}")
  add_test(
    NAME ${_ctest_name}
    COMMAND ${CONTRACT_UNIT_TEST_EXE}
            --run_test=${SUITE_NAME}
            --report_level=detailed
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(${_ctest_name} PROPERTIES
    LABELS "contract;${SUITE_ACCOUNTS}"
    TIMEOUT ${CONTRACT_UNIT_TEST_TIMEOUT_SECONDS}
  )
endfunction()

# Every runnable top-level suite is visible here with its owning source.
add_contract_suite(amm_math_tests amm_math_tests.cpp ACCOUNTS sysio.opp.common)
add_contract_suite(council_math_tests council_math_tests.cpp ACCOUNTS sysio.councl)
add_contract_suite(sysio_emissions_tests emissions_tests.cpp ACCOUNTS
  sysio.dclaim sysio.epoch sysio.msig sysio.opreg
  sysio.reserv sysio.system sysio.token sysio.uwrit)
add_contract_suite(t5_emissions_tests emissions_tests.cpp ACCOUNTS
  sysio.dclaim sysio.epoch sysio.msig sysio.opreg
  sysio.reserv sysio.system sysio.token sysio.uwrit)
add_contract_suite(sysio_producer_eligibility_tests emissions_tests.cpp ACCOUNTS
  sysio.dclaim sysio.epoch sysio.msig sysio.opreg
  sysio.reserv sysio.system sysio.token sysio.uwrit)
add_contract_suite(sysio_producer_score_tests emissions_tests.cpp ACCOUNTS
  sysio.dclaim sysio.epoch sysio.msig sysio.opreg
  sysio.reserv sysio.system sysio.token sysio.uwrit)
add_contract_suite(getpeerkeys_tests getpeerkeys_tests.cpp ACCOUNTS
  sysio.msig sysio.system sysio.token)
add_contract_suite(safe_ops_tests safe_ops_tests.cpp ACCOUNTS sysio.opp.common)
add_contract_suite(sysio_authex_tests sysio.authex_tests.cpp ACCOUNTS sysio.authex)
add_contract_suite(sysio_bios_if_tests sysio.bios_inst_fin_tests.cpp ACCOUNTS sysio.bios)
add_contract_suite(sysio_chains_tests sysio.chains_tests.cpp ACCOUNTS sysio.chains)
add_contract_suite(sysio_councl_tests sysio.councl_tests.cpp ACCOUNTS
  sysio.councl sysio.msig sysio.system sysio.token)
add_contract_suite(sysio_dclaim_tests sysio.dclaim_tests.cpp ACCOUNTS sysio.dclaim)
add_contract_suite(sysio_dispatch_tests sysio.dispatch_tests.cpp ACCOUNTS
  sysio.authex sysio.chains sysio.chalg sysio.epoch
  sysio.msgch sysio.opreg sysio.opp.common sysio.reserv
  sysio.system sysio.token sysio.uwrit)
add_contract_suite(sysio_dispute_tests sysio.dispute_tests.cpp ACCOUNTS
  sysio.chains sysio.chalg sysio.epoch sysio.msgch
  sysio.msig sysio.opreg sysio.roa sysio.system
  sysio.token sysio.uwrit)
add_contract_suite(sysio_epoch_flushwtdw_tests sysio.epoch_flushwtdw_tests.cpp ACCOUNTS
  sysio.chains sysio.epoch sysio.msgch sysio.opreg
  sysio.system sysio.token sysio.uwrit)
add_contract_suite(sysio_epoch_tests sysio.epoch_tests.cpp ACCOUNTS sysio.chains sysio.epoch)
add_contract_suite(sysio_system_finalizer_key_tests sysio.finalizer_key_tests.cpp ACCOUNTS
  sysio.msig sysio.opreg sysio.system sysio.token)
add_contract_suite(sysio_inline_auth_tests sysio.inline_auth_tests.cpp ACCOUNTS sysio.system test.sendinline)
add_contract_suite(sysio_system_limitauth_tests sysio.limitauth_tests.cpp ACCOUNTS
  sysio.msig sysio.system sysio.token)
add_contract_suite(sysio_msgch_chain_tests sysio.msgch_chain_tests.cpp ACCOUNTS
  sysio.chains sysio.chalg sysio.epoch sysio.msgch
  sysio.opreg sysio.roa sysio.system sysio.token
  sysio.uwrit)
add_contract_suite(sysio_msgch_tests sysio.msgch_tests.cpp ACCOUNTS
  sysio.chains sysio.epoch sysio.msgch)
add_contract_suite(sysio_msgch_envlog_tests sysio.msgch_tests.cpp ACCOUNTS
  sysio.chains sysio.epoch sysio.msgch)
add_contract_suite(sysio_msig_tests sysio.msig_tests.cpp ACCOUNTS
  sysio.msig sysio.system sysio.token test.sendinline)
add_contract_suite(sysio_opreg_tests sysio.opreg_tests.cpp ACCOUNTS
  sysio.epoch sysio.opreg sysio.token)
add_contract_suite(sysio_reserve_tests sysio.reserv_tests.cpp ACCOUNTS
  sysio.authex sysio.chains sysio.msgch sysio.opp.common
  sysio.reserv sysio.token)
add_contract_suite(sysio_roa_tests sysio.roa_tests.cpp ACCOUNTS
  sysio.authex sysio.msig sysio.roa sysio.system
  sysio.token test.noop test.sendinline)
add_contract_suite(sysio_snapshot_attest_tests sysio.snapshot_attest_tests.cpp ACCOUNTS
  sysio.msig sysio.system sysio.token)
add_contract_suite(sysio_system_blockinfo_tests sysio.system_blockinfo_tests.cpp ACCOUNTS
  sysio.msig sysio.system sysio.token test.blockinfo_tester)
add_contract_suite(sysio_system_tests sysio.system_tests.cpp ACCOUNTS
  sysio.bios sysio.msig sysio.system sysio.token)
add_contract_suite(sysio_token_tests sysio.token_tests.cpp ACCOUNTS
  sysio.msig sysio.system sysio.token)
add_contract_suite(sysio_tokens_tests sysio.tokens_tests.cpp ACCOUNTS sysio.tokens)
add_contract_suite(sysio_uwrit_tests sysio.uwrit_tests.cpp ACCOUNTS sysio.uwrit)
add_contract_suite(sysio_wrap_tests sysio.wrap_tests.cpp ACCOUNTS sysio.msig sysio.wrap)
get_property(_registered_suites GLOBAL PROPERTY CONTRACT_REGISTERED_SUITES)

# Fail at configure time when a source introduces a suite without registering it.
# Contract tests must live in a named suite: file-scope test cases cannot be
# discovered by this inventory and therefore cannot be registered separately.
set(_source_suites)
file(GLOB _contract_suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
foreach(_source IN LISTS _contract_suite_sources)
  file(READ "${_source}" _source_text)
  string(REGEX MATCHALL "${CONTRACT_SUITE_MACRO_PATTERN}" _suite_macros "${_source_text}")
  foreach(_macro IN LISTS _suite_macros)
    string(REGEX REPLACE "^${CONTRACT_SUITE_MACRO_PATTERN}$" "\\2" _declared_suite "${_macro}")
    list(APPEND _source_suites ${_declared_suite})
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _source_suites)
list(SORT _source_suites)
list(SORT _registered_suites)
set(_missing_suites ${_source_suites})
list(REMOVE_ITEM _missing_suites ${_registered_suites})
set(_extra_suites ${_registered_suites})
list(REMOVE_ITEM _extra_suites ${_source_suites})
if(_missing_suites OR _extra_suites)
  message(FATAL_ERROR
    "Contract suites declared but not registered: ${_missing_suites}; "
    "registered but not declared: ${_extra_suites}")
endif()
