#cmake_minimum_required(VERSION 3.16)

#file(GLOB_RECURSE SRC_FILES src/*.cpp)
set(TARGET_NAME outpost_client_plugin)

plugin_target(
        ${TARGET_NAME}
        LIBRARIES
        libsodium::libsodium
        Boost::asio
        Boost::beast

        appbase
        chainbase
        chain_plugin
        
        fc-lite
        fc
        
        signature_provider_manager_plugin
        sysio_chain
        )
