file(GLOB_RECURSE headerFiles "include/sysio/challenger_plugin/*.hpp")
file(GLOB_RECURSE sourceFiles "src/*.cpp")
add_library(challenger_plugin
  ${sourceFiles}
  ${headerFiles})

target_link_libraries(challenger_plugin http_plugin sysio_chain signature_provider_plugin appbase fc batch_operator_plugin)
target_include_directories(challenger_plugin PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/plugins/chain_interface/include
  ${CMAKE_SOURCE_DIR}/plugins/http_plugin/include
  ${CMAKE_SOURCE_DIR}/plugins/batch_operator_plugin/include
)

# add_subdirectory( tests )