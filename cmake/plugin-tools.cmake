
macro(plugin_target TARGET_NAME)

    cmake_parse_arguments(ARG "" "" "LIBRARIES;SOURCE_FILES;SOURCE_GLOBS;TEST_SOURCE_FILES;TEST_SOURCE_GLOBS" ${ARGN})

    message(STATUS "Building plugin ${TARGET_NAME}: src=${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(SRC_FILES "")
    if (IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
    set(TEST_TARGET_NAME test_${TARGET_NAME})

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

    if (ENABLE_TESTS)
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
                COMMAND ${CMAKE_CURRENT_BINARY_DIR}/test/${TEST_TARGET_NAME}
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
    endif ()
endmacro()
