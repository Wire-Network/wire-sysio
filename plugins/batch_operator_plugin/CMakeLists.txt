plugin_target(
        batch_operator_plugin
        LIBRARIES
        appbase
        chainbase
        chain_plugin
        cron_plugin
        signature_provider_manager_plugin
        outpost_client_plugin
        outpost_ethereum_client_plugin
        outpost_solana_client_plugin
        fc
        opp
        sysio_chain)

# The kick crank's tests decode table rows with the committed contract ABIs, the way the node renders them.
if(TARGET test_batch_operator_plugin)
   target_compile_definitions(test_batch_operator_plugin PRIVATE
           KICKER_ABI_PATH="${CMAKE_SOURCE_DIR}/contracts/sysio.kicker/sysio.kicker.abi"
           LIQ_ABI_PATH="${CMAKE_SOURCE_DIR}/contracts/sysio.liq/sysio.liq.abi")
endif()
