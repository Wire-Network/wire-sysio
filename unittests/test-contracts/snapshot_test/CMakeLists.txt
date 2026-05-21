if( BUILD_TEST_CONTRACTS )
   add_contract( snapshot_test snapshot_test snapshot_test.cpp )
   # long double secondary index key drives softfloat ops; --use-rt links libsf locally
   target_link_libraries( snapshot_test PUBLIC --use-rt )
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/snapshot_test.wasm ${CMAKE_CURRENT_BINARY_DIR}/snapshot_test.wasm COPYONLY )
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/snapshot_test.abi  ${CMAKE_CURRENT_BINARY_DIR}/snapshot_test.abi  COPYONLY )
endif()
