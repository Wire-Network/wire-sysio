# Shared CMake logic for linking native intrinsic exports into an executable.
# Both unittest_target (test-tools.cmake) and chain_target (chain-tools.cmake)
# include this file to export the ~149 INTRINSIC_EXPORT C symbols that natively
# compiled contract .so files resolve at dlopen time.
#
# Usage: call  link_native_exports(TARGET)  after add_executable().
#
# Note: This file is include()'d at CMake configuration time *before*
# SYSIO_WASM_RUNTIMES is populated, so all runtime-dependent logic must
# live inside the macro (which is called later when targets are defined).

macro(link_native_exports TARGET)
   if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
      set(NATIVE_INTRINSIC_EXPORTS_SRC "${CMAKE_SOURCE_DIR}/libraries/testing/native_intrinsic_exports.cpp")
      set(NATIVE_EXPORT_LIST "${CMAKE_BINARY_DIR}/native_intrinsic_exports.list")

      # Generate the --dynamic-list file once (shared by all targets).
      if(NOT TARGET native_export_list)
         add_custom_command(
            OUTPUT "${NATIVE_EXPORT_LIST}"
            COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_export_list.py"
               --format=dynamic-list -o "${NATIVE_EXPORT_LIST}"
               "${NATIVE_INTRINSIC_EXPORTS_SRC}"
            DEPENDS "${NATIVE_INTRINSIC_EXPORTS_SRC}" "${CMAKE_SOURCE_DIR}/scripts/gen_export_list.py"
            COMMENT "Generating native intrinsic export list"
            VERBATIM
         )
         add_custom_target(native_export_list DEPENDS "${NATIVE_EXPORT_LIST}")
      endif()

      add_dependencies(${TARGET} native_export_list)

      # Link intrinsic exports with --whole-archive so the extern "C" symbols
      # are included even though nothing in the binary references them directly.
      # They're resolved at dlopen time by native contract .so files.
      target_link_libraries(${TARGET} PUBLIC
         -Wl,${whole_archive_flag}
         native_intrinsic_exports
         -Wl,${no_whole_archive_flag}
      )

      if(NOT APPLE)
         target_link_options(${TARGET} PRIVATE
            "-Wl,--dynamic-list=${NATIVE_EXPORT_LIST}")
         # Override the global -static-libgcc (from linker-config.cmake).
         # Both the executable and the .so must use the SAME shared libgcc_s.so
         # for C++ exception unwinding to work across the dlopen boundary.
         target_link_libraries(${TARGET} PUBLIC gcc_s stdc++)
      else()
         target_link_options(${TARGET} PRIVATE
            "-Wl,-exported_symbols_list,${NATIVE_EXPORT_LIST}")
      endif()
   endif()
endmacro()
