# Debugging Contracts with the Native Module Runtime

The native-module runtime lets you debug smart contracts with standard C/C++ debuggers (LLDB, GDB) by compiling them as native shared libraries (`.so`) instead of executing WASM bytecode. You can set breakpoints, step through code, and inspect variables in your contract source.

## Prerequisites

1. **wire-cdt** built and available via `find_package(cdt)` (either installed or pointed to via `CMAKE_PREFIX_PATH`).

2. **wire-sysio** configured with native-module support:
   ```bash
   cmake -B cmake-build-debug -S . -G Ninja \
     -DCMAKE_BUILD_TYPE=Debug \
     -DBUILD_SYSTEM_CONTRACTS=ON \
     -DENABLE_TESTS=ON \
     # ... other flags as usual
   ```
   Native contracts are built automatically when `BUILD_SYSTEM_CONTRACTS=ON` and
   the native-module runtime is enabled. CDT's `add_native_contract()` macro
   handles compiling sysiolib sources with the host compiler.

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

## Debugging Contracts on a Running nodeop

The `--native-contract` flag lets you attach a debugger to a running nodeop and step through contract execution on a live chain. Only the contracts you specify run natively; everything else uses the normal WASM runtime.

### How It Works

1. nodeop copies the state and blocks directories to `<dir>.native-debug/` so the original chain data is never modified
2. At startup it looks up each account's on-chain code hash and loads the corresponding `.so`
3. When a transaction hits one of those contracts, execution is routed through the native `.so` instead of the WASM runtime
4. All other contracts continue to execute via WASM as normal

### Usage

```bash
nodeop \
  --native-contract sysio.token:/path/to/sysio.token_native.so \
  --native-contract mycontract:/path/to/mycontract_native.so \
  # ... other nodeop flags
```

The format is `account:/path/to/contract_native.so`. You can specify multiple `--native-contract` flags. The account must already have code deployed on-chain.

### Step-by-Step Example

1. Build nodeop and the native contract `.so` files:
   ```bash
   ninja -C cmake-build-debug nodeop sysio.token_native
   ```

2. Start nodeop under a debugger:
   ```bash
   lldb -- ./cmake-build-debug/programs/nodeop/nodeop \
     --native-contract sysio.token:cmake-build-debug/contracts/sysio.token/sysio.token_native.so \
     --data-dir /path/to/data --config-dir /path/to/config
   ```

3. Set a pending breakpoint on the contract source before running:
   ```
   (lldb) b sysio.token.cpp:42
   (lldb) run
   ```

4. Send a transaction (from another terminal):
   ```bash
   clio push action sysio.token transfer '["alice","bob","1.0000 SYS",""]' -p alice
   ```

5. nodeop hits the breakpoint in the native `.so` — step through, inspect variables, etc.

### State Isolation

When `--native-contract` is used, nodeop automatically copies the chain state before starting:

- `<state_dir>.native-debug/` — working copy of state
- `<blocks_dir>.native-debug/` — working copy of blocks

Previous debug copies are removed on each launch. The original data directory is untouched, so you can kill the debug session at any time without corrupting your chain. Delete the `.native-debug/` directories when done.

## Adding Native Builds for Other Contracts

To make another contract debuggable:

### 1. Add a CMake Target

The dispatch file (the `apply()` entry point) is auto-generated from the contract's ABI at build time. Just add an `add_native_contract()` call in the contract's `CMakeLists.txt`:

```cmake
if("native-module" IN_LIST SYSIO_WASM_RUNTIMES)
   add_native_contract(
      TARGET         mycontract_native
      SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/mycontract.cpp
      INCLUDE_DIRS   ${CMAKE_CURRENT_SOURCE_DIR}
      CONTRACT_CLASS "myns::mycontract"
      HEADERS        "mycontract.hpp"
      ABI_FILE       ${CMAKE_BINARY_DIR}/contracts/mycontract/mycontract.abi
   )
endif()
```

