if( BUILD_TEST_CONTRACTS )
   add_contract( get_table_test get_table_test get_table_test.cpp )
   # long double secondary index key drives softfloat ops; --use-rt links libsf locally
   target_link_libraries( get_table_test PUBLIC --use-rt )
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/get_table_test.wasm ${CMAKE_CURRENT_BINARY_DIR}/get_table_test.wasm COPYONLY )
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/get_table_test.abi  ${CMAKE_CURRENT_BINARY_DIR}/get_table_test.abi  COPYONLY )
endif()
