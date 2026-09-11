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

# add_sysio_test_executable(${CONTRACT_UNIT_TEST_EXE} ${UNIT_TESTS}) # build unit tests as one executable
# The snapshot-attestation suite runs as its OWN ctest entry instead of inside the aggregate one.
# Reaching an attestable height means building the production 25,000-block cadence, which costs
# more than the other ~685 cases put together -- inline, it made this single entry the critical
# path of the whole parallel lane and pushed it against its timeout under CI contention. The two
# filters are exact complements, so every case still runs in the same gate; the pair now finishes
# in the time the slower half takes rather than the sum.
set(CONTRACT_SNAPSHOT_ATTEST_SUITE sysio_snapshot_attest_tests)
set(CONTRACT_SNAPSHOT_ATTEST_TEST contracts_snapshot_attest_test)

add_test(NAME ${CONTRACT_UNIT_TEST_EXE}
        COMMAND ${CONTRACT_UNIT_TEST_EXE} --run_test=!${CONTRACT_SNAPSHOT_ATTEST_SUITE}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME ${CONTRACT_SNAPSHOT_ATTEST_TEST}
        COMMAND ${CONTRACT_UNIT_TEST_EXE} --run_test=${CONTRACT_SNAPSHOT_ATTEST_SUITE}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Both halves keep the full budget: they are the same binary, and a sanitizer configuration slows
# either one well past the parallel lane's default 1000 s.
set_tests_properties(${CONTRACT_UNIT_TEST_EXE} ${CONTRACT_SNAPSHOT_ATTEST_TEST}
        PROPERTIES TIMEOUT ${CONTRACT_UNIT_TEST_TIMEOUT_SECONDS})
