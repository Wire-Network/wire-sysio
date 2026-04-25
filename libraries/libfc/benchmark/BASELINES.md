# fc::variant benchmark baselines

These numbers establish the starting point for the fc::variant
performance follow-on series.  Every subsequent optimization commit
should re-run the same benchmark and record the delta in its commit
message (and update the "latest" column below when it merges).

## How to run

```
ninja -C cmake-build-relwithdebinfo -j8 variant_bench
./cmake-build-relwithdebinfo/libraries/libfc/benchmark/variant_bench
```

The series is pinned to `RelWithDebInfo` (`-O2 -g -DNDEBUG`) so that
deltas between commits remain directly comparable AND the binary is
debuggable when investigating a regression.  Pure Release (`-O3`)
would shave a few percent more off the absolute numbers but would
not change the relative ordering of the scenarios; if you want a
final number for an external audience, re-baseline once at `-O3`
before publishing.

Debug numbers are NOT comparable and should not be posted.

The harness warms up, then takes the median of 10 runs of N iterations
to damp out context-switch and thermal-throttle outliers.

## Scenarios

| ID | What it measures |
|---|---|
| ctor_null                 | `fc::variant()` default ctor + dtor. |
| ctor_int64                | `fc::variant(int64_t)` -- inline primitive path. |
| ctor_double               | `fc::variant(double)` -- inline primitive path. |
| ctor_short_string         | `fc::variant("short")` -- 5 char string (SSO inline once C6 lands). |
| ctor_sso_boundary_14      | `fc::variant("fourteen_bytex")` -- exactly at SSO threshold. |
| ctor_just_over_sso_15     | `fc::variant("fifteen_bytes_x")` -- one byte past, heap path. |
| ctor_long_string          | `fc::variant("...64 chars...")` -- past any plausible SSO threshold. |
| ctor_empty_mvo            | `fc::mutable_variant_object()` default ctor + dtor.  Phase A item 1 watch. |
| ctor_empty_vo             | `fc::variant_object()` default ctor + dtor.  Phase A item 1 watch. |
| copy_int64                | Copy ctor of an int64-bearing variant -- no heap. |
| copy_short_string         | Copy ctor of a short-string variant -- one heap alloc. |
| copy_long_string          | Copy ctor of a 128-char-string variant -- big heap copy. |
| copy_object_50key         | Copy ctor of a 50-key object variant -- shared_ptr bump. |
| assign_long_string_to_long | `op=(const variant&)` between two heap-string variants -- watches Phase B5. |
| assign_object_to_object   | `op=(const variant&)` between two object variants -- watches Phase B5. |
| assign_array_to_array     | `op=(const variant&)` between two array variants -- watches Phase B5. |
| find_hit_4key             | `variant_object::find` hit on small object. |
| find_miss_4key            | `variant_object::find` miss on small object. |
| find_hit_50key_first      | Hit on first key (best case for linear scan). |
| find_hit_50key_last       | Hit on last key (worst case for linear scan).  Phase B item 4 watch. |
| find_miss_50key           | Full scan + miss.  Phase B item 4 watch. |
| contains_then_op_50key    | `contains()` followed by `operator[]` on hit -- the double-scan. |
| find_or_50key_hit         | `variant_object::find_or` on hit -- the single-scan replacement. |
| find_or_50key_miss        | `variant_object::find_or` on miss -- no throw on miss. |
| as_enum_int               | `as_enum_value<E>` from int variant -- numeric fast path. |
| as_enum_string_valid      | `as_enum_value<E>` from numeric-string variant.  Phase A item 3 watch. |
| as_enum_string_invalid    | `as_enum_value<E>` from non-numeric string -- throw path. |
| as_string_int64           | `as_string()` on int64 -- `std::to_string`. |
| as_int64_string           | `as_int64()` on int64 -- inline read. |
| json_parse_50key          | `fc::json::from_string` of a 50-key object payload. |
| json_to_string_50key      | `fc::json::to_string` of the same payload. |
| walk_50key_by_name        | `from_variant<Row>` shape -- 30 named lookups + as_int64. |

## Baseline

Environment:
- CPU: 12th Gen Intel Core i9-12900K
- OS: WSL2 Linux 6.6.87.2
- Compiler: clang 18.1.8
- Build type: RelWithDebInfo (-O2 -g -DNDEBUG)

