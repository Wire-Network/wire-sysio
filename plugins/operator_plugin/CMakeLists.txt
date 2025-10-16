cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE SRC_FILES src/*.cpp)
add_library(operator_plugin STATIC ${SRC_FILES})

target_include_directories(
  operator_plugin
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  operator_plugin
  PUBLIC
  appbase
  chainbase
  chain_plugin
  fc
  sysio_chain
)

# Install headers
install(
  TARGETS operator_plugin
  # TODO: When cleaning up the installation targets, include this library
  #  allowing other projects to link against it via CMake's find_package
  #  EXPORT sysioTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include

)

if(ENABLE_TESTS)
  file(GLOB_RECURSE TEST_FILES test/*.cpp)
  add_executable(test_operator_plugin ${TEST_FILES})

  target_link_libraries(
    test_operator_plugin
    PUBLIC
    operator_plugin
    sysio_testing
    sysio_chain_wrap
  )
  add_test(
    NAME test_plugin_common
    COMMAND libraries/plugin_common/test/test_plugin_common
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()