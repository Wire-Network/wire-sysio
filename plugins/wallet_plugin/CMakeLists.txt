file(GLOB HEADERS "include/sysio/wallet_plugin/*.hpp")

add_library( wallet_plugin
             wallet.cpp
             wallet_plugin.cpp
             wallet_manager.cpp
             ${SE_WALLET_SOURCES}
             ${HEADERS} )

target_link_libraries( wallet_plugin sysio_chain custom_appbase ${security_framework} ${corefoundation_framework} ${localauthentication_framework} ${cocoa_framework})
target_include_directories( wallet_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

# shim that forwards only include dirs
add_library(wallet_plugin_headers INTERFACE)
target_link_libraries(wallet_plugin_headers INTERFACE wallet_plugin)