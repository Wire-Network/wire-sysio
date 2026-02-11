if (PLUGIN_DEFAULT_DEPENDENCIES)
    return()
endif ()

set(PLUGIN_DEFAULT_DEPENDENCIES
        fc-lite
        fc

        Boost::asio
        Boost::beast
        Boost::bimap
        Boost::chrono
        Boost::date_time
        Boost::dll
        Boost::interprocess
        Boost::iostreams
        Boost::multiprecision
        Boost::multi_index
        Boost::process
        Boost::thread

        boringssl::ssl
        boringssl::crypto
        boringssl::decrepit

        libsodium::libsodium

)

macro(plugin_target TARGET_NAME)

    cmake_parse_arguments(ARG "SKIP_TEST_CONFIG" "" "LIBRARIES;SOURCE_FILES;SOURCE_GLOBS;TEST_SOURCE_FILES;TEST_SOURCE_GLOBS" ${ARGN})

    set(PLUGIN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
    message(STATUS "Building plugin ${TARGET_NAME} @ ${CMAKE_CURRENT_SOURCE_DIR}")
    set(SRC_FILES "")
    if (IS_DIRECTORY ${PLUGIN_SRC_DIR})
        file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
    endif ()
    if (ARG_SOURCE_FILES)
        list(APPEND SRC_FILES ${ARG_SOURCE_FILES})
    endif ()
    if (ARG_SOURCE_GLOBS)
        foreach (GLOB_PATTERN ${ARG_SOURCE_GLOBS})
            file(GLOB_RECURSE GLOB_FILES ${GLOB_PATTERN})
            list(APPEND SRC_FILES ${GLOB_FILES})
        endforeach ()
    endif ()

    add_library(${TARGET_NAME} STATIC ${SRC_FILES})

    target_include_directories(
            ${TARGET_NAME}
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(
            ${TARGET_NAME}
            PUBLIC
            -Wl,${whole_archive_flag}
            ${PLUGIN_DEFAULT_DEPENDENCIES}
            -Wl,${no_whole_archive_flag}

            ${ARG_LIBRARIES}
    )

    # Install headers
    install(
            TARGETS ${TARGET_NAME}
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin
            INCLUDES DESTINATION include

    )

    set(PLUGIN_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)
    if(NOT ARG_SKIP_TEST_CONFIG AND ENABLE_TESTS AND IS_DIRECTORY ${PLUGIN_TEST_DIR})

        if (EXISTS ${PLUGIN_TEST_DIR}/CMakeLists.txt)
            message(NOTICE "Plugin ${TARGET_NAME} test directory (${PLUGIN_TEST_DIR}) has a cmake file, so we will use it")
            add_subdirectory(test)
        else()
            set(TEST_TARGET_NAME test_${TARGET_NAME})

            set(TEST_FILES "")
            if (IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)
                file(GLOB_RECURSE TEST_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp)
            endif ()
            if (ARG_TEST_SOURCE_FILES)
                list(APPEND TEST_FILES ${ARG_TEST_SOURCE_FILES})
            endif ()
            if (ARG_TEST_SOURCE_GLOBS)
                foreach (GLOB_PATTERN ${ARG_TEST_SOURCE_GLOBS})
                    file(GLOB_RECURSE GLOB_FILES ${GLOB_PATTERN})
                    list(APPEND TEST_FILES ${GLOB_FILES})
                endforeach ()
            endif ()

            add_executable(${TEST_TARGET_NAME} ${TEST_FILES})

            target_link_libraries(
                    ${TEST_TARGET_NAME}
                    PUBLIC
                    ${TARGET_NAME}
                    sysio_testing
                    sysio_chain_wrap
            )
            add_test(
                    NAME ${TEST_TARGET_NAME}
                    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${TEST_TARGET_NAME}
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            )
        endif ()
    endif ()
endmacro()
