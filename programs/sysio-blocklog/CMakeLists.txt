add_executable( sysio-blocklog main.cpp )

if( UNIX AND NOT APPLE )
  set(rt_library rt )
endif()

target_include_directories(sysio-blocklog PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries( sysio-blocklog
        PRIVATE appbase
        PRIVATE sysio_chain fc ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS} )

copy_bin( sysio-blocklog )
install( TARGETS
   sysio-blocklog

   COMPONENT base

   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)
