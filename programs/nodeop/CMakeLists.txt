
configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

chain_target(${NODE_EXECUTABLE_NAME} SOURCE_FILES main.cpp)

include(plugin-tools)

# Install nodeop into the main (base) package
if(TARGET ${NODE_EXECUTABLE_NAME})
  install(TARGETS ${NODE_EXECUTABLE_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT base)
endif()
