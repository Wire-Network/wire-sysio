add_executable( test_sub_chain
   test_sub_chain.cpp
   test_contract_action_match.cpp )
target_link_libraries( test_sub_chain sub_chain_plugin sysio_testing )

add_test(NAME test_sub_chain COMMAND plugins/sub_chain_plugin/test/test_sub_chain WORKING_DIRECTORY ${CMAKE_BINARY_DIR})