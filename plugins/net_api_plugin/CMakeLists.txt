file(GLOB HEADERS "include/sysio/net_api_plugin/*.hpp")
add_library( net_api_plugin
             net_api_plugin.cpp
             ${HEADERS} )

target_link_libraries( net_api_plugin net_plugin http_plugin appbase )
target_include_directories( net_api_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
