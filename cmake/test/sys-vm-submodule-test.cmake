# Exercise the real submodule integration, option isolation, and installed SDK interface.
if(NOT WORK_DIR OR NOT SOURCE_DIR OR NOT SOFTFLOAT_DIR)
   message(FATAL_ERROR "WORK_DIR, SOURCE_DIR, and SOFTFLOAT_DIR are required")
endif()
if(NOT GENERATOR)
   set(GENERATOR Ninja)
endif()
file(MAKE_DIRECTORY "${WORK_DIR}/source/sentinel-modules" "${WORK_DIR}/consumer")
file(WRITE "${WORK_DIR}/source/sentinel-modules/dependencies.cmake"
     "message(FATAL_ERROR \"The VM resolved its parent's dependencies.cmake\")\n")

set(project [=[
cmake_minimum_required(VERSION 3.19)
project(vm_submodule_probe CXX)
include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME parent_component)
foreach(option ENABLE_TESTS ENABLE_SPEC_TESTS ENABLE_FUZZ_TESTS ENABLE_TOOLS ENABLE_CCACHE
               ENABLE_PROFILE ENABLE_GPERFTOOLS ENABLE_ADDRESS_SANITIZER ENABLE_UNDEFINED_BEHAVIOR_SANITIZER)
   set(${option} ON CACHE BOOL "Parent setting that the VM must preserve" FORCE)
endforeach()
get_property(original_launcher GLOBAL PROPERTY RULE_LAUNCH_COMPILE)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sentinel-modules")
set(SYSVM_INSTALL_COMPONENT dev)
include("@SOURCE_DIR@/cmake/add-sys-vm.cmake")
if(NOT VM_SOURCE)
   set(VM_SOURCE "@SOURCE_DIR@/libraries/sys-vm")
endif()
add_sys_vm_submodule("${VM_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/vm")
foreach(option ENABLE_TESTS ENABLE_SPEC_TESTS ENABLE_FUZZ_TESTS ENABLE_TOOLS ENABLE_CCACHE
               ENABLE_PROFILE ENABLE_GPERFTOOLS ENABLE_ADDRESS_SANITIZER ENABLE_UNDEFINED_BEHAVIOR_SANITIZER)
   get_property(cached CACHE ${option} PROPERTY VALUE)
   if(NOT ${option} OR NOT cached)
      message(FATAL_ERROR "The VM changed parent setting ${option}")
   endif()
endforeach()
get_property(current_launcher GLOBAL PROPERTY RULE_LAUNCH_COMPILE)
if(NOT CMAKE_CXX_STANDARD EQUAL 23 OR
   NOT CMAKE_INSTALL_DEFAULT_COMPONENT_NAME STREQUAL "parent_component" OR
   NOT "${current_launcher}" STREQUAL "${original_launcher}")
   message(FATAL_ERROR "The VM changed parent compiler or installation settings")
endif()
if(TARGET unit_tests OR TARGET sys_vm_spec_tests OR TARGET sys-vm-interp)
   message(FATAL_ERROR "The VM enabled its standalone tests or tools")
endif()
add_executable(vm_probe main.cpp)
target_link_libraries(vm_probe PRIVATE sys-vm::sys-vm)
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${WORK_DIR}/source/CMakeLists.txt" "${project}")
file(WRITE "${WORK_DIR}/source/main.cpp" [=[
#include <sysio/vm/config.hpp>
#include <sysio/vm/signals.hpp>
static_assert(sysio::vm::use_softfloat);
/// Verify that both build-tree and installed consumers use the fixed signal disposition handling.
int main() {
   auto& previous = sysio::vm::prev_signal_handler<SIGILL>;
   previous = {};
   previous.sa_handler = SIG_IGN;
   previous.sa_flags = SA_SIGINFO;
   sysio::vm::chain_previous_signal_handler(SIGILL, nullptr, nullptr);
}
]=])
file(COPY "${WORK_DIR}/source/main.cpp" DESTINATION "${WORK_DIR}/consumer")
file(WRITE "${WORK_DIR}/consumer/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.19)
project(vm_sdk_probe CXX)
find_package(softfloat CONFIG REQUIRED)
find_package(sys-vm CONFIG REQUIRED PATHS "${VM_PREFIX}/share/sys-vm" NO_DEFAULT_PATH)
add_executable(vm_probe main.cpp)
target_link_libraries(vm_probe PRIVATE sys-vm::sys-vm)
]=])

