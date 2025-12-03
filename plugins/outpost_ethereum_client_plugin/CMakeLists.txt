set(TARGET_LIB_NAME outpost_ethereum_client_plugin)
set(TARGET_TOOL_NAME outpost_ethereum_client_tool)

file(GLOB_RECURSE TOOL_FILES tools/ethereum_client_rpc_tool/*.cpp)

plugin_target(
        ${TARGET_LIB_NAME}
        LIBRARIES
        chain_plugin
        JsonCpp::JsonCpp
        CURL::libcurl
        boringssl::ssl
        boringssl::crypto
        signature_provider_manager_plugin
)


if (ENABLE_TESTS)
    add_executable(${TARGET_TOOL_NAME} ${TOOL_FILES})
    target_include_directories(
            ${TARGET_TOOL_NAME}
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
#           ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            ${TARGET_TOOL_NAME}
            PUBLIC
            ${TARGET_LIB_NAME}
            fc
            JsonCpp::JsonCpp
            CURL::libcurl
            boringssl::ssl
            boringssl::crypto
    )
endif ()
