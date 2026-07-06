file(GLOB_RECURSE SRCS main.cpp actions/*.cpp)

chain_target(${SYS_UTIL_EXECUTABLE_NAME} SOURCE_FILES ${SRCS})

# Install sys-util into the main (base) package
if(TARGET ${SYS_UTIL_EXECUTABLE_NAME})
  install(TARGETS ${SYS_UTIL_EXECUTABLE_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT base)
endif()
