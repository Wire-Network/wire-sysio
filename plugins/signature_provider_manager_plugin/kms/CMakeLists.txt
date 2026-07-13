# AWS KMS-backed signature-provider sub-library, co-located with
# signature_provider_manager_plugin but built as a separate static library.
# Carries the AWS-SDK dependency that we deliberately keep OUT of the plugin
# itself: host applications that want KMS-backed signing link this library
# AND call
#   plugin.register_spec_handler("KMS", &sysio::sigprov::kms::create_kms_provider);
# from main() before app().initialize(...). The plugin itself stays
# AWS-free, so binaries that don't need KMS don't pay the link or runtime
# cost.
set(TARGET_NAME sigprov_kms)

find_package(aws-cpp-sdk-kms CONFIG REQUIRED)

file(GLOB_RECURSE ${TARGET_NAME}_headers
        include/*.hpp include/*.h src/*.hpp src/*.h)

add_library(${TARGET_NAME}
        src/kms_signature_provider.cpp
        ${${TARGET_NAME}_headers})

target_include_directories(${TARGET_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME}
        PUBLIC
        # `fc` provides the crypto / public_key / private_key types named in
        # both the kms public header and the plugin's public header it
        # consumes for `sysio::provider_spec_result`.
        fc
        sysio_chain
        # Headers-only target from the parent plugin; carries the include
        # path for `signature_provider_manager_plugin.hpp` (which defines
        # `sysio::provider_spec_result` and the `spec_handler` typedef this
        # library targets) without pulling the plugin's whole archive (and
        # appbase / http_client / wallet_plugin) into the link line.
        signature_provider_manager_plugin_headers
        # Shared AWS glue (SDK lifecycle, per-region client cache,
        # transient-vs-permanent error split) co-owned with the ssm/ sibling.
        sigprov_aws_common
        aws-cpp-sdk-kms)

if (ENABLE_TESTS)
    add_subdirectory(test)
endif ()

install(
        TARGETS ${TARGET_NAME}
        LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}
)
