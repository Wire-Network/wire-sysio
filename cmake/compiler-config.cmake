
# Option to enable/disable ccache usage

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fcolor-diagnostics)
    endif()
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

# Allocator overrides (applied per-target via apply_malloc_config in cmake/linker-config.cmake).
# The two allocator backends are mutually exclusive.
if(ENABLE_JEMALLOC AND ENABLE_TCMALLOC)
  message(FATAL_ERROR "ENABLE_JEMALLOC and ENABLE_TCMALLOC are mutually exclusive")
endif()

# ASan/UBSan ship their own malloc interceptors; linking jemalloc's overrides on top produces undefined
# behavior (double-interception, heap metadata mismatch) that makes nodeop crash on startup. The sanitizer
# CI platforms inject -fsanitize=... via CMAKE_{C,CXX}_FLAGS (see .cicd/platforms/{asan,ubsan}.Dockerfile)
# rather than toggling the ENABLE_*_SANITIZER options, so detect either path.
if(ENABLE_JEMALLOC AND (ENABLE_ADDRESS_SANITIZER OR ENABLE_UNDEFINED_BEHAVIOR_SANITIZER
                        OR "${CMAKE_C_FLAGS}${CMAKE_CXX_FLAGS}" MATCHES "-fsanitize="))
  message(STATUS "ENABLE_JEMALLOC disabled: sanitizer detected (ENABLE_*_SANITIZER option or -fsanitize= flag)")
  set(ENABLE_JEMALLOC OFF CACHE BOOL "" FORCE)
endif()

