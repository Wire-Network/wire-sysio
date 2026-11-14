
plugin_target(
        producer_plugin
        LIBRARIES
        chain_plugin
        http_client_plugin
        signature_provider_manager_plugin
        appbase
        sysio_chain
        opp
)

target_include_directories(producer_plugin
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../chain_interface/include")

# subjective_exempt_db reads tables of these contracts under the names their headers share.
target_include_directories(producer_plugin PRIVATE
        "${CMAKE_SOURCE_DIR}/contracts/sysio.system/include"
        "${CMAKE_SOURCE_DIR}/contracts/sysio.opreg/include")

