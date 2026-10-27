# Keep a reproducible contract fixture available to builds without CDT.
if(BUILD_TEST_CONTRACTS)
   add_contract(ed25519_verify_test ed25519_verify_test ed25519_verify_test.cpp)
else()
   configure_file(ed25519_verify_test.wasm ed25519_verify_test.wasm COPYONLY)
   configure_file(ed25519_verify_test.abi ed25519_verify_test.abi COPYONLY)
endif()
