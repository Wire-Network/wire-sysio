add_executable( test_chain_plugin
        test_account_query_db.cpp
        test_sync_gate.cpp
        test_trx_retry_db.cpp
        test_trx_finality_status_processing.cpp
        plugin_config_test.cpp
        main.cpp
        )
target_link_libraries( test_chain_plugin chain_plugin sysio_testing sysio_chain_wrap version )
set(CHAIN_PLUGIN_TEST_CASES
        chain_plugin_default_tests
        account_query_db_tests
        sync_gate_tests
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
