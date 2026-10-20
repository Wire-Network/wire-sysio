
plugin_target( chain_plugin
        LIBRARIES
        producer_plugin
        http_client_plugin
        sysio_chain
        custom_appbase
        appbase
        resource_monitor_plugin
        signature_provider_manager_plugin
 )

# chain_plugin.hpp includes the snapshot-attestation constants owned by the sysio.system contract.
target_include_directories(chain_plugin
        PUBLIC "${CMAKE_SOURCE_DIR}/contracts/sysio.system/include")
