# Trace transaction-index lifetime validation

The trace transaction-index reader now uses `fc::read_only_array_view<trx_id_bucket>` to interpret its native
bucket representations. The view calls `std::start_lifetime_as_array` when the standard library advertises
`__cpp_lib_start_lifetime_as >= 202207L`. Its C++20 fallback keeps a byte pointer and copies each requested
record into a value using `memcpy` and `bit_cast`. Both paths return values and preserve lazy, read-only access.
Construction touches no mapped pages. The file layout, validation, bounded probing, and full-ID confirmation
performed by callers are unchanged.

This is a view abstraction rather than a complete backport of the standard lifetime API. A self-`memmove`
fallback can start implicit object lifetimes in writable storage, but cannot promise the standard API's lack
of memory accesses. A local Clang 18 `-O0` probe using self-`memmove` on a read-only mapping terminated with
SIGSEGV; `-O3` removed the call. The view avoids relying on that optimization. See the
[C++23 lifetime specification](https://timsong-cpp.github.io/cppwp/n4950/obj.lifetime) and
[P0593R6](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html).

Validation on 2026-11-16 used Clang 18.1.8 with libstdc++ 14 on x86-64 Linux. Focused builds reused the existing
audit worktree's dependency archives and compiled the changed reader and tests in this worktree. These checks
do not constitute a full repository rebuild or test sweep. The focused suite passed 22 cases and 515 assertions
in release, debug, and scoped ASan/UBSan builds. It includes the existing index corruption and probing tests,
reader move construction and assignment, and five view cases. Guard pages verify that construction does not
access the mapping and that reads at page boundaries neither write into read-only storage nor access protected
neighboring pages.
Separate C++20 builds of the five view cases passed with Clang 18 and GCC 13, including a record with a deleted
default constructor. Compile-time constraints reject nontrivial, cv-qualified, void, and array element types.

The installed libraries lack the standard lifetime API, so the runtime checks exercise the fallback. The native
branch is feature-gated and follows the standard signature; it still needs a run on a library that provides it.

The optional `benchmark_trx_id_index` target exercises the actual production reader. The baseline used the
reader and header from `f9ef731d4d`, with the identical benchmark source, compiler flags (`-O3 -DNDEBUG`), and
dependency archives. Measurements ran on an Intel Xeon 6975P-C, pinned to CPU 0, in two ABBA process sequences.
Each process recorded seven samples, producing 28 samples per implementation and scenario. Every lookup sample
performs 1,048,576 operations with an independently checked result checksum. Tables contain 262,144 entries in
1,048,576 buckets, approximately 16 MiB. Collision workloads use groups of 32 prefixes sharing an initial slot.
Missing queries retain an occupied initial slot and exercise probing. Reader-open samples include mapping,
header validation, and unmapping, repeated 1,024 times with a warm file cache.

| Scenario | Baseline median ns/op | View median ns/op | Change |
| --- | ---: | ---: | ---: |
| Hit | 6.29 | 6.14 | -2.5% |
| Miss | 14.45 | 14.15 | -2.0% |
| Collision hit | 44.85 | 44.98 | +0.3% |
| Collision miss | 43.80 | 39.87 | -9.0% |
| Open and close reader | 5,874.69 | 5,964.14 | +1.5% |

The collision-hit and reader-open timing distributions overlap: their interquartile ranges were respectively
44.73–45.09 versus 44.82–45.17 ns, and 5,768.51–5,968.08 versus 5,862.39–6,165.32 ns. These measurements show no
material lookup regression in the tested workloads; they do not guarantee identical performance on other hosts
or on cold storage. Optimized lookup disassembly contains direct field loads, no calls, and no stack copy.
Keeping the validated bucket count in 32 bits avoids a slower probe loop observed with a `size_t` loop bound.

An open/close follow-up compared both readers in the same process, pinned to CPU 0 and using the same index.
The original reader's symbols were renamed for this diagnostic so both implementations could be linked into
one executable. Twenty ABBA rounds produced 40 samples per implementation, each with 16,384 open/close cycles.
The medians were 5,639.15 ns for the original and 5,636.91 ns for the view, a -0.04% difference. Their interquartile
ranges were 5,521.80–5,781.09 and 5,553.73–5,769.40 ns. This did not reproduce the initial +1.5% difference and
supports treating it as measurement variation, without assigning a specific kernel or scheduling cause.

Constructor/destructor assembly shows the same successful-path calls to `open`, `fstat`, `mmap`, `close`, and
`munmap`. View setup adds no allocation, bucket access, or copy call. The reader remains 40 bytes: its `size_t`
count occupies the old 32-bit count and adjacent padding. The visible changes are a wider count store and
different scalar/SIMD stores when clearing the reader object. Those stores affect the reader, not the mapped
bucket array. Diagnostic source, raw samples, and the summary are saved as `open_close_probe.cpp`,
`open-close-probe.csv`, and `open-close-probe-summary.json` under the local validation build directory.

Local raw CSV samples, assembly, logs, and `benchmark-summary.json` are retained under `build/trace-lifetime/`.
The benchmark is excluded from the default build and test discovery. In a configured build, reproduce the
candidate checks with:

```sh
cmake --build build/<config> --target test_fc test_trace_api_plugin benchmark_trx_id_index
python3 tests/bind_registry.py build/<config>/libraries/libfc/test/test_fc \
  --run_test=read_only_array_view_tests
python3 tests/bind_registry.py build/<config>/plugins/trace_api_plugin/test/test_trace_api_plugin \
  --run_test=trx_id_index_tests
python3 tests/bind_registry.py build/<config>/plugins/trace_api_plugin/test/benchmark_trx_id_index \
  > /tmp/trace-index-benchmark.csv
```
