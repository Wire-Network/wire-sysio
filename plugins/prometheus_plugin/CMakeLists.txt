
plugin_target(
        prometheus_plugin
        LIBRARIES
        appbase
        prometheus-cpp::core
        http_plugin
        chain_plugin
        net_plugin
        outpost_solana_client_plugin
)
