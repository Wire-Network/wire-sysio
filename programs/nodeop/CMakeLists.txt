add_executable( ${NODE_EXECUTABLE_NAME} main.cpp )

if( UNIX AND NOT APPLE )
  set(rt_library rt )
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../.git)
  find_package(Git)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
      OUTPUT_VARIABLE "nodeop_BUILD_VERSION"
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Git commit revision: ${nodeop_BUILD_VERSION}")
  else()
    set(nodeop_BUILD_VERSION 0)
  endif()
else()
  set(nodeop_BUILD_VERSION 0)
endif()

configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

target_include_directories(${NODE_EXECUTABLE_NAME} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

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

target_link_libraries( ${NODE_EXECUTABLE_NAME}
        PRIVATE appbase version
        PRIVATE -Wl,${whole_archive_flag} login_plugin               -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} state_history_plugin       -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} trace_api_plugin           -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} chain_api_plugin           -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} net_plugin                 -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} net_api_plugin             -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} txn_test_gen_plugin        -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} db_size_api_plugin         -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} producer_api_plugin        -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} resource_monitor_plugin    -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} test_control_plugin        -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} test_control_api_plugin    -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} sub_chain_plugin           -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${build_id_flag}
        PRIVATE chain_plugin http_plugin producer_plugin http_client_plugin
        PRIVATE sysio_chain_wrap fc ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS} )

include(additionalPlugins)

copy_bin( ${NODE_EXECUTABLE_NAME} )
install( TARGETS
   ${NODE_EXECUTABLE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT base
)