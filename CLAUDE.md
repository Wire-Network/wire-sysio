# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Wire Sysio is a C++ implementation of the AntelopeIO protocol (a fork of Spring), containing blockchain node software and supporting tools. The main executable is `nodeop` (blockchain node), with supporting tools `clio` (CLI client), `kiod` (key store daemon), and `sys-util`.

## Build Commands

**Note:** The build directory MUST be located under `<wire-sysio>/build/`, examples include `<wire-sysio>/build/claude`, `<wire-sysio>/build/debug-claude`, etc). Examples below use `$BUILD_DIR` — substitute your actual build path.

### Prerequisites (one-time setup)
```bash
# Install system packages (Ubuntu 24.04+)
sudo apt-get install -y build-essential binutils ccache cmake curl git ninja-build \
    libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-pip clang-18 libclang-18-dev

# Bootstrap vcpkg
./vcpkg/bootstrap-vcpkg.sh
```

### Configure and Build
```bash

# Clear `linuxbrew` from PATH (if present) to avoid conflicts with system libraries and compilers.
# This is important for consistent builds.
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v linuxbrew | tr '\n' ':' | sed 's/:$//')

# Example build directory
export BUILD_DIR=$PWD/build/claude

# Set compiler environment
export CC=/usr/bin/clang-18
export CXX=/usr/bin/clang++-18

# Configure with CMake (Ninja recommended)
cmake \
-B $BUILD_DIR \
-S . \
-G Ninja \
-DCMAKE_BUILD_TYPE=Debug \
-DBUILD_SYSTEM_CONTRACTS=ON \
-DBUILD_TEST_CONTRACTS=ON \
-DENABLE_CCACHE=ON \
-DENABLE_DISTCC=OFF \
-DENABLE_TESTS=ON \
-DCMAKE_INSTALL_PREFIX=/opt/prefixes/wire-001 \
-DCMAKE_PREFIX_PATH="/opt/prefixes/wire-001" \
-DCMAKE_TOOLCHAIN_FILE=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake

export NUM_JOBS=$(echo $(($(nproc) - 2)))

# Build (use -j${NUM_JOBS} to avoid memory exhaustion; some files need 4GB RAM)
cmake --build $BUILD_DIR -- -j${NUM_JOBS}
```

### Building Specific Targets
```bash
ninja -C $BUILD_DIR fc              # Build libfc library only
ninja -C $BUILD_DIR test_fc         # Build fc tests
ninja -C $BUILD_DIR nodeop          # Build main node executable
ninja -C $BUILD_DIR unit_test       # Build unit tests
```

## Testing Commands

### Run All Parallelizable Tests
```bash
cd $BUILD_DIR && ctest -j "$(nproc)" -LE _tests
```

**Tip:** ctest runs take a long time. Always log output to a temp file so you can grep/tail without re-running:
```bash
cd $BUILD_DIR && ctest -j "$(nproc)" -LE "(nonparallelizable_tests|long_running_tests|wasm_spec_tests)" --output-on-failure --timeout 1000 2>&1 | tee /tmp/ctest-run.log
# Then analyze without re-running:
grep "Failed" /tmp/ctest-run.log
grep "% tests passed" /tmp/ctest-run.log
```

### Run Specific Test Suite
```bash
# Run a single Boost.Test suite
./$BUILD_DIR/libraries/libfc/test/test_fc --run_test=solana_client_tests

# Run specific test case
./$BUILD_DIR/libraries/libfc/test/test_fc --run_test=solana_client_tests/test_pubkey_base58_roundtrip

# Run unit tests with specific WASM runtime
./$BUILD_DIR/unittests/unit_test --run_test=block_tests -- --sys-vm
./$BUILD_DIR/unittests/unit_test --run_test=block_tests -- --sys-vm-jit
./$BUILD_DIR/unittests/unit_test --run_test=block_tests -- --sys-vm-oc
```

