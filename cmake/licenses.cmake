configure_file(${CMAKE_SOURCE_DIR}/LICENSE.md                                                  licenses/sysio/LICENSE.md        COPYONLY)

# TODO: prometheus-cpp, softfloat and CLI11 license files are no longer available since moving to vcpkg
# The licenses would need to be copied separately from vcpkg or original repositories
#configure_file(${CMAKE_SOURCE_DIR}/libraries/libfc/libraries/boringssl/boringssl/src/LICENSE   licenses/sysio/LICENSE.boringssl COPYONLY)
#configure_file(${CMAKE_SOURCE_DIR}/libraries/libfc/include/fc/crypto/webauthn_json/license.txt licenses/sysio/LICENSE.rapidjson COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/libfc/libraries/bls12-381/LICENSE                 licenses/sysio/LICENSE.bls12-381 COPYONLY)
#configure_file(${CMAKE_SOURCE_DIR}/libraries/libfc/secp256k1/secp256k1/COPYING                 licenses/sysio/LICENSE.secp256k1 COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/sys-vm/LICENSE                                    licenses/sysio/LICENSE.sys-vm    COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/wasm-jit/LICENSE                                  licenses/sysio/LICENSE.wavm      COPYONLY)