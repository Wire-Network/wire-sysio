add_executable(${SYS_UTIL_EXECUTABLE_NAME} main.cpp actions/subcommand.cpp actions/generic.cpp actions/blocklog.cpp actions/snapshot.cpp actions/chain.cpp)

if(UNIX AND NOT APPLE)
  set(rt_library rt)
endif()
configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

target_include_directories(${SYS_UTIL_EXECUTABLE_NAME} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${SYS_UTIL_EXECUTABLE_NAME}
  PRIVATE
  appbase
  fc
  plugin_common
  version

  sysio_chain
  chain_plugin
  wallet_plugin
  wallet_api_plugin
  producer_plugin

  CLI11::CLI11

  ${CMAKE_DL_LIBS}
  ${PLATFORM_SPECIFIC_LIBS}
)

copy_bin(${SYS_UTIL_EXECUTABLE_NAME})
install(TARGETS
  ${SYS_UTIL_EXECUTABLE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT base
)
