# AWS KMS-backed signature-provider plugin: supplies the `KMS:<key-ref>`
# scheme (remote Sign call per signature; the key never leaves AWS KMS;
# secp256k1/ethereum keys only). One target carries both the provider
# machinery and the appbase plugin class; enablement is per config via
# `plugin = sysio::signature_provider_kms_plugin`. Deliberately linked into
# nodeop only (see programs/nodeop/CMakeLists.txt), never into
# chain-tools.cmake's shared list, so sys-util and the outpost tools stay
# AWS-free.
find_package(aws-cpp-sdk-kms CONFIG REQUIRED)

plugin_target(
        signature_provider_kms_plugin

        LIBRARIES
        # The plugin class calls register_spec_handler / create_configured_providers
        # on the manager at initialize, so it links the manager's archive (which
        # also carries the include path for `sysio::provider_spec_result`).
        signature_provider_manager_plugin
        # Shared AWS glue (SDK lifecycle, per-region client cache,
        # transient-vs-permanent error split -- libraries/signature_provider_aws)
        # co-owned with the ssm sibling.
        signature_provider_aws
        aws-cpp-sdk-kms)

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)
