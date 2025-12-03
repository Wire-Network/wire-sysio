#cmake_minimum_required(VERSION 3.16)

#file(GLOB_RECURSE SRC_FILES src/*.cpp)
set(TARGET_NAME outpost_client_plugin)

plugin_target(
        ${TARGET_NAME}
        LIBRARIES
        appbase
        chainbase
        chain_plugin
        fc
        sysio_chain)
#add_library(${TARGET_NAME} STATIC ${SRC_FILES})
#
#target_include_directories(
#  ${TARGET_NAME}
#  PUBLIC
#  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
#  $<INSTALL_INTERFACE:include>
#)
#
#target_link_libraries(
#  ${TARGET_NAME}
#  PUBLIC
#  appbase
#  chainbase
#  chain_plugin
#  fc
#  sysio_chain
#)
#
## Install headers
#install(
#  TARGETS ${TARGET_NAME}
#  # TODO: When cleaning up the installation targets, include this library
#  #  allowing other projects to link against it via CMake's find_package
#  #  EXPORT sysioTargets
#  ARCHIVE DESTINATION lib
#  LIBRARY DESTINATION lib
#  RUNTIME DESTINATION bin
#  INCLUDES DESTINATION include
#
#)
#
#if(ENABLE_TESTS)
#  set(TEST_TARGET_NAME test_${TARGET_NAME})
#  file(GLOB_RECURSE TEST_FILES test/*.cpp)
#  add_executable(${TEST_TARGET_NAME} ${TEST_FILES})
#
#  target_link_libraries(
#    ${TEST_TARGET_NAME}
#    PUBLIC
#    ${TARGET_NAME}
#    sysio_testing
#    sysio_chain_wrap
#  )
#  add_test(
#    NAME ${TEST_TARGET_NAME}
#    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/test/${TEST_TARGET_NAME}
#    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
#  )
#endif()