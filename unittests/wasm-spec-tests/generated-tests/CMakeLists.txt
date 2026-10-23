### Build contracts with cdt if available ###
include(ExternalProject)

set(SPEC_TEST_WASM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/wasms")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/wasm_spec_tests.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/wasm_spec_tests.hpp ESCAPE_QUOTES)

### BUILD UNIT TEST EXECUTABLE ###
file(GLOB WASM_TESTS CONFIGURE_DEPENDS "*.cpp") # find all unit test suites
add_executable( wasm_spec_test ${WASM_TESTS}) # build unit tests as one executable

target_link_libraries( wasm_spec_test sysio_chain_wrap chainbase sysio_testing fc appbase ${PLATFORM_SPECIFIC_LIBS} )

target_compile_options(wasm_spec_test PUBLIC -DDISABLE_SYSLIB_SERIALIZE)
target_include_directories( wasm_spec_test PUBLIC
                            ${CMAKE_SOURCE_DIR}/libraries/testing/include
                            ${CMAKE_CURRENT_BINARY_DIR}/include )

### MARK TEST SUITES FOR EXECUTION ###
# One entry per generated test case per runtime, discovered from the built executable. COST starts the longest cases
# first, even on a fresh run before ctest has measured them.
boost_test_discover_tests(wasm_spec_test
        NAME @UNIT@_unit_test_@RUNTIME@
        RUNTIMES ${UNIT_TEST_RUNTIMES}
        ARGS --report_level=detailed --color_output
        PROPERTIES LABELS wasm_spec_tests
        COST_BY_NAME "^memory_trap_1_check_throw_[12]$" 4000
             "^(conversions|unreachable)_0_check_throw$|^(f32|f64|f32_cmp|f64_cmp)_0_pass$" 2500)
