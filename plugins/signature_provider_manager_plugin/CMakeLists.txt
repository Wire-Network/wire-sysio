file(GLOB_RECURSE HEADER_FILES include/*.hpp)
file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp)
set(TARGET_NAME signature_provider_manager_plugin)
add_library(${TARGET_NAME}
        ${SRC_FILES}
        ${HEADER_FILES})

target_link_libraries(${TARGET_NAME} custom_appbase fc http_client_plugin wallet_plugin_headers)
target_include_directories(
        ${TARGET_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

if (ENABLE_TESTS)
    add_subdirectory(test)
endif ()
