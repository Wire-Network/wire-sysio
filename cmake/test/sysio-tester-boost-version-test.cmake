# Configures a project that includes the generated SysioTester.cmake against a Boost package config of another version.
# The configure must stop at the Boost lookup with CMake's exact-version error naming the version Sysio was built with.
if(NOT WORK_DIR OR NOT SYSIO_TESTER OR NOT BOOST_VERSION OR NOT CXX)
   message(FATAL_ERROR "WORK_DIR, SYSIO_TESTER, BOOST_VERSION, and CXX are required")
endif()
if(NOT GENERATOR)
   set(GENERATOR Ninja)
endif()
set(other_version 1.70.0)

file(REMOVE_RECURSE "${WORK_DIR}")
# A copy in a fresh prefix, so the build tree's vcpkg Boost is not on its search path.
file(COPY "${SYSIO_TESTER}" DESTINATION "${WORK_DIR}/sdk/lib/cmake/sysio")
set(stub_dir "${WORK_DIR}/stub/lib/cmake/Boost-${other_version}")
file(WRITE "${stub_dir}/BoostConfig.cmake" "set(Boost_FOUND TRUE)\n")
file(WRITE "${stub_dir}/BoostConfigVersion.cmake" "
set(PACKAGE_VERSION ${other_version})
if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
   set(PACKAGE_VERSION_EXACT TRUE)
endif()
if(NOT PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
   set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
")
file(WRITE "${WORK_DIR}/consumer/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.15)
project(sysio_tester_boost_version_probe CXX)
include(\"${WORK_DIR}/sdk/lib/cmake/sysio/SysioTester.cmake\")
")

execute_process(
   COMMAND "${CMAKE_COMMAND}" -S "${WORK_DIR}/consumer" -B "${WORK_DIR}/consumer/build" -G "${GENERATOR}"
           "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_PREFIX_PATH=${WORK_DIR}/stub" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
   RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(result EQUAL 0)
   message(FATAL_ERROR
           "Configure accepted Boost ${other_version}; SysioTester must require ${BOOST_VERSION}:\n${output}")
endif()
# CMake wraps its error text, so whitespace runs are collapsed before matching.
string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
string(FIND "${flat}" "exactly matches requested version \"${BOOST_VERSION}\"" exact_error)
string(FIND "${flat}" "BoostConfig.cmake, version: ${other_version}" rejected_stub)
if(exact_error EQUAL -1 OR rejected_stub EQUAL -1)
   message(FATAL_ERROR "Configure failed, but not on the exact Boost ${BOOST_VERSION} requirement:\n${output}")
endif()
message(STATUS "SysioTester rejected Boost ${other_version} and requires exactly ${BOOST_VERSION}")
