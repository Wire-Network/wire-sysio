file(GLOB HEADERS "include/sysio/signature_provider_plugin/*.hpp")
add_library( signature_provider_plugin
             signature_provider_plugin.cpp
             ${HEADERS} )

target_link_libraries( signature_provider_plugin custom_appbase fc http_client_plugin )
target_include_directories( signature_provider_plugin PUBLIC include )