# Keep exact configure/build/install diagnostics for failed integration checks.
function(run_checked name)
   execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
   file(WRITE "${WORK_DIR}/${name}.log" "${output}${error}")
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "${name} failed (${result}):\n${output}${error}")
   endif()
endfunction()

set(configure_options -G "${GENERATOR}" "-Dsoftfloat_DIR=${SOFTFLOAT_DIR}" -DCMAKE_BUILD_TYPE=Release)
if(CXX)
   list(APPEND configure_options "-DCMAKE_CXX_COMPILER=${CXX}")
endif()
set(config_options "")
if(CONFIG)
   list(APPEND configure_options "-DCMAKE_CONFIGURATION_TYPES=${CONFIG}")
   set(config_options --config "${CONFIG}")
endif()
run_checked(configure "${CMAKE_COMMAND}" -S "${WORK_DIR}/source" -B "${WORK_DIR}/build" ${configure_options})
run_checked(build "${CMAKE_COMMAND}" --build "${WORK_DIR}/build" ${config_options})
run_checked(install-dev "${CMAKE_COMMAND}" --install "${WORK_DIR}/build" ${config_options}
            --prefix "${WORK_DIR}/sdk" --component dev)
file(SHA256 "${SOURCE_DIR}/libraries/sys-vm/include/sysio/vm/signals.hpp" source_hash)
file(SHA256 "${WORK_DIR}/sdk/include/sysio/vm/signals.hpp" installed_hash)
if(NOT source_hash STREQUAL installed_hash)
   message(FATAL_ERROR "The development package does not contain the pinned VM headers")
endif()
run_checked(install-base "${CMAKE_COMMAND}" --install "${WORK_DIR}/build" ${config_options}
            --prefix "${WORK_DIR}/base" --component base)
if(EXISTS "${WORK_DIR}/base/include/sysio/vm")
   message(FATAL_ERROR "VM development headers leaked into the base component")
endif()
run_checked(consumer-configure "${CMAKE_COMMAND}" -S "${WORK_DIR}/consumer" -B "${WORK_DIR}/consumer-build"
            ${configure_options} "-DVM_PREFIX=${WORK_DIR}/sdk")
run_checked(consumer-build "${CMAKE_COMMAND}" --build "${WORK_DIR}/consumer-build" ${config_options})
foreach(build IN ITEMS build consumer-build)
   set(probe "${WORK_DIR}/${build}/vm_probe")
   if(CONFIG AND EXISTS "${WORK_DIR}/${build}/${CONFIG}/vm_probe")
      set(probe "${WORK_DIR}/${build}/${CONFIG}/vm_probe")
   endif()
   run_checked("${build}-run" "${probe}")
endforeach()

# A checkout without the submodule must fail early with an actionable initialization command.
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK_DIR}/source" -B "${WORK_DIR}/missing-build"
                ${configure_options} "-DVM_SOURCE=${WORK_DIR}/missing"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(WRITE "${WORK_DIR}/missing-submodule.log" "${output}${error}")
string(REGEX REPLACE "[ \t\r\n]+" " " diagnostic "${output}${error}")
if(result EQUAL 0 OR NOT diagnostic MATCHES "git submodule update --init libraries/sys-vm")
   message(FATAL_ERROR "A missing VM submodule did not produce the expected initialization error")
endif()
message(STATUS "VM submodule, SDK consumers, option isolation, and missing-submodule checks passed")
