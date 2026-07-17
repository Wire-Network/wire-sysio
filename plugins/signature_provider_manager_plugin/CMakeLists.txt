# The manager plugin carries NO AWS dependency. The built-in `KEY:` / `KIOD:`
# schemes live here; the AWS-backed `SSM:` / `KMS:` schemes are supplied by
# the sibling signature_provider_ssm_plugin / signature_provider_kms_plugin,
# which register their spec handler and claim their configured specs when
# enabled via `plugin = ...`. A spec whose scheme has no handler at parse
# time is retained for the owning plugin to claim; specs still unclaimed at
# startup fail the boot with an error naming the plugin to enable.
plugin_target(
        signature_provider_manager_plugin

        LIBRARIES
        custom_appbase
        http_client_plugin
        wallet_plugin_headers)

install(
        DIRECTORY include/sysio
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
)
