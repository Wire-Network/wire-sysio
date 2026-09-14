# badtoken.wasm / .abi are committed, and bootstrap_contract copies them into the
# build tree where the test embed (contracts/tests/contracts.hpp.in) loads them.
# That copy is what makes the suite run in CI at all: PR builds set
# BUILD_SYSTEM_CONTRACTS=OFF and compile no contract, so a committed artifact is
# the only one there. add_contract still compiles the source as a build-time
# check and overwrites the copy locally, so if badtoken.cpp changes, rebuild and
# re-commit the wasm/abi.
bootstrap_contract(badtoken)
if(BUILD_SYSTEM_CONTRACTS)
    add_contract(badtoken badtoken badtoken.cpp)
endif()
