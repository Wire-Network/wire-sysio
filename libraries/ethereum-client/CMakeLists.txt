file(GLOB_RECURSE SRC_FILES_ALL src/*.cpp)
set(SRC_FILES ${SRC_FILES_ALL})
list(FILTER SRC_FILES EXCLUDE REGEX ".*main\\.cpp$")


set(TARGET_LIB_NAME ethereum-client)
set(TARGET_TOOL_NAME ethereum-client-tool)

macro(ETH_TARGET_SETUP TARGET_NAME)
  target_include_directories(
    ${TARGET_NAME}
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )

  target_link_libraries(
    ${TARGET_NAME}
    PUBLIC
    fc
    JsonCpp::JsonCpp
    CURL::libcurl
    boringssl::ssl
    boringssl::crypto
  )
endmacro()

add_library(${TARGET_LIB_NAME} STATIC ${SRC_FILES})
eth_target_setup(${TARGET_LIB_NAME})

if(ENABLE_TESTS)
  add_executable(${TARGET_TOOL_NAME} ${SRC_FILES_ALL})
  eth_target_setup(${TARGET_TOOL_NAME})
endif()

# Install headers
install(
  TARGETS ${TARGET_LIB_NAME}
  # TODO: When cleaning up the installation targets, include this library
  #  allowing other projects to link against it via CMake's find_package
  #  EXPORT sysioTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include

)

if(ENABLE_TESTS)
#  file(GLOB_RECURSE TEST_FILES test/*.cpp)
#  add_executable(test_${TARGET_NAME} ${TEST_FILES})
#
#  target_link_libraries(
#    test_${TARGET_NAME}
#    PUBLIC
#    ${TARGET_NAME}
#    sysio_testing
#    sysio_chain_wrap
#  )
#  add_test(
#    NAME test_plugin_common
#    COMMAND libraries/plugin_common/test/test_plugin_common
#    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
#  )
endif()