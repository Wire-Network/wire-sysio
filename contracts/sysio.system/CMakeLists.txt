if(BUILD_SYSTEM_CONTRACTS)
  # When building from source, copy pre-built contracts only if they don't exist (bootstrap)
  if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.wasm)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.wasm COPYONLY)
  endif()
  if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.abi)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.abi COPYONLY)
  endif()
  add_contract(
    sysio.system
    sysio.system
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sysio.system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/producer_pay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/voting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/limit_auth_changes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/finalizer_key.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/peer_keys.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/block_info.cpp)

  target_include_directories(sysio.system
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../)
else()
  # When not building from source, always copy pre-built contracts (ensures fresh copy on branch switch)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.wasm COPYONLY)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.system.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.system.abi COPYONLY)
endif()

