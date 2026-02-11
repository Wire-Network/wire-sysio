
plugin_target(
        producer_plugin
        LIBRARIES
        chain_plugin
        http_client_plugin
        signature_provider_manager_plugin
        appbase
        sysio_chain
)

target_include_directories(producer_plugin
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../chain_interface/include")

