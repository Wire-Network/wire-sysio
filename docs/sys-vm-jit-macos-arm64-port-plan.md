# sys-vm-jit macOS arm64 Port Plan

## Goal

Enable `sys-vm-jit` on Apple Silicon macOS as the default developer/test WASM runtime for local development, CI, replay experiments, and performance investigation. All macOS builds are developer-only regardless of WASM runtime. This port must not imply production deployment support for macOS. Once the arm64 JIT backend is validated, a normal macOS arm64 configure should build and select `sys-vm-jit` automatically, without requiring a special CMake option to enable it.

This plan is scoped to the `sys-vm-jit` runtime backed by the external `wire-sys-vm` package. It does not port `sys-vm-oc`; the OC runtime has separate Linux/x86_64 assumptions around LLVM object generation, executable cache mappings, GS segment helpers, and stack switching.

## Current State

`CMakeLists.txt` enables:

- `sys-vm-oc` only on Linux x86_64 when `ENABLE_OC` is on.
- `sys-vm` and `sys-vm-jit` only when `CMAKE_SYSTEM_PROCESSOR` is `x86_64` or `amd64`.
- `sys-vm` only on other 64-bit non-Windows targets, which includes macOS arm64.
- `native-module` on Debug Linux/macOS, independent of JIT.

The chain integration already has a mostly architecture-neutral runtime wrapper:

- `libraries/chain/webassembly/runtimes/sys-vm.cpp` instantiates `sys_vm_runtime<sysio::vm::interpreter>` everywhere.
- The `sysio::vm::jit` and profiling runtime instantiations are guarded by `#ifdef __x86_64__`.
- `libraries/chain/include/sysio/chain/wasm_interface_private.hpp` selects `sysio::vm::jit` when `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` exists.

The real architecture dependency is inside `wire-sys-vm`:

- `include/sysio/vm/backend.hpp` includes `x86_64.hpp` only under `__x86_64__`.
- `sysio::vm::jit` and `sysio::vm::jit_profile` are declared only under `__x86_64__`.
- `include/sysio/vm/x86_64.hpp` contains the machine-code writer.
- `include/sysio/vm/execution_context.hpp` contains x86_64-specific JIT invocation assembly, backtrace walking, and Darwin/Linux ucontext register reads.
- `include/sysio/vm/config.hpp` exposes an x86-named architecture flag (`sys_vm_amd64`) rather than a general JIT backend capability.
- JIT executable memory is allocated in `include/sysio/vm/allocator.hpp` with anonymous RW `mmap`, code copy, `mprotect(PROT_EXEC)`, and code-page toggling. On Apple Silicon this allocation sequence is architecturally incompatible with executable anonymous memory unless the mapping was created with `MAP_JIT`.
- The allocator does not currently flush the instruction cache after writing generated code. This is harmless on x86_64 cache-coherent instruction/data paths but is a correctness blocker on arm64.
- The JIT allocator is accessed through `jit_allocator::instance()`; concurrent JIT compilation must prove the singleton allocator and segment pool are serialized or made thread-safe.
- `signals.hpp` uses a process-wide timeout flag (`timed_run_has_timed_out`) for timed-run fault classification; that needs a thread/concurrency audit before using arm64 JIT in multi-threaded paths.

There is also a default-runtime coupling in this repo: `libraries/chain/include/sysio/chain/config.hpp` currently makes `sys-vm-jit` the default whenever `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` is defined. The macOS arm64 rollout should make that default explicit and intentional: macOS arm64 should select `sys-vm-jit` automatically only after the backend is present and validated, while other platforms keep their existing defaults unless separately changed.

Additional chain-side issues:

- `libraries/chain/include/sysio/chain/wasm_interface_private.hpp` includes WAVM headers (`IR/Module.h`, `Platform/Platform.h`, `WAST/WAST.h`, `IR/Validate.h`) unconditionally even though this file does not appear to use `IR::`, `WAST::`, or `Platform::` symbols directly. Treat these as dead direct includes unless a deeper audit finds a real dependency.
- `wasm_interface_private.hpp` constructs `sys_vm_profile_runtime` for `sys-vm-jit` when profiling is requested, but `sys_vm_profile_runtime` is implemented only for x86_64 today. arm64 JIT builds must gate this path behind a separate profile capability and fail cleanly when profile mode is requested.
- `plugins/chain_plugin/src/chain_plugin.cpp` describes `sys-vm-jit` as native x86 code, which becomes inaccurate once arm64 is supported.

## Success Criteria

1. A normal macOS arm64 configure builds `sys-vm-jit` and selects it as the default runtime without requiring a special CMake option.
2. `nodeop` starts and executes contracts on macOS arm64 with the default `sys-vm-jit` runtime, and `nodeop --wasm-runtime sys-vm` remains available for cross-runtime comparison.
3. `sys-vm-jit` passes the same core WASM behavior tests as `sys-vm` on macOS arm64.
4. JIT output is deterministic for all consensus-relevant WASM semantics, including traps, integer overflow behavior, softfloat-backed `f32`/`f64` behavior, memory bounds checks, call-depth enforcement, and deadline interruption.
5. JIT memory management uses an Apple Silicon-compatible `MAP_JIT` allocation path, flushes the instruction cache after generated-code writes, and works for normal local macOS developer binaries without requiring signing, hardened-runtime, or `com.apple.security.cs.allow-jit` entitlement support.
6. Cross-runtime contract scenarios produce the same final state, traces, block/snapshot artifacts, and failure types under `sys-vm` and `sys-vm-jit`.
7. The build system exposes and defaults to `sys-vm-jit` on macOS arm64 only when the arm64 JIT backend is present and tested.
8. CI proves it is building/running native arm64 binaries, not x86_64 under Rosetta.
9. Documentation and release notes clearly state that macOS builds are developer-only regardless of selected WASM runtime.
10. Profiling and benchmark data shows `sys-vm-jit` is materially better than `sys-vm` for the intended macOS developer workflows; otherwise the runtime is not included.
11. `sys-vm-jit` profile mode either works through an explicitly supported `jit_profile` capability or returns a clear unsupported-runtime error; it must not compile a dangling profile path or leave `runtime_interface` null.