| Benchmark              | median ns/op |  min ns/op |  max ns/op |
|---|---:|---:|---:|
| ctor_null              |          1.8 |        1.7 |        2.2 |
| ctor_int64             |          2.0 |        1.9 |        2.0 |
| ctor_double            |          2.0 |        2.0 |        2.1 |
| ctor_short_string      |         14.2 |       13.0 |       15.2 |
| ctor_long_string       |         20.5 |       18.7 |       20.9 |
| ctor_empty_mvo         |          7.5 |        6.9 |        8.2 |
| ctor_empty_vo          |          8.6 |        8.4 |       10.0 |
| copy_int64             |          2.2 |        2.1 |        2.2 |
| copy_short_string      |         11.3 |       10.8 |       12.4 |
| copy_long_string       |         17.4 |       16.7 |       18.2 |
| copy_object_50key      |         11.2 |       11.0 |       11.3 |
| find_hit_4key          |          4.1 |        4.1 |        4.2 |
| find_miss_4key         |          5.2 |        4.9 |        6.4 |
| find_hit_50key_first   |          2.9 |        2.9 |        3.0 |
| find_hit_50key_last    |         51.0 |       50.3 |       57.2 |
| find_miss_50key        |         16.2 |       13.2 |       17.0 |
| contains_then_op_50key |         36.6 |       36.0 |       41.9 |
| as_enum_int            |          1.8 |        1.7 |        2.2 |
| as_enum_string_valid   |         11.6 |       10.8 |       11.9 |
| as_enum_string_invalid |       3976.4 |     3903.3 |     4517.6 |
| as_string_int64        |          6.4 |        6.3 |        6.5 |
| as_int64_string        |          1.4 |        1.3 |        1.6 |
| json_parse_50key       |       9760.6 |     9613.0 |    10455.6 |
| json_to_string_50key   |       3389.9 |     3282.3 |     4237.8 |
| walk_50key_by_name     |        997.4 |      987.1 |     1131.7 |

## Same-type op= deltas (Phase B5)

Captured by stashing the variant.cpp change and rerunning:

| Scenario | Pre-B5 ns | Post-B5 ns | Speedup |
|---|---:|---:|---|
| assign_long_string_to_long | 17.0 |  3.3 | 5.2x |
| assign_object_to_object    | 10.3 |  2.0 | 5.1x |
| assign_array_to_array      | 32.9 | 12.1 | 2.7x |

The pre-B5 path was clear() (which delete'd the existing heap object)
followed by a fresh `new`; B5 routes same-type assign through the
existing heap object directly (`*existing = other_value`), saving the
dealloc+alloc pair.

## Observations from baseline

- `as_enum_string_invalid` is **~4 µs** -- the cost of `stoll` throwing
  and the `catch(...)` unwinding.  Phase A item 3 (replace with
  `from_chars`) should drop this by 1-2 orders of magnitude.

- `find_hit_50key_last` is **17x** slower than `find_hit_50key_first`
  (51 ns vs 2.9 ns).  That is the linear-scan cost on a 50-entry
  vector.  Phase B item 4 (hash side-table at some threshold) targets
  this directly; the small-object scenarios (`find_hit_4key` at 4 ns)
  are the regression watch -- a hash index that hurts 4-key lookups
  is not a win.

- `contains_then_op_50key` (36.6 ns) is roughly the sum of two scans:
  one for the `contains` check, one for the `operator[]` lookup.
  Phase A item 2 (`find_or` non-throwing helper) collapses this back
  to a single scan.

- `ctor_empty_mvo` and `ctor_empty_vo` (7.5 / 8.6 ns) are the
  `make_shared<vector<entry>>` allocation cost -- a default-
  constructed `mutable_variant_object` always allocates.  Phase A
  item 1 (lazy-allocate) targets this.

- `walk_50key_by_name` (~1 µs) is the ABI-decode-shaped workload --
  30 named lookups + as_int64.  This is the integrated regression
  watch for the whole series.

## Log

Append one row per merged commit in the follow-on series.

| Commit | ctor_short_string | ctor_empty_mvo | find_hit_50key_last | find_miss_50key | find_or_50key_hit | as_enum_string_valid | as_enum_string_invalid | walk_50key_by_name |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline                                  |  14.2 |   7.5 |  51.0 |  16.2 |    -- |  11.6 | 3976.4 |  997.4 |
| A3 as_enum_value uses from_chars          |  12.7 |   7.3 |  51.1 |  16.4 |    -- |   4.6 | 2965.0 |  893.5 |
| A1 lazy-allocate variant_object vector    |  13.9 |   1.4 |  55.1 |  15.0 |    -- |   5.7 | 3012.7 |  972.2 |
| A2 add variant_object::find_or helper     |  14.7 |   1.6 |  58.0 |  15.0 |  19.9 |   4.1 | 3433.7 | 1038.3 |
| C6 SSO for short strings (<= 14 bytes)    |   3.6 |   1.5 |  59.1 |  16.9 |  22.6 |   3.8 | 3611.1 | 1094.8 |
| B5 same-type op= reuses heap object       |   3.6 |   1.5 |  59.1 |  16.9 |  22.6 |   3.8 | 3611.1 | 1094.8 |
