add_executable( test_producer_plugin
        test_production_pause_vote_tracker.cpp
        test_trx_full.cpp
        test_options.cpp
        test_block_timing_util.cpp
        test_disallow_delayed_trx.cpp
        test_mockable_timers.cpp
        test_snapshot_attestation_recovery.cpp
        main.cpp
        )
target_link_libraries( test_producer_plugin producer_plugin sysio_testing sysio_chain_wrap fc-lite opp )
target_include_directories(test_producer_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/contracts/tests
        ${CMAKE_BINARY_DIR}/contracts/tests)
add_dependencies(test_producer_plugin contracts_project)
add_p_test(NAME test_producer_plugin COMMAND plugins/producer_plugin/test/test_producer_plugin)
