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

configure_file(contracts.hpp.in include/testing_contracts/contracts.hpp ESCAPE_QUOTES)
add_library(sysio_testing_contracts INTERFACE)
target_include_directories(sysio_testing_contracts INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/include/testing_contracts)
add_dependencies(sysio_testing_contracts contracts_project)

configure_file(contracts.cpp.in contracts.cpp ESCAPE_QUOTES)

## SORT .cpp by most likely to change / break compile
add_library(sysio_testing
  tester.cpp
  tester_network.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/contracts.cpp
  ${HEADERS}
)

add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios/sysio.bios.wasm
               ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios/sysio.bios.abi
               ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa/sysio.roa.wasm
               ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa/sysio.roa.abi
        COMMAND mkdir -p ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios
        COMMAND mkdir -p ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa
        COMMAND cp ${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.wasm ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios/sysio.bios.wasm
        COMMAND cp ${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.abi ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios/sysio.bios.abi
        COMMAND cp ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.wasm ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa/sysio.roa.wasm
        COMMAND cp ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.abi ${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa/sysio.roa.abi
        DEPENDS ${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.wasm
                ${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.abi
                ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.wasm
                ${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.abi
)

# Could not figure out how to make this work correctly.
# builds the wasm file and then the next build includes the wasm file in the library. Before the only
# way to get it to work was to remove the contracts.o file. This is better than a clean everytime.
if(BUILD_TEST_CONTRACTS)
  set(CONTRACT_WASM_FILES
    "${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.wasm"
    "${CMAKE_BINARY_DIR}/contracts/sysio.roa/sysio.roa.abi"
    "${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.wasm"
    "${CMAKE_BINARY_DIR}/contracts/sysio.bios/sysio.bios.abi"
    "${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios/sysio.bios.wasm"
    "${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.bios/sysio.bios.abi"
    "${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa/sysio.roa.wasm"
    "${CMAKE_BINARY_DIR}/libraries/testing/contracts/sysio.roa/sysio.roa.abi"
  )

  # Make the main testing library wait for the contracts to be built.
  add_dependencies(sysio_testing sysio_testing_contracts contracts_project)

  # Set the explicit dependency. This is the magic link.
  #    It tells CMake: "If ${CONTRACT_WASM_FILE} is newer than contracts.cpp.o, recompile contracts.cpp".
  #    Note: The source file is specified relative to the current CMakeLists.txt.
  set_source_files_properties(
    "contracts.cpp"
    PROPERTIES OBJECT_DEPENDS "${CONTRACT_WASM_FILES}"
  )
endif()

target_link_libraries(sysio_testing sysio_testing_contracts sysio_chain fc chainbase Logging IR WAST WASM)
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