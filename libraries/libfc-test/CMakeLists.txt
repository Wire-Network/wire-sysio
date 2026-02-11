# fc-test: test tools library
file(GLOB_RECURSE SRC_FILES src/*.cpp  src/*.hpp)
file(GLOB_RECURSE HEADER_FILES include/*.hpp)
add_library(fc-test
        ${SRC_FILES}
        ${HEADER_FILES}
)

# Public include directory for consumers
# The headers are under libraries/fc-test/include/fc-test
target_include_directories(fc-test
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fc-test PUBLIC fc fc-lite magic_enum::magic_enum)

