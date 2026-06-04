# sys-vm-jit macOS arm64 Benchmark Report

## Build Under Test

This report covers the first native Apple Silicon `sys-vm-jit` integration build validated with the wide host-call fix.

- Host: macOS arm64
- Chain build directory: `build/macos-arm64-jit-release`
- Upstream VM benchmark directory: `/Users/huangming/opensource/eos-vm/build/macos-arm64-port-release-vcpkg`
- Build type: `Release`
- CMake architecture: `-DCMAKE_OSX_ARCHITECTURES=arm64`
- vcpkg triplet: `arm64-osx`
- JIT default: `sys-vm-jit` is selected by default for macOS arm64 when the backend is compiled in.
- `wire-sys-vm`: `1.1.1#11`
- `wire-vcpkg-registry`: `fa3a78b3aa09310bad0ceca37de8898ac09291a4`
- `eos-vm`: `0fdc743a084c97fbc17f8a11a9fe463cb9d43ab1`

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
interpreter_avg_ms=315.801
aarch64_jit_avg_ms=95.378
speedup_avg=3.320
checksum=10039950000
```

The five raw Release runs were:

```text
interpreter_ms=317.995 aarch64_jit_ms=91.8275 speedup=3.46296
interpreter_ms=307.135 aarch64_jit_ms=103.454 speedup=2.96879
interpreter_ms=313.852 aarch64_jit_ms=92.1501 speedup=3.40588
interpreter_ms=314.214 aarch64_jit_ms=91.5273 speedup=3.433
interpreter_ms=325.808 aarch64_jit_ms=97.9293 speedup=3.32697
```

## Interpretation

The AArch64 JIT is materially faster than the interpreter in a hot VM execution loop, with a measured 3.139x Release
speedup for the upstream `sum_to` benchmark. The current chain-level unit suites are roughly tied because they are not
designed to isolate warm JIT execution.

Based on this evidence, `sys-vm-jit` is worth including as the default macOS arm64 developer runtime while keeping
`sys-vm` available through explicit runtime selection for comparison and fallback. All macOS builds remain
developer-only. The next useful benchmark improvement is a chain-level warm contract execution harness that separates
module instantiation/setup from repeated action execution.
The existing `kv_benchmark_contract` suite is currently compiled as `perf_benchmarks_disabled` unless the build defines
`RUN_PERF_BENCHMARKS`, so it was not used for this report.
