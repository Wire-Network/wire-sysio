# The manager plugin carries NO AWS dependency. The built-in `KEY:` / `KIOD:`
# schemes live here; the AWS-backed `SSM:` / `KMS:` schemes are supplied by
# the sibling signature_provider_ssm_plugin / signature_provider_kms_plugin,
# which register their scheme handler from their constructor and are enabled
# via `plugin = ...`. The manager creates every configured provider at its own
# plugin_initialize, gated on the owning plugin being enabled; a spec whose
# plugin is not enabled fails the boot with an error naming the plugin to add.
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
