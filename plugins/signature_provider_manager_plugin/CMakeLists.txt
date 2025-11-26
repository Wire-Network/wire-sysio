file(GLOB HEADERS "include/sysio/signature_provider_manager_plugin/*.hpp")
add_library( signature_provider_manager_plugin
        signature_provider_manager_plugin.cpp
             ${HEADERS} )

target_link_libraries( signature_provider_manager_plugin custom_appbase fc http_client_plugin wallet_plugin_headers)
target_include_directories( signature_provider_manager_plugin PUBLIC include )
