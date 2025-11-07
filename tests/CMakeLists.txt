include_directories( "${CMAKE_SOURCE_DIR}/plugins/wallet_plugin/include" )

if( DONT_SKIP_TESTS )
  add_compile_definitions(RUN_FAILING_TESTS=TRUE)
endif()

file(GLOB UNIT_TESTS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*.cpp")
list(REMOVE_ITEM UNIT_TESTS ship_client.cpp)
list(REMOVE_ITEM UNIT_TESTS ship_streamer.cpp)

add_executable( plugin_test ${UNIT_TESTS} )
target_link_libraries( plugin_test sysio_testing sysio_chain_wrap chainbase chain_plugin producer_plugin wallet_plugin fc state_history ${PLATFORM_SPECIFIC_LIBS} )

target_include_directories( plugin_test PUBLIC
                            ${CMAKE_SOURCE_DIR}/plugins/net_plugin/include
                            ${CMAKE_SOURCE_DIR}/plugins/chain_plugin/include
                            ${CMAKE_SOURCE_DIR}/unittests
                            ${CMAKE_BINARY_DIR}/unittests/include/ )

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_tests/dawn_515/test.sh ${CMAKE_CURRENT_BINARY_DIR}/p2p_tests/dawn_515/test.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/block_log_util_test.py ${CMAKE_CURRENT_BINARY_DIR}/block_log_util_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/block_log_retain_blocks_test.py ${CMAKE_CURRENT_BINARY_DIR}/block_log_retain_blocks_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/bridge_for_fork_test_shape.json ${CMAKE_CURRENT_BINARY_DIR}/bridge_for_fork_test_shape.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cluster_launcher.py ${CMAKE_CURRENT_BINARY_DIR}/cluster_launcher.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/distributed-transactions-test.py ${CMAKE_CURRENT_BINARY_DIR}/distributed-transactions-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sample-cluster-map.json ${CMAKE_CURRENT_BINARY_DIR}/sample-cluster-map.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/restart-scenarios-test.py ${CMAKE_CURRENT_BINARY_DIR}/restart-scenarios-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/terminate-scenarios-test.py ${CMAKE_CURRENT_BINARY_DIR}/terminate-scenarios-test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_startup_catchup.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_startup_catchup.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_snapshot_diff_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_snapshot_diff_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_snapshot_forked_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_snapshot_forked_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_forked_chain_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_forked_chain_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_short_fork_take_over_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_short_fork_take_over_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_run_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_run_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_under_min_avail_ram.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_under_min_avail_ram.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_irreversible_mode_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_irreversible_mode_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_read_terminate_at_block_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_read_terminate_at_block_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_chainbase_allocation_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_chainbase_allocation_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_protocol_feature_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_protocol_feature_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_multiple_version_protocol_feature_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_multiple_version_protocol_feature_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_extra_packed_data_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_extra_packed_data_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/validate-dirty-db.py ${CMAKE_CURRENT_BINARY_DIR}/validate-dirty-db.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/kiod_auto_launch_test.py ${CMAKE_CURRENT_BINARY_DIR}/kiod_auto_launch_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/db_modes_test.sh ${CMAKE_CURRENT_BINARY_DIR}/db_modes_test.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/release-build.sh ${CMAKE_CURRENT_BINARY_DIR}/release-build.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version-label.sh ${CMAKE_CURRENT_BINARY_DIR}/version-label.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/full-version-label.sh ${CMAKE_CURRENT_BINARY_DIR}/full-version-label.sh COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_producer_watermark_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_producer_watermark_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py ${CMAKE_CURRENT_BINARY_DIR}/cli_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ship_test.py ${CMAKE_CURRENT_BINARY_DIR}/ship_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ship_streamer_test.py ${CMAKE_CURRENT_BINARY_DIR}/ship_streamer_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/lib_advance_test.py ${CMAKE_CURRENT_BINARY_DIR}/lib_advance_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/http_plugin_test.py ${CMAKE_CURRENT_BINARY_DIR}/http_plugin_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_high_latency_test.py ${CMAKE_CURRENT_BINARY_DIR}/p2p_high_latency_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_multiple_listen_test.py ${CMAKE_CURRENT_BINARY_DIR}/p2p_multiple_listen_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_no_listen_test.py ${CMAKE_CURRENT_BINARY_DIR}/p2p_no_listen_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_sync_throttle_test.py ${CMAKE_CURRENT_BINARY_DIR}/p2p_sync_throttle_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/p2p_sync_throttle_test_shape.json ${CMAKE_CURRENT_BINARY_DIR}/p2p_sync_throttle_test_shape.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/compute_transaction_test.py ${CMAKE_CURRENT_BINARY_DIR}/compute_transaction_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/subjective_billing_test.py ${CMAKE_CURRENT_BINARY_DIR}/subjective_billing_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/get_account_test.py ${CMAKE_CURRENT_BINARY_DIR}/get_account_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_high_transaction_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_high_transaction_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_retry_transaction_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_retry_transaction_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/trx_finality_status_test.py ${CMAKE_CURRENT_BINARY_DIR}/trx_finality_status_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/trx_finality_status_forked_test.py ${CMAKE_CURRENT_BINARY_DIR}/trx_finality_status_forked_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/plugin_http_api_test.py ${CMAKE_CURRENT_BINARY_DIR}/plugin_http_api_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nodeop_contrl_c_test.py ${CMAKE_CURRENT_BINARY_DIR}/nodeop_contrl_c_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/read_only_trx_test.py ${CMAKE_CURRENT_BINARY_DIR}/read_only_trx_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource_monitor_plugin_test.py ${CMAKE_CURRENT_BINARY_DIR}/resource_monitor_plugin_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/light_validation_sync_test.py ${CMAKE_CURRENT_BINARY_DIR}/light_validation_sync_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/trace_plugin_test.py ${CMAKE_CURRENT_BINARY_DIR}/trace_plugin_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nested_container_multi_index_test.py ${CMAKE_CURRENT_BINARY_DIR}/nested_container_multi_index_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/auto_bp_peering_test.py ${CMAKE_CURRENT_BINARY_DIR}/auto_bp_peering_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/auto_bp_peering_test_shape.json ${CMAKE_CURRENT_BINARY_DIR}/auto_bp_peering_test_shape.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/gelf_test.py ${CMAKE_CURRENT_BINARY_DIR}/gelf_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/split_blocklog_replay_test.py ${CMAKE_CURRENT_BINARY_DIR}/split_blocklog_replay_test.py COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/PerformanceHarnessScenarioRunner.py ${CMAKE_CURRENT_BINARY_DIR}/PerformanceHarnessScenarioRunner.py COPYONLY)

