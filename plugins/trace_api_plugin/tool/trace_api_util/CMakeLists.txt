file(GLOB_RECURSE SRC_FILES src/*.cpp src/*.hpp include/*.hpp)
add_executable(trace_api_util
        ${SRC_FILES}
)
target_link_libraries(
        trace_api_util
        PUBLIC
        trace_api_plugin
)

target_include_directories(
        trace_api_util
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

copy_bin(trace_api_util)
install(TARGETS
        trace_api_util
        RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)
