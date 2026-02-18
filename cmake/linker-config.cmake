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
