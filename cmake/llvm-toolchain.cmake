# Shared by the host configure and all vcpkg dependency builds. CDT uses its
# own ExternalProject toolchain and must never inherit these host ABI flags.
include_guard(GLOBAL)
if(NOT WIRE_LLVM_ROOT)
  set(WIRE_LLVM_ROOT "$ENV{WIRE_LLVM_ROOT}" CACHE PATH "LLVM 23.1.3 distribution")
endif()
if(NOT EXISTS "${WIRE_LLVM_ROOT}/bin/clang")
  message(FATAL_ERROR "Set WIRE_LLVM_ROOT to LLVM 23.1.3; provision it with scripts/llvm/install-toolchain.sh")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WIRE_LLVM_ROOT CMAKE_C_COMPILER CMAKE_CXX_COMPILER)

# Explicit compiler overrides retain the GCC CI lane; the default is Clang 23.
if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${WIRE_LLVM_ROOT}/bin/clang")
endif()
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER "${WIRE_LLVM_ROOT}/bin/clang++")
endif()
set(CMAKE_AR "${WIRE_LLVM_ROOT}/bin/llvm-ar")
set(CMAKE_RANLIB "${WIRE_LLVM_ROOT}/bin/llvm-ranlib")

# Chainloading replaces vcpkg's native toolchain. Retain its architecture, PIC,
# port flags and configuration-specific linker settings before adding our ABI.
if(VCPKG_TARGET_ARCHITECTURE)
  # Installed tester consumers may use a vcpkg checkout outside the source tree.
  get_filename_component(_wire_toolchain_directory "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
  if(Z_VCPKG_ROOT_DIR)
    set(_wire_vcpkg_toolchains "${Z_VCPKG_ROOT_DIR}/scripts/toolchains")
  elseif(EXISTS "${_wire_toolchain_directory}/../toolchains/linux.cmake")
    set(_wire_vcpkg_toolchains "${_wire_toolchain_directory}/../toolchains")
  else()
    set(_wire_vcpkg_toolchains "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/scripts/toolchains")
  endif()
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    include("${_wire_vcpkg_toolchains}/osx.cmake")
  else()
    include("${_wire_vcpkg_toolchains}/linux.cmake")
  endif()
endif()

execute_process(COMMAND "${WIRE_LLVM_ROOT}/bin/clang" -print-target-triple
                OUTPUT_VARIABLE _wire_triple OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
find_path(_wire_libcxx_include __config
          PATHS "${WIRE_LLVM_ROOT}/include/c++/v1" NO_DEFAULT_PATH REQUIRED)
find_library(_wire_libcxx_archive NAMES libc++.a
             PATHS "${WIRE_LLVM_ROOT}/lib/${_wire_triple}" "${WIRE_LLVM_ROOT}/lib"
             NO_DEFAULT_PATH REQUIRED)
get_filename_component(_wire_runtime_dir "${_wire_libcxx_archive}" DIRECTORY)
set(_wire_static_dir "${WIRE_LLVM_ROOT}/lib/wire-static")
if(NOT EXISTS "${_wire_static_dir}/libc++.a")
  message(FATAL_ERROR "Prepare this LLVM installation with cmake -DWIRE_LLVM_ROOT=${WIRE_LLVM_ROOT} -P scripts/llvm/prepare-runtime.cmake")
endif()
foreach(_wire_runtime c++abi unwind)
  if(NOT EXISTS "${_wire_runtime_dir}/lib${_wire_runtime}.a")
    message(FATAL_ERROR "LLVM distribution is missing static ${_wire_runtime} in ${_wire_runtime_dir}")
  endif()
endforeach()

# Suppress automatic libstdc++/system libc++ selection. Put the matching static
# archives after object files through STANDARD_LIBRARIES, including in try_compile.
set(_wire_cxx_flags "-nostdinc++ -isystem \"${_wire_libcxx_include}\"")
if(EXISTS "${WIRE_LLVM_ROOT}/include/${_wire_triple}/c++/v1/__config_site")
  string(PREPEND _wire_cxx_flags "-isystem \"${WIRE_LLVM_ROOT}/include/${_wire_triple}/c++/v1\" ")
elseif(NOT EXISTS "${_wire_libcxx_include}/__config_site")
  message(FATAL_ERROR "LLVM distribution is missing libc++ target configuration")
endif()
set(_wire_cxx_libraries "-L\"${_wire_static_dir}\" -lc++ -lc++abi -lunwind")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER MATCHES "(^|/)([^/]*-)?g[+][+](-[0-9]+)?$")
  string(APPEND _wire_cxx_libraries " -nodefaultlibs -lgcc -lm -lc -lpthread -ldl")
else()
  string(APPEND _wire_cxx_libraries " -nostdlib++")
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    string(APPEND _wire_cxx_libraries " --rtlib=compiler-rt --unwindlib=libunwind -static-libgcc -L\"${_wire_runtime_dir}\"")
  endif()
endif()
set(CMAKE_CXX_FLAGS_INIT "${_wire_cxx_flags} ${CMAKE_CXX_FLAGS_INIT}")
# CI sanitizer/GCC presets may populate the cache before language activation,
# which bypasses *_INIT. Preserve their flags and still enforce libc++ headers.
if(DEFINED CMAKE_CXX_FLAGS)
  string(FIND "${CMAKE_CXX_FLAGS}" "${_wire_cxx_flags}" _wire_flags_position)
  if(_wire_flags_position EQUAL -1)
    set(CMAKE_CXX_FLAGS "${_wire_cxx_flags} ${CMAKE_CXX_FLAGS}" CACHE STRING "C++ flags" FORCE)
  endif()
endif()
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "${_wire_cxx_libraries}")
# Reconfiguration must not retain a previous compiler's implicit runtime flags.
set(CMAKE_CXX_STANDARD_LIBRARIES "${_wire_cxx_libraries}" CACHE STRING "Pinned C++ runtime" FORCE)

# CMake adds C's implicit libraries when a C++ executable contains C objects.
# Select the same compiler builtins during C ABI detection, otherwise mixed
# targets acquire libgcc_s ahead of the intended LLVM unwinder.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER MATCHES "(^|/)([^/]*-)?gcc(-[0-9]+)?$")
    set(_wire_c_libraries "-nodefaultlibs -lgcc -lc -lgcc")
  else()
    set(_wire_c_libraries "--rtlib=compiler-rt --unwindlib=libunwind -static-libgcc -L\"${_wire_runtime_dir}\"")
  endif()
  set(CMAKE_C_STANDARD_LIBRARIES_INIT "${_wire_c_libraries}")
  set(CMAKE_C_STANDARD_LIBRARIES "${_wire_c_libraries}" CACHE STRING "Pinned compiler runtime" FORCE)
endif()
