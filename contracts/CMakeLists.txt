cmake_minimum_required(VERSION 3.8)
project(contracts)

if(BUILD_SYSTEM_CONTRACTS)
  find_package(cdt REQUIRED)
endif()

# Macro to handle contract bootstrap/copy logic.
# When BUILD_SYSTEM_CONTRACTS is ON, copies pre-built .wasm/.abi only if they don't exist (bootstrap).
# When OFF, always copies pre-built contracts (ensures fresh copy on branch switch).
# Extra files beyond .wasm/.abi can be passed as additional arguments.
macro(bootstrap_contract CONTRACT_NAME)
  set(_extra_files ${ARGN})
  set(_all_files ${CONTRACT_NAME}.wasm ${CONTRACT_NAME}.abi ${_extra_files})

  if(BUILD_SYSTEM_CONTRACTS)
    foreach(_file IN LISTS _all_files)
      if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${_file})
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_file} ${CMAKE_CURRENT_BINARY_DIR}/${_file} COPYONLY)
      endif()
    endforeach()
  else()
    foreach(_file IN LISTS _all_files)
      configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${_file} ${CMAKE_CURRENT_BINARY_DIR}/${_file} COPYONLY)
    endforeach()
  endif()
endmacro()

add_subdirectory(sysio.bios)
add_subdirectory(sysio.system)
add_subdirectory(sysio.msig)
add_subdirectory(sysio.roa)

add_subdirectory(sysio.token)
add_subdirectory(sysio.wrap)
add_subdirectory(test_contracts)

