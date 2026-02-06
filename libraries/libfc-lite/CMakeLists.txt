# fc-lite: header-only library

add_library(fc-lite INTERFACE)

# Public include directory for consumers
# The headers are under libraries/fc-lite/include/fc-lite

target_include_directories(fc-lite 
  INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if (ENABLE_TESTS)
    add_subdirectory(test)
endif ()

# Installation: headers to include/fc-lite
install(
  DIRECTORY include/fc-lite
  DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}
)

# Install the INTERFACE target for completeness (no artifacts produced)
install(
  TARGETS fc-lite
  EXPORT fc-lite-targets
)