## Non-Goals

- Do not port `sys-vm-oc` as part of this work.
- Do not add production deployment support for macOS with any WASM runtime.
- Do not rely on Rosetta x86_64 execution.
- Do not redesign, replace, or migrate the existing x86_64 hand-written JIT to a shared codegen strategy as part of this port.
- Do not add signing, hardened-runtime, or `com.apple.security.cs.allow-jit` entitlement support for this developer-only port.
- Do not change contract ABI, host intrinsic behavior, or consensus serialization.

## Phase 1: Inventory and Upstream Alignment

1. Identify the exact `wire-sys-vm` source revision used by vcpkg override `1.1.1#2`.
2. Check whether upstream `wire-sys-vm` or its ancestors already have an AArch64 writer, Apple `MAP_JIT` allocator support, or a partial branch.
3. Decide where the port lands first:
   - preferred: upstream `wire-sys-vm`, then update this repo's vcpkg override;
   - fallback: temporary vcpkg overlay/patch in this repo while upstream review is pending.
4. Define overlay lifetime and drift control:
   - if upstream review is not on a mergeable path within two weeks after a ready PR is opened, proceed with a vcpkg overlay for this repo;
   - keep the overlay as a small patch series with an upstream commit hash, weekly rebase notes, and a removal task tied to upstream merge;
   - do not let the overlay carry unrelated x86_64 rewrites.
5. Attempt a minimal standalone tracer-bullet reproducer in `wire-sys-vm`:
   - write the smallest test program that compiles a WASM module with no imports and calls an exported function through
     `jit_execution_context`;
   - attempt to compile and run it under AppleClang arm64, expecting it to fail before Phases 2-4 because `sysio::vm::jit`
     is currently guarded behind x86_64-only declarations;
   - document every compile, link, allocation, signal, and runtime failure as concrete input to the Phase 2 capability
     work, Phase 3 architecture plumbing, and Phase 4 allocator redesign.

Deliverable: a short design note in the `wire-sys-vm` change or PR explaining the arm64 backend shape and macOS executable-memory strategy.

## Phase 2: Build-System Feature Detection

1. Add an explicit architecture capability in `wire-sys-vm`, for example `SYS_VM_HAS_JIT_BACKEND`, rather than using `__x86_64__` directly in downstream code.
2. Teach `wire-sys-vm` to define the capability for x86_64 and, after the port, arm64/aarch64.
3. In this repo, change top-level runtime selection to enable `sys-vm-jit` when all are true:
   - 64-bit host;
   - not Windows;
   - `sys-vm::sys-vm` advertises a JIT backend for the target;
   - architecture is x86_64/amd64 or arm64/aarch64.
4. Make the default-runtime selection explicit before enabling any macOS arm64 JIT build:
   - add a CMake-controlled `SYSIO_SYS_VM_JIT_IS_DEFAULT` macro, or equivalent explicit default-runtime macro;
   - do not expose this as a required manual CMake selection for macOS arm64;
   - automatically define it for macOS arm64 when `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` is defined and the validated
     AArch64 backend is available;
   - make `default_wasm_runtime` select `sys-vm-jit` only when both `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` and
     `SYSIO_SYS_VM_JIT_IS_DEFAULT` are defined;
   - assert that a standard macOS arm64 configure selects `sys-vm-jit` as the default once the backend is enabled, and
     that the selected default is a compiled-in runtime.
5. Audit and remove dead direct WAVM includes:
   - confirm whether `wasm_interface_private.hpp` really needs `IR/Module.h`, `Platform/Platform.h`, `WAST/WAST.h`, `IR/Validate.h`, or `using namespace IR`;
   - if the symbols are unused, remove the direct includes and namespace import instead of treating WAVM availability as a JIT blocker;
   - if a real dependency remains, make it explicit and prove the macOS arm64 vcpkg triplet provides it before enabling JIT.
6. Update macOS arm64 configure docs and CI commands to include `sys-vm-jit` in the built runtime list once validated.

Deliverable: CMake no longer hard-codes x86_64 as the only JIT-capable architecture, macOS arm64 automatically builds
and defaults to the validated JIT backend without special CMake configuration, and unnecessary WAVM coupling in
`wasm_interface_private.hpp` is removed or proven necessary.

## Phase 3: Hidden Architecture Dependencies

Before writing the AArch64 emitter, remove or abstract x86-only assumptions outside `x86_64.hpp`.

1. Add an AArch64-compatible JIT invocation trampoline:
   - replace x86 inline assembly in `jit_execution_context::execute()` with an architecture-specific trampoline interface;
   - preserve the generated function ABI `native_value (*)(void*, void*)`;
   - preserve alternate-stack behavior and call-depth accounting;
   - avoid longjmp through frames with non-trivial destructors.
2. Split JIT profiling/backtrace support from normal JIT support:
   - keep `jit_profile` async backtrace/profiling support x86_64-only for this port;
   - add `SYS_VM_HAS_JIT_PROFILE` or equivalent for profile-only code paths;
   - make normal arm64 JIT usable without implementing Darwin arm64 ucontext register reads for `jit_profile`;
   - gate the `wasm_interface_private.hpp` `profile == true` branch behind `SYS_VM_HAS_JIT_PROFILE`;
   - when `--profile` is requested with arm64 `sys-vm-jit`, throw a clear unsupported-runtime error instead of compiling a dangling `sys_vm_profile_runtime` reference or falling through to a null runtime.
