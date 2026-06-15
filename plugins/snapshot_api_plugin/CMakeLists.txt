
plugin_target(
        snapshot_api_plugin
        LIBRARIES
        chain_plugin
        producer_plugin
        http_plugin
        appbase
)
