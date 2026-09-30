# Tests boost-test-discovery-impl.cmake against a stand-in test executable. Run with `cmake -P` from a scratch
# directory; ctest runs it from the build directory.
cmake_minimum_required(VERSION 3.19)

include("${CMAKE_CURRENT_LIST_DIR}/../boost-test-discovery-impl.cmake")

set(work "${CMAKE_CURRENT_BINARY_DIR}/boost-test-discovery-test")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# add_test and set_tests_properties exist only while ctest reads its test files, so record the calls instead.
function(add_test name)
   list(JOIN ARGN " " command)
   set_property(GLOBAL APPEND PROPERTY recorded_tests "${name}")
   set_property(GLOBAL PROPERTY "command:${name}" "${command}")
endfunction()
function(set_tests_properties name)
   list(JOIN ARGN " " properties)
   set_property(GLOBAL APPEND_STRING PROPERTY "properties:${name}" " ${properties}")
endfunction()

function(expect_equal actual expected what)
   if(NOT actual STREQUAL expected)
      message(FATAL_ERROR "${what}:\n  expected [${expected}]\n  actual   [${actual}]")
   endif()
endfunction()

function(expect_contains text part what)
   string(FIND "${text}" "${part}" found)
   if(found EQUAL -1)
      message(FATAL_ERROR "${what}: [${part}] not in [${text}]")
   endif()
endfunction()

