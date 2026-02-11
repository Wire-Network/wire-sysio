set(TARGET_LIB_NAME outpost_solana_client_plugin)
set(TARGET_TOOL_NAME outpost_solana_client_tool)

plugin_target(
        ${TARGET_LIB_NAME}
        LIBRARIES
        chain_plugin
        outpost_client_plugin
        signature_provider_manager_plugin
)

if (ENABLE_TESTS)
    file(GLOB_RECURSE TOOL_FILES tools/solana_client_rpc_tool/*.cpp)
    chain_target(${TARGET_TOOL_NAME} SOURCE_FILES ${TOOL_FILES})
    target_link_libraries(
            ${TARGET_TOOL_NAME}
            PUBLIC
            ${TARGET_LIB_NAME}
            ${PLUGIN_DEFAULT_DEPENDENCIES}

    )
endif ()
