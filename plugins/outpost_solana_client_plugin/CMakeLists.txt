set(TARGET_LIB_NAME outpost_solana_client_plugin)

plugin_target(
        ${TARGET_LIB_NAME}
        LIBRARIES
        chain_plugin
        fc
        fc-lite
        JsonCpp::JsonCpp
        CURL::libcurl
        boringssl::ssl
        boringssl::crypto
        outpost_client_plugin
        signature_provider_manager_plugin
)

