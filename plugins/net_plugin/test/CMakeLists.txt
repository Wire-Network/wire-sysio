add_executable( test_net_plugin
        auto_bp_peering_unittest.cpp
        rate_limit_parse_unittest.cpp
        net_msg_wire_unittest.cpp
        peer_auth_unittest.cpp
        queued_buffer_unittest.cpp
        subnet_mask_unittest.cpp
        main.cpp
)
target_link_libraries( test_net_plugin net_plugin sysio_testing sysio_chain_wrap )
add_p_test(NAME test_net_plugin COMMAND plugins/net_plugin/test/test_net_plugin)