# A stand-in for a Boost.Test executable: it writes a canned --list_content listing to the report sink and prints
# noise on its own streams, as real executables may.
function(make_executable name listing exit_code)
   file(WRITE "${work}/${name}.listing" "${listing}")
   file(WRITE "${work}/${name}.sh"
      "#!/bin/sh\n"
      "for argument in \"$@\"; do\n"
      "   case \"$argument\" in --report_sink=*) sink=\"\${argument#--report_sink=}\";; esac\n"
      "done\n"
      "echo 'Random number generator seeded' ; echo 'stderr noise' >&2\n"
      "cat '${work}/${name}.listing' > \"$sink\"\n"
      "exit ${exit_code}\n")
   file(CHMOD "${work}/${name}.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endfunction()

# Discover with the given options, then load the generated script so its registrations are recorded.
function(discover name)
   set_property(GLOBAL PROPERTY recorded_tests "")
   boost_test_discover_tests_impl(EXECUTABLE "${work}/${name}.sh" TESTS_FILE "${work}/${name}_tests.cmake"
                                  FAILURE_NAME ${name}_DISCOVERY_FAILED CMAKE_COMMAND "${CMAKE_COMMAND}"
                                  WORKING_DIRECTORY "${work}" ${ARGN})
   if(EXISTS "${work}/${name}_tests.cmake")
      include("${work}/${name}_tests.cmake")
   endif()
endfunction()

# Built from escaped strings so the trailing space that marks a default-disabled unit survives editors.
string(CONCAT listing
   "alpha_tests*\n"
   "    alpha_case*\n"
   "beta_tests \n"
   "    nested_suite*\n"
   "        deep_case*\n"
   "described_tests*: covers a;b and [the rest\n"
   "quiet_one : disabled, with a description\n"
   "tmpl_case<unsigned long>*\n"
   "bare_tests\n"
   "loose_case*\n")

# Every top-level unit, each runtime, filters, name stripping, COST, default-disabled units and quoting.
make_executable(full "${listing}" 0)
# Lists arrive joined into single arguments, as the generated include script passes them.
discover(full NAME "@NAME@_unit_test_@RUNTIME@" NAME_STRIP "s$;_test$" RUNTIMES "r1;r2" EXCLUDE "^loose"
         ARGS "--report_level=detailed" PROPERTIES "LABELS;lane" COST_BY_NAME "^alpha$;500")
get_property(tests GLOBAL PROPERTY recorded_tests)
list(SORT tests)
set(expected alpha_unit_test_r1 alpha_unit_test_r2 bare_unit_test_r1 bare_unit_test_r2 beta_unit_test_r1
    beta_unit_test_r2 described_unit_test_r1 described_unit_test_r2 quiet_one_unit_test_r1 quiet_one_unit_test_r2
    "tmpl_case<unsigned long>_unit_test_r1" "tmpl_case<unsigned long>_unit_test_r2")
expect_equal("${tests}" "${expected}" "registered tests")
get_property(command GLOBAL PROPERTY "command:alpha_unit_test_r2")
expect_equal("${command}" "${work}/full.sh --run_test=alpha_tests --report_level=detailed -- --r2" "command")
get_property(properties GLOBAL PROPERTY "properties:alpha_unit_test_r1")
expect_equal("${properties}" " PROPERTIES WORKING_DIRECTORY ${work} LABELS lane COST 500" "enabled unit properties")
get_property(properties GLOBAL PROPERTY "properties:quiet_one_unit_test_r1")
expect_contains("${properties}" "DISABLED TRUE" "disabled unit with a description")
get_property(properties GLOBAL PROPERTY "properties:beta_unit_test_r2")
expect_contains("${properties}" "DISABLED TRUE" "disabled unit")
get_property(properties GLOBAL PROPERTY "properties:described_unit_test_r1")
string(FIND "${properties}" "DISABLED" found)
expect_equal("${found}" "-1" "enabled unit with a description")

# Without RUNTIMES each unit gets one entry and no runtime argument; INCLUDE narrows the units.
discover(full NAME "contract.@UNIT@" INCLUDE "^alpha")
get_property(tests GLOBAL PROPERTY recorded_tests)
expect_equal("${tests}" "contract.alpha_tests" "single-entry registration")
get_property(command GLOBAL PROPERTY "command:contract.alpha_tests")
expect_equal("${command}" "${work}/full.sh --run_test=alpha_tests" "single-entry command")

# A filter that matches none of the listed units registers nothing and is not a failure.
discover(full NAME "@UNIT@" INCLUDE "^nothing_matches")
get_property(tests GLOBAL PROPERTY recorded_tests)
expect_equal("${tests}" "" "empty filter result")

# An executable that cannot list its units registers a single failing entry and leaves no script behind, so the
# next ctest run discovers again.
make_executable(broken "${listing}" 3)
discover(broken NAME "@UNIT@" PROPERTIES LABELS lane)
get_property(tests GLOBAL PROPERTY recorded_tests)
expect_equal("${tests}" "broken_DISCOVERY_FAILED" "failing listing")
get_property(command GLOBAL PROPERTY "command:broken_DISCOVERY_FAILED")
expect_contains("${command}" "(exit 3)" "failure message")
get_property(properties GLOBAL PROPERTY "properties:broken_DISCOVERY_FAILED")
expect_equal("${properties}" " PROPERTIES WILL_FAIL TRUE LABELS lane" "failure entry properties")
if(EXISTS "${work}/broken_tests.cmake")
   message(FATAL_ERROR "a failed discovery left a tests script behind")
endif()

# Units whose names coincide after NAME_STRIP are reported by a failing entry.
make_executable(dups "dup_tests*\ndup_test*\n" 0)
discover(dups NAME "@NAME@" NAME_STRIP "s$;_test$")
get_property(tests GLOBAL PROPERTY recorded_tests)
expect_equal("${tests}" "dup;dup;dups_DISCOVERY_FAILED" "colliding names")

# Without the lock, discovery leaves the shared files alone, keeps the current list and reports why. Another process
# holds the lock, since CMake crashes when one process locks a file twice, while this one tries once.
make_executable(lockfail "${listing}" 0)
file(WRITE "${work}/lockfail_tests.cmake" "# the current list\n")
file(WRITE "${work}/hold.cmake"
   "file(LOCK [==[${work}/lockfail_tests.cmake.lock]==])\n"
   "file(TOUCH [==[${work}/held]==])\n"
   "foreach(i RANGE 100)\n"
   "   if(EXISTS [==[${work}/tried]==])\n      break()\n   endif()\n"
   "   execute_process(COMMAND [==[${CMAKE_COMMAND}]==] -E sleep 0.1)\n"
   "endforeach()\n")
file(WRITE "${work}/try.cmake"
   "foreach(i RANGE 100)\n"
   "   if(EXISTS [==[${work}/held]==])\n      break()\n   endif()\n"
   "   execute_process(COMMAND [==[${CMAKE_COMMAND}]==] -E sleep 0.1)\n"
   "endforeach()\n"
   "function(add_test name)\n   message(\"registered \${name} \${ARGN}\")\nendfunction()\n"
   "function(set_tests_properties)\nendfunction()\n"
   "include([==[${CMAKE_CURRENT_LIST_DIR}/../boost-test-discovery-impl.cmake]==])\n"
   "boost_test_discover_tests_impl(EXECUTABLE [==[${work}/lockfail.sh]==]\n"
   "   TESTS_FILE [==[${work}/lockfail_tests.cmake]==] FAILURE_NAME lockfail_DISCOVERY_FAILED\n"
   "   CMAKE_COMMAND [==[${CMAKE_COMMAND}]==] WORKING_DIRECTORY [==[${work}]==] NAME @UNIT@ LOCK_TIMEOUT 0)\n"
   "file(TOUCH [==[${work}/tried]==])\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${work}/hold.cmake" COMMAND "${CMAKE_COMMAND}" -P "${work}/try.cmake"
                OUTPUT_VARIABLE output ERROR_VARIABLE output TIMEOUT 60)
expect_contains("${output}" "registered lockfail_DISCOVERY_FAILED" "failed lock")
expect_contains("${output}" "cannot lock" "failed lock message")
file(READ "${work}/lockfail_tests.cmake" kept)
expect_equal("${kept}" "# the current list\n" "list kept without the lock")

make_executable(empty "" 0)
discover(empty NAME "@UNIT@")
get_property(tests GLOBAL PROPERTY recorded_tests)
expect_equal("${tests}" "empty_DISCOVERY_FAILED" "executable listing no units")

# End to end: a tiny project using boost_test_discover_tests(), read back through ctest. GENERATOR, CXX and CONFIG let
# the ctest entry build it the way the enclosing build is built; a multi-config generator needs CONFIG for the nested
# build and every ctest call.
get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
set(ctest_command "${cmake_bin}/ctest")
if(NOT GENERATOR)
   set(GENERATOR Ninja)
endif()
set(build_config "")
set(ctest_config "")
if(CONFIG)
   set(build_config --config "${CONFIG}")
   set(ctest_config -C "${CONFIG}")
endif()
set(project "${work}/project")
file(WRITE "${project}/listing.txt" "one_tests*\n    one_case*\ntwo_tests*\n")
file(WRITE "${project}/fake.cpp" [==[
// Stand-in for a Boost.Test executable: writes LISTING_FILE to the --report_sink file, and passes when run as a test.
#include <fstream>
#include <string>
int main(int argc, char** argv) {
   for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument.rfind("--report_sink=", 0) == 0) {
         std::ifstream listing(LISTING_FILE);
         std::ofstream(argument.substr(14)) << listing.rdbuf();
      }
   }
   return 0;
}
]==])
file(WRITE "${project}/CMakeLists.txt"
   "cmake_minimum_required(VERSION 3.19)\n"
   "project(boost_test_discovery_e2e LANGUAGES CXX)\n"
   "enable_testing()\n"
   "include([==[${CMAKE_CURRENT_LIST_DIR}/../boost-test-discovery.cmake]==])\n"
   "add_executable(fake fake.cpp)\n"
   "set_target_properties(fake PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"$<1:\${CMAKE_BINARY_DIR}>\")\n"
   "target_compile_definitions(fake PRIVATE [==[LISTING_FILE=\"${project}/listing.txt\"]==])\n"
   "boost_test_discover_tests(fake NAME e2e.@UNIT@.@RUNTIME@ RUNTIMES a b ARGS --x PROPERTIES LABELS lane TIMEOUT 7)\n")