- **CONTRACT_CLASS**: The fully-qualified C++ class name (as used in `SYSIO_DISPATCH`)
- **HEADERS**: The contract header file(s) (used in the `#include` of the generated dispatch)
- **ABI_FILE**: Path to the `.abi` JSON file (usually in the contract's build directory)

CDT's `gen_native_dispatch.py` reads the `.abi` JSON, extracts all action names, and generates a dispatch `.cpp`. This runs automatically whenever the ABI changes.

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

## End-to-End Example: Debugging sysio.token on a Live Node

This walkthrough shows the full debug cycle: build the native contract, start a local chain, stop it, relaunch under a debugger with `--native-contract`, set a breakpoint in the token transfer code, and step through execution.

### 1. Build nodeop and the native contract

```bash
$ ninja -C build nodeop sysio.token_native
[1/2] Building CXX object contracts/sysio.token/...
[2/2] Linking CXX shared library contracts/sysio.token/sysio.token_native.so
```

### 2. Bootstrap a local single-producer chain

Start nodeop normally to create accounts and deploy the token contract:

```bash
# Terminal 1 — start a fresh single-producer node
$ build/programs/nodeop/nodeop \
    --data-dir ./my-debug-chain/data \
    --config-dir ./my-debug-chain/config \
    --plugin sysio::chain_api_plugin \
    --plugin sysio::http_plugin \
    --plugin sysio::producer_plugin \
    --plugin sysio::producer_api_plugin \
    --http-server-address 127.0.0.1:8888 \
    --p2p-listen-endpoint 127.0.0.1:9876 \
    --producer-name sysio \
    --enable-stale-production \
    -e
info  ... producer_plugin.cpp:...  Producing block ...

# Terminal 2 — create accounts, deploy contract, issue tokens
$ alias clio='build/programs/clio/clio'

$ clio create account sysio sysio.token SYS6...pubkey SYS6...pubkey
executed transaction: abc123...  200 bytes  300 us

$ clio set contract sysio.token build/contracts/sysio.token \
    sysio.token.wasm sysio.token.abi
executed transaction: def456...  Reading WASM from build/contracts/sysio.token/sysio.token.wasm...

$ clio push action sysio.token create \
    '["sysio","1000000.0000 SYS"]' -p sysio.token
executed transaction: 789abc...

$ clio push action sysio.token issue \
    '["sysio","10000.0000 SYS","initial"]' -p sysio
executed transaction: bcd012...

$ clio create account sysio alice SYS6...pubkey
$ clio create account sysio bob   SYS6...pubkey

$ clio push action sysio.token transfer \
    '["sysio","alice","500.0000 SYS","fund alice"]' -p sysio
executed transaction: cde345...
```

### 3. Stop the running node

```bash
# Graceful shutdown (SIGINT or SIGTERM)
$ pkill -INT nodeop
# Or: kill -INT <pid>
```

Wait for nodeop to exit cleanly (you should see `nodeop successfully exiting` in the log).

### 4. Restart under a debugger with --native-contract

```bash
# Terminal 1 — launch nodeop under LLDB with the native token contract
$ lldb -- build/programs/nodeop/nodeop \
    --data-dir ./my-debug-chain/data \
    --config-dir ./my-debug-chain/config \
    --plugin sysio::chain_api_plugin \
    --plugin sysio::http_plugin \
    --plugin sysio::producer_plugin \
    --plugin sysio::producer_api_plugin \
    --http-server-address 127.0.0.1:8888 \
    --p2p-listen-endpoint 127.0.0.1:9876 \
    --producer-name sysio \
    --enable-stale-production \
    -e \
    --native-contract sysio.token:build/contracts/sysio.token/sysio.token_native.so

(lldb) target create "build/programs/nodeop/nodeop"
Current executable set to 'build/programs/nodeop/nodeop' (x86_64).
```

### 5. Set a breakpoint and run

Set a pending breakpoint on the token transfer action before starting:

```
(lldb) b sysio.token.cpp:134
Breakpoint 1: no locations (pending).
WARNING: Unable to resolve breakpoint to any currently loaded shared library.

(lldb) run
Process 12345 launched: 'build/programs/nodeop/nodeop' (x86_64)
info  ... chain_plugin.cpp:...  Native debug mode: copying state data for safe debugging...
info  ... chain_plugin.cpp:...  Native debug mode: operating on copied state data.
    Original chain data is untouched. Debug copies:
      state:  ./my-debug-chain/data/state.native-debug
      blocks: ./my-debug-chain/data/blocks.native-debug
info  ... chain_plugin.cpp:...  Native contract debug: sysio.token -> build/contracts/sysio.token/sysio.token_native.so
info  ... producer_plugin.cpp:...  Producing block ...
1 location added to breakpoint 1
```

The `1 location added to breakpoint 1` message confirms that the native `.so` was loaded and the breakpoint resolved.

### 6. Send a transaction to hit the breakpoint

```bash
# Terminal 2 — send a transfer that will trigger the breakpoint
$ clio push action sysio.token transfer \
    '["alice","bob","1.0000 SYS","hello"]' -p alice
```

The debugger stops at the breakpoint:

```
Process 12345 stopped
* thread #7, name = 'chain-0', stop reason = breakpoint 1.1
    frame #0: sysio.token_native.so`sysio::token::transfer(
        this=0x..., from=name{value=0x...}, to=name{value=0x...},
        quantity=asset{amount=10000, sym=...}, memo="hello")
        at sysio.token.cpp:134:4
   131    void token::transfer( const name& from, const name& to,
   132                          const asset& quantity, const string& memo )
   133    {
-> 134       check( from != to, "cannot transfer to self" );
   135       require_auth( from );
   136
   137       check( is_account( to ), "to account does not exist" );
```

### 7. Inspect variables and step through

```
(lldb) p from
(sysio::name) $0 = {value = 3607749779137757184}  ;; "alice"

(lldb) p to
(sysio::name) $1 = {value = 3574538305133010944}  ;; "bob"

(lldb) p quantity
(sysio::asset) $2 = {amount = 10000, symbol = ...} ;; "1.0000 SYS"

(lldb) p memo
(std::string) $3 = "hello"

(lldb) n     ;; step over — passes the self-transfer check
Process 12345 stopped
    frame #0: sysio.token.cpp:135:4
-> 135       require_auth( from );

(lldb) n     ;; step over require_auth
Process 12345 stopped
    frame #0: sysio.token.cpp:137:4
-> 137       check( is_account( to ), "to account does not exist" );

(lldb) n     ;; step over — bob exists, passes
(lldb) n     ;; ...continue stepping through balance checks, sub_balance, add_balance

(lldb) c     ;; continue execution — transaction completes
Process 12345 resuming
```

Back in Terminal 2, `clio` returns:

```
executed transaction: f01234...  128 bytes  850 us
#  sysio.token <= sysio.token::transfer       {"from":"alice","to":"bob","quantity":"1.0000 SYS","memo":"hello"}
#         alice <= sysio.token::transfer       {"from":"alice","to":"bob","quantity":"1.0000 SYS","memo":"hello"}
#           bob <= sysio.token::transfer       {"from":"alice","to":"bob","quantity":"1.0000 SYS","memo":"hello"}
```

### 8. Clean up

Kill the debug session (`Ctrl+C` then `quit` in LLDB), and remove the debug copies:

```bash
$ rm -rf ./my-debug-chain/data/state.native-debug \
         ./my-debug-chain/data/blocks.native-debug
```

The original chain data in `./my-debug-chain/data/` is untouched and can be restarted normally.
