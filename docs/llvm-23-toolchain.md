# LLVM and libc++ 23.1.3

Wire host programs and their C++ dependencies use libc++ 23.1.3, libc++abi and
libunwind from the matching LLVM distribution. Clang builds use compiler-rt;
GCC builds retain libgcc for compiler builtins. Release programs
link the C++ runtime statically. Debug native-module builds share the pinned
libc++/libc++abi/libunwind so exceptions can cross the loaded module boundary;
Debug packages include those libraries with a relative installation RPATH.
The default host
compiler is Clang 23.1.3. The GCC CI lane uses GCC 15 with the same libc++ headers
and static runtime. Reconfigure in a new build directory when moving from
libstdc++ or an older libc++; existing C++ archives cannot be mixed across ABIs.

## Provision and build

The installer verifies the pinned upstream release archive before extracting it.
It supports Linux x86_64/AArch64 and macOS AArch64. Python 3.11+, CMake, curl and
tar with xz support are required.

```sh
bash scripts/llvm/install-toolchain.sh "$PWD/build/llvm-23.1.3"
export WIRE_LLVM_ROOT="$PWD/build/llvm-23.1.3"
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SYSTEM_CONTRACTS=ON -DBUILD_TEST_CONTRACTS=ON \
  -Dcdt_DIR=/path/to/unchanged/cdt/lib/cmake/cdt
cmake --build build/release --parallel 8
```

For Intel macOS or a source-built distribution, use
`bash scripts/llvm/build-toolchain.sh PREFIX BUILD_DIRECTORY` with an LLVM-supported
bootstrap compiler. Export that prefix as `WIRE_LLVM_ROOT`. The source builder
verifies the pinned source archive and builds both X86 and AArch64 backends.
To select GCC for both the host project and its dependencies, set
`CC=gcc-15 CXX=g++-15` for a fresh configure. The triplets track these compiler
overrides in the dependency cache. All paths must refer to native host toolchains.

`cmake/llvm-toolchain.cmake` applies the same C++ ABI to the host project and
vcpkg dependencies. `lib/wire-static` contains only links to the distribution's
static runtime archives, preventing shared-library selection while supporting
autoconf dependency probes. Configuration checks `_LIBCPP_VERSION == 230103`
and links an exception probe. CI also runs `libcxx_runtime_test` and inspects
the final programs for accidental shared C++ runtime dependencies.

## Dependency and runtime scope

The default vcpkg baseline stays fixed. LLVM is overridden to 23.1.3 using upstream vcpkg commit
`c52c09caca13081c628ade492d334033f7130edc`; its upstream patch set replaces the
18.1.6 port patches. The pinned Wire registry carries the following compatibility
patches while retaining existing dependency source revisions:

- BLS12-381 explicitly includes `<vector>` rather than depending on a transitive include.
- GMP's custom C++ compiler probe honors `LDFLAGS` and `LIBS` for libc++ linkage.
- sys-vm preserves caller C++ flags and qualifies its `module` type for Clang 23.
- AWS CRT disables speculative devirtualization only for its MQTT adapter source
  when compiled with GCC 15 and libc++ 23, avoiding recursive `std::function`
  inlining failures. Clang and libstdc++ builds retain their optimization settings.

The LLVM dependency builds X86 and AArch64 targets with RTTI. Runtime platform
gates remain in the root `CMakeLists.txt`; this upgrade does not enable OC on
additional platforms. OC continues to use the existing RuntimeDyld compilation
pipeline. Its persisted cache format and code-generation version advance to 4,
so previous native code is discarded and compiled again after restart.

CDT remains an independent toolchain. The [companion CDT header fix](https://github.com/Wire-Network/wire-cdt/commit/03603e7676b23c6a7c9393790a8288b33e8e7ab0) removes
redundant byte-string datastream overloads that modern host libc++ cannot parse;
CDT's compiler and contract standard library stay unchanged. Native Debug builds
require a CDT SDK containing that header fix. Neither host compiler flags nor the host
libc++ headers are passed into the CDT contract child project. Build contracts
through `contracts_project` and compare generated WASM/ABI files with the
tracked artifacts; this host upgrade must not change those artifacts.

## Consumer and runtime checks

Installed `SysioTester.cmake` requires a consumer-owned `WIRE_LLVM_ROOT` with
the same version. Provision the toolchain before configuring the consumer; do
not reuse C++ dependencies built with libstdc++. The package checks the selected
headers and runtime at configure time rather than retaining producer compiler
paths.

Select the installed toolchain before the consumer's `project()` enables C++:

```sh
cmake -S /path/to/consumer -B /path/to/consumer/build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/sysio/lib/cmake/sysio/llvm-toolchain.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/sysio
```

The standalone probe can validate a toolchain before building Wire:

```sh
cmake -S cmake/test/libcxx -B build/libcxx-probe -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/llvm-toolchain.cmake"
cmake --build build/libcxx-probe
ctest --test-dir build/libcxx-probe --output-on-failure
python3 .github/scripts/check-libcxx-linkage.py build/libcxx-probe/libcxx_runtime_test
```

The probe exercises allocation, exceptions, RTTI and threads. Configure a separate
probe directory with `-DCMAKE_BUILD_TYPE=Debug` to additionally check bidirectional
exceptions, destructors, RTTI and ownership across a loaded native module.
OC limit, interrupt,
cache restart and WASM suites cover code generation and execution; GCC and macOS
CI builds exercise their existing supported runtime sets.