function(run what)
   execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "${what} failed (${result}):\n${output}")
   endif()
   set(output "${output}" PARENT_SCOPE)
endfunction()

# The test names ctest lists in `build`, in order.
function(listed_tests out build)
   run("ctest --show-only" "${ctest_command}" --show-only=json-v1 ${ARGN} WORKING_DIRECTORY "${build}")
   string(JSON count LENGTH "${output}" tests)
   set(names "")
   if(count GREATER 0)
      math(EXPR last "${count} - 1")
      foreach(i RANGE ${last})
         string(JSON name GET "${output}" tests ${i} name)
         list(APPEND names "${name}")
      endforeach()
   endif()
   list(SORT names)
   set(${out} "${names}" PARENT_SCOPE)
   set(json "${output}" PARENT_SCOPE)
endfunction()

set(compiler "")
if(CXX)
   set(compiler "-DCMAKE_CXX_COMPILER=${CXX}")
endif()
set(build "${work}/build")
run("configure" "${CMAKE_COMMAND}" -S "${project}" -B "${build}" -G "${GENERATOR}" ${compiler})
listed_tests(tests "${build}" ${ctest_config})
expect_equal("${tests}" "fake_NOT_BUILT" "before the executable is built")

run("build" "${CMAKE_COMMAND}" --build "${build}" ${build_config})
listed_tests(tests "${build}" ${ctest_config})
expect_equal("${tests}" "e2e.one_tests.a;e2e.one_tests.b;e2e.two_tests.a;e2e.two_tests.b" "after the build")
string(JSON length LENGTH "${json}" tests 0 command)
math(EXPR last "${length} - 1")
set(arguments "")
foreach(i RANGE 1 ${last})
   string(JSON argument GET "${json}" tests 0 command ${i})
   list(APPEND arguments "${argument}")
endforeach()
expect_equal("${arguments}" "--run_test=one_tests;--x;--;--a" "discovered command")
string(JSON properties GET "${json}" tests 0 properties)
expect_contains("${properties}" "\"lane\"" "LABELS")
expect_contains("${properties}" "\"TIMEOUT\"" "TIMEOUT")
run("running the entries" "${ctest_command}" --output-on-failure ${ctest_config} WORKING_DIRECTORY "${build}")

# A newer executable is listed again: a suite added to it appears without reconfiguring.
file(APPEND "${project}/listing.txt" "three_tests*\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(TOUCH "${build}/fake")
listed_tests(tests "${build}" ${ctest_config})
list(LENGTH tests count)
expect_equal("${count}" "6" "after the executable changed")

if(GENERATOR STREQUAL "Ninja")
   set(multi "${work}/multi")
   run("configure multi-config" "${CMAKE_COMMAND}" -S "${project}" -B "${multi}" -G "Ninja Multi-Config" ${compiler})
   run("build multi-config" "${CMAKE_COMMAND}" --build "${multi}" --config Debug)
   listed_tests(tests "${multi}" -C Debug)
   list(LENGTH tests count)
   expect_equal("${count}" "6" "multi-config Debug")
endif()

file(REMOVE_RECURSE "${work}")
message(STATUS "boost-test-discovery: all checks passed")
