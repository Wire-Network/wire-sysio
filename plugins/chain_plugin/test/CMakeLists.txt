add_executable( test_chain_plugin
        test_account_query_db.cpp
        test_trx_retry_db.cpp
        test_trx_finality_status_processing.cpp
        plugin_config_test.cpp
        main.cpp
        )
target_link_libraries( test_chain_plugin chain_plugin sysio_testing sysio_chain_wrap version )
target_include_directories(test_chain_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/unittests
        ${CMAKE_BINARY_DIR}/unittests/include)
set(CHAIN_PLUGIN_TEST_CASES
        chain_plugin_default_tests
        chain_plugin_accepts_attested_auto_fetched_snapshot
        chain_plugin_rejects_auto_fetched_snapshot_without_enabled_config
        chain_plugin_accepts_trusted_manual_snapshot_without_attestation_table
        snapshot_attestation_table_read_failure_policy
        snapshot_attestation_table_read_timeout_policy
        snapshot_attestation_table_read_retry_policy
        snapshot_attestation_required_schema_policy
        snapshot_attestation_enabled_config_policy
        snapshot_attestation_record_tuple_policy
        snapshot_endpoint_block_identity_policy
        chain_plugin_snapshot_endpoint_option_registration
        outbound_http_global_option_registration
        chain_plugin_snapshot_endpoint_option_validation
        account_query_db_tests
        trx_finality_status_processing_test
        trx_retry_db_test
        )

if("sys-vm-oc" IN_LIST SYSIO_WASM_RUNTIMES)
   list(APPEND CHAIN_PLUGIN_TEST_CASES
           chain_plugin_default_sys_vm_oc_whitelist
           chain_plugin_sys_vm_oc_whitelist
           )
endif()

foreach(TEST_CASE IN LISTS CHAIN_PLUGIN_TEST_CASES)
   add_p_test(
           NAME test_chain_plugin_${TEST_CASE}
           COMMAND plugins/chain_plugin/test/test_chain_plugin --run_test=${TEST_CASE} --report_level=detailed --color_output
           )
endforeach()
