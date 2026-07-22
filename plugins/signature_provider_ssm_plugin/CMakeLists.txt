# AWS SSM Parameter Store-backed signature-provider plugin: supplies the
# `SSM:<param-ref>` scheme (private key fetched once from a KMS-encrypted
# SecureString at initialize, local signing thereafter -- suitable for every
# signing path including producer block signing). One target carries both the
# provider machinery and the appbase plugin class; enablement is per config
# via `plugin = sysio::signature_provider_ssm_plugin`. Deliberately linked
# into nodeop only (see programs/nodeop/CMakeLists.txt), never into
# chain-tools.cmake's shared list, so sys-util and the outpost tools stay
# AWS-free.
find_package(aws-cpp-sdk-ssm CONFIG REQUIRED)

plugin_target(
        signature_provider_ssm_plugin

        LIBRARIES
        # The plugin class calls register_spec_handler / create_configured_providers
        # on the manager at initialize, so it links the manager's archive (which
        # also carries the include path for `sysio::provider_spec_result`).
        signature_provider_manager_plugin
        # Shared AWS glue (SDK lifecycle, per-region client cache,
        # transient-vs-permanent error split -- libraries/signature_provider_aws)
        # co-owned with the kms sibling.
        signature_provider_aws
        aws-cpp-sdk-ssm)

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)
