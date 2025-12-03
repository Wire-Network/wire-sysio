set(TARGET_NAME operator_plugin)

plugin_target(
        ${TARGET_NAME}
        LIBRARIES
        appbase
        chainbase
        chain_plugin
        fc
        sysio_chain)