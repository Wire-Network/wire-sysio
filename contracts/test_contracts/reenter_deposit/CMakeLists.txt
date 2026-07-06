# reenter_deposit.wasm / .abi are committed and read from ${CMAKE_SOURCE_DIR} by the
# test embed (contracts/tests/contracts.hpp.in), like reject_all -- contract-test wasms
# are not reliably placed in the build tree under CI's ExternalProject layout, so the
# committed artifact is the source of truth. add_contract still compiles the source as a
# build-time check; if reenter_deposit.cpp changes, rebuild and re-commit the wasm/abi.
if(BUILD_SYSTEM_CONTRACTS)
    add_contract(reenter_deposit reenter_deposit reenter_deposit.cpp)
endif()
