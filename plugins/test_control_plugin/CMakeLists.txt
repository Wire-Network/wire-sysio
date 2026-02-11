plugin_target(test_control_plugin
        LIBRARIES
        producer_plugin
        chain_plugin
        net_plugin
        http_client_plugin
        appbase
        sysio_chain)
