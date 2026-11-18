# The fork case reuses the unit tests' Savanna cluster fixture: unittests/ supplies savanna_cluster.hpp and .cpp, and
# unittests/include the generated test-contract and snapshot accessors they and the chain cases read.
add_executable( test_account_watch_plugin main.cpp test_account_watch.cpp ${CMAKE_SOURCE_DIR}/unittests/savanna_cluster.cpp )
target_link_libraries( test_account_watch_plugin account_watch_plugin sysio_testing sysio_chain_wrap )
target_include_directories( test_account_watch_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/unittests
        ${CMAKE_BINARY_DIR}/unittests/include )
add_p_test(NAME test_account_watch_plugin COMMAND plugins/account_watch_plugin/test/test_account_watch_plugin)
