plugin_target(
        trace_api_plugin
        LIBRARIES
        chain_plugin
        http_plugin
        sysio_chain
        appbase
)


add_subdirectory(tool/trace_api_util)