3. Audit signal handling for Darwin arm64:
   - verify SIGSEGV, SIGBUS, and SIGFPE are installed, chained, masked, and restored correctly;
   - confirm faults from code pages, WASM memory pages, and genuine process bugs are distinguished correctly;
   - test nested/chained signal handlers because node processes may load plugins that install their own handlers.
4. Replace x86-specific naming in `wire-sys-vm` configuration:
   - keep backwards-compatible aliases if needed;
   - introduce architecture-neutral flags such as `sys_vm_has_jit_backend` and `sys_vm_target_arch`.
5. Confirm helper-call ABI compatibility:
   - softfloat helper calls must receive and return the same bit patterns as interpreter execution;
   - imported host functions must preserve callee-saved registers and stack alignment across the trampoline.
6. Fix timed-run timeout state before multi-threaded JIT use:
   - audit the process-wide `timed_run_has_timed_out` flag used for fault classification;
   - replace it with thread-local or execution-context-owned state if one thread can otherwise mask another thread's genuine SIGSEGV as a timeout;
   - add a concurrent timed-run test where one JIT execution times out while another thread faults for a non-timeout reason.

Deliverable: `wire-sys-vm` can compile architecture-neutral JIT plumbing on x86_64 and arm64 before the AArch64 emitter is enabled.

## Phase 4: macOS JIT Memory and Timer Redesign

Apple Silicon executable-memory rules differ from Linux and Intel macOS. The current allocator sequence is anonymous
RW `mmap`, copy generated code, then `mprotect(PROT_EXEC)`. That sequence is a blocker on Apple Silicon because
anonymous pages that were not allocated with `MAP_JIT` cannot later be made executable. Treat this as a structural
redesign of `jit_allocator::allocate_segment()` and `end_code<IsJit>()`, not as a flag-only platform addition. This port
targets normal local developer binaries; signed/hardened runtime entitlement support is out of scope.

Apple Silicon implies a macOS 11.0+ deployment target. `MAP_JIT` was introduced before that and should be treated as
unconditionally available for Apple Silicon builds; do not add an arm64 fallback that attempts anonymous RW mappings and
later `mprotect(PROT_EXEC)`.

1. Redesign macOS arm64 JIT allocation:
   - allocate JIT segments with `MAP_JIT` at mapping creation time;
   - use the Apple-required `PROT_READ | PROT_WRITE | PROT_EXEC` mapping form when paired with per-thread write
     protection toggles;
   - wrap generated-code writes and patches in `pthread_jit_write_protect_np(false)` and
     `pthread_jit_write_protect_np(true)` where required;
   - document that `pthread_jit_write_protect_np` is per-thread: writes and relocation patches must happen on the same
     thread that opened the write window, and other threads remain write-protected;
   - remove the current `end_code<IsJit>()` write path that grants `PROT_READ | PROT_WRITE` with `mprotect`; on `MAP_JIT`
     pages write access is controlled by `pthread_jit_write_protect_np(false)`, not by granting write permission with
     `mprotect`;
   - keep Linux and Intel macOS behavior unchanged.
2. Make instruction-cache flushing mandatory before any arm64 prototype executes:
   - call `__builtin___clear_cache(begin, end)` after every generated-code write or relocation patch and before
     execution;
   - treat arm64 JIT execution without this flush as invalid because it can silently execute stale instruction-cache
     contents.
3. Revisit the 1 GiB JIT segment allocation policy:
   - validate whether large `MAP_JIT` reservations are acceptable on Apple Silicon;
   - consider smaller segments or adaptive segment sizing if memory pressure is poor;
   - test allocation/free/reuse behavior under many contract instantiations.
4. Audit `jit_allocator::instance()` concurrency:
   - first verify the existing chain lock path: read-only execution appears to call `get_or_build_instantiated_module()`
     while holding `instantiation_cache_mutex`, and `runtime_interface->instantiate_module()` is called inside that path;
   - if that lock chain serializes all calls into `end_code<IsJit>()`, document the existing serialization instead of
     adding new allocator locking;
   - if any JIT compilation path can bypass that lock, add locking, thread-local allocation state, or another explicit
     ownership model;
   - add a test that instantiates and JIT-compiles distinct modules concurrently.
5. Keep W^X intent explicit:
   - generated code should not remain writable while executing;
   - any temporary RWX mapping state must be covered by Apple per-thread write protection and documented.
6. Specify and test `enable_code()` and `disable_code()` on `MAP_JIT` pages:
   - deadline interruption currently disables code by making executable pages `PROT_NONE`;
   - verify `mprotect(page, PROT_NONE)` works on `MAP_JIT` memory;
   - verify re-enabling restores `PROT_READ | PROT_EXEC` without requiring `pthread_jit_write_protect_np`, because no
     write permission is being granted;
   - if either transition fails or changes fault classification, introduce a macOS arm64 interruption path that preserves
     deadline behavior without invalid page-protection transitions.
7. Add signal-path tests:
   - faults inside executable code pages during deadline interruption;
   - faults inside WASM linear memory;
   - SIGFPE paths for integer divide traps;
   - SIGBUS behavior on Darwin arm64;
   - concurrent timed runs where a timeout on one thread must not mask a genuine fault on another thread.
8. Add tests that repeatedly instantiate, execute, disable, re-enable, and free JIT code on macOS arm64.

Deliverable: JIT memory management works under normal local developer binaries without adding signing, hardened-runtime,
or `com.apple.security.cs.allow-jit` entitlement support, and no arm64 JIT code can execute without an instruction-cache
flush.

