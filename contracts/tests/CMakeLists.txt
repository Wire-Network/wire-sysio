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
add_test(NAME ${CONTRACT_UNIT_TEST_EXE} COMMAND ${CONTRACT_UNIT_TEST_EXE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
