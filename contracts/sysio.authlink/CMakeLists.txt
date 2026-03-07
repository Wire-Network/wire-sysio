set(contract_name sysio.authlink)
bootstrap_contract(${contract_name})
if(BUILD_SYSTEM_CONTRACTS)
  file(GLOB_RECURSE SOURCES src/*.cpp)
  file(GLOB_RECURSE HEADERS include/*.hpp src/*.hpp)
  add_contract(${contract_name} ${contract_name} ${SOURCES})
  set(targets ${contract_name})

  # Native .so for native-module runtime debugging
  if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
    list(APPEND targets ${contract_name}_native)
    add_native_contract(
            TARGET         ${contract_name}_native
            SOURCES        ${SOURCES}
            INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}/include
            CONTRACT_CLASS "sysio::authlink"
            HEADERS        ${HEADERS}
            ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/sysio.authlink/sysio.authlink.abi
    )
  endif()

  foreach(target ${targets})
    if(NOT TARGET ${target})
      message(WARNING "Target ${target} not found, skipping include directory setup")
      continue()
    endif()
    target_include_directories(${target}
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sysio.system/include>
    )
  endforeach()
endif()