## Phase 5: Feasibility Emitter and Early Inclusion Gate

Do not implement the full AArch64 backend before proving the port is worth carrying. After the architecture plumbing and
macOS allocator are working, build a narrow AArch64 feasibility emitter and benchmark it against `sys-vm`.

1. Implement only the minimum emitter surface needed for representative measurements:
   - function entry/exit and the `native_value (*)(void*, void*)` call path;
   - constants, integer arithmetic, comparisons, shifts, loads, stores, and bounds checks;
   - the smallest function-call path needed by the benchmark modules;
   - only the branch/trap handling needed to return from curated benchmark modules and validate signal/deadline behavior;
   - a small softfloat-heavy subset if helper-call overhead is a major inclusion concern.
2. Keep the feasibility target explicitly experimental:
   - do not expose it as the general `sys-vm-jit` runtime for arbitrary contracts;
   - run only curated modules that are known to use the supported subset;
   - keep all interpreter/JIT differential checks enabled.
3. Run an early benchmark subset:
   - integer-heavy modules;
   - memory-heavy modules;
   - import-heavy modules if the bridge is included in the subset;
   - at least one softfloat-heavy module if float helper calls are included;
   - cold compile+execute time and warm execute-only time.
4. Make a go/no-go decision before the full opcode implementation:
   - continue only if the subset shows a credible performance win over `sys-vm` for intended developer workflows;
   - defer or keep the work as an experiment if cold-start cost, helper-call overhead, or allocator/timer cost erase the
     benefit;
   - document the benchmark commands, raw logs, and decision.

Deliverable: an early feasibility report that justifies implementing the full AArch64 emitter, or a decision to stop
before the most expensive part of the port.

## Phase 6: Full AArch64 Code Generator

Implement `wire-sys-vm/include/sysio/vm/aarch64.hpp` beside `x86_64.hpp` only after the feasibility gate passes.

The AArch64 backend should mirror the existing x86_64 hand-written JIT approach. Keep x86_64 behavior stable and avoid
broad shared-codegen rewrites. Any x86_64 changes should be limited to narrow compatibility seams needed for
architecture selection, trampoline dispatch, capability macros, or tests that prove x86_64 behavior is unchanged.

Required backend responsibilities:

1. Produce an opcode coverage inventory before implementation:
   - enumerate every opcode group handled by the x86_64 writer;
   - state whether the supported scope is WASM MVP only or includes extensions such as sign-extension, bulk memory,
     reference types, SIMD, atomics, or other locally supported operations;
   - map each supported x86_64 lowering to an AArch64 lowering, helper call, or explicit non-support decision;
   - use the inventory as the test checklist for the arm64 backend.
2. Emit AArch64 machine code for every WASM opcode currently supported by the x86_64 writer and included in the
   inventory.
3. Preserve WASM semantics where native instructions differ:
   - division by zero traps;
   - signed division overflow traps;
   - shift counts are masked to WASM width;
   - unaligned loads/stores remain allowed and little-endian.
4. Route all WASM `f32`/`f64` arithmetic, comparisons, rounding, square root, min/max, and numeric conversions through the existing softfloat-backed helpers:
   - do not emit hardware FP arithmetic/comparison/conversion instructions for consensus-relevant WASM operations;
   - keep `f32`/`f64` load, store, const, and reinterpret operations as bit-preserving moves;
   - make the AArch64 path honor `SYS_VM_SOFTFLOAT` exactly as the interpreter and x86_64 JIT path do;
   - verify NaN, signed-zero, rounding, overflow, and invalid-conversion behavior against softfloat golden tests.
5. Preserve `wire-sys-vm` call ABI:
   - generated function entry signature remains compatible with `native_value (*)(void*, void*)`;
   - first argument carries execution/context data;
   - second argument carries linear memory or stack data as expected by `jit_execution_context`;
   - imported host functions are called through the existing import-function resolver.
6. Implement function prologue/epilogue:
   - obey Darwin AArch64 calling convention;
   - preserve callee-saved registers;
   - maintain 16-byte stack alignment;
   - reserve scratch registers consistently;
   - verify AppleClang pointer-authentication settings for the build;
   - either keep PAC disabled/inapplicable for JIT-invoked function pointers or explicitly handle PAC in the trampoline
     and generated return path.
7. Implement control flow:
   - direct calls;
   - indirect calls and type checks;
   - block/loop/br/br_if/br_table lowering;
   - trap path lowering;
   - branch range handling, literal pools, and relocation/patching for large generated modules.
8. Implement memory operations:
   - address zero-extension from 32-bit WASM pointers;
   - bounds checks before load/store;
   - correct sign/zero extension for narrow loads;
   - little-endian access on AArch64.
9. Implement host-call bridge:
   - marshal `native_value` operands into host function ABI;
   - handle return values;
   - call softfloat helper functions with the same argument and result bit patterns used by the interpreter;
   - route host exceptions through existing `sysio::vm` exception paths.

Deliverable: `wire-sys-vm` builds with a first-class `sysio::vm::jit` on arm64 and an opcode inventory proving it matches
the x86_64 JIT's supported surface.

## Phase 7: Chain Integration Changes

1. Replace `#ifdef __x86_64__` in `libraries/chain/webassembly/runtimes/sys-vm.cpp` with a capability macro from `wire-sys-vm`, such as `SYS_VM_HAS_JIT_BACKEND`.
2. Keep `jit_profile` behind a second capability and leave it x86_64-only for this port, for example `SYS_VM_HAS_JIT_PROFILE`:
   - guard the `sys_vm_profile_runtime` declaration, definition, and `wasm_interface_private.hpp` construction branch consistently;
   - if `profile == true` and `SYS_VM_HAS_JIT_PROFILE` is false, throw an explicit unsupported profiling error for `sys-vm-jit`;
   - do not allow a null `runtime_interface` failure to be the user-facing behavior.
