# fc::variant benchmark baselines

These numbers establish the starting point for the fc::variant
performance follow-on series.  Every subsequent optimization commit
should re-run the same benchmark and record the delta in its commit
message (and update the "latest" column below when it merges).

## How to run

```
cmake -B cmake-build-release -S . -G Ninja \
   -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_TOOLCHAIN_FILE=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake \
   -DCMAKE_PREFIX_PATH=/opt/prefixes/wire-001
ninja -C cmake-build-release -j8 variant_bench
./cmake-build-release/libraries/libfc/benchmark/variant_bench
```

Release build (`-DCMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`) is
required.  Debug or RelWithDebInfo numbers are not comparable and
should not be posted.

The harness warms up, then takes the median of 10 runs of N iterations
to damp out context-switch and thermal-throttle outliers.

## Scenarios

| ID | What it measures |
|---|---|
| ctor_null                 | `fc::variant()` default ctor + dtor. |
| ctor_int64                | `fc::variant(int64_t)` -- inline primitive path. |
| ctor_double               | `fc::variant(double)` -- inline primitive path. |
| ctor_short_string         | `fc::variant("short")` -- 5 char heap string. |
| ctor_long_string          | `fc::variant("...64 chars...")` -- past any plausible SSO threshold. |
| ctor_empty_mvo            | `fc::mutable_variant_object()` default ctor + dtor.  Phase A item 1 watch. |
| ctor_empty_vo             | `fc::variant_object()` default ctor + dtor.  Phase A item 1 watch. |
| copy_int64                | Copy ctor of an int64-bearing variant -- no heap. |
| copy_short_string         | Copy ctor of a short-string variant -- one heap alloc. |
| copy_long_string          | Copy ctor of a 128-char-string variant -- big heap copy. |
| copy_object_50key         | Copy ctor of a 50-key object variant -- shared_ptr bump. |
| find_hit_4key             | `variant_object::find` hit on small object. |
| find_miss_4key            | `variant_object::find` miss on small object. |
| find_hit_50key_first      | Hit on first key (best case for linear scan). |
| find_hit_50key_last       | Hit on last key (worst case for linear scan).  Phase B item 4 watch. |
| find_miss_50key           | Full scan + miss.  Phase B item 4 watch. |
| contains_then_op_50key    | `contains()` followed by `operator[]` on hit -- the double-scan. |
| as_enum_int               | `as_enum_value<E>` from int variant -- numeric fast path. |
| as_enum_string_valid      | `as_enum_value<E>` from numeric-string variant.  Phase A item 3 watch. |
| as_enum_string_invalid    | `as_enum_value<E>` from non-numeric string -- throw path. |
| as_string_int64           | `as_string()` on int64 -- `std::to_string`. |
| as_int64_string           | `as_int64()` on int64 -- inline read. |
| json_parse_50key          | `fc::json::from_string` of a 50-key object payload. |
| json_to_string_50key      | `fc::json::to_string` of the same payload. |
| walk_50key_by_name        | `from_variant<Row>` shape -- 30 named lookups + as_int64. |

## Baseline (commit `fc::variant: add microbenchmark scaffold`)

Environment:
- CPU: <fill in from /proc/cpuinfo model name>
- OS: WSL2 Linux 6.6.87.2
- Compiler: clang 18
- Build type: Release (-O3 -DNDEBUG)

| Benchmark | median ns/op | min ns/op | max ns/op |
|---|---:|---:|---:|
| (filled in by the baseline-capture commit)                        |           |           |           |

## Log

Append one row per merged commit in the follow-on series.

| Commit | ctor_empty_mvo | find_hit_50key_last | find_miss_50key | as_enum_string_valid | walk_50key_by_name |
|---|---:|---:|---:|---:|---:|
