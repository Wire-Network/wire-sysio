set(TARGET_LIB_NAME beacon_chain_update_plugin)

plugin_target(
        ${TARGET_LIB_NAME}
        LIBRARIES
        outpost_ethereum_client_plugin
        cron_plugin
        CURL::libcurl
)
