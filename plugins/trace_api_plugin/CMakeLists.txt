file(GLOB HEADERS "include/sysio/trace_api_plugin/*.hpp")
add_library( trace_api_plugin
             request_handler.cpp
             store_provider.cpp
             abi_data_handler.cpp
             compressed_file.cpp
             configuration_utils.cpp
             trace_api_plugin.cpp
             ${HEADERS} )

target_link_libraries( trace_api_plugin chain_plugin http_plugin sysio_chain appbase )
target_include_directories( trace_api_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

add_subdirectory( utils )
add_subdirectory( test )
