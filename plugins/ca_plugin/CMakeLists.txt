file(GLOB HEADERS "include/sysio/ca_plugin/*.hpp")
add_library(ca_plugin
  src/ca_plugin.cpp
  ${HEADERS}
)
target_include_directories(ca_plugin PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
#//OpenSSL::SSL OpenSSL::Crypto
target_link_libraries(ca_plugin appbase fc http_plugin chain_plugin net_plugin)
#set_target_properties(ca_plugin PROPERTIES PREFIX "" SUFFIX ".plugin")
