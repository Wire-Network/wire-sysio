# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Wire Sysio is a C++ implementation of the AntelopeIO protocol (a fork of Spring), containing blockchain node software and supporting tools. The main executable is `nodeop` (blockchain node), with supporting tools `clio` (CLI client), `kiod` (key store daemon), and `sys-util`.

## Build Commands

### Prerequisites (one-time setup)
```bash
# Install system packages (Ubuntu 24.04+)
sudo apt-get install -y build-essential binutils ccache cmake curl git ninja-build \
    libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-pip

# Build Clang 18 from source
export BASE_DIR=/opt/clang
sudo mkdir -p "$BASE_DIR" && sudo chown "$USER":"$USER" "$BASE_DIR"
./scripts/clang-18/clang-18-ubuntu-build-source.sh

# Build LLVM 11 from source (required for sys-vm-oc JIT)
export BASE_DIR=/opt/llvm
sudo mkdir -p "$BASE_DIR" && sudo chown "$USER":"$USER" "$BASE_DIR"
./scripts/llvm-11/llvm-11-ubuntu-build-source.sh

# Bootstrap vcpkg
./vcpkg/bootstrap-vcpkg.sh
```

### Configure and Build
```bash
# Set compiler environment
export CC=/opt/clang/clang-18/bin/clang
export CXX=/opt/clang/clang-18/bin/clang++

# Configure with CMake (Ninja recommended)
cmake -B build -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/llvm/llvm-11;/opt/clang/clang-18" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DENABLE_CCACHE=ON \
  -DENABLE_TESTS=ON

# Build (use -j4 to avoid memory exhaustion; some files need 4GB RAM)
cmake --build build -- -j4
```

### Building Specific Targets
```bash
ninja -C build fc              # Build libfc library only
ninja -C build test_fc         # Build fc tests
ninja -C build nodeop          # Build main node executable
ninja -C build unit_test       # Build unit tests
```

## Testing Commands

### Run All Parallelizable Tests
```bash
cd build && ctest -j "$(nproc)" -LE _tests
```

### Run Specific Test Suite
```bash
# Run a single Boost.Test suite
./build/libraries/libfc/test/test_fc --run_test=solana_client_tests

# Run specific test case
./build/libraries/libfc/test/test_fc --run_test=solana_client_tests/test_pubkey_base58_roundtrip

# Run unit tests with specific WASM runtime
./build/unittests/unit_test --run_test=block_tests -- --sys-vm
./build/unittests/unit_test --run_test=block_tests -- --sys-vm-jit
./build/unittests/unit_test --run_test=block_tests -- --sys-vm-oc
```

### Test Categories
```bash
ctest -j "$(nproc)" -LE _tests           # Parallelizable unit tests
ctest -j "$(nproc)" -L wasm_spec_tests   # WASM specification tests (CPU-intensive)
ctest -L "nonparallelizable_tests"       # Serial component/integration tests
ctest -L "long_running_tests"            # Long-running integration tests
```

## Architecture Overview

### Directory Structure
- **libraries/** - Core libraries (libfc, chain, appbase, testing, state_history)
- **plugins/** - Plugin system (~25 plugins linked into nodeop)
- **programs/** - Executables (nodeop, clio, kiod, sys-util)
- **contracts/** - Smart contracts (sysio.system, sysio.token, sysio.msig)
- **unittests/** - Boost.Test unit tests
- **tests/** - Python integration tests using TestHarness

### Key Libraries
- **libfc** (`libraries/libfc/`) - Foundation library: crypto, serialization, HTTP/JSON-RPC clients, logging. Contains network clients for Ethereum and Solana.
- **chain** (`libraries/chain/`) - Blockchain core: block/transaction processing, WASM execution (sys-vm, sys-vm-jit, sys-vm-oc runtimes), authorization, resource limits
- **appbase** (`libraries/appbase/`) - Application framework for plugin management

### Plugin Architecture
Plugins are static libraries linked with whole-archive into the main executable. Each plugin directory contains:
- `include/` - Public headers
- `src/` - Implementation
- `test/` (optional) - Plugin-specific tests with CMakeLists.txt

Key plugins: chain_plugin, producer_plugin, net_plugin, http_plugin, wallet_plugin, state_history_plugin

### Serialization Patterns
```cpp
// Types use fc::variant for JSON serialization
void to_variant(const MyType& e, fc::variant& v);
void from_variant(const fc::variant& e, MyType& v);

// Use FC_REFLECT for automatic serialization
FC_REFLECT(my_namespace::my_type, (field1)(field2)(field3))

// For enums, use FC_REFLECT_ENUM
FC_REFLECT_ENUM(my_namespace::my_enum, (value1)(value2)(value3))
```

## Code Style

Uses `.clang-format` with LLVM base style and these key differences:
- **Indent: 3 spaces** (not 4)
- **Line limit: 120 characters**
- **Pointer alignment: Left** (`int* ptr` not `int *ptr`)
- **Constructor initializers: Break before comma**

Format code: `clang-format -i <file>`

## Key CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTS` | ON | Build test executables |
| `ENABLE_CCACHE` | ON | Use compiler cache |
| `BUILD_TEST_CONTRACTS` | OFF | Compile test smart contracts |
| `DONT_SKIP_TESTS` | FALSE | Include currently failing tests |
| `DISABLE_WASM_SPEC_TESTS` | OFF | Skip WASM spec compliance tests |

## WASM Runtimes

Three WASM execution runtimes available (x86_64 Linux):
- `sys-vm` - Standard interpreter
- `sys-vm-jit` - JIT compiled
- `sys-vm-oc` - LLVM-optimized JIT (requires LLVM 11)

Tests run against all runtimes by default. Specify runtime with `-- --sys-vm`, `-- --sys-vm-jit`, or `-- --sys-vm-oc`.

## Docker Build

```bash
# Build using Docker (includes all dependencies)
./scripts/docker-build.sh

# Build from local source
./scripts/docker-build.sh --target=app-build-local
```
