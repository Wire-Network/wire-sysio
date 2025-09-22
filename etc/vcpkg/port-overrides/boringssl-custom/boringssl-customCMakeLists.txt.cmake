include(GNUInstallDirs)

add_compile_definitions(BORINGSSL_IMPLEMENTATION=1)
add_compile_definitions(OPENSSL_EXPORT=1)

target_compile_options(fipsmodule PRIVATE -Wno-error)
target_compile_options(crypto PRIVATE -Wno-error)
target_compile_options(decrepit PRIVATE -Wno-error)

#paranoia for when a dependent library depends on openssl (such as libcurl)
set_target_properties(fipsmodule PROPERTIES C_VISIBILITY_PRESET hidden)
set_target_properties(crypto PROPERTIES C_VISIBILITY_PRESET hidden)
set_target_properties(decrepit PROPERTIES C_VISIBILITY_PRESET hidden)

add_library(boringssl INTERFACE)
target_link_libraries(boringssl INTERFACE crypto decrepit)
target_include_directories(boringssl INTERFACE src/include)

# avoid conflict with system lib
set_target_properties(crypto PROPERTIES PREFIX libbs)

install(TARGETS crypto decrepit fipsmodule
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# TODO: Generate cmake config files for boringssl
