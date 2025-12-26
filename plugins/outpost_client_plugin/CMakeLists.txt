set(TARGET_NAME outpost_client_plugin)

plugin_target(
        ${TARGET_NAME}
        LIBRARIES
        appbase
        chainbase
        chain_plugin

        signature_provider_manager_plugin
        sysio_chain
        )