3. Make error messages architecture-neutral:
   - current instantiate errors say `Error building sys-vm interp` even for JIT;
   - update to include the runtime name.
4. Confirm `wasm_interface_private.hpp` still cleanly rejects `sys-vm-jit` when the runtime is not compiled in.
5. Update `plugins/chain_plugin/src/chain_plugin.cpp` runtime option description:
   - remove x86-only wording for `sys-vm-jit`;
   - say it compiles WASM to native host code on supported architectures.
6. Make macOS arm64 default to `sys-vm-jit` automatically after the backend passes validation:
   - implement the Phase 2 `SYSIO_SYS_VM_JIT_IS_DEFAULT` default-selection macro before enabling arm64 JIT;
   - have CMake define it by default for macOS arm64 when the JIT backend is available;
   - do not require developers or CI to pass a special `-D...` flag to enable or select the JIT;
   - do not let `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` alone accidentally select the default on other platforms;
   - add a configure-time or compile-time assertion that the default is a compiled-in runtime.
7. Document the platform-wide macOS developer-only policy:
   - do not add a JIT-specific production guard, because macOS developer-only status applies regardless of runtime;
   - make docs explicit that `sys-vm`, `sys-vm-jit`, and any other macOS runtime are for development, CI, replay/testing, and benchmarking only;
   - keep local developer nodes and test binaries able to opt back to the interpreter with `--wasm-runtime sys-vm`;
   - if a production-deployment guard is needed, implement it as a macOS-wide policy outside this JIT port rather than as a `sys-vm-jit` special case.
8. Add a macOS arm64 build assertion or configure message listing enabled runtimes.

Deliverable: this repo can compile `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` on macOS arm64 without x86-only includes or
template instantiations, while making JIT the normal macOS arm64 default and preserving the platform-wide developer-only
status of macOS builds.

## Phase 8: Tests in `wire-sys-vm`

Start below the chain layer, because generator bugs are easier to isolate there.

1. Enable the existing JIT template tests on arm64 by replacing `#ifdef __x86_64__` test guards with the new capability macro.
2. Add instruction-level tests tied to the opcode coverage inventory for:
   - integer arithmetic and bit operations;
   - signed and unsigned comparisons;
   - shifts and rotates;
   - load/store widths and sign extension;
   - unaligned memory;
   - direct and indirect calls;
   - softfloat-backed `f32`/`f64` arithmetic, comparison, rounding, min/max, sqrt, and conversion operations;
   - all trap paths;
   - host imports with each supported value type.
3. Add trampoline and platform tests:
   - AArch64 prologue/epilogue preserves callee-saved registers and 16-byte stack alignment;
   - indirect generated-function calls work under the effective AppleClang pointer-authentication settings;
   - generated-code return paths do not SIGILL under the configured compiler and deployment target.
4. Add backend-specific stress tests:
   - many functions to force long branches;
   - many locals to stress stack/frame layout;
   - nested blocks and large `br_table`;
   - code sizes near allocator page boundaries.
5. Add differential tests that execute the same module through interpreter and JIT and compare:
   - return values;
   - memory output;
   - trap type;
   - imported function call sequence.
6. Add a generated-code audit for representative float-heavy modules:
   - disassemble the AArch64 JIT output;
   - confirm WASM float arithmetic/comparison/conversion lowers to softfloat helper calls;
   - allow only bit-moving instructions for float load/store/const/reinterpret plumbing.
7. Add randomized differential and fuzz tests:
   - generate modules with varied control flow, calls, locals, branches, tables, memory operations, and imports;
   - compare interpreter and JIT return values, memory, trap types, and import call traces;
   - seed and persist failing modules for regression tests.
8. Treat fuzzing as a new validation budget, not an inherited one:
   - current repo coverage includes committed fuzz-regression WASM files and an old AFL helper, but no established VM
     fuzzing time budget for pre-merge CI or nightly validation;
   - pre-merge CI should run the fixed fuzz-regression corpus plus bounded deterministic differential tests with fixed seeds;
   - live fuzzing should run only in nightly/manual jobs with an explicit wall-clock budget and artifact retention;
   - every live-fuzz failure must be minimized, checked in as a regression module, and added to the deterministic pre-merge
     suite before the issue is considered closed.

Deliverable: `wire-sys-vm` arm64 JIT is validated before chain-level activation.

## Phase 9: Chain and Contract Test Matrix

Use a build directory under `build/`, for example `build/macos-arm64-jit`.

Configure:

```bash
cmake \
  -B build/macos-arm64-jit \
  -S . \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DBUILD_SYSTEM_CONTRACTS=OFF \
  -DBUILD_TEST_CONTRACTS=OFF \
  -DENABLE_CCACHE=ON \
  -DENABLE_DISTCC=OFF \
  -DENABLE_TESTS=ON \
  -DENABLE_JEMALLOC=OFF \
  -DDISABLE_WASM_SPEC_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Do not pass a special JIT enable/default option in this baseline command. A correctly implemented macOS arm64 build
should discover the validated JIT backend, compile it, and select it as the default automatically.

Build:

```bash
cmake --build build/macos-arm64-jit --target fc nodeop clio unit_test plugin_test test_fc -- -j$(sysctl -n hw.logicalcpu)
```

Smoke tests:

```bash
build/macos-arm64-jit/programs/nodeop/nodeop --version
build/macos-arm64-jit/programs/clio/clio version client
build/macos-arm64-jit/unittests/unit_test --run_test=noop_tests
build/macos-arm64-jit/unittests/unit_test --run_test=noop_tests -- --sys-vm-jit
build/macos-arm64-jit/unittests/unit_test --run_test=wasm_part1_tests -- --sys-vm-jit
build/macos-arm64-jit/unittests/unit_test --run_test=wasm_part1_tests -- --sys-vm
build/macos-arm64-jit/libraries/libfc/test/test_fc --run_test=traits
```

Core matrix:

```bash
build/macos-arm64-jit/unittests/unit_test -- --sys-vm
build/macos-arm64-jit/unittests/unit_test -- --sys-vm-jit
build/macos-arm64-jit/tests/plugin_test
```

WASM spec matrix:

```bash
cd build/macos-arm64-jit
ctest -j "$(sysctl -n hw.logicalcpu)" -L wasm_spec_tests --output-on-failure --timeout 1000 2>&1 | tee /tmp/macos-arm64-jit-wasm-spec.log
```

Targeted risk tests:

- `wasm_part1_tests`, `wasm_part2_tests`, and `wasm_part3_tests`
- `wasm_config_tests`
- `checktime_tests`
- `softfloat_golden_tests`
- `crypto_golden_tests`
- `system_host_tests`
- any `--list_content` entries that mention memory, traps, call depth, profiling, or read-only execution

Before committing suite names to scripts or CI, verify Boost.Test filters from the built binary:

```bash
build/macos-arm64-jit/unittests/unit_test --list_content | grep -Ei 'softfloat|crypto_golden|wasm|checktime'
```

The current source declares `softfloat_golden_tests` and `crypto_golden_tests`, but `--list_content` is the source of
truth for the linked `unit_test` binary.

Cross-runtime equivalence tests:

1. Run identical contract scenarios under `--sys-vm` and `--sys-vm-jit`.
2. Compare final chain state and relevant table contents.
3. Compare traces, action results, exception classes, and exception messages where consensus-relevant.
4. Compare generated blocks and snapshots for deterministic scenarios.
5. Include read-only and parallel execution scenarios if the JIT is used by those paths.

Deliverable: macOS arm64 JIT test logs show parity with `sys-vm` for developer/test use, without implying production deployment support for macOS.

## Phase 10: Full Profiling and Final Inclusion Gate

Do not include macOS arm64 `sys-vm-jit` merely because it works. It must demonstrate enough value over `sys-vm` to justify carrying the backend, tests, CI cost, documentation, and maintenance burden. This gate uses benchmark data, scoped timers, and external profilers; it does not require porting `jit_profile` to arm64.

1. Define the benchmark set before optimizing:
   - representative system-contract actions;
   - common developer replay and local-node workflows;
   - WASM-heavy tests such as `wasm_part1_tests`, `wasm_part2_tests`, `wasm_part3_tests`, and selected WASM spec cases;
   - softfloat-heavy contracts to measure helper-call overhead;
   - import-heavy contracts to measure host-call bridge overhead;
   - small contracts where JIT compile overhead may dominate;
   - quantitative acceptance thresholds for speedup, cold-start overhead, and memory overhead.
2. Measure both cold and warm behavior:
   - first execution with parse/JIT compile cost included;
   - repeated execution after JIT code is ready;
   - module instantiate time;
   - peak and steady-state RSS;
   - generated-code size and allocator/cache behavior.
3. Compare directly against `sys-vm`:
   - same machine, same build type, same compiler, same contract artifacts;
   - multiple runs with variance reported;
   - wall-clock time, CPU time, throughput, and deadline/checktime behavior;
   - memory overhead per instantiated contract.
4. Produce a profiling report:
   - identify where `sys-vm-jit` wins, ties, or loses;
   - separate JIT compile cost from execution speedup;
   - call out softfloat, host-call bridge, signal/timer, and allocator overhead;
   - use scoped timers and external tools such as Instruments or `sample` when hotspot data is needed;
   - include raw commands, logs, and summarized tables.
5. Make an inclusion decision:
   - make `sys-vm-jit` the normal macOS arm64 default only if it gives a clear win for developer workflows after
     accounting for cold-start and memory overhead;
   - keep `sys-vm` available through `--wasm-runtime sys-vm` for comparison and fallback;
   - defer the default switch, or defer the port entirely, if benefits are marginal.

Deliverable: a checked-in or attached profiling report that justifies including macOS arm64 `sys-vm-jit`, or a decision to defer inclusion until it provides measurable value.

Current report: [sys-vm-jit macOS arm64 Benchmark Report](sys-vm-jit-macos-arm64-benchmark-report.md) records the
first native arm64 validation and benchmark pass. After the final validation gate, the intended decision is to include
`sys-vm-jit` as the default macOS arm64 developer runtime because hot VM execution is materially faster than `sys-vm`,
while keeping `sys-vm` available for explicit comparison and treating existing chain-level unit-test wall-clock
measurements as correctness validation rather than isolated JIT throughput.

## Phase 11: CI Rollout

1. Add a macOS arm64 JIT-default job that builds `sys-vm-jit` and runs smoke tests.
   - use the Phase 9 configure command as the CI baseline;
   - do not omit `-DCMAKE_OSX_ARCHITECTURES=arm64`;
   - do not pass a special CMake option to enable or select `sys-vm-jit`;
   - fail the job if configure output does not show `sys-vm-jit` enabled and selected as the macOS arm64 default.
2. Promote to targeted unit tests once the job is stable.
3. Promote to the full macOS arm64 unit-test matrix after spec and chain tests are stable locally.
4. Keep failures isolated from the existing interpreter-only macOS job during initial rollout.
5. Prove the job is native arm64:
   - set `CMAKE_OSX_ARCHITECTURES=arm64` explicitly;
   - log `uname -m`;
   - log `file` output for `nodeop`, `unit_test`, and any standalone `wire-sys-vm` JIT test binaries;
   - fail the job if any binary is x86_64 or if Rosetta is detected.
6. Capture JIT-specific logs as artifacts:
   - configure output showing enabled runtimes;
   - configure output showing the selected default runtime;
   - `nodeop --version`;
   - targeted Boost.Test logs;
   - WASM spec log.

Deliverable: CI proves the port without destabilizing the current macOS arm64 path.

## Phase 12: Developer-Only Release Policy

Release macOS arm64 `sys-vm-jit` only as a developer/test runtime after:

1. Full unit, plugin, and WASM spec tests pass consistently.
2. The Phase 10 profiling report shows `sys-vm-jit` is materially better than `sys-vm` for intended developer workflows.
3. Deadline interruption behavior is equivalent to `sys-vm`.
4. Memory protection works for normal local developer binaries without requiring signing or `com.apple.security.cs.allow-jit` entitlement support.
5. A pre-release CI/developer soak period has exercised `sys-vm-jit` as the default and `--wasm-runtime sys-vm` as an
   explicit fallback/comparison path.
6. Cross-runtime state/block/snapshot equivalence tests are stable in CI.
7. Documentation and release notes state that all macOS builds remain developer-only regardless of runtime.

The release policy remains: macOS arm64 defaults to `sys-vm-jit` after the validation gates pass; `sys-vm` remains
available through explicit runtime selection for developers, CI comparison, replay experiments, and benchmarking; macOS
builds are developer-only regardless of runtime.

## Blocker Checklist

These items must be resolved before macOS arm64 `sys-vm-jit` is exposed as the default chain runtime.

| Blocker | Resolved In | Required resolution |
| --- | --- | --- |
| Anonymous RW `mmap` followed by `mprotect(PROT_EXEC)` is incompatible with Apple Silicon JIT execution. | Phase 4.1 | Redesign `jit_allocator::allocate_segment()` and `end_code<IsJit>()` around `MAP_JIT`, remove the current `mprotect(executable_code, _code_size, PROT_READ | PROT_WRITE)` write step, use Apple write-protect toggles, and preserve Linux/x86_64 behavior. |
| Generated arm64 code can execute stale instruction-cache contents if the I-cache is not flushed. | Phase 4.2 | Call `__builtin___clear_cache(begin, end)` after generated-code writes and relocation patches before any arm64 JIT execution. |
| `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED` currently flips `default_wasm_runtime` to `sys-vm-jit` implicitly. | Phase 2.4, Phase 7.6 | Add explicit default-selection logic such as `SYSIO_SYS_VM_JIT_IS_DEFAULT`; set it automatically for validated macOS arm64 JIT builds, require no manual CMake selection, and assert the selected default is compiled in. |
| `wasm_interface_private.hpp` has direct WAVM includes that appear unused. | Phase 2.5 | Remove the dead includes and `using namespace IR`; only prove WAVM header availability if a real direct dependency remains. |
| `sys_vm_profile_runtime` is x86_64-only but the chain profile branch is selected by `SYSIO_SYS_VM_JIT_RUNTIME_ENABLED`. | Phase 3.2, Phase 7.2 | Gate profile mode with `SYS_VM_HAS_JIT_PROFILE` and return a clear unsupported-runtime error on arm64. |
| Timed-run timeout state appears process-wide, so one thread can affect signal classification in another. | Phase 3.6 | Make timeout/fault classification thread-local or execution-context-owned before multi-threaded arm64 JIT use. |
| `jit_allocator::instance()` may be shared across concurrent JIT compilation. | Phase 4.4 | First prove whether `instantiation_cache_mutex` already serializes the instantiate-module-to-`end_code<IsJit>()` path; document that serialization or make the allocator thread-safe before read-only threads can compile distinct modules concurrently. |

## Main Risks

1. Consensus drift from incorrect AArch64 lowering.
   - Mitigation: interpreter/JIT differential tests plus WASM spec tests.
2. Apple Silicon executable memory allocation is structurally incompatible with the current allocator.
   - Mitigation: redesign allocation around `MAP_JIT` at mapping creation time, `pthread_jit_write_protect_np` write windows, and unchanged Linux/x86_64 paths.
3. Missing instruction-cache flush causes silent arm64 misexecution.
   - Mitigation: require `__builtin___clear_cache` after every generated-code write or patch and before any prototype execution.
4. Deadline interruption regression.
   - Mitigation: explicitly test `PROT_NONE` and `PROT_READ | PROT_EXEC` transitions on `MAP_JIT` pages, plus focused `checktime_tests`.
5. Host-call ABI mismatch.
   - Mitigation: import-heavy standalone tests and chain intrinsic tests.
6. Accidental hardware FP execution in the JIT.
   - Mitigation: require softfloat helper lowering for all WASM float operations, add softfloat golden tests, and audit representative generated code.
7. AArch64 trampoline, pointer-authentication, or signal-handler bugs.
   - Mitigation: isolate architecture-specific trampoline code, verify PAC compiler settings or explicitly handle PAC, test SIGSEGV/SIGBUS/SIGFPE paths, and keep `jit_profile` behind a separate x86_64-only capability.
8. `jit_allocator` singleton races during concurrent JIT compilation.
   - Mitigation: audit `jit_allocator::instance()` and its segment pool, add explicit synchronization or thread-local ownership, and test concurrent module instantiation.
9. Large `MAP_JIT` allocation or code-page toggling failures.
   - Mitigation: test under memory pressure, validate segment sizing, and support an alternate interruption path if `PROT_NONE` transitions are unreliable.
10. The default runtime is selected implicitly, inconsistently, or only when a developer remembers a special CMake flag.
   - Mitigation: use explicit default-selection logic such as `SYSIO_SYS_VM_JIT_IS_DEFAULT`, define it automatically for
     validated macOS arm64 JIT builds, and assert the selected default is intentional and compiled in.
11. Dead WAVM includes create unnecessary chain build coupling.
   - Mitigation: remove unused `IR/Module.h`, `Platform/Platform.h`, `WAST/WAST.h`, `IR/Validate.h`, and `using namespace IR` from `wasm_interface_private.hpp`; only keep a dependency if direct symbol use is proven.
12. `sys_vm_profile_runtime` compiles or fails incorrectly on arm64.
   - Mitigation: gate profile construction behind `SYS_VM_HAS_JIT_PROFILE` and provide a clear unsupported-runtime error for profile mode.
13. Shared timed-run timeout state masks genuine faults across threads.
   - Mitigation: make timeout classification thread-local or execution-context-owned and test concurrent timeout/fault scenarios.
14. Opcode coverage scope is too vague to review.
   - Mitigation: create an x86_64 JIT opcode inventory and use it as the AArch64 implementation and test checklist.
15. The full emitter is implemented before there is evidence that it is worth maintaining.
   - Mitigation: require the Phase 5 feasibility emitter and early benchmark gate before the full opcode implementation.
16. Upstream review stalls while a local overlay diverges.
   - Mitigation: use a two-week threshold for a mergeable upstream path, then maintain a small rebased overlay with explicit removal criteria.
17. The port is mistaken for macOS production support.
   - Mitigation: document that all macOS builds are developer-only regardless of runtime, and avoid adding runtime-specific production semantics.
18. Insufficient consensus equivalence coverage.
   - Mitigation: compare state, traces, blocks, snapshots, and failure types across `sys-vm` and `sys-vm-jit`.
19. Confusion between benchmark/profiling report requirements and `jit_profile` support.
   - Mitigation: leave `jit_profile` x86_64-only for this port and use scoped timers/external profilers for the macOS arm64 inclusion report.
20. `sys-vm-jit` works but is not worth maintaining as the macOS arm64 default.
   - Mitigation: require a profiling/inclusion report with clear wins over `sys-vm`; defer the default switch or the port
     if benefits are marginal.
21. Excessive churn in the existing x86_64 JIT.
   - Mitigation: mirror the hand-written x86_64 design for AArch64, keep x86_64 changes limited to compatibility seams, and run x86_64 JIT regression tests after each shared plumbing change.
22. Accidental `sys-vm-oc` scope creep.
   - Mitigation: keep CMake gates and documentation separate.

## Suggested PR Breakdown

1. `wire-sysio`: add explicit default-runtime selection such as `SYSIO_SYS_VM_JIT_IS_DEFAULT`, automatically select
   `sys-vm-jit` for validated macOS arm64 JIT builds, and assert the selected default is compiled in.
2. `wire-sysio`: remove dead direct WAVM includes from `wasm_interface_private.hpp`, or prove and document the dependency if direct symbol use remains.
3. `wire-sys-vm`: add architecture capability macros and keep x86_64 behavior unchanged.
4. `wire-sys-vm`: add minimal architecture-selection seams for trampoline, x86_64-only `jit_profile`, signal, timeout-state ownership, and architecture config plumbing while preserving x86_64 behavior.
5. `wire-sys-vm`: redesign macOS arm64 JIT allocation around `MAP_JIT`, Apple write-protect toggles, instruction-cache flushing, allocator singleton thread safety, and `enable_code()`/`disable_code()` tests.
6. `wire-sys-vm`: add a minimal AArch64 feasibility emitter and early benchmark report proving the port is worth continuing.
7. `wire-sys-vm`: add the opcode coverage inventory and full hand-written AArch64 code generator mirroring the x86_64 JIT design, with standalone differential, fuzz, PAC/trampoline, and generated-code audit tests.
8. `wire-sysio`: update vcpkg override or maintain a temporary overlay to the JIT-capable `wire-sys-vm`, with the upstream merge/removal plan tracked.
9. `wire-sysio`: replace x86_64 JIT gates with capability gates, gate profile mode with `SYS_VM_HAS_JIT_PROFILE`, improve instantiate errors, and expose `sys-vm-jit` on arm64.
10. `wire-sysio`: update runtime option text and document that macOS builds are developer-only regardless of runtime.
11. `wire-sysio`: add profiling and benchmark report proving `sys-vm-jit` is worth including over `sys-vm`.
12. `wire-sysio`: add cross-runtime chain equivalence tests, with `sys-vm-jit` as the macOS arm64 default and explicit
    `--wasm-runtime sys-vm` coverage for fallback/comparison.
13. `wire-sysio`: add macOS arm64 JIT CI smoke tests with native-arm64 assertions.

## Fuzzing Budget Policy

No existing VM fuzzing budget was found in this repo. The port should define one explicitly instead of relying on the
historical AFL helper or committed fuzz-regression modules.

1. Pre-merge CI:
   - run the existing committed fuzz-regression corpus;
   - run deterministic differential tests with fixed seeds and a fixed module count;
   - do not run open-ended AFL/libFuzzer-style jobs.
2. Nightly/manual validation:
   - run live fuzzing with a fixed wall-clock budget per target and per worker;
   - prioritize interpreter-versus-JIT differential fuzzing before pure crash fuzzing;
   - retain generated corpora, crashing inputs, timeout inputs, logs, and minimized reproducers as artifacts.
3. Regression promotion:
   - minimize every unique live-fuzz failure;
   - add it to the deterministic regression corpus;
   - require the promoted regression to pass under both `sys-vm` and `sys-vm-jit` before closing the bug.
