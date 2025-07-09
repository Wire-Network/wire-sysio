file(GLOB HEADERS "include/sysio/sub_chain_plugin/*.hpp")
add_library( sub_chain_plugin
             sub_chain_plugin.cpp
             ${HEADERS} )

target_link_libraries( sub_chain_plugin appbase fc chain_plugin sysio_chain http_plugin )
target_include_directories( sub_chain_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

add_subdirectory( test )