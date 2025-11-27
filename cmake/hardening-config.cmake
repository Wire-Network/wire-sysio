# cmake/hardening-config.cmake
# Normalize glibc fortify across hosts for byte-stable releases.

# 1) Undefine any inherited _FORTIFY_SOURCE from distro flags (needs -U, so use compile *options*)
add_compile_options(
  $<$<COMPILE_LANGUAGE:C>:-U_FORTIFY_SOURCE>
  $<$<COMPILE_LANGUAGE:CXX>:-U_FORTIFY_SOURCE>
)

# 2) Set _FORTIFY_SOURCE to 0 explicitly and make _GNU_SOURCE visible (use compile *definitions*)
#    If you ever want fortify on, change 0 -> 2 here (on both builds).
add_compile_definitions(
  _FORTIFY_SOURCE=2
  _GNU_SOURCE
)
