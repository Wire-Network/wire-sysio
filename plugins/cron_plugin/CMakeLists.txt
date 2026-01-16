set(TARGET_NAME cron_plugin)

plugin_target(
        ${TARGET_NAME}
        LIBRARIES
        appbase
        chainbase
        chain_plugin
        sysio_chain)