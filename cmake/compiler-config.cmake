
# Option to enable/disable ccache usage
option(ENABLE_CCACHE "Enable ccache if available" ON)
option(ENABLE_DISTCC "Enable distcc if available" ON)

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
  add_compile_options(-fcolor-diagnostics)
endif()

if(ENABLE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)

  if(CCACHE_PROGRAM)
    message(STATUS "ccache found: ${CCACHE_PROGRAM}")

    # Prefer per-language compiler launchers (CMake >= 3.4)
    foreach(_lang C CXX OBJC OBJCXX CUDA)
      if(CMAKE_${_lang}_COMPILER)
        set(CMAKE_${_lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "Launcher for ${_lang}" FORCE)
      endif()
    endforeach()

  else()
    message(STATUS "ccache not found (ENABLE_CCACHE=ON).")
  endif()
else()
  message(STATUS "ccache disabled (ENABLE_CCACHE=OFF).")
endif()