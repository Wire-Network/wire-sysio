# epoch_stub.wasm / .abi are committed and read from ${CMAKE_SOURCE_DIR} by the test embed
# (contracts/tests/contracts.hpp.in), like block_transfer: contract-test wasms are not reliably placed in
# the build tree under CI's ExternalProject layout, so the committed artifact is the source of truth.
# add_contract still compiles the source as a build-time check; if epoch_stub.cpp or the epoch state it
# writes changes, rebuild and re-commit the wasm/abi.
if(BUILD_SYSTEM_CONTRACTS)
    add_contract(epoch_stub epoch_stub epoch_stub.cpp)
    target_include_directories(epoch_stub
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../sysio.epoch/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../sysio.opp.common/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../../../libraries/opp/generated-cdt>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../libraries/libfc-lite/include>
    )
endif()
