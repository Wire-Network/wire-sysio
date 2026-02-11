set(APPBASE_INSTALL_COMPONENT "dev")
set(SYSVM_INSTALL_COMPONENT "dev")


add_subdirectory(libfc-lite)
add_subdirectory(libfc)
add_subdirectory(libfc-test)
add_subdirectory(builtins)
add_subdirectory(wasm-jit)
add_subdirectory(chaindb)
set(APPBASE_ENABLE_AUTO_VERSION OFF CACHE BOOL "enable automatic discovery of version via 'git describe'")
add_subdirectory(appbase)
add_subdirectory(custom_appbase)
add_subdirectory(chain)
add_subdirectory(testing)
add_subdirectory(version)
add_subdirectory(state_history)

if(sys-vm IN_LIST SYSIO_WASM_RUNTIMES OR sys-vm-jit IN_LIST SYSIO_WASM_RUNTIMES)
  find_package(sys-vm REQUIRED)
  # add_library(sys-vm ALIAS sys-vm::sys-vm)
endif()

