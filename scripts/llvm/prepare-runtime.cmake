# Give CMake and non-CMake dependency builders a portable static-only library
# search directory. vcpkg's autotools adapter cannot forward absolute .a names.
if(NOT WIRE_LLVM_ROOT)
  message(FATAL_ERROR "Pass -DWIRE_LLVM_ROOT=<toolchain installation>")
endif()
execute_process(COMMAND "${WIRE_LLVM_ROOT}/bin/clang" -print-target-triple
                OUTPUT_VARIABLE triple OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(destination "${WIRE_LLVM_ROOT}/lib/wire-static")
file(MAKE_DIRECTORY "${destination}")
foreach(runtime c++ c++abi unwind)
  unset(archive CACHE)
  find_library(archive NAMES "lib${runtime}.a"
               PATHS "${WIRE_LLVM_ROOT}/lib/${triple}" "${WIRE_LLVM_ROOT}/lib"
               NO_DEFAULT_PATH REQUIRED)
  file(RELATIVE_PATH relative_archive "${destination}" "${archive}")
  file(CREATE_LINK "${relative_archive}" "${destination}/lib${runtime}.a" SYMBOLIC)
endforeach()
