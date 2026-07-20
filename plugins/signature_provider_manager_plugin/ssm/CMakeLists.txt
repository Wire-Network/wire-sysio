# AWS SSM Parameter Store-backed signature-provider sub-library, co-located
# with signature_provider_manager_plugin but built as a separate static
# library. Carries the AWS-SDK dependency that we deliberately keep OUT of the
# plugin itself: host applications that want SSM-backed key loading link this
# library AND call
#   plugin.register_spec_handler("SSM", &sysio::sigprov::ssm::create_ssm_provider);
# from main() before app().initialize(...). nodeop does so; the plugin itself
# stays AWS-free, so binaries that don't need SSM don't pay the link or
# runtime cost.
set(TARGET_NAME sigprov_ssm)

find_package(aws-cpp-sdk-ssm CONFIG REQUIRED)

file(GLOB_RECURSE ${TARGET_NAME}_headers
        include/*.hpp include/*.h src/*.hpp src/*.h)

add_library(${TARGET_NAME}
        src/ssm_signature_provider.cpp
        ${${TARGET_NAME}_headers})

target_include_directories(${TARGET_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(${TARGET_NAME}
        PUBLIC
        # `fc` provides the crypto / public_key / private_key types named in
        # both the ssm public header and the plugin's public header it
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
        # transient-vs-permanent error split) co-owned with the kms/ sibling.
        sigprov_aws_common
        aws-cpp-sdk-ssm)

if (ENABLE_TESTS)
    add_subdirectory(test)
endif ()

# Internal plugin support library: linked into nodeop, never shipped as a
# standalone artifact (see cmake/plugin-tools.cmake for rationale).

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)
