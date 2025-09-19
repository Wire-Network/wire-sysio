file(GLOB_RECURSE headerFiles "include/sysio/batch_operator_plugin/*.hpp")
file(GLOB_RECURSE sourceFiles "src/*.cpp")
add_library( batch_operator_plugin
             ${sourceFiles}
             ${headerFiles} )

target_link_libraries( batch_operator_plugin http_plugin sysio_chain signature_provider_plugin appbase fc)
target_include_directories( batch_operator_plugin PUBLIC 
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/plugins/chain_interface/include
  ${CMAKE_SOURCE_DIR}/plugins/http_plugin/include
)

# add_subdirectory( tests )