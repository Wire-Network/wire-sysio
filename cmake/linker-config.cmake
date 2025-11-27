# cmake/linker-config.cmake
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_link_options($<$<LINK_LANGUAGE:C>:-fuse-ld=bfd>)
  add_link_options($<$<LINK_LANGUAGE:CXX>:-fuse-ld=bfd>)
endif()

# Mild, deterministic defaults for Release; split into separate genex args
add_link_options(
  $<$<CONFIG:Release>:-Wl,--as-needed>
  $<$<CONFIG:Release>:-Wl,-O1>
)