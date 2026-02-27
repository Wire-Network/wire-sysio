# Debugging Contracts with the Native Module Runtime

The native-module runtime lets you debug smart contracts with standard C/C++ debuggers (LLDB, GDB) by compiling them as native shared libraries (`.so`) instead of executing WASM bytecode. You can set breakpoints, step through code, and inspect variables in your contract source.

## Prerequisites

1. **wire-cdt** source tree checked out locally (e.g., `/path/to/wire-cdt`).

2. **wire-sysio** configured with native-module support:
   ```bash
   cmake -B cmake-build-debug -S . -G Ninja \
     -DCMAKE_BUILD_TYPE=Debug \
     -DNATIVE_CDT_DIR=/path/to/wire-cdt \
     -DBUILD_SYSTEM_CONTRACTS=ON \
     -DENABLE_TESTS=ON \
     # ... other flags as usual
   ```
   The `NATIVE_CDT_DIR` variable points to the wire-cdt source root. CDT's
   sysiolib headers and source files (`crypto.cpp`, `sysiolib.cpp`, `base64.cpp`)
   are compiled directly into each native `.so` by the host compiler.

3. **Build** the test executables and native contracts:
   ```bash
   # Build all native contracts + both test executables
   ninja -C cmake-build-debug unit_test contracts_unit_test \
     sysio.bios_native sysio.token_native sysio.msig_native \
     sysio.wrap_native sysio.system_native sysio.roa_native
   ```
   This produces:
   - `cmake-build-debug/unittests/unit_test` — chain/WASM unit tests (exports intrinsic symbols)
   - `cmake-build-debug/contracts/tests/contracts_unit_test` — contract-specific tests (system, token, msig, roa, etc.)
   - `cmake-build-debug/contracts/<name>/<name>_native.so` — native contract shared libraries

## LLDB Setup (required)

The blockchain uses a real-time signal (`SIG34` / `SIGRTMIN`) for transaction deadline enforcement. LLDB stops on this signal by default, which makes debugging impossible.

Add this to `~/.lldbinit`:

```
process handle SIG34 --notify false --pass true --stop false
```

This tells LLDB to silently pass the signal through to the process.

## How It Works

When you run tests with `--native-module`:

1. The test framework deploys WASM bytecode via `set_code()` (normal path)
2. It also creates a symlink: `<temp_dir>/<sha256_of_wasm>.so -> sysio.bios_native.so`
3. When a transaction triggers `apply()`, the native runtime does `dlopen(<hash>.so)` + `dlsym("apply")`
4. The contract's native `apply()` calls intrinsics (`db_store_i64`, `require_auth`, etc.) which resolve to symbols exported from the test executable
5. You can set breakpoints anywhere in the contract source code

## Running Tests

There are two test executables, each covering different contracts:

| Executable | Location |
|------------|----------|
| `unit_test` | `cmake-build-debug/unittests/unit_test` |
| `contracts_unit_test` | `cmake-build-debug/contracts/tests/contracts_unit_test` |

Run with `--native-module` to use native contracts:

```bash
# Chain-level tests
./cmake-build-debug/unittests/unit_test --run_test=currency_tests -- --native-module

# Contract-level tests (system, roa, etc.)
./cmake-build-debug/contracts/tests/contracts_unit_test --run_test=sysio_roa_tests -- --native-module

# Run all contract tests natively
./cmake-build-debug/contracts/tests/contracts_unit_test -- --native-module
```

## Debugging in CLion

### 1. Create a Run/Debug Configuration

- **Run > Edit Configurations > + > Custom Build Application**
- **Executable**: select `cmake-build-debug/unittests/unit_test` or `cmake-build-debug/contracts/tests/contracts_unit_test`
- **Program arguments**: `--run_test=sysio_roa_tests -- --native-module`
- **Working directory**: the project root

If using CLion's built-in CMake integration, you can instead:
- Select the `unit_test` or `contracts_unit_test` target from the CMake tool window
- Edit its run configuration to add program arguments: `--run_test=sysio_roa_tests -- --native-module`

### 2. Set Breakpoints

Open the contract source file (e.g., `contracts/sysio.bios/sysio.bios.cpp`) and click the gutter to set a breakpoint on any line — for example, inside `bios::setfinalizer()`.

The native `.so` is compiled with debug symbols (`-g`), so CLion resolves source locations after the `.so` is loaded.

### 3. Debug

Click the **Debug** button (or press Shift+F9). The test will run with the native runtime, and execution will stop at your breakpoint.

From there you can:
- **Step Into** (F7) — step into CDT library calls, intrinsic wrappers, etc.
- **Step Over** (F8) — execute the current line
- **Evaluate Expression** — inspect `multi_index` iterators, `name` values, action data
- **View Variables** — see contract state in the Variables pane

### 4. Run a Specific Test Case

Narrow to a single test case to reduce noise:

```
--run_test=bios_tests/setfinalizer_test -- --native-module
```

## Debugging on the Command Line (LLDB)

```bash
lldb -- ./cmake-build-debug/unittests/unit_test \
  --run_test=bios_tests -- --native-module
```

