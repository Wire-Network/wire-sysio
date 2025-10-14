add_executable(${SYS_UTIL_EXECUTABLE_NAME} main.cpp actions/subcommand.cpp actions/generic.cpp actions/blocklog.cpp actions/snapshot.cpp actions/chain.cpp)

if(UNIX AND NOT APPLE)
  set(rt_library rt)
endif()

if(UNIX)
  if(APPLE)
    set(whole_archive_flag "-force_load")
    set(no_whole_archive_flag "")
    set(build_id_flag "")
  else()
    set(whole_archive_flag "--whole-archive")
    set(no_whole_archive_flag "--no-whole-archive")
    set(build_id_flag "--build-id")
  endif()
else()
  set(whole_archive_flag "--whole-archive")
  set(no_whole_archive_flag "--no-whole-archive")
  set(build_id_flag "")
endif()

configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

target_include_directories(${SYS_UTIL_EXECUTABLE_NAME} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${SYS_UTIL_EXECUTABLE_NAME}
  PRIVATE
  appbase
  fc
  plugin_common
  version
  PRIVATE -Wl,${whole_archive_flag} state_history_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} trace_api_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} chain_api_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} net_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} net_api_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} db_size_api_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} producer_api_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} resource_monitor_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} test_control_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} test_control_api_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${whole_archive_flag} prometheus_plugin -Wl,${no_whole_archive_flag}
  PRIVATE -Wl,${build_id_flag}

  sysio_chain
  chain_plugin

  net_plugin


  producer_plugin

  http_plugin

  wallet_plugin
  wallet_api_plugin
  sysio_chain_wrap

  gsl-lite::gsl-lite
  CLI11::CLI11

  ${CMAKE_DL_LIBS}
  ${PLATFORM_SPECIFIC_LIBS}

  Boost::process
  Boost::dll
)

copy_bin(${SYS_UTIL_EXECUTABLE_NAME})
install(TARGETS
  ${SYS_UTIL_EXECUTABLE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT base
)
