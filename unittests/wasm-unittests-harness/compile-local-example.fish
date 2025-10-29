#!/usr/bin/env fish

set CDT_ROOT /opt/prefixes/wire-001

set INCLUDE_PATHS \
  -I../../contracts/sysio.system/include \
  -I../../contracts/sysio.depot/include \
  -I../../libraries/libfc-lite/include \
  -I$CDT_ROOT/include/sysiolib/capi \
      -I$CDT_ROOT/include/sysiolib/core \
      -I$CDT_ROOT/include/sysiolib/native \
      -I$CDT_ROOT/include/sysiolib/contracts \
      -I$CDT_ROOT/include/bluegrass \
      -I$CDT_ROOT/include

/opt/prefixes/wasi-sysroot/bin/clang++ --target=wasm32-wasi \
  --sysroot=/opt/prefixes/wasi-sysroot/share/wasi-sysroot \
  $INCLUDE_PATHS \
  -DWASMTIME \
  -O2 -fno-exceptions \
  -Wl,--export=__heap_base \
  -Wl,--export=__data_end -Wl,--export=main \
  \
  -Wl,--export-table \
  -Wl,--no-gc-sections \
  -std=c++23 -lc++ -lc++abi -o example_boost_wasi_test.wasm \
  example_boost_wasi_test.cpp
