# cmake/linker-config.cmake
# Hard-pin LLD from our toolchain; adjustable via -DWIRE_LLD_PATH=...
set(WIRE_LLD_PATH "/opt/clang/clang-18/bin/ld.lld" CACHE FILEPATH "Path to ld.lld to use")

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # Apply per language, one flag per generator expression (no spaces inside)
  add_link_options($<$<LINK_LANGUAGE:CXX>:-fuse-ld=lld>)
  add_link_options($<$<LINK_LANGUAGE:CXX>:--ld-path=${WIRE_LLD_PATH}>)

  add_link_options($<$<LINK_LANGUAGE:C>:-fuse-ld=lld>)
  add_link_options($<$<LINK_LANGUAGE:C>:--ld-path=${WIRE_LLD_PATH}>)
endif()
