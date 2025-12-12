set(TARGET_LIB_NAME outpost_ethereum_client_plugin)
set(TARGET_TOOL_NAME outpost_ethereum_client_tool)



plugin_target(
        ${TARGET_LIB_NAME}
        LIBRARIES
        chain_plugin
        fc
        fc-lite
#        JsonCpp::JsonCpp
#        CURL::libcurl
#        boringssl::ssl
#        boringssl::crypto
        outpost_client_plugin
        signature_provider_manager_plugin
)


if (ENABLE_TESTS)
    file(GLOB_RECURSE TOOL_FILES tools/ethereum_client_rpc_tool/*.cpp)
    chain_target(${TARGET_TOOL_NAME} SOURCE_FILES ${TOOL_FILES})
    target_link_libraries(
            ${TARGET_TOOL_NAME}
            PUBLIC
            ${TARGET_LIB_NAME}
            signature_provider_manager_plugin
            outpost_client_plugin
            outpost_ethereum_client_plugin
            fc-lite
            Boost::date_time
            Boost::chrono
            Boost::iostreams
            Boost::interprocess
            Boost::multi_index
            Boost::dll
            Boost::multiprecision
            Boost::beast
            Boost::asio
            Boost::thread
#            boringssl::ssl
#            boringssl::crypto
#            boringssl::decrepit

    )
endif ()
