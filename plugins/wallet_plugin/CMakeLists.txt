plugin_target(
        wallet_plugin
        LIBRARIES
        sysio_chain
        custom_appbase
)

if (APPLE)
    target_link_libraries(
            wallet_plugin
            PUBLIC
            ${security_framework}
            ${corefoundation_framework}
            ${localauthentication_framework}
            ${cocoa_framework}
    )
endif ()

# shim that forwards only include dirs
add_library(wallet_plugin_headers INTERFACE)
target_link_libraries(wallet_plugin_headers INTERFACE wallet_plugin)