set(TARGET_NAME signature_provider_manager_plugin)

find_package(aws-cpp-sdk-kms CONFIG REQUIRED)

plugin_target(
        ${TARGET_NAME}

        SOURCE_GLOBS
        src/*.cpp
        src/*.hpp
        include/*.hpp

        LIBRARIES
        custom_appbase
        http_client_plugin
        wallet_plugin_headers
        aws-cpp-sdk-kms)

