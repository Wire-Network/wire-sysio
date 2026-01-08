
plugin_target( chain_plugin
        LIBRARIES
        producer_plugin
        sysio_chain
        custom_appbase
        appbase
        resource_monitor_plugin
        signature_provider_manager_plugin
 )
