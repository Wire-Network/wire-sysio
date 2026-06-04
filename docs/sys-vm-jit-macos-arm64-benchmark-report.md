# sys-vm-jit macOS arm64 Benchmark Report

## Build Under Test

This report covers the first native Apple Silicon `sys-vm-jit` integration build validated with the wide host-call fix.

- Host: macOS arm64
- Chain build directory: `build/macos-arm64-jit-release`
- Upstream VM benchmark directory: `/Users/huangming/opensource/eos-vm/build/macos-arm64-port-release-vcpkg`
- Build type: `Release`
- CMake architecture: `-DCMAKE_OSX_ARCHITECTURES=arm64`
- vcpkg triplet: `arm64-osx`
- JIT default: `-DSYSIO_SYS_VM_JIT_IS_DEFAULT=OFF`
- `wire-sys-vm`: `1.1.1#10`
- `wire-vcpkg-registry`: `e95cc91626099c197d38881c89ac3aa9348dce3e`
- `eos-vm`: `9ca8b3fc8173310bf47203c574b5115f7719f9b2`

The local build was configured with `-DDISABLE_WASM_SPEC_TESTS=ON`, so this report does not claim a full WASM spec
CTest sweep.

## Native arm64 Verification

| Check | Result |
| --- | --- |
| `file build/macos-arm64-jit-release/programs/nodeop/nodeop` | `Mach-O 64-bit executable arm64` |
| `file build/macos-arm64-jit-release/unittests/unit_test` | `Mach-O 64-bit executable arm64` |
| `nodeop --version` | `v1.0.0-dev` |
| `clio version client` | `v1.0.0-dev` |

## Validation Matrix

| Command | Result |
| --- | --- |
| `cmake --build build/macos-arm64-jit-release -- -j$(sysctl -n hw.logicalcpu)` | Passed |
| `ctest -R '^crypto_primitives_unit_test_sys-vm-jit$'` | Passed |
| Isolated Release comparison: `noop`, `wasm_part1-3`, `crypto_primitives`, `system_host` under `sys-vm` and `sys-vm-jit` | Passed, 12/12 tests |
| Release sweep excluding `nonparallelizable_tests`, `long_running_tests`, and `wasm_spec_tests` | Passed, 216/216 tests |
| `eos-vm build/macos-arm64-port/tests/unit_tests '[jit][aarch64]'` | Passed, 83 assertions in 31 test cases |
| `eos-vm ctest --test-dir build/macos-arm64-port --output-on-failure` | Passed, 285/285 tests |

The earlier Debug broad sweep timed out in `finality_unit_test_sys-vm` at 1800 seconds. Per the macOS arm64 test policy,
the broad sweep was moved to the Release build; the same finality tests completed in about 35 seconds there.

## Benchmark Results

The chain-level rows are isolated Release CTest wall-clock measurements. These suites are useful correctness and
integration checks, but they include fixture setup, chain execution, module parsing, and Boost.Test overhead. They should
not be read as isolated hot WASM execution throughput.

| Workload | `sys-vm` | `sys-vm-jit` | Speedup |
| --- | ---: | ---: | ---: |
| `noop_unit_test` | 0.54 s | 0.54 s | 1.00x |
| `crypto_primitives_unit_test` | 0.85 s | 0.83 s | 1.02x |
| `system_host_unit_test` | 1.03 s | 1.06 s | 0.97x |
| `wasm_part1_unit_test` | 2.56 s | 2.55 s | 1.00x |
| `wasm_part2_unit_test` | 1.79 s | 1.86 s | 0.96x |
| `wasm_part3_unit_test` | 1.53 s | 1.50 s | 1.02x |
| Combined rows above | 8.30 s | 8.38 s | 0.99x |
| `eos-vm` `sum_to` hot loop, 20,000 iterations, 5-run Release average | 309.759 ms | 98.687 ms | 3.139x |

The upstream VM benchmark used:

```text
kernel=sum_to
input=1000
iterations=20000
runs=5
interpreter_avg_ms=309.759
aarch64_prototype_jit_avg_ms=98.687
speedup_avg=3.139
checksum=10039950000
```

The five raw Release runs were:

```text
interpreter_ms=306.602 aarch64_prototype_jit_ms=98.4484 speedup=3.11434
interpreter_ms=307.179 aarch64_prototype_jit_ms=98.4267 speedup=3.12089
interpreter_ms=313.512 aarch64_prototype_jit_ms=99.9926 speedup=3.13535
interpreter_ms=306.085 aarch64_prototype_jit_ms=98.7459 speedup=3.09972
interpreter_ms=315.415 aarch64_prototype_jit_ms=97.8232 speedup=3.22434
```

## Interpretation

The AArch64 JIT is materially faster than the interpreter in a hot VM execution loop, with a measured 3.139x Release
speedup for the upstream `sum_to` benchmark. The current chain-level unit suites are roughly tied because they are not
designed to isolate warm JIT execution.

Based on this evidence, `sys-vm-jit` is worth including as an opt-in macOS arm64 developer/runtime experiment, while
keeping `sys-vm` as the default and keeping all macOS builds developer-only. The next useful benchmark improvement is a
chain-level warm contract execution harness that separates module instantiation/setup from repeated action execution.
The existing `kv_benchmark_contract` suite is currently compiled as `perf_benchmarks_disabled` unless the build defines
`RUN_PERF_BENCHMARKS`, so it was not used for this report.
