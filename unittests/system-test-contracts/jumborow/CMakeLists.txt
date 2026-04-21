# jumborow.wasm is pre-compiled from jumborow.wast (hand-written WAT).
# To rebuild after editing jumborow.wast:
#   $CDT/bin/sysio-wast2wasm -o jumborow.wasm jumborow.wast
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/jumborow.wasm ${CMAKE_CURRENT_BINARY_DIR}/jumborow.wasm COPYONLY )
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/jumborow.abi  ${CMAKE_CURRENT_BINARY_DIR}/jumborow.abi  COPYONLY )
