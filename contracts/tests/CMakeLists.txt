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

# One CTest entry per top-level suite, contract.<suite>, discovered from the built executable. The entries share it
# and can run concurrently under ctest -j.
boost_test_discover_tests(${CONTRACT_UNIT_TEST_EXE}
        NAME contract.@UNIT@
        ARGS --report_level=detailed
        PROPERTIES LABELS contract TIMEOUT ${CONTRACT_UNIT_TEST_TIMEOUT_SECONDS})
