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

# Each owning source links with the shared Boost.Test entry point into its own
# executable. CTest may then schedule suites from different binaries together.
set(CONTRACT_UNIT_TEST_TIMEOUT_SECONDS 2700)

function(contract_binary_for_source SOURCE_FILE OUTPUT_TARGET)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
    message(FATAL_ERROR "Contract suite source is missing: ${SOURCE_FILE}")
  endif()
  get_filename_component(_binary_id "${SOURCE_FILE}" NAME)
  string(REGEX REPLACE "\\.cpp$" "" _binary_id "${_binary_id}")
  string(REPLACE "." "_" _binary_id "${_binary_id}")
  set(_binary_target "contract_${_binary_id}")
  get_property(_binary_source GLOBAL PROPERTY "CONTRACT_BINARY_SOURCE_${_binary_target}")
  if(_binary_source AND NOT _binary_source STREQUAL SOURCE_FILE)
    message(FATAL_ERROR "Two contract sources map to ${_binary_target}: ${_binary_source} and ${SOURCE_FILE}")
  endif()
  if(NOT TARGET ${_binary_target})
    set_property(GLOBAL PROPERTY "CONTRACT_BINARY_SOURCE_${_binary_target}" "${SOURCE_FILE}")
    set(_binary_sources
      ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}
      ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    )
    unittest_target(${_binary_target} SOURCE_FILES ${_binary_sources})
    target_link_libraries(${_binary_target} PRIVATE fc-lite opp)
    target_include_directories(${_binary_target} PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/opp/test/include)
    # Target builds provide generated WASMs; CTest still does not build or sync.
    if(BUILD_SYSTEM_CONTRACTS)
      if(NOT TARGET contracts_project)
        message(FATAL_ERROR "ON/ON contract project is missing")
      endif()
      add_dependencies(${_binary_target} contracts_project)
    endif()
    if(BUILD_TEST_CONTRACTS)
      if(NOT TARGET test_contracts_project)
        message(FATAL_ERROR "ON/ON test fixture project is missing")
      endif()
      add_dependencies(${_binary_target} test_contracts_project)
    endif()
  endif()
  set(${OUTPUT_TARGET} "${_binary_target}" PARENT_SCOPE)
endfunction()

function(add_contract_suite SUITE_NAME SOURCE_FILE)
  cmake_parse_arguments(SUITE "" "" "ACCOUNTS" ${ARGN})
  if(SUITE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unexpected arguments for contract suite ${SUITE_NAME}: ${SUITE_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
    message(FATAL_ERROR "Contract suite source is missing: ${SOURCE_FILE}")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}" _suite_source_text)
  string(FIND "${_suite_source_text}" "BOOST_AUTO_TEST_SUITE(${SUITE_NAME})" _suite_declaration)
  if(_suite_declaration EQUAL -1)
    message(FATAL_ERROR "Contract suite ${SUITE_NAME} is not declared in ${SOURCE_FILE}")
  endif()
  get_property(_registered_suites GLOBAL PROPERTY CONTRACT_REGISTERED_SUITES)
  if(SUITE_NAME IN_LIST _registered_suites)
    message(FATAL_ERROR "Contract suite ${SUITE_NAME} is registered twice")
  endif()
  set_property(GLOBAL APPEND PROPERTY CONTRACT_REGISTERED_SUITES ${SUITE_NAME})
  contract_binary_for_source(${SOURCE_FILE} _suite_binary)
  get_property(_binary_targets GLOBAL PROPERTY CONTRACT_BINARY_TARGETS)
  if(NOT _suite_binary IN_LIST _binary_targets)
    set_property(GLOBAL APPEND PROPERTY CONTRACT_BINARY_TARGETS ${_suite_binary})
  endif()
  set(_ctest_name "contract.${SUITE_NAME}")
  add_test(
    NAME ${_ctest_name}
    COMMAND ${_suite_binary}
            --run_test=${SUITE_NAME}
            --report_level=detailed
            -- --sys-vm
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(${_ctest_name} PROPERTIES
    LABELS "contract;${SUITE_ACCOUNTS}"
    TIMEOUT ${CONTRACT_UNIT_TEST_TIMEOUT_SECONDS}
  )
endfunction()

# Each owning source builds a separate executable. Every runnable top-level
# suite is visible here; multiple suites may intentionally share one source.
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
get_property(_contract_binary_targets GLOBAL PROPERTY CONTRACT_BINARY_TARGETS)
add_custom_target(contract_suite_binaries DEPENDS ${_contract_binary_targets})

# Fail at configure time when a source introduces a suite without declaring it.
set(_source_suites)
file(GLOB _contract_suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
foreach(_source IN LISTS _contract_suite_sources)
  file(READ "${_source}" _source_text)
  string(REGEX MATCHALL "BOOST_AUTO_TEST_SUITE\\([A-Za-z_][A-Za-z_0-9]*\\)" _suite_macros "${_source_text}")
  foreach(_macro IN LISTS _suite_macros)
    string(REGEX REPLACE "^BOOST_AUTO_TEST_SUITE\\(([A-Za-z_][A-Za-z_0-9]*)\\)$" "\\1" _declared_suite "${_macro}")
    list(APPEND _source_suites ${_declared_suite})
  endforeach()
endforeach()
list(SORT _source_suites)
list(SORT _registered_suites)
if(NOT "${_source_suites}" STREQUAL "${_registered_suites}")
  message(FATAL_ERROR "Direct contract suite registrations do not match source declarations")
endif()
