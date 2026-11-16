# cmake/hardening-config.cmake
# Set glibc fortify the same way on every host, for byte-stable releases.

# Drop any _FORTIFY_SOURCE inherited from distro or toolchain flags, then set ours. Both are compile *options*, in
# this order: CMake emits compile definitions ahead of options, so a definition would be undone by the -U.
# Debug is left out because glibc fortifies optimized code only and warns in every translation unit otherwise.
add_compile_options(
  $<$<COMPILE_LANGUAGE:C,CXX>:-U_FORTIFY_SOURCE>
  $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<CONFIG:Debug>>>:-D_FORTIFY_SOURCE=2>
)

add_compile_definitions(
  _GNU_SOURCE
)
