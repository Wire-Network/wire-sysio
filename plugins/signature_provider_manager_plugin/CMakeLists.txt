set(TARGET_NAME signature_provider_manager_plugin)

# The plugin itself carries NO AWS dependency. KMS-backed signing is provided
# by the `sigprov_kms` sub-library (defined below), registered at runtime by
# the host application via
# `register_spec_handler("KMS", &sysio::sigprov::kms::create_kms_provider)`.
# The AWS-backed sub-libraries share this directory's src/ and include/ trees,
# so EXCLUDE_SOURCE_REGEX keeps their translation units out of the plugin's
# own archive. SKIP_TEST_CONFIG defers test/ to the bottom of this file: the
# test binaries link the sub-library targets, so those targets are defined
# first.
plugin_target(
        ${TARGET_NAME}

        SKIP_TEST_CONFIG

        SOURCE_GLOBS
        src/*.cpp
        src/*.hpp
        include/*.hpp

        EXCLUDE_SOURCE_REGEX
        "/(aws_common|kms_signature_provider|ssm_signature_provider)\\.(cpp|hpp)$"

        LIBRARIES
        custom_appbase
        http_client_plugin
        wallet_plugin_headers)

# Headers-only shim: external libraries that implement spec handlers (e.g.
# the sigprov_kms sub-library) need
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

# Shared AWS-SDK glue for the provider sub-libraries (sigprov_kms,
# sigprov_ssm), built as its own static library so both link one owner of the
# SDK lifecycle. Aws::InitAPI / Aws::ShutdownAPI must run exactly once per
# process; if each sub-library carried its own lifecycle singleton, a binary
# linking both would double-init / double-shutdown the SDK. This library also
# carries the shared per-region client cache, the transient-vs-permanent
# error split, and the service-agnostic ARN parsing pieces.
find_package(aws-cpp-sdk-core CONFIG REQUIRED)

add_library(sigprov_aws_common
        src/aws_common.cpp
        include/sysio/signature_provider_manager_plugin/aws_common.hpp)

target_include_directories(sigprov_aws_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(sigprov_aws_common
        PUBLIC
        # `fc` for the exception machinery named in the public header;
        # `sysio_chain` for the chain exception types the error split throws.
        fc
        sysio_chain
        aws-cpp-sdk-core)

# AWS KMS-backed signature-provider sub-library, co-located with
# signature_provider_manager_plugin but built as a separate static library.
# Carries the AWS-SDK dependency that we deliberately keep OUT of the plugin
# itself: host applications that want KMS-backed signing link this library
# AND call
#   plugin.register_spec_handler("KMS", &sysio::sigprov::kms::create_kms_provider);
# from main() before app().initialize(...). The plugin itself stays
# AWS-free, so binaries that don't need KMS don't pay the link or runtime
# cost.
find_package(aws-cpp-sdk-kms CONFIG REQUIRED)

add_library(sigprov_kms
        src/kms_signature_provider.cpp
        include/sysio/signature_provider_manager_plugin/kms_signature_provider.hpp)

target_include_directories(sigprov_kms PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(sigprov_kms
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
        # transient-vs-permanent error split) co-owned with the sigprov_ssm
        # sibling.
        sigprov_aws_common
        aws-cpp-sdk-kms)

# AWS SSM Parameter Store-backed signature-provider sub-library, co-located
# with signature_provider_manager_plugin but built as a separate static
# library. Carries the AWS-SDK dependency that we deliberately keep OUT of the
# plugin itself: host applications that want SSM-backed key loading link this
# library AND call
#   plugin.register_spec_handler("SSM", &sysio::sigprov::ssm::create_ssm_provider);
# from main() before app().initialize(...). nodeop does so; the plugin itself
# stays AWS-free, so binaries that don't need SSM don't pay the link or
# runtime cost.
find_package(aws-cpp-sdk-ssm CONFIG REQUIRED)

add_library(sigprov_ssm
        src/ssm_signature_provider.cpp
        include/sysio/signature_provider_manager_plugin/ssm_signature_provider.hpp)

target_include_directories(sigprov_ssm PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(sigprov_ssm
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
        # transient-vs-permanent error split) co-owned with the sigprov_kms
        # sibling.
        sigprov_aws_common
        aws-cpp-sdk-ssm)

# The sub-libraries are internal plugin support libraries: linked into nodeop,
# never shipped as standalone artifacts (see cmake/plugin-tools.cmake for
# rationale). Only the headers install. The single include/ tree here covers
# the plugin's own public header plus the provider headers -- the provider
# headers #include the plugin header, so they ship together.
install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)

if (ENABLE_TESTS)
    add_subdirectory(test)
endif ()
