# Fail at configuration rather than mixing standard-library ABIs at link time.
include(CheckCXXSourceCompiles)
unset(WIRE_HAS_LIBCXX_23 CACHE)
check_cxx_source_compiles("
#include <version>
#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 230103
#error Wire requires libc++ 23.1.3
#endif
#include <stdexcept>
#include <string>
int main() { try { throw std::runtime_error(std::string(\"libc++\")); }
             catch (const std::exception&) { return 0; } }
" WIRE_HAS_LIBCXX_23)
if(NOT WIRE_HAS_LIBCXX_23)
  message(FATAL_ERROR "Wire host builds require libc++ 23.1.3 headers and matching runtime libraries")
endif()