### Test Categories
```bash
# Run from $BUILD_DIR
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
- **libfc** (`libraries/libfc/`) - Foundation library providing:
  - Crypto: SHA256, RIPEMD160, Keccak256, secp256k1, NIST P-256 (R1), BLS, Ed25519, WebAuthn
  - Serialization: JSON via `fc::variant`, binary packing
  - Network clients: HTTP/JSON-RPC, Ethereum (ABI/RLP encoding), Solana (Borsh/IDL)
  - Logging framework with configurable sinks
- **chain** (`libraries/chain/`) - Blockchain core: block/transaction processing, WASM execution (sys-vm, sys-vm-jit, sys-vm-oc runtimes), authorization, resource limits
- **appbase** (`libraries/appbase/`) - Application framework for plugin management

### Plugin Architecture
Plugins are static libraries linked with whole-archive into the main executable. Each plugin directory contains:
- `include/sysio/<plugin_name>/` - Public headers
- `src/` - Implementation
- `test/` (optional) - Plugin-specific tests with CMakeLists.txt

Key plugins: chain_plugin, producer_plugin, net_plugin, http_plugin, wallet_plugin, state_history_plugin, outpost_ethereum_client_plugin, outpost_solana_client_plugin

Plugin lifecycle (see `plugins/usage_pattern.md` for details):
1. **Registration** - `APPBASE_PLUGIN_REQUIRES` declares dependencies
2. **Initialization** - Configure plugin from options, connect signals, create objects
3. **Startup** - Activate io_contexts, thread pools, establish connections
4. **Shutdown** - Stop threads, cancel timers (reverse order of startup)

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

## Git Practices

**NEVER use `git add -A` or `git add .`** — these will stage build artifacts, core dumps, submodules, and other untracked files. Always stage specific files by name.

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
| `ENABLE_OC` | ON | Enable sys-vm-oc LLVM JIT optimization (x86_64 Linux) |

## WASM Runtimes

Three WASM execution runtimes available (x86_64 Linux):
- `sys-vm` - Standard interpreter
- `sys-vm-jit` - JIT compiled
- `sys-vm-oc` - LLVM-optimized JIT

Tests run against all runtimes by default. Specify runtime with `-- --sys-vm`, `-- --sys-vm-jit`, or `-- --sys-vm-oc`.

## OPP Protobufs & Libraries

WIRE uses OPP to communicate between WIRE BLOCKCHAIN and EXTERNAL BLOCKCHAINS.

OPP is a protocol (encoding scheme) that uses protobufs; the library is located at [libraries/opp](libraries/opp).
The protobufs are located at [libraries/opp/proto](libraries/opp/proto).

After updating the protobufs, before you can use them in the ethereum/solana contract/program repos, you need to run `cd wire-sysio/libraries/opp/tools &&
  ./generate-opp-bundles.fish  --target=[solidity|solana]`. Additionally, WIRE contract code uses a C++ protoc plugin named `zpp_bits`, this is largely irrelevant,
but keep in mind they are generated when the project is configured/built (CMake). Just an FYI.

### TARGET: solidity

**IMPORTANT** IF `<wire-sysio>/../wire-opp/solidity` exists, then run:
```shell
rm -Rf <wire-sysio>/../wire-opp/solidity/node_modules || true
cp -R <wire-sysio>/build/opp/solidity/* <wire-sysio>/../wire-opp/solidity/ && \
    cd <wire-sysio>/../wire-opp/solidity && \
    pnpm i
```

## Docker Build

```bash
# Build using Docker (includes all dependencies)
./scripts/docker-build.sh

# Build from local source
./scripts/docker-build.sh --target=app-build-local
```

### Python Integration Tests

The `tests/` directory contains Python integration tests using TestHarness framework (`TestHarness/Cluster.py`, `TestHarness/Node.py`, `TestHarness/Launcher.py`).

**IMPORTANT:** CMake copies the Python test scripts into the build directory. Always run them **from the build directory** so they use the correct built binaries and copied test scripts:
```bash
cd $BUILD_DIR && python3 tests/<test_name>.py
```
Do NOT run from the source root — that would use stale or missing binaries.

## Smart Contract Compilation (System Contracts specifically)

The system contracts are compiled via the `contracts_project` CMake target.

> DO NOT COMPILE THE SYSTEM CONTRACTS DIRECTLY! Always use the `contracts_project` target.

### CDT-Generated Artifacts

> NOTE: There are `.gitignore` files specifically excluding the artifacts described below
> but just in case, here are the details

CDT generates `.actions.cpp`, `.dispatch.cpp`, and `.desc` files alongside compiled contracts. These are **not committed** — `.gitignore` files in `contracts/` and `unittests/test-contracts/` exclude them. If they appear as untracked, delete them:

```bash
find contracts/ unittests/test-contracts/ -name "*.actions.cpp" -o -name "*.dispatch.cpp" -o -name "*.desc" | xargs rm -f
```

## Generate client types for system contracts

> NOTE: In a dev environment, `pnpm link` should be configured to avoid the need to publish
> the contract types changes until fully integrated. If the host OS system username is in
> the following list, then packages have been linked and the following does not
> require publishing [`jglanz`]

To generate the client types for the system contracts,run the following commands.

`<wire-sysio>/contracts/tools/generate-system-contract-types.py -B . -O /tmp/ctt -P snake -f` then `cp
  /tmp/ctt/typescript/SystemContractTypes.ts  <wire-libraries-ts>/packages/sdk-core/src/types/` and lastly run `cd <wire-libraries-ts> &&
  pnpm build`

This makes the types available in the SDK as:
```ts
import { SystemContracts } from '@wire-libraries/sdk-core';
```


## Regenerating Test Reference Data

Some tests compare against pre-generated reference data. When contracts are recompiled (different WASM = different action merkle roots), this data must be regenerated.

### Deep Mind Log

The `deep_mind_tests` compare against `unittests/deep-mind/deep-mind.log`. To regenerate:
```bash
$BUILD_DIR/unittests/unit_test --run_test=deep_mind_tests -- --sys-vm --save-dmlog
```

### Snapshot Compatibility Data

The `snapshot_part2_tests/test_compatible_versions` test uses reference blockchain and snapshot files in `unittests/snapshots/`. To regenerate:

**Step 1:** Delete stale files from BOTH source and build directories. The `--save-snapshot` run replays blocks.log if it exists — if it contains WASMs with old host function signatures, replay fails with `wasm_serialization_error: wrong type for imported function`. You must delete first:
```bash
rm -f unittests/snapshots/blocks.* unittests/snapshots/snap_v1.*
rm -f $BUILD_DIR/unittests/snapshots/blocks.* $BUILD_DIR/unittests/snapshots/snap_v1.*
```

**Step 2:** Regenerate. This creates a fresh blockchain, deploys the current embedded contracts, and writes new reference files:
```bash
$BUILD_DIR/unittests/unit_test --run_test="snapshot_part2_tests/*" -- --sys-vm --save-snapshot --generate-snapshot-log
```
The test writes to `$BUILD_DIR/unittests/snapshots/` (NOT the source tree, despite the flag description).

**Step 3:** Copy from build dir to source tree (for git), and ensure the build dir has them for subsequent test runs:
```bash
cp $BUILD_DIR/unittests/snapshots/blocks.* $BUILD_DIR/unittests/snapshots/snap_v1.* unittests/snapshots/
```

**Step 4:** Re-run CMake or ninja so `configure_file` picks up the new source-tree files:

```bash
cmake --build $BUILD_DIR --target unit_test
```
If CMake fails because snapshot files are missing from the source tree, run step 3 first.

**Common pitfall:** If you only delete source-tree files but not build-dir files, the test replays the stale build-dir blocks.log and fails. Always delete from both locations.

### Consensus Blockchain Data

The `savanna_misc_tests/verify_block_compatibitity` test uses `unittests/test-data/consensus_blockchain/`. To regenerate:
```bash
$BUILD_DIR/unittests/unit_test -t "savanna_misc_tests/verify_block_compatibitity" -- --sys-vm --save-blockchain
```

### When to Regenerate

Regenerate all reference data whenever:
- Any production contract is recompiled (changes action merkle roots)
- Chain-level serialization changes (block format, snapshot format)
- Genesis intrinsics change (different genesis state)
