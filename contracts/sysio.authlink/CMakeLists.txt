if(BUILD_SYSTEM_CONTRACTS)
  file(GLOB_RECURSE SRCS src/*.cpp)
  add_contract(
    sysio.authlink
    sysio.authlink
    ${SRCS}
  )

  target_include_directories(sysio.authlink
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include>
    $<INSTALL_INTERFACE:include>
  )

  target_include_directories(sysio.authlink
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
  )

  #  if(NOT CDT_BUILD)
  #    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.depot.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.depot.wasm COPYONLY)
  #    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.depot.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.depot.abi COPYONLY)
  #  endif()
endif()
