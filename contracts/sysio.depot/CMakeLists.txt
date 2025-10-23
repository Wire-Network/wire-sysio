configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.depot.wasm ${CMAKE_CURRENT_BINARY_DIR}/sysio.depot.wasm COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sysio.depot.abi ${CMAKE_CURRENT_BINARY_DIR}/sysio.depot.abi COPYONLY)
if(BUILD_SYSTEM_CONTRACTS)
  file(GLOB_RECURSE SRCS src/*.cpp)
  add_contract(
    sysio.depot
    sysio.depot
    ${SRCS}
  )

  target_include_directories(sysio.depot
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include>
    $<INSTALL_INTERFACE:include>
  )

  target_include_directories(sysio.depot
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
  )

  if (BUILD_TEST_CONTRACTS AND NOT CDT_BUILD)
    file(GLOB_RECURSE TEST_SRCS tests/*.cpp)
    list(FILTER SRCS EXCLUDE REGEX ".*sysio.depot\\.(hpp|cpp)$")
    set(CONTRACT_UNIT_TEST_EXE contracts_sysio_depot_unittests)
    unittest_target(
      ${CONTRACT_UNIT_TEST_EXE}
      SOURCE_FILES ${SRCS} ${TEST_SRCS}
    )

    target_include_directories(
      ${CONTRACT_UNIT_TEST_EXE}
      PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include>
    )
    unittest_tests_add(
      ${CONTRACT_UNIT_TEST_EXE}
      UNITTEST_FILES ${TEST_SRCS}
    )
  endif()
    
endif()
