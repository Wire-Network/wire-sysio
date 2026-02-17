set(TARGET_NAME cron_plugin)

plugin_target(
        ${TARGET_NAME}
        LIBRARIES
        appbase
        custom_appbase
        sysio_chain
)