# cmake/linker-config.cmake
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_link_options($<$<LINK_LANGUAGE:C>:-fuse-ld=bfd>)
  add_link_options($<$<LINK_LANGUAGE:CXX>:-fuse-ld=bfd>)
endif()

# Portable binary: statically link C++ runtime on Linux
# so the only NEEDED shared libs are libc.so, libm.so, and ld-linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options(
          -static-libgcc
          $<$<LINK_LANGUAGE:CXX>:-static-libstdc++>
  )

  # Locate static libatomic for use as a link target (instead of -latomic)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} --print-file-name=libatomic.a
    OUTPUT_VARIABLE LIBATOMIC_STATIC
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  if(EXISTS "${LIBATOMIC_STATIC}")
    message(STATUS "Static libatomic: ${LIBATOMIC_STATIC}")
  else()
    message(FATAL_ERROR "Static libatomic.a not found – binary can not be built")
  endif()
endif()

# Mild, deterministic defaults for Release; split into separate genex args
add_link_options(
  $<$<CONFIG:Release>:-Wl,--as-needed>
  $<$<CONFIG:Release>:-Wl,-O1>
)

# apply_malloc_config(target)
#
# Link the selected allocator override (jemalloc or tcmalloc) into `target`.
# Call this from each executable that wants its malloc/free interceptor
# (currently nodeop; auxiliary binaries like clio/kiod/trx_generator stay
# on libc malloc to avoid unexpected interactions with short-lived processes).
#
# ENABLE_JEMALLOC and ENABLE_TCMALLOC are declared in cmake/build-options.cmake
# and validated mutually exclusive in cmake/compiler-config.cmake.
function(apply_malloc_config target)
  if(ENABLE_JEMALLOC)
    # --whole-archive forces all jemalloc symbols in so malloc/free override libc.
    target_link_options(${target} PRIVATE
      "LINKER:--whole-archive" "${JEMALLOC_LIB_PATH}" "LINKER:--no-whole-archive")
  elseif(ENABLE_TCMALLOC)
    # tcmalloc replaces malloc/free at link time. Linking last keeps the heap
    # profiler/checker accurate; target_link_libraries appends here, which is
    # close enough in practice for our single-executable use.
    target_link_libraries(${target} PRIVATE ${GPERFTOOLS_TCMALLOC})
  endif()
endfunction()