Set a breakpoint before running. Use a **pending breakpoint** since the `.so` isn't loaded yet:

```
(lldb) b sysio.bios.cpp:42
Breakpoint 1: no locations (pending).
WARNING: Unable to resolve breakpoint to any currently loaded shared library.
(lldb) run
```

LLDB will resolve the breakpoint when the `.so` is dlopen'd and stop there.

Useful LLDB commands during debugging:

```
(lldb) bt              # backtrace
(lldb) frame variable  # show local variables
(lldb) p my_var        # print a variable
(lldb) n               # step over
(lldb) s               # step into
(lldb) c               # continue
```

## Debugging on the Command Line (GDB)

```bash
gdb --args ./cmake-build-debug/unittests/unit_test \
  --run_test=bios_tests -- --native-module
```

Handle the real-time signal first, then set a pending breakpoint:

```
(gdb) handle SIG34 nostop noprint pass
(gdb) set breakpoint pending on
(gdb) b sysio.bios.cpp:42
(gdb) run
```

## Debugging in VS Code

### 1. Install the CodeLLDB Extension

Install [CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb) (recommended) or the Microsoft C/C++ extension.

### 2. Add a Launch Configuration

Add to `.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Debug Native Contract",
      "type": "lldb",
      "request": "launch",
      "program": "${workspaceFolder}/cmake-build-debug/unittests/unit_test",
      "args": [
        "--run_test=bios_tests",
        "--",
        "--native-module"
      ],
      "cwd": "${workspaceFolder}",
      "initCommands": [
        "process handle SIG34 --notify false --pass true --stop false"
      ]
    }
  ]
}
```

If using the Microsoft C/C++ extension (`cppdbg` type) with GDB instead:

```json
{
  "name": "Debug Native Contract (GDB)",
  "type": "cppdbg",
  "request": "launch",
  "program": "${workspaceFolder}/cmake-build-debug/unittests/unit_test",
  "args": [
    "--run_test=bios_tests",
    "--",
    "--native-module"
  ],
  "cwd": "${workspaceFolder}",
  "setupCommands": [
    {
      "text": "handle SIG34 nostop noprint pass"
    },
    {
      "text": "set breakpoint pending on"
    }
  ]
}
```

### 3. Set Breakpoints and Debug

Open the contract source file, set breakpoints in the gutter, and press F5 to start debugging.

## Adding Native Builds for Other Contracts

To make another contract debuggable:

### 1. Add a CMake Target

The dispatch file (the `apply()` entry point) is auto-generated from the contract's ABI at build time. Just add a `native_contract_target()` call in the contract's `CMakeLists.txt`:

```cmake
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES AND NATIVE_CDT_DIR)
   native_contract_target(
      TARGET         mycontract_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/mycontract.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
      CONTRACT_CLASS "myns::mycontract"
      HEADERS        "mycontract.hpp"
      ABI_TARGET     mycontract
   )
endif()
```

- **CONTRACT_CLASS**: The fully-qualified C++ class name (as used in `SYSIO_DISPATCH`)
- **HEADER**: The contract header file (used in the `#include` of the generated dispatch)
- **ABI_TARGET**: The CMake target name that produces the `.abi` file (usually the contract name)

The script `scripts/gen_native_dispatch.py` reads the `.abi` JSON, extracts all action names, and generates a dispatch `.cpp` with `SYSIO_DISPATCH`. This runs automatically whenever the ABI changes.

The naming convention `<name>_native.so` alongside `<name>.wasm` is important — the test framework auto-discovers native `.so` files by scanning the contracts build directory at startup. No manual registration is needed.

### 2. Build and Test

```bash
ninja -C cmake-build-debug unit_test mycontract_native
./cmake-build-debug/unittests/unit_test --run_test=mycontract_tests -- --native-module
```

## Troubleshooting

**LLDB keeps stopping on SIG34**
Verify `~/.lldbinit` contains `process handle SIG34 --notify false --pass true --stop false` and restart the debugger.

**Breakpoints show "no locations (pending)"**
This is normal before the `.so` is loaded. The breakpoint resolves when `dlopen` loads the contract. If it never resolves, verify the `.so` was built with `-g` (debug symbols) and the source path matches.

**SIGABRT during exception handling**
Both the test executable and the `.so` must link against shared `libgcc_s.so` for C++ exception unwinding to work across the dlopen boundary. This is handled automatically by `cmake/test-tools.cmake` and `cmake/contract-tools.cmake`. If you see this, check that the CMake configuration is correct.

**Tests pass with `--sys-vm` but crash with `--native-module`**
The contract is compiled as native x86-64 code where `sizeof(void*)` is 8, not 4 as in WASM. Watch for CDT code that assumes 32-bit pointer arithmetic — e.g., negating a `uint32_t` used in `std::advance()` wraps to a large positive value on 64-bit instead of the expected negative offset.

**`dlopen failed` errors**
Check that the `.so` was built and the symlink exists. Run `ls -la /tmp/wire-sysio-native-contracts/` to verify. Also check `ldd cmake-build-debug/contracts/sysio.bios/sysio.bios_native.so` to ensure all shared library dependencies are available.
