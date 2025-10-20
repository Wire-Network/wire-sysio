file(GLOB_RECURSE SRCS main.cpp actions/*.cpp)


configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

chain_target(${SYS_UTIL_EXECUTABLE_NAME} SOURCE_FILES ${SRCS})

