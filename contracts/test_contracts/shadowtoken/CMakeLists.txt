# shadowtoken.wasm / .abi are committed, and bootstrap_contract copies them into
# the build tree where the test embed (contracts/tests/contracts.hpp.in) loads
# them. That copy is what makes the suite run in CI at all: PR builds set
# BUILD_SYSTEM_CONTRACTS=OFF and compile no contract, so a committed artifact is
# the only one there. add_contract still compiles the source as a build-time
# check and overwrites the copy locally, so if shadowtoken.cpp changes, rebuild
# and re-commit the wasm/abi.
bootstrap_contract(shadowtoken)
if(BUILD_SYSTEM_CONTRACTS)
    add_contract(shadowtoken shadowtoken shadowtoken.cpp)
    target_include_directories(shadowtoken
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../sysio.opp.common/include>)
endif()
