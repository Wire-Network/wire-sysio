if( BUILD_SYSTEM_CONTRACTS AND CDT_BUILD)
   add_contract(
           sysio.system
           sysio.system
           ${CMAKE_CURRENT_SOURCE_DIR}/src/sysio.system.cpp
           ${CMAKE_CURRENT_SOURCE_DIR}/src/native.cpp
           ${CMAKE_CURRENT_SOURCE_DIR}/src/producer_pay.cpp
           ${CMAKE_CURRENT_SOURCE_DIR}/src/voting.cpp
           ${CMAKE_CURRENT_SOURCE_DIR}/src/limit_auth_changes.cpp
           ${CMAKE_CURRENT_SOURCE_DIR}/src/block_info.cpp)

   target_include_directories(sysio.system
           PUBLIC
           ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${CMAKE_CURRENT_SOURCE_DIR}/../)
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.wasm COPYONLY )
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.abi  ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.abi  COPYONLY )
endif()
