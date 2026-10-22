include(CMakeParseArguments)
include(native-exports)

macro(unittest_target TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCE_FILES" ${ARGN})
    message(NOTICE "Creating unittest target (${TARGET}) with sources: ${ARG_SOURCE_FILES}")
    add_executable(${TARGET} ${ARG_SOURCE_FILES})

    target_link_libraries(
            ${TARGET}
            PUBLIC
            -Wl,${whole_archive_flag}

            ${PLUGIN_DEFAULT_DEPENDENCIES}
            -Wl,${no_whole_archive_flag}

            sysio_chain_wrap
            state_history
            chainbase
            sysio_testing
            custom_appbase
            libsodium::libsodium
    )

    target_include_directories(${TARGET} PUBLIC
            ${CMAKE_SOURCE_DIR}/libraries/testing/include
            ${CMAKE_BINARY_DIR}/contracts
            ${CMAKE_SOURCE_DIR}/contracts
            ${CMAKE_SOURCE_DIR}/unittests
            ${CMAKE_SOURCE_DIR}/unittests/system-test-contracts
            ${CMAKE_BINARY_DIR}/unittests/system-test-contracts
            ${CMAKE_BINARY_DIR}/unittests/include
            ${CMAKE_SOURCE_DIR}/plugins/http_plugin/include
            ${CMAKE_SOURCE_DIR}/plugins/chain_interface/include)

    # Export intrinsic symbols for native-module contract .so loading.
    link_native_exports(${TARGET})

endmacro()
