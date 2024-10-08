add_executable( sysio-launcher main.cpp )
if( UNIX AND NOT APPLE )
  set(rt_library rt )
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../.git)
  find_package(Git)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
      OUTPUT_VARIABLE "launcher_BUILD_VERSION"
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Git commit revision: ${launcher_BUILD_VERSION}")
  else()
    set(launcher_BUILD_VERSION 0)
  endif()
endif()

configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

target_include_directories(sysio-launcher PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(sysio-launcher
                      PRIVATE sysio_chain fc Boost::program_options ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS} )

install( TARGETS
   sysio-launcher

   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT dev EXCLUDE_FROM_ALL
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
)
