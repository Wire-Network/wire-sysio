configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sendinline.wasm ${CMAKE_CURRENT_BINARY_DIR}/sendinline.wasm COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sendinline.abi ${CMAKE_CURRENT_BINARY_DIR}/sendinline.abi COPYONLY)

if(BUILD_SYSTEM_CONTRACTS)
    add_contract(sendinline sendinline sendinline.cpp)
endif()
