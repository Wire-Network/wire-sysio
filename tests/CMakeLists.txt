include_directories( "${CMAKE_SOURCE_DIR}/plugins/wallet_plugin/include" )

file(GLOB UNIT_TESTS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*.cpp")
list(REMOVE_ITEM UNIT_TESTS ship_client.cpp)
list(REMOVE_ITEM UNIT_TESTS ship_streamer.cpp)

add_executable( plugin_test ${UNIT_TESTS} )
target_link_libraries( plugin_test sysio_testing sysio_chain chainbase chain_plugin wallet_plugin fc state_history ${PLATFORM_SPECIFIC_LIBS} )

target_include_directories( plugin_test PUBLIC
                            ${CMAKE_SOURCE_DIR}/plugins/net_plugin/include
                            ${CMAKE_SOURCE_DIR}/plugins/chain_plugin/include
                            ${CMAKE_SOURCE_DIR}/unittests
                            ${CMAKE_BINARY_DIR}/unittests/include/ )

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/core_symbol.py.in ${CMAKE_CURRENT_BINARY_DIR}/core_symbol.py)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/testUtils.py ${CMAKE_CURRENT_BINARY_DIR}/testUtils.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/WalletMgr.py ${CMAKE_CURRENT_BINARY_DIR}/WalletMgr.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Node.py ${CMAKE_CURRENT_BINARY_DIR}/Node.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Cluster.py ${CMAKE_CURRENT_BINARY_DIR}/Cluster.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/TestHelper.py ${CMAKE_CURRENT_BINARY_DIR}/TestHelper.py COPYONLY)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_tests/dawn_515/test.sh ${CMAKE_CURRENT_BINARY_DIR}/p2p_tests/dawn_515/test.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/block_log_util_test.py ${CMAKE_CURRENT_BINARY_DIR}/block_log_util_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/distributed-transactions-test.py ${CMAKE_CURRENT_BINARY_DIR}/distributed-transactions-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/distributed-transactions-remote-test.py ${CMAKE_CURRENT_BINARY_DIR}/distributed-transactions-remote-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sample-cluster-map.json ${CMAKE_CURRENT_BINARY_DIR}/sample-cluster-map.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/restart-scenarios-test.py ${CMAKE_CURRENT_BINARY_DIR}/restart-scenarios-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/terminate-scenarios-test.py ${CMAKE_CURRENT_BINARY_DIR}/terminate-scenarios-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_startup_catchup.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_startup_catchup.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_forked_chain_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_forked_chain_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_short_fork_take_over_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_short_fork_take_over_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_run_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_run_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_run_remote_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_run_remote_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_under_min_avail_ram.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_under_min_avail_ram.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_voting_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_voting_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_irreversible_mode_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_irreversible_mode_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_read_terminate_at_block_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_read_terminate_at_block_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_chainbase_allocation_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_chainbase_allocation_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_protocol_feature_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_protocol_feature_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_multiple_version_protocol_feature_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_multiple_version_protocol_feature_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/validate-dirty-db.py ${CMAKE_CURRENT_BINARY_DIR}/validate-dirty-db.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/launcher_test.py ${CMAKE_CURRENT_BINARY_DIR}/launcher_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/kiod_auto_launch_test.py ${CMAKE_CURRENT_BINARY_DIR}/kiod_auto_launch_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/db_modes_test.sh ${CMAKE_CURRENT_BINARY_DIR}/db_modes_test.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/prod_preactivation_test.py ${CMAKE_CURRENT_BINARY_DIR}/prod_preactivation_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/release-build.sh ${CMAKE_CURRENT_BINARY_DIR}/release-build.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version-label.sh ${CMAKE_CURRENT_BINARY_DIR}/version-label.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/full-version-label.sh ${CMAKE_CURRENT_BINARY_DIR}/full-version-label.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_producer_watermark_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_producer_watermark_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py ${CMAKE_CURRENT_BINARY_DIR}/cli_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ship_test.py ${CMAKE_CURRENT_BINARY_DIR}/ship_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ship_streamer_test.py ${CMAKE_CURRENT_BINARY_DIR}/ship_streamer_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/large-lib-test.py ${CMAKE_CURRENT_BINARY_DIR}/large-lib-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/http_plugin_test.py ${CMAKE_CURRENT_BINARY_DIR}/http_plugin_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_high_latency_test.py ${CMAKE_CURRENT_BINARY_DIR}/p2p_high_latency_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/compute_transaction_test.py ${CMAKE_CURRENT_BINARY_DIR}/compute_transaction_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/subjective_billing_test.py ${CMAKE_CURRENT_BINARY_DIR}/subjective_billing_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/get_account_test.py ${CMAKE_CURRENT_BINARY_DIR}/get_account_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_high_transaction_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_high_transaction_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_retry_transaction_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_retry_transaction_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/trx_finality_status_test.py ${CMAKE_CURRENT_BINARY_DIR}/trx_finality_status_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/trx_finality_status_forked_test.py ${CMAKE_CURRENT_BINARY_DIR}/trx_finality_status_forked_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/plugin_http_api_test.py ${CMAKE_CURRENT_BINARY_DIR}/plugin_http_api_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_contrl_c_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_contrl_c_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource_monitor_plugin_test.py ${CMAKE_CURRENT_BINARY_DIR}/resource_monitor_plugin_test.py COPYONLY)

