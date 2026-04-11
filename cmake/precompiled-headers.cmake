# precompiled-headers.cmake
#
# Precompiled header (PCH) support for fc and sysio_chain targets.
# Toggle with -DENABLE_PCH=ON|OFF (default ON).
#
# The actual header list lives in cmake/include/sysio-pch.hpp.
# Both targets share a single file; chain-only headers are guarded
# with __has_include so the fc PCH compilation silently skips them.
#
# Benchmarked on fc + sysio_chain clean build (12-core, clang-18, Debug):
#   Without PCH: 2m 55s wall / 3014s CPU
#   With PCH:    1m 36s wall / 1129s CPU  (45% wall-time reduction)

option(ENABLE_PCH "Enable precompiled headers for faster builds" ON)

set(SYSIO_PCH_HEADER "${CMAKE_SOURCE_DIR}/cmake/include/sysio-pch.hpp")

# ---------------------------------------------------------------------------
# fc (foundation library, ~70 TUs)
# ---------------------------------------------------------------------------
function(configure_fc_pch target)
   if(NOT ENABLE_PCH)
      return()
   endif()

   target_precompile_headers(${target} PRIVATE ${SYSIO_PCH_HEADER})
endfunction()

# ---------------------------------------------------------------------------
# sysio_chain (blockchain core, ~50 C++ TUs + 1 C file + 1 .s file)
# ---------------------------------------------------------------------------
function(configure_chain_pch target)
   if(NOT ENABLE_PCH)
      return()
   endif()

   # Exclude C source from C++ PCH (would fail on #include <algorithm> etc.)
   set_source_files_properties(
      webassembly/runtimes/sys-vm-oc/gs_seg_helpers.c
      DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON
   )

   target_precompile_headers(${target} PRIVATE ${SYSIO_PCH_HEADER})
endfunction()
