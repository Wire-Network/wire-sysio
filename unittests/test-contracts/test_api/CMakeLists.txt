if( BUILD_TEST_CONTRACTS )
   add_executable( test_api test_api.cpp )
   # test_compiler_builtins.cpp drives non-inlinable int128 ops (full divmod, dynamic
   # shifts) which lower to librt calls; test_print.cpp uses long double which lowers
   # to softfloat (libsf). --use-rt links both archives locally so the contract WASM
   # carries the implementations rather than importing them as env symbols (which the
   # host no longer provides).
   target_link_libraries( test_api PUBLIC --use-rt )
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/test_api.wasm ${CMAKE_CURRENT_BINARY_DIR}/test_api.wasm COPYONLY )
endif()
