file(TOUCH ${CMAKE_BINARY_DIR}/.wire-build-root)

# SETUP CXX/CC COMPILER/LINKER FLAGS
if (UNIX)
    if (APPLE)
        # APPLE SPECIFIC OPTIONS HERE
        message(STATUS "Configuring WIRE on macOS")

        set(whole_archive_flag "-force_load")
        set(no_whole_archive_flag "")
        set(build_id_flag "")

        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -Wall -Wno-deprecated-declarations")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-deprecated-declarations")
    else ()
        # LINUX SPECIFIC OPTIONS HERE
        message(STATUS "Configuring WIRE on Linux")

        set(whole_archive_flag "--whole-archive")
        set(no_whole_archive_flag "--no-whole-archive")
        set(build_id_flag "--build-id")

        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -Wall")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
        if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" AND (CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL 4.0.0 OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 4.0.0))
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-invalid-partial-specialization")
        endif()
    endif ()
else ()
    message(FATAL_ERROR "WIN32 PLATFORMS ARE UNSUPPORTED")
endif ()


# ENABLE WARNING AS ERROR
if(ENABLE_WERROR)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -Werror")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif()
# ENABLE EXTRA WARNINGS
if(ENABLE_WEXTRA)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -Wextra")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wextra")
endif()


# GLOBAL FLAGS
set(DEBUG_ENABLED OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEBUG_ENABLED ON)
endif()
message(STATUS "DEBUG_ENABLED=${DEBUG_ENABLED}")
if (DEBUG_ENABLED)
    message(STATUS "if(DEBUG_ENABLED) is TRUE")
endif()

# REMOVE ANY DANGEROUS PATHS FROM THE ENVIRONMENT
set(_bad_paths_regex "linuxbrew")
string(REPLACE ":" ";" _path_list "$ENV{PATH}")
set(_clean_path "")
foreach(_p IN LISTS _path_list)
    if(NOT _p MATCHES "${_bad_paths_regex}")
        list(APPEND _clean_path "${_p}")
    endif()
endforeach()
string(REPLACE ";" ":" _clean_path_str "${_clean_path}")
set(ENV{PATH} "${_clean_path_str}")
