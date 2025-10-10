cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE SRC_FILES src/*.cpp)
add_library(plugin_common STATIC ${SRC_FILES})

target_include_directories(
  plugin_common
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  plugin_common
  PUBLIC
  fc
  chainbase
  appbase
  sysio_chain
)

# Install headers
install(
  TARGETS plugin_common
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
  add_executable(test_plugin_common ${TEST_FILES})

  target_link_libraries(
    test_plugin_common
    PUBLIC
    plugin_common
    sysio_testing
    sysio_chain_wrap
  )
  add_test(
    NAME test_plugin_common
    COMMAND libraries/plugin_common/test/test_plugin_common
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()