
# Options to enable/disable ccache, icecc, etc usage
option(ENABLE_CCACHE "Enable ccache if available" ON)
option(ENABLE_ICECC "Enable icecc distributed compilation if available" ON)
option(ICECC_VERBOSE "Enable verbose icecc output" OFF)

# icecc scheduler configuration
set(ICECC_SCHEDULER_HOST "44.201.196.198" CACHE STRING "icecc scheduler hostname or IP address")
set(ICECC_SCHEDULER_PORT "8765" CACHE STRING "icecc scheduler port (default: 8765)")

# Handle icecc first (it can wrap ccache)
if(ENABLE_ICECC)
  find_program(ICECC_PROGRAM icecc)
  
  if(ICECC_PROGRAM)
    message(STATUS "icecc found: ${ICECC_PROGRAM}")
    
    # Configure icecc scheduler
    if(ICECC_SCHEDULER_HOST)
      if(ICECC_SCHEDULER_PORT)
        set(ENV{ICECC_PREFERRED_HOST} "${ICECC_SCHEDULER_HOST}:${ICECC_SCHEDULER_PORT}")
        message(STATUS "icecc: Using scheduler ${ICECC_SCHEDULER_HOST}:${ICECC_SCHEDULER_PORT}")
      else()
        set(ENV{ICECC_PREFERRED_HOST} "${ICECC_SCHEDULER_HOST}")
        message(STATUS "icecc: Using scheduler ${ICECC_SCHEDULER_HOST} (default port)")
      endif()
    else()
      message(STATUS "icecc: Using auto-discovery for scheduler (set ICECC_SCHEDULER_HOST to override)")
    endif()
    
    # Configure icecc environment variables
    set(ENV{ICECC_VERSION} "")  # Let icecc auto-detect
    
    if(ICECC_VERBOSE)
      set(ENV{ICECC_DEBUG} "debug")
      message(STATUS "icecc verbose mode enabled")
    endif()
    
    # Set job limit based on available remote nodes
    if(NOT DEFINED ENV{ICECC_JOBS})
      # Try to get number of available nodes
      execute_process(
        COMMAND bash -c "icecc --build-native 2>/dev/null | grep -c 'building native' || echo 1"
        OUTPUT_VARIABLE ICECC_ESTIMATED_JOBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      message(NOTICE "ICECC_ESTIMATED_JOBS: ${ICECC_ESTIMATED_JOBS}")
      if(ICECC_ESTIMATED_JOBS GREATER 1)
        # Use detected nodes + some local parallelism
        math(EXPR ICECC_JOBS "${ICECC_ESTIMATED_JOBS} + 2")
        set(ENV{ICECC_JOBS} "${ICECC_JOBS}")
        message(STATUS "icecc: Using ${ICECC_JOBS} parallel jobs")
      endif()
    endif()
    
    # icecc with ccache integration
    if(ENABLE_CCACHE)
      find_program(CCACHE_PROGRAM ccache)
      if(CCACHE_PROGRAM)
        message(STATUS "ccache found: ${CCACHE_PROGRAM} (will be used with icecc)")
        # Set CCACHE_PREFIX so ccache calls icecc
        set(ENV{CCACHE_PREFIX} "${ICECC_PROGRAM}")
        
        # Set the compiler launcher to ccache (which will call icecc)
        foreach(_lang C CXX OBJC OBJCXX)
          if(CMAKE_${_lang}_COMPILER)
            set(CMAKE_${_lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "ccache+icecc launcher for ${_lang}" FORCE)
          endif()
        endforeach()
      else()
        # No ccache, use icecc directly
        foreach(_lang C CXX OBJC OBJCXX)
          if(CMAKE_${_lang}_COMPILER)
            set(CMAKE_${_lang}_COMPILER_LAUNCHER "${ICECC_PROGRAM}" CACHE STRING "icecc launcher for ${_lang}" FORCE)
          endif()
        endforeach()
      endif()
    else()
      # ccache disabled, use icecc directly
      foreach(_lang C CXX OBJC OBJCXX)
        if(CMAKE_${_lang}_COMPILER)
          set(CMAKE_${_lang}_COMPILER_LAUNCHER "${ICECC_PROGRAM}" CACHE STRING "icecc launcher for ${_lang}" FORCE)
        endif()
      endforeach()
    endif()
    
    # Special handling for CUDA (exclude by default)
    if(CMAKE_CUDA_COMPILER)
      message(STATUS "icecc: CUDA compiler detected but not using icecc (not typically supported)")
    endif()
    
    # Create a target to show icecc status
    add_custom_target(icecc-status
      COMMAND ${ICECC_PROGRAM} --help || echo "icecc not available"
      COMMENT "Checking icecc status"
      VERBATIM
    )
    
  else()
    message(STATUS "icecc not found (ENABLE_ICECC=ON), falling back to ccache if available.")
    set(ENABLE_ICECC OFF)
  endif()
endif()

# Handle ccache (either standalone when icecc is disabled)
if(ENABLE_CCACHE AND NOT ENABLE_ICECC)
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
elseif(NOT ENABLE_CCACHE AND NOT ENABLE_ICECC)
  message(STATUS "Both ccache and icecc disabled.")
endif()