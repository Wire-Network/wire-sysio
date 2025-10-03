cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE SRC_FILES src/*.cpp)
add_library(operator_plugin_base STATIC ${SRC_FILES})

target_include_directories(
  operator_plugin_base
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(
  operator_plugin_base
  PUBLIC
  fc
  chainbase
  appbase
  sysio_chain
)

# Install headers
install(
  TARGETS operator_plugin_base
  # TODO: When cleaning up the installation targets, include this library
  #  allowing other projects to link against it via CMake's find_package
  #  EXPORT sysioTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include

)
