
# Option to enable/disable ccache usage

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

if(ENABLE_DISTCC)
  find_program(DISTCC_PROGRAM distcc)

  if(DISTCC_PROGRAM)
    message(STATUS "distcc found: ${DISTCC_PROGRAM}")

    # Configure distcc for all compilers
    foreach(_lang C CXX OBJC OBJCXX CUDA)
      if(CMAKE_${_lang}_COMPILER)
        if(CMAKE_${_lang}_COMPILER_LAUNCHER)
          set(CMAKE_${_lang}_COMPILER_LAUNCHER "${CMAKE_${_lang}_COMPILER_LAUNCHER}" "${DISTCC_PROGRAM}" CACHE STRING "Launcher for ${_lang}" FORCE)
        else()
          set(CMAKE_${_lang}_COMPILER_LAUNCHER "${DISTCC_PROGRAM}" CACHE STRING "Launcher for ${_lang}" FORCE)
        endif()
      endif()
    endforeach()

  else()
    message(STATUS "distcc not found (ENABLE_DISTCC=ON).")
  endif()
else()
  message(STATUS "distcc disabled (ENABLE_DISTCC=OFF).")
endif()

