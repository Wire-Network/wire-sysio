file(GLOB_RECURSE SRC_FILES *.cpp)
set(TARGET_NAME test_signature_provider_manager_plugin)
add_executable(
        ${TARGET_NAME}
        ${SRC_FILES}
)
target_link_libraries(${TARGET_NAME} PRIVATE signature_provider_manager_plugin sysio_testing sysio_chain_wrap Boost::process Boost::dll)
add_test(NAME ${TARGET_NAME} COMMAND plugins/signature_provider_manager_plugin/test/${TARGET_NAME} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# COPY THE FIXTURES TO THE BUILD DIRECTORY SO THE TESTS CAN FIND THEM
file(COPY fixtures DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

