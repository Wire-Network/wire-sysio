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
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME contract_type_generator_test
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_SOURCE_DIR}/contracts/tools/tests
                -p test_generate_sysio_contract_types.py)
add_test(NAME contract_suite_discovery_test
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_SOURCE_DIR}/contracts/tools/tests
                -p test_discover_contract_suites.py)

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

# Register every top-level Boost unit as its own CTest entry, contract.<unit>, so each can be selected alone and all
# can run concurrently under ctest -j while sharing this executable. The linked executable's own listing is the
# inventory, so a new suite needs no edit here. Each entry is labelled with the contracts its sources use plus the
# ones libtester deploys during default setup.
set(CONTRACT_SUITE_TEST_PREFIX contract.)
set(CONTRACT_SUITE_LABEL contract)
set(CONTRACT_SUITE_DISCOVERY_SCRIPT ${CMAKE_SOURCE_DIR}/contracts/tools/discover_contract_suites.py)
set(CONTRACT_SUITE_TESTS_FILE ${CMAKE_CURRENT_BINARY_DIR}/${CONTRACT_UNIT_TEST_EXE}_suites.cmake)
set(CONTRACT_SUITE_INCLUDE_FILE ${CMAKE_CURRENT_BINARY_DIR}/${CONTRACT_UNIT_TEST_EXE}_include.cmake)
add_custom_command(TARGET ${CONTRACT_UNIT_TEST_EXE} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CONTRACT_SUITE_DISCOVERY_SCRIPT}
                --executable $<TARGET_FILE:${CONTRACT_UNIT_TEST_EXE}>
                --working-directory ${CMAKE_BINARY_DIR}
                --test-sources ${UNIT_TESTS}
                --accessor-header ${CMAKE_CURRENT_SOURCE_DIR}/contracts.hpp.in
                --accessor-header ${CMAKE_SOURCE_DIR}/unittests/test_contracts.hpp.in
                --implicit-accessor-header ${CMAKE_SOURCE_DIR}/libraries/testing/contracts.cpp.in
                --timeout ${CONTRACT_UNIT_TEST_TIMEOUT_SECONDS}
                --test-argument=--report_level=detailed
                --test-prefix ${CONTRACT_SUITE_TEST_PREFIX}
                --base-label ${CONTRACT_SUITE_LABEL}
                --output ${CONTRACT_SUITE_TESTS_FILE}
        BYPRODUCTS ${CONTRACT_SUITE_TESTS_FILE}
        COMMENT "Registering ${CONTRACT_UNIT_TEST_EXE} suites with CTest"
        VERBATIM)
# Relink, and so rediscover, when the discovery rules change and not only when a test source does.
set_property(TARGET ${CONTRACT_UNIT_TEST_EXE} APPEND PROPERTY LINK_DEPENDS ${CONTRACT_SUITE_DISCOVERY_SCRIPT})
# Until the executable is built, a placeholder entry fails instead of the contract suites silently not running.
file(WRITE ${CONTRACT_SUITE_INCLUDE_FILE}
        "if(EXISTS \"${CONTRACT_SUITE_TESTS_FILE}\")\n"
        "  include(\"${CONTRACT_SUITE_TESTS_FILE}\")\n"
        "else()\n"
        "  add_test(${CONTRACT_SUITE_TEST_PREFIX}NOT_BUILT ${CONTRACT_UNIT_TEST_EXE}_NOT_BUILT)\n"
        "  set_tests_properties(${CONTRACT_SUITE_TEST_PREFIX}NOT_BUILT PROPERTIES LABELS ${CONTRACT_SUITE_LABEL})\n"
        "endif()\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${CONTRACT_SUITE_INCLUDE_FILE})
