file(GLOB HEADERS "include/sysio/wallet_plugin/*.hpp")

if(APPLE)
  set(SE_WALLET_SOURCES se_wallet.cpp macos_user_auth.m)
  set_source_files_properties(macos_user_presence.m PROPERTIES COMPILE_FLAGS "-x objective-c")

  find_library(security_framework Security)
  find_library(localauthentication_framework LocalAuthentication)
  find_library(corefoundation_framework CoreFoundation)
  find_library(cocoa_framework Cocoa)

  if(MAS_KEYCHAIN_GROUP)
    add_definitions(-DMAS_KEYCHAIN_GROUP=${MAS_KEYCHAIN_GROUP})
  endif(MAS_KEYCHAIN_GROUP)
endif(APPLE)

add_library( wallet_plugin
             wallet.cpp
             wallet_plugin.cpp
             wallet_manager.cpp
             ${SE_WALLET_SOURCES}
             ${HEADERS} )

target_link_libraries( wallet_plugin sysio_chain custom_appbase ${security_framework} ${corefoundation_framework} ${localauthentication_framework} ${cocoa_framework})
target_include_directories( wallet_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
