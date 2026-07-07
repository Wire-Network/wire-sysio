set(TARGET_NAME signature_provider_manager_plugin)

# The plugin itself carries NO AWS dependency. KMS-backed signing is provided
# by the `kms/` sub-library, registered at runtime by the host application via
# `register_spec_handler("KMS", &sysio::sigprov::kms::create_kms_provider)`.
plugin_target(
        ${TARGET_NAME}

        SOURCE_GLOBS
        src/*.cpp
        src/*.hpp
        include/*.hpp

        LIBRARIES
        custom_appbase
        http_client_plugin
        wallet_plugin_headers)

# Headers-only shim: external libraries that implement spec handlers (e.g.
# the kms sub-library) need
# `sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp`
# at compile time so they can return the public `sysio::provider_spec_result`
# type. Pulling the plugin's whole-archive into those libraries' link line
# would drag appbase, http_client_plugin, and the rest into anyone who only
# wanted the extension types. This INTERFACE target carries:
#   - the plugin's own include path,
#   - the http_client_plugin include path (the plugin's public header
#     transitively #includes <sysio/http_client_plugin/http_client_plugin.hpp>
#     because of `APPBASE_PLUGIN_REQUIRES((http_client_plugin))`; we expose
#     only the headers here, never the static archive),
#   - the `custom_appbase` INTERFACE library (provides
#     <sysio/chain/application.hpp> + appbase + fc-lite), and
#   - `fc` for the crypto types referenced in `provider_spec_result`.
# Anything that needs to *call* the plugin or run appbase machinery still
# links `signature_provider_manager_plugin` itself.
add_library(signature_provider_manager_plugin_headers INTERFACE)
target_include_directories(signature_provider_manager_plugin_headers
        INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/plugins/http_client_plugin/include>)
target_link_libraries(signature_provider_manager_plugin_headers
        INTERFACE
        custom_appbase
        fc)

add_subdirectory(kms)