#To run plugin_test with all log from blockchain displayed, put --verbose after --, i.e. plugin_test -- --verbose
add_test(NAME plugin_test COMMAND plugin_test --report_level=detailed --color_output)

add_test(NAME nodeop_sanity_test COMMAND tests/nodeop_run_test.py -v --sanity-test --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_sanity_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME nodeop_run_test COMMAND tests/nodeop_run_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_run_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME block_log_util_test COMMAND tests/block_log_util_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(block_log_util_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

option(ABISYS_ONLY_LIBRARY "define and build the ABISYS library" ON)
set(ABISYS_INSTALL_COMPONENT "dev")
set(SKIP_SUBMODULE_TEST ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE "StopAbieosFromTouchingThis")
   add_subdirectory(abieos)
   unset(CMAKE_BUILD_TYPE)
else()
   add_subdirectory(abieos)
endif()

find_package(Threads)
add_executable(ship_client ship_client.cpp)
target_link_libraries(ship_client abieos Boost::program_options Boost::system Threads::Threads)
add_executable(ship_streamer ship_streamer.cpp)
target_link_libraries(ship_streamer abieos Boost::program_options Boost::system Threads::Threads)

add_test(NAME ship_test COMMAND tests/ship_test.py -v --num-clients 1 --num-requests 5000 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(ship_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME ship_streamer_test COMMAND tests/ship_streamer_test.py -v --num-clients 1 --num-blocks 50 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(ship_streamer_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME p2p_dawn515_test COMMAND tests/p2p_tests/dawn_515/test.sh WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(p2p_dawn515_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

# Disable producer-preactivate-feature-test because it verifies unsupported legacy functionality
#add_test(NAME producer-preactivate-feature-test COMMAND tests/prod_preactivation_test.py --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
#set_tests_properties(producer-preactivate-feature-test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_protocol_feature_test COMMAND tests/nodeop_protocol_feature_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_protocol_feature_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME compute_transaction_test COMMAND tests/compute_transaction_test.py -v -p 2 -n 3 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(compute_transaction_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME subjective_billing_test COMMAND tests/subjective_billing_test.py -v -p 2 -n 4 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(subjective_billing_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME get_account_test COMMAND tests/get_account_test.py -v -p 2 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(get_account_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME distributed-transactions-test COMMAND tests/distributed-transactions-test.py -d 2 -p 4 -n 6 -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(distributed-transactions-test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME restart-scenarios-test-resync COMMAND tests/restart-scenarios-test.py -c resync -p4 -v --clean-run --dump-error-details WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(restart-scenarios-test-resync PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME restart-scenarios-test-hard_replay COMMAND tests/restart-scenarios-test.py -c hardReplay -p4 -v --clean-run --dump-error-details WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(restart-scenarios-test-hard_replay PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME restart-scenarios-test-none COMMAND tests/restart-scenarios-test.py -c none --kill-sig term -p4 -v --clean-run --dump-error-details WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(restart-scenarios-test-none PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME terminate-scenarios-test-resync COMMAND tests/terminate-scenarios-test.py -p 1 -c resync --kill-count 1 --terminate-at-block 10 --kill-sig term --dump-error-details WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(terminate-scenarios-test-resync PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME terminate-scenarios-test-replay COMMAND tests/terminate-scenarios-test.py -p 1 -c replay --kill-count 1 --terminate-at-block 10 --kill-sig term --dump-error-details WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(terminate-scenarios-test-replay PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME terminate-scenarios-test-hard_replay COMMAND tests/terminate-scenarios-test.py -p 1 -c hardReplay --kill-count 1 --terminate-at-block 10 --kill-sig term --dump-error-details WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(terminate-scenarios-test-hard_replay PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME validate_dirty_db_test COMMAND tests/validate-dirty-db.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(validate_dirty_db_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME launcher_test COMMAND tests/launcher_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(launcher_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME kiod_auto_launch_test COMMAND tests/kiod_auto_launch_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(kiod_auto_launch_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME trx_finality_status_test COMMAND tests/trx_finality_status_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(trx_finality_status_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME trx_finality_status_forked_test COMMAND tests/trx_finality_status_forked_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(trx_finality_status_forked_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME db_modes_test COMMAND tests/db_modes_test.sh WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(db_modes_test PROPERTIES COST 6000)
add_test(NAME release-build-test COMMAND tests/release-build.sh WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME version-label-test COMMAND tests/version-label.sh "v${VERSION_FULL}" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME full-version-label-test COMMAND tests/full-version-label.sh "v${VERSION_FULL}" ${CMAKE_SOURCE_DIR} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Long running tests
add_test(NAME nodeop_sanity_lr_test COMMAND tests/nodeop_run_test.py -v --sanity-test --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_sanity_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
add_test(NAME nodeop_run_check_lr_test COMMAND tests/nodeop_run_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_run_check_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
add_test(NAME nodeop_remote_lr_test COMMAND tests/nodeop_run_remote_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_remote_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

# needs iproute-tc or iproute2 depending on platform
#add_test(NAME p2p_high_latency_test COMMAND tests/p2p_high_latency_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
#set_tests_properties(p2p_high_latency_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

#add_test(NAME distributed_transactions_lr_test COMMAND tests/distributed-transactions-test.py -d 2 -p 21 -n 21 -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
#set_property(TEST distributed_transactions_lr_test PROPERTY LABELS long_running_tests)

add_test(NAME nodeop_forked_chain_lr_test COMMAND tests/nodeop_forked_chain_test.py -v --wallet-port 9901 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_forked_chain_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_contrl_c_lr_test COMMAND tests/nodeop_contrl_c_test.py -v --wallet-port 9901 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_contrl_c_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_voting_lr_test COMMAND tests/nodeop_voting_test.py -v --wallet-port 9902 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_voting_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_under_min_avail_ram_lr_test COMMAND tests/nodeop_under_min_avail_ram.py -v --wallet-port 9904 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_under_min_avail_ram_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE TIMEOUT 3000)

add_test(NAME nodeop_irreversible_mode_lr_test COMMAND tests/nodeop_irreversible_mode_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_irreversible_mode_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_read_terminate_at_block_lr_test COMMAND tests/nodeop_read_terminate_at_block_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_read_terminate_at_block_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_chainbase_allocation_lr_test COMMAND tests/nodeop_chainbase_allocation_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_chainbase_allocation_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_startup_catchup_lr_test COMMAND tests/nodeop_startup_catchup.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_startup_catchup_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE TIMEOUT 3000)

add_test(NAME nodeop_short_fork_take_over_lr_test COMMAND tests/nodeop_short_fork_take_over_test.py -v --wallet-port 9905 --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_short_fork_take_over_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

if(ENABLE_MULTIVERSION_PROTOCOL_TEST)
  set(ALTERNATE_VERSION_LABELS_FILE "${CMAKE_BINARY_DIR}/tests/multiversion_paths.conf")

  add_test(NAME nodeop_multiple_version_protocol_feature_mv_test COMMAND tests/nodeop_multiple_version_protocol_feature_test.py
           -v --clean-run --dump-error-detail --alternate-version-labels-file ${ALTERNATE_VERSION_LABELS_FILE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_property(TEST nodeop_multiple_version_protocol_feature_mv_test PROPERTY LABELS mixed_version_tests)
endif()

add_test(NAME nodeop_producer_watermark_lr_test COMMAND tests/nodeop_producer_watermark_test.py -v --clean-run --dump-error-detail WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_producer_watermark_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_high_transaction_lr_test COMMAND tests/nodeop_high_transaction_test.py -v --clean-run --dump-error-detail -p 4 -n 8 --num-transactions 10000 --max-transactions-per-second 500 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_high_transaction_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_retry_transaction_lr_test COMMAND tests/nodeop_retry_transaction_test.py -v --clean-run --dump-error-detail --num-transactions 100 --max-transactions-per-second 10 --total-accounts 5 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_retry_transaction_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)


add_test(NAME cli_test COMMAND tests/cli_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME larger_lib_test COMMAND tests/large-lib-test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(larger_lib_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME http_plugin_test COMMAND tests/http_plugin_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(http_plugin_test PROPERTIES TIMEOUT 100)
set_tests_properties(http_plugin_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME plugin_http_api_test COMMAND tests/plugin_http_api_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_http_api_test PROPERTIES TIMEOUT 40)
set_tests_properties(plugin_http_api_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME resource_monitor_plugin_test COMMAND tests/resource_monitor_plugin_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(resource_monitor_plugin_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

if(ENABLE_COVERAGE_TESTING)

  set(Coverage_NAME ${PROJECT_NAME}_coverage)

  if(NOT LCOV_PATH)
    message(FATAL_ERROR "lcov not found! Aborting...")
  endif() # NOT LCOV_PATH

  if(NOT LLVMCOV_PATH)
    message(FATAL_ERROR "llvm-cov not found! Aborting...")
  endif()

  if(NOT GENHTML_PATH)
    message(FATAL_ERROR "genhtml not found! Aborting...")
  endif() # NOT GENHTML_PATH

  # no spaces allowed within tests list
  set(ctest_tests 'plugin_test|p2p_dawn515_test|nodeop_run_test|distributed-transactions-test|restart-scenarios-test_resync')
  set(ctest_exclude_tests 'nodeop_run_remote_test|distributed-transactions-remote-test|restart-scenarios-test_replay')

  # Setup target
  add_custom_target(${Coverage_NAME}

    COMMAND ${LCOV_PATH} --directory . --zerocounters

    COMMAND ctest -R ${ctest_tests} -E ${ctest_exclude_tests}

    COMMENT "Capturing lcov counters and generating report"
    COMMAND ${LCOV_PATH} --directory . --capture --gcov-tool ${CMAKE_SOURCE_DIR}/tools/llvm-gcov.sh --output-file ${Coverage_NAME}.info

    COMMENT "Processing code coverage counters and generating report."
    COMMAND ${GENHTML_PATH} -o ${Coverage_NAME} ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info
    COMMAND ${CMAKE_COMMAND} -E remove ${Coverage_NAME}.base ${Coverage_NAME}.info ${Coverage_NAME}.total ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned

    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    COMMENT "Resetting code coverage counters to zero. Processing code coverage counters and generating report."
    )

  # Show info where to find the report
  add_custom_command(TARGET ${Coverage_NAME} POST_BUILD
    COMMAND ;
    COMMENT "Open ./${Coverage_NAME}/index.html in your browser to view the coverage report."
    )
endif()
