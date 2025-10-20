
configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

chain_target(${NODE_EXECUTABLE_NAME} SOURCE_FILES main.cpp)

include(plugin-tools)