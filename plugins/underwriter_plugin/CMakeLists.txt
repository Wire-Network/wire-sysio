plugin_target(
        underwriter_plugin
        LIBRARIES
        appbase
        chainbase
        chain_plugin
        cron_plugin
        http_plugin
        signature_provider_manager_plugin
        outpost_client_plugin
        outpost_ethereum_client_plugin
        outpost_solana_client_plugin
        opp
        fc
        sysio_chain)

if(TARGET test_underwriter_plugin)
    target_include_directories(test_underwriter_plugin PRIVATE
            ${CMAKE_SOURCE_DIR}/libraries/opp/test/include)
endif()
