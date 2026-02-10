### Build contracts with cdt if available ###
include(ExternalProject)

if(BUILD_TEST_CONTRACTS)
  set(SYSIO_WASM_OLD_BEHAVIOR "Off")

  message(STATUS "Building contracts in directory `./libraries/contracts/`")
  contracts_target(
    testing_contracts_project
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/contracts
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/contracts
  )
  add_dependencies(testing_contracts_project contracts_project)
endif()

add_subdirectory(contracts)
file(GLOB HEADERS "include/sysio/testing/*.hpp")

# TODO: move src files to subdirectory `src` and then make this GLOB_RECURSIVE `src/*.cpp`
file(GLOB SRC_FILES *.cpp)
configure_file(contracts.hpp.in include/testing_contracts/contracts.hpp ESCAPE_QUOTES)
add_library(sysio_testing_contracts INTERFACE)
target_include_directories(sysio_testing_contracts INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/include/testing_contracts)
add_dependencies(sysio_testing_contracts contracts_project)

if( BUILD_TEST_CONTRACTS )
   add_dependencies( sysio_testing_contracts testing_contracts_project)
endif()

configure_file(contracts.cpp.in contracts.cpp ESCAPE_QUOTES)

## SORT .cpp by most likely to change / break compile
add_library(sysio_testing
  ${SRC_FILES}
  ${CMAKE_CURRENT_BINARY_DIR}/contracts.cpp
  ${HEADERS}
)

# Explicitly produce copied contract artifacts so cmake knows how to build them
set(TESTING_DST_BIOS_DIR ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios)
set(TESTING_DST_ROA_DIR  ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa)

set(BIOS_WASM_SRC ${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.wasm)
set(BIOS_ABI_SRC  ${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.abi)
set(ROA_WASM_SRC  ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.wasm)
set(ROA_ABI_SRC   ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.abi)

set(BIOS_WASM_DST ${TESTING_DST_BIOS_DIR}/sysio.bios.wasm)
set(BIOS_ABI_DST  ${TESTING_DST_BIOS_DIR}/sysio.bios.abi)
set(ROA_WASM_DST  ${TESTING_DST_ROA_DIR}/sysio.roa.wasm)
set(ROA_ABI_DST   ${TESTING_DST_ROA_DIR}/sysio.roa.abi)

if(BUILD_SYSTEM_CONTRACTS OR BUILD_TEST_CONTRACTS)
  # When building contracts from source, copy only if changed and update stamp file
  # We use a custom target because external project dependencies don't work well with add_custom_command
  set(CONTRACTS_COPY_STAMP ${CMAKE_CURRENT_BINARY_DIR}/contracts_copy.stamp)

  add_custom_target(sysio_testing_contracts_copy ALL
    COMMAND ${CMAKE_COMMAND}
      -DSRC1="${BIOS_WASM_SRC}"
      -DDST1="${BIOS_WASM_DST}"
      -DSRC2="${BIOS_ABI_SRC}"
      -DDST2="${BIOS_ABI_DST}"
      -DSRC3="${ROA_WASM_SRC}"
      -DDST3="${ROA_WASM_DST}"
      -DSRC4="${ROA_ABI_SRC}"
      -DDST4="${ROA_ABI_DST}"
      -DCOUNT=4
      -DSTAMP_FILE="${CONTRACTS_COPY_STAMP}"
      -P "${CMAKE_SOURCE_DIR}/cmake/scripts/contract_copy_if_changed_and_stamp.cmake"
    BYPRODUCTS ${BIOS_WASM_DST} ${BIOS_ABI_DST} ${ROA_WASM_DST} ${ROA_ABI_DST} ${CONTRACTS_COPY_STAMP}
    COMMENT "Checking and copying test contracts to testing libraries build directory"
  )
  # Ensure contracts are built before copying
  add_dependencies(sysio_testing_contracts_copy contracts_project)
else()
  # When using pre-built contracts, use add_custom_command for proper dependency tracking
  add_custom_command(
    OUTPUT ${BIOS_WASM_DST} ${BIOS_ABI_DST} ${ROA_WASM_DST} ${ROA_ABI_DST}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TESTING_DST_BIOS_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TESTING_DST_ROA_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${BIOS_WASM_SRC} ${BIOS_WASM_DST}
    COMMAND ${CMAKE_COMMAND} -E copy ${BIOS_ABI_SRC}  ${BIOS_ABI_DST}
    COMMAND ${CMAKE_COMMAND} -E copy ${ROA_WASM_SRC}  ${ROA_WASM_DST}
    COMMAND ${CMAKE_COMMAND} -E copy ${ROA_ABI_SRC}   ${ROA_ABI_DST}
    DEPENDS ${BIOS_WASM_SRC} ${BIOS_ABI_SRC} ${ROA_WASM_SRC} ${ROA_ABI_SRC}
    COMMENT "Copying test contracts to testing libraries build directory"
  )
  add_custom_target(sysio_testing_contracts_copy ALL
    DEPENDS ${BIOS_WASM_DST} ${BIOS_ABI_DST} ${ROA_WASM_DST} ${ROA_ABI_DST}
  )
endif()

add_dependencies(sysio_testing sysio_testing_contracts_copy)

# When building contracts from source, ensure contracts.cpp.o is recompiled when contract files change
if(BUILD_SYSTEM_CONTRACTS OR BUILD_TEST_CONTRACTS)
  # Make the main testing library wait for the contracts to be built.
  add_dependencies(sysio_testing sysio_testing_contracts contracts_project)

  # Set explicit file-level dependencies on contracts.cpp object file.
  # We depend on the stamp file which gets touched only when contracts actually change.
  # This ensures contracts.cpp.o is rebuilt when contracts are updated, but not on every build.
  set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/contracts.cpp"
    PROPERTIES OBJECT_DEPENDS "${CONTRACTS_COPY_STAMP}"
  )
endif()

target_link_libraries(sysio_testing sysio_testing_contracts sysio_chain fc-lite fc fc-test chainbase Logging IR WAST WASM)
target_include_directories(sysio_testing
  PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-jit/Include"
  "${CMAKE_CURRENT_SOURCE_DIR}/contracts"
  "${CMAKE_CURRENT_BINARY_DIR}/contracts"
  "${CMAKE_SOURCE_DIR}/contracts"
  "${CMAKE_BINARY_DIR}/contracts"
)

set_target_properties(sysio_testing PROPERTIES PUBLIC_HEADER "${HEADERS}")
install(TARGETS sysio_testing
  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/sysio/testing
)