if(DEFINED ENV{GITHUB_ACTIONS})
  set(UNSHARE "--unshared")
else()
  set(UNSHARE "")
endif()

#To run plugin_test with all log from blockchain displayed, put --verbose after --, i.e. plugin_test -- --verbose
add_test(NAME plugin_test COMMAND plugin_test --report_level=detailed --color_output)

add_test(NAME nodeop_sanity_test COMMAND tests/nodeop_run_test.py -v --sanity-test ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_sanity_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME nodeop_run_test COMMAND tests/nodeop_run_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_run_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME block_log_util_test COMMAND tests/block_log_util_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(block_log_util_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
add_test(NAME block_log_retain_blocks_test COMMAND tests/block_log_retain_blocks_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(block_log_retain_blocks_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

option(ABIEOS_ONLY_LIBRARY "define and build the ABIEOS library" ON)
set(ABIEOS_INSTALL_COMPONENT "dev")
set(SKIP_SUBMODULE_TEST ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE "StopAbieosFromTouchingThis")
   add_subdirectory(abieos)
   unset(CMAKE_BUILD_TYPE)
else()
   add_subdirectory(abieos)
endif()

add_subdirectory( TestHarness )
add_subdirectory( trx_generator )
add_subdirectory( PerformanceHarness )

find_package(Threads)
add_executable(ship_client ship_client.cpp)
target_link_libraries(ship_client abieos Boost::program_options Boost::system Boost::algorithm Boost::asio Boost::beast Threads::Threads)
add_executable(ship_streamer ship_streamer.cpp)
target_link_libraries(ship_streamer abieos Boost::program_options Boost::system Boost::asio Boost::beast Threads::Threads)

add_test(NAME cluster_launcher COMMAND tests/cluster_launcher.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(cluster_launcher PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME ship_test COMMAND tests/ship_test.py -v --num-clients 10 --num-requests 5000 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(ship_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
add_test(NAME ship_test_unix COMMAND tests/ship_test.py -v --num-clients 10 --num-requests 5000 ${UNSHARE} --unix-socket WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(ship_test_unix PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME ship_streamer_test COMMAND tests/ship_streamer_test.py -v --num-clients 10 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(ship_streamer_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME p2p_dawn515_test COMMAND tests/p2p_tests/dawn_515/test.sh WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(p2p_dawn515_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_protocol_feature_test COMMAND tests/nodeop_protocol_feature_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_protocol_feature_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME compute_transaction_test COMMAND tests/compute_transaction_test.py -v -p 2 -n 3 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(compute_transaction_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME read-only-trx-basic-test COMMAND tests/read_only_trx_test.py -p 2 -n 3 --read-only-threads 1 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST read-only-trx-basic-test PROPERTY LABELS nonparallelizable_tests)
add_test(NAME read-only-trx-parallel-test COMMAND tests/read_only_trx_test.py -p 2 -n 3 --read-only-threads 16 --num-test-runs 3 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST read-only-trx-parallel-test PROPERTY LABELS long_running_tests)
add_test(NAME read-only-trx-parallel-sys-vm-oc-test COMMAND tests/read_only_trx_test.py -p 2 -n 3 --sys-vm-oc-enable all --read-only-threads 16 --num-test-runs 3 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST read-only-trx-parallel-sys-vm-oc-test PROPERTY LABELS long_running_tests)
add_test(NAME read-only-trx-parallel-no-oc-test COMMAND tests/read_only_trx_test.py -p 2 -n 3 --sys-vm-oc-enable none --read-only-threads 6 --num-test-runs 2 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST read-only-trx-parallel-no-oc-test PROPERTY LABELS long_running_tests)
add_test(NAME subjective_billing_test COMMAND tests/subjective_billing_test.py -v -p 2 -n 4 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(subjective_billing_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME get_account_test COMMAND tests/get_account_test.py -v -p 2 -n 3 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(get_account_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME distributed-transactions-test COMMAND tests/distributed-transactions-test.py -d 2 -p 4 -n 6 -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST distributed-transactions-test PROPERTY LABELS long_running_tests)
add_test(NAME distributed-transactions-speculative-test COMMAND tests/distributed-transactions-test.py -d 2 -p 4 -n 6 --speculative -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST distributed-transactions-speculative-test PROPERTY LABELS nonparallelizable_tests)
add_test(NAME restart-scenarios-test-resync COMMAND tests/restart-scenarios-test.py -c resync -p4 -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST restart-scenarios-test-resync PROPERTY LABELS long_running_tests)
add_test(NAME restart-scenarios-test-hard_replay COMMAND tests/restart-scenarios-test.py -c hardReplay -p4 -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST restart-scenarios-test-hard_replay PROPERTY LABELS nonparallelizable_tests)
add_test(NAME restart-scenarios-test-none COMMAND tests/restart-scenarios-test.py -c none --kill-sig term -p4 -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST restart-scenarios-test-none PROPERTY LABELS long_running_tests)
add_test(NAME terminate-scenarios-test-resync COMMAND tests/terminate-scenarios-test.py -c resync --terminate-at-block 10 --kill-sig term ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST terminate-scenarios-test-resync PROPERTY LABELS long_running_tests)
add_test(NAME terminate-scenarios-test-replay COMMAND tests/terminate-scenarios-test.py -c replay --terminate-at-block 10 --kill-sig term ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST terminate-scenarios-test-replay PROPERTY LABELS nonparallelizable_tests)
add_test(NAME terminate-scenarios-test-hard_replay COMMAND tests/terminate-scenarios-test.py -c hardReplay --terminate-at-block 10 --kill-sig term ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST terminate-scenarios-test-hard_replay PROPERTY LABELS long_running_tests)
add_test(NAME validate_dirty_db_test COMMAND tests/validate-dirty-db.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(validate_dirty_db_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME kiod_auto_launch_test COMMAND tests/kiod_auto_launch_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(kiod_auto_launch_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME nodeop_snapshot_diff_test COMMAND tests/nodeop_snapshot_diff_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_snapshot_diff_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
add_test(NAME nodeop_snapshot_forked_test COMMAND tests/nodeop_snapshot_forked_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_snapshot_forked_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME trx_finality_status_test COMMAND tests/trx_finality_status_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(trx_finality_status_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME trx_finality_status_forked_test COMMAND tests/trx_finality_status_forked_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(trx_finality_status_forked_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME db_modes_test COMMAND tests/db_modes_test.sh -v WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(db_modes_test PROPERTIES COST 6000)
add_test(NAME release-build-test COMMAND tests/release-build.sh WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME version-label-test COMMAND tests/version-label.sh "v${VERSION_FULL}" WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME full-version-label-test COMMAND tests/full-version-label.sh "v${VERSION_FULL}" ${CMAKE_SOURCE_DIR} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME nested_container_multi_index_test COMMAND tests/nested_container_multi_index_test.py -n 2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nested_container_multi_index_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME p2p_multiple_listen_test COMMAND tests/p2p_multiple_listen_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(p2p_multiple_listen_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
add_test(NAME p2p_no_listen_test COMMAND tests/p2p_no_listen_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(p2p_no_listen_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
add_test(NAME p2p_sync_throttle_test COMMAND tests/p2p_sync_throttle_test.py -v -d 2 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(p2p_sync_throttle_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

# needs iproute-tc or iproute2 depending on platform
#add_test(NAME p2p_high_latency_test COMMAND tests/p2p_high_latency_test.py -v WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
#set_tests_properties(p2p_high_latency_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

#add_test(NAME distributed_transactions_lr_test COMMAND tests/distributed-transactions-test.py -d 2 -p 21 -n 21 -v WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
#set_tests_properties(distributed_transactions_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_forked_chain_lr_test COMMAND tests/nodeop_forked_chain_test.py -v --wallet-port 9901 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_forked_chain_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_contrl_c_test COMMAND tests/nodeop_contrl_c_test.py -v --wallet-port 9901 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_contrl_c_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_under_min_avail_ram_lr_test COMMAND tests/nodeop_under_min_avail_ram.py -v --wallet-port 9904 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_under_min_avail_ram_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_irreversible_mode_lr_test COMMAND tests/nodeop_irreversible_mode_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_irreversible_mode_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_read_terminate_at_block_lr_test COMMAND tests/nodeop_read_terminate_at_block_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_read_terminate_at_block_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_chainbase_allocation_test COMMAND tests/nodeop_chainbase_allocation_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_chainbase_allocation_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_startup_catchup_lr_test COMMAND tests/nodeop_startup_catchup.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_startup_catchup_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_short_fork_take_over_test COMMAND tests/nodeop_short_fork_take_over_test.py -v --wallet-port 9905 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_short_fork_take_over_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_extra_packed_data_test COMMAND tests/nodeop_extra_packed_data_test.py -v -p 2 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_extra_packed_data_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_producer_watermark_lr_test COMMAND tests/nodeop_producer_watermark_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_producer_watermark_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_high_transaction_lr_test COMMAND tests/nodeop_high_transaction_test.py -v -p 4 -n 8 --num-transactions 10000 --max-transactions-per-second 500 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_high_transaction_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_retry_transaction_lr_test COMMAND tests/nodeop_retry_transaction_test.py -v --num-transactions 100 --max-transactions-per-second 10 --total-accounts 5 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_retry_transaction_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME cli_test COMMAND tests/cli_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(cli_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME lib_advance_test COMMAND tests/lib_advance_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_property(TEST lib_advance_test PROPERTY LABELS nonparallelizable_tests)

add_test(NAME http_plugin_test COMMAND tests/http_plugin_test.py ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(http_plugin_test PROPERTIES TIMEOUT 100)
set_tests_properties(http_plugin_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME plugin_http_api_test COMMAND tests/plugin_http_api_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_http_api_test PROPERTIES TIMEOUT 100)
set_tests_properties(plugin_http_api_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME plugin_http_category_api_test COMMAND tests/plugin_http_api_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_http_category_api_test PROPERTIES TIMEOUT 100 ENVIRONMENT "PLUGIN_HTTP_TEST_CATEGORY=ON")
set_tests_properties(plugin_http_category_api_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME trace_plugin_test COMMAND tests/trace_plugin_test.py -v WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(trace_plugin_test PROPERTIES TIMEOUT 300)
set_tests_properties(trace_plugin_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME resource_monitor_plugin_test COMMAND tests/resource_monitor_plugin_test.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(resource_monitor_plugin_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME nodeop_repeat_transaction_lr_test COMMAND tests/nodeop_high_transaction_test.py -v -p 4 -n 8 --num-transactions 1000 --max-transactions-per-second 500 --send-duplicates ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(nodeop_repeat_transaction_lr_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME light_validation_sync_test COMMAND tests/light_validation_sync_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(light_validation_sync_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME auto_bp_peering_test COMMAND tests/auto_bp_peering_test.py -v ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(auto_bp_peering_test PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)

add_test(NAME gelf_test COMMAND tests/gelf_test.py ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(gelf_test PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

add_test(NAME performance_test_bp COMMAND tests/PerformanceHarnessScenarioRunner.py findMax testBpOpMode --max-tps-to-test 50 --test-iteration-min-step 10 --test-iteration-duration-sec 10 --final-iterations-duration-sec 10 --calc-chain-threads lmax overrideBasicTestConfig -v --tps-limit-per-generator 25 --chain-state-db-size-mb 200 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_api COMMAND tests/PerformanceHarnessScenarioRunner.py findMax testApiOpMode --max-tps-to-test 50 --test-iteration-min-step 10 --test-iteration-duration-sec 10 --final-iterations-duration-sec 10 --calc-chain-threads lmax overrideBasicTestConfig -v --tps-limit-per-generator 25 --chain-state-db-size-mb 200 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_read_only_trxs COMMAND tests/PerformanceHarnessScenarioRunner.py findMax testApiOpMode --max-tps-to-test 50 --test-iteration-min-step 10 --test-iteration-duration-sec 10 --final-iterations-duration-sec 10 overrideBasicTestConfig -v --tps-limit-per-generator 25 --api-nodes-read-only-threads 2 --read-only-write-window-time-us 1000 --read-only-read-window-time-us 165000 --account-name "payloadless" --abi-file payloadless.abi --wasm-file payloadless.wasm --contract-dir unittests/test-contracts/payloadless --user-trx-data-file tests/PerformanceHarness/readOnlySlowTrxData.json --chain-state-db-size-mb 200 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_cpu_trx_spec COMMAND tests/PerformanceHarnessScenarioRunner.py findMax testBpOpMode --max-tps-to-test 50 --test-iteration-min-step 10 --test-iteration-duration-sec 10 --final-iterations-duration-sec 10 overrideBasicTestConfig -v --tps-limit-per-generator 25 --chain-state-db-size-mb 200 --account-name "c" --abi-file sysmechanics.abi --wasm-file sysmechanics.wasm --contract-dir unittests/system-test-contracts/sysio.mechanics --user-trx-data-file tests/PerformanceHarness/cpuTrxData.json ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_p2p COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --producer-nodes 1 --validation-nodes 1 --target-tps 20 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_http COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --endpoint-mode http --producer-nodes 1 --validation-nodes 1 --api-nodes 1 --target-tps 10 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_transfer_trx_spec COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --producer-nodes 1 --validation-nodes 1 --target-tps 20 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 --user-trx-data-file tests/PerformanceHarness/userTrxDataTransfer.json ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_new_acct_trx_spec COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --producer-nodes 1 --validation-nodes 1 --target-tps 20 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 --user-trx-data-file tests/PerformanceHarness/userTrxDataNewAccount.json ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_cpu_trx_spec COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --producer-nodes 1 --validation-nodes 1 --target-tps 20 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 --account-name "c" --abi-file sysmechanics.abi --wasm-file sysmechanics.wasm --contract-dir unittests/system-test-contracts/sysio.mechanics --user-trx-data-file tests/PerformanceHarness/cpuTrxData.json ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_ram_trx_spec COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --producer-nodes 1 --validation-nodes 1 --target-tps 20 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 --account-name "r" --abi-file sysmechanics.abi --wasm-file sysmechanics.wasm --contract-dir unittests/system-test-contracts/sysio.mechanics --user-trx-data-file tests/PerformanceHarness/ramTrxData.json ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME performance_test_basic_read_only_trxs COMMAND tests/PerformanceHarnessScenarioRunner.py singleTest -v --endpoint-mode http --producer-nodes 1 --validation-nodes 1 --api-nodes 1 --api-nodes-read-only-threads 2 --read-only-write-window-time-us 1000 --read-only-read-window-time-us 165000 --target-tps 20 --tps-limit-per-generator 10 --test-duration-sec 5 --chain-state-db-size-mb 200 --account-name "payloadless" --abi-file payloadless.abi --wasm-file payloadless.wasm --contract-dir unittests/test-contracts/payloadless --user-trx-data-file tests/PerformanceHarness/readOnlySlowTrxData.json ${UNSHARE} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(performance_test_bp PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_api PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_read_only_trxs PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_cpu_trx_spec PROPERTIES LABELS long_running_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_p2p PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_http PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_transfer_trx_spec PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_new_acct_trx_spec PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_cpu_trx_spec PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_ram_trx_spec PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)
set_tests_properties(performance_test_basic_read_only_trxs PROPERTIES LABELS nonparallelizable_tests RUN_SERIAL TRUE)

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
  set(ctest_exclude_tests 'distributed-transactions-remote-test|restart-scenarios-test_replay')

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